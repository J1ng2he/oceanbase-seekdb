/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "observer/ob_inner_sql_connection.h"
#include "rootserver/ob_server_thread_helper.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "lib/string/ob_sql_string.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_isql_connection.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{

namespace
{
// The table that holds the AI column tasks.
const char *const OB_AI_TASK_TABLE = "oceanbase.__all_ai_column_task";
// The interval between two polls of the task table.
const int64_t OB_AI_WORKER_POLL_INTERVAL_US = 50 * 1000;
// The maximum number of tasks that one poll reads.
const int64_t OB_AI_WORKER_TASK_LIMIT = 64;
// The number of seconds that a task waits before it is tried again.
const int64_t OB_AI_WORKER_RETRY_DELAY_SEC = 1;
// The name of the thread of the worker.
const char *const OB_AI_WORKER_THREAD_NAME = "AiColumnWorker";

// All the integer types are rendered as a base-10 integer by the model, the
// same rule is used by the DDL resolver, see ObAiColumnUtil::get_type_hint.
bool is_ai_integer_type(ObObjType data_type)
{
  return (data_type >= ObTinyIntType && data_type <= ObIntType)
         || (data_type >= ObUTinyIntType && data_type <= ObUInt64Type);
}

// Copy a string of a result set into the allocator, the string of the result
// set is only valid while the result set is open.
int copy_result_string(ObIAllocator &allocator, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(src.length() + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (src.length() > 0) {
      memcpy(buf, src.ptr(), src.length());
    }
    buf[src.length()] = '\0';
    dst.assign_ptr(buf, src.length());
  }
  return ret;
}

// Build the literal of a string value.
int build_string_literal(ObIAllocator &allocator, const ObString &value, ObString &literal)
{
  int ret = OB_SUCCESS;
  ObString escaped;
  if (OB_FAIL(share::ObAiColumnUtil::escape_sql_string(allocator, value, escaped))) {
  } else {
    char *buf = static_cast<char *>(allocator.alloc(escaped.length() + 3));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      buf[0] = '\'';
      if (escaped.length() > 0) {
        memcpy(buf + 1, escaped.ptr(), escaped.length());
      }
      buf[escaped.length() + 1] = '\'';
      buf[escaped.length() + 2] = '\0';
      literal.assign_ptr(buf, escaped.length() + 2);
    }
  }
  return ret;
}

// Build the literal of an integer value.  The model answers with a base-10
// integer, the answer is checked before it is used, so that the value of the
// AI column is never a wrong number.
int build_integer_literal(ObIAllocator &allocator, const ObString &value, ObString &literal)
{
  int ret = OB_SUCCESS;
  bool valid = (value.length() > 0);
  for (int64_t i = 0; valid && i < value.length(); i++) {
    const char c = value.ptr()[i];
    if (c < '0' || c > '9') {
      valid = false;
    }
  }
  if (!valid) {
    ret = OB_INVALID_DATA;
    LOG_WARN("the answer of the model is not an integer", K(value));
  } else {
    char *buf = static_cast<char *>(allocator.alloc(value.length() + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      memcpy(buf, value.ptr(), value.length());
      buf[value.length()] = '\0';
      literal.assign_ptr(buf, value.length());
    }
  }
  return ret;
}

// Build the literal of a vector value.  The answer of the embedding model is a
// json array of the floats, the whitespace is removed, so that the text is a
// valid vector literal.
int build_vector_literal(ObIAllocator &allocator, const ObString &value, ObString &literal)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(value.length() + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t j = 0;
    for (int64_t i = 0; i < value.length(); i++) {
      const char c = value.ptr()[i];
      if (' ' == c || '\t' == c || '\n' == c || '\r' == c) {
        continue;
      }
      buf[j++] = c;
    }
    buf[j] = '\0';
    literal.assign_ptr(buf, j);
  }
  return ret;
}

// One row of the task table.
struct ObAiWorkerTask
{
  ObString task_id_;
  uint64_t table_id_;
  uint64_t column_id_;
  ObString rowkey_predicate_;
  ObString request_id_;
  int64_t state_;
  int64_t retry_count_;
};

// The value of one AI column of a row, it is the literal of the sql that
// writes the value into the row.
struct ObAiWorkerValue
{
  uint64_t column_id_;
  ObString literal_;
};

} // namespace

// The worker that computes the value of the AI columns.  The worker reads the
// tasks of `oceanbase.__all_ai_column_task`, calls the model of every AI
// column of the row and writes the result back into the row.
//
// The tasks of one row share the request id of the invalidation that created
// them, so the worker groups the tasks by the request id.  The values of all
// the AI columns of a group are written by one statement, so that the columns
// of a row are never partly visible.  The request id of the row is checked
// before the value is written, so that the result of an old request never
// overwrites the value of a newer one.
class ObAiColumnWorker : public rootserver::ObServerThreadHelper
{
public:
  ObAiColumnWorker() : is_inited_(false) {}
  virtual ~ObAiColumnWorker() { destroy(); }
  int init();
  int start();
  void stop();
  void wait();
  void destroy();
  virtual void do_work() override;
  // the worker is stopped when the server module is deactivated and it is
  // started again when the server module is activated
  virtual void deactivate() override { stop(); }
  virtual int activate() override { return start(); }
private:
  int process_pending_tasks();
  int read_tasks(sqlclient::ObISQLConnection &conn,
                 ObIAllocator &allocator,
                 ObSEArray<ObAiWorkerTask, 64> &tasks);
  int handle_group(sqlclient::ObISQLConnection &conn,
                   ObIAllocator &allocator,
                   const ObSEArray<ObAiWorkerTask, 64> &tasks,
                   int64_t begin,
                   int64_t end);
  int compute_value(sqlclient::ObISQLConnection &conn,
                    ObIAllocator &allocator,
                    share::schema::ObSchemaGetterGuard &guard,
                    const share::schema::ObTableSchema &table_schema,
                    const share::schema::ObDatabaseSchema &database_schema,
                    const share::schema::ObColumnSchemaV2 &ai_column,
                    const share::ObAiColumnSpec &spec,
                    const ObAiWorkerTask &task,
                    ObAiWorkerValue &value);
  int eval_prompt(sqlclient::ObISQLConnection &conn,
                  ObIAllocator &allocator,
                  const share::schema::ObTableSchema &table_schema,
                  const share::schema::ObDatabaseSchema &database_schema,
                  const share::ObAiColumnSpec &spec,
                  const ObAiWorkerTask &task,
                  ObString &prompt);
  int write_back_group(sqlclient::ObISQLConnection &conn,
                       ObIAllocator &allocator,
                       const share::schema::ObTableSchema &table_schema,
                       const share::schema::ObDatabaseSchema &database_schema,
                       const ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns,
                       const ObIArray<const share::schema::ObColumnSchemaV2 *> &state_columns,
                       const ObIArray<const share::schema::ObColumnSchemaV2 *> &request_columns,
                       const ObSEArray<ObAiWorkerTask, 64> &tasks,
                       int64_t begin,
                       int64_t end,
                       const ObSEArray<ObAiWorkerValue, 8> &values,
                       const ObString &request_id,
                       const ObString &rowkey_predicate);
  int handle_group_failure(sqlclient::ObISQLConnection &conn,
                           const share::schema::ObTableSchema *table_schema,
                           const share::schema::ObDatabaseSchema *database_schema,
                           const ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns,
                           const ObIArray<const share::schema::ObColumnSchemaV2 *> &state_columns,
                           const ObIArray<const share::schema::ObColumnSchemaV2 *> &request_columns,
                           const ObSEArray<ObAiWorkerTask, 64> &tasks,
                           int64_t begin,
                           int64_t end,
                           const ObString &request_id,
                           int64_t max_retries,
                           bool terminal);
private:
  bool is_inited_;
  ObArenaAllocator allocator_;
  sqlclient::ObISQLConnectionGuard conn_;
};

int ObAiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(rootserver::ObServerThreadHelper::create(OB_AI_WORKER_THREAD_NAME, 1))) {
  } else {
    is_inited_ = true;
    LOG_INFO("[AI_COLUMN_WORKER] init success");
  }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(rootserver::ObServerThreadHelper::start())) {
  } else {
    LOG_INFO("[AI_COLUMN_WORKER] start success");
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  if (is_inited_) {
    rootserver::ObServerThreadHelper::stop();
    wakeup();
    LOG_INFO("[AI_COLUMN_WORKER] stop success");
  }
}

void ObAiColumnWorker::wait()
{
  if (is_inited_) {
    rootserver::ObServerThreadHelper::wait();
    LOG_INFO("[AI_COLUMN_WORKER] wait success");
  }
}

void ObAiColumnWorker::destroy()
{
  if (is_inited_) {
    rootserver::ObServerThreadHelper::destroy();
    is_inited_ = false;
    LOG_INFO("[AI_COLUMN_WORKER] destroy success");
  }
}

void ObAiColumnWorker::do_work()
{
  while (!has_set_stop()) {
    int ret = process_pending_tasks();
    if (OB_FAIL(ret)) {
      LOG_WARN("[AI_COLUMN_WORKER] failed to process the tasks", K(ret));
    }
    idle(OB_AI_WORKER_POLL_INTERVAL_US);
  }
}

int ObAiColumnWorker::process_pending_tasks()
{
  int ret = OB_SUCCESS;
  allocator_.reset();
  if (!conn_.is_valid()) {
    if (OB_FAIL(ObInnerSQLConnection::create_connection_with_owned_session(false, 0, conn_))) {
      // the observer is not ready yet, the tasks are read later
      LOG_WARN("[AI_COLUMN_WORKER] failed to create the connection", K(ret));
      return OB_SUCCESS;
    }
  }
  ObSEArray<ObAiWorkerTask, 64> tasks;
  if (OB_FAIL(read_tasks(*conn_.get_ptr(), allocator_, tasks))) {
    LOG_WARN("[AI_COLUMN_WORKER] failed to read the tasks", K(ret));
    // the connection may be broken, it is created again by the next poll
    conn_.reset();
    return ret;
  }
  // the tasks are ordered by the request id, the tasks of one row are
  // consecutive, they are processed together
  int64_t begin = 0;
  while (OB_SUCC(ret) && begin < tasks.count()) {
    int64_t end = begin + 1;
    while (end < tasks.count()
           && tasks.at(end).request_id_.length() == tasks.at(begin).request_id_.length()
           && 0 == tasks.at(end).request_id_.compare(tasks.at(begin).request_id_)) {
      end++;
    }
    if (OB_FAIL(handle_group(*conn_.get_ptr(), allocator_, tasks, begin, end))) {
      LOG_WARN("[AI_COLUMN_WORKER] failed to handle the task group", K(ret), K(begin), K(end));
    }
    begin = end;
  }
  return ret;
}

int ObAiColumnWorker::read_tasks(sqlclient::ObISQLConnection &conn,
                                 ObIAllocator &allocator,
                                 ObSEArray<ObAiWorkerTask, 64> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("select task_id, table_id, column_id, rowkey_predicate, request_id, "
                              "state, retry_count from %s where state in (%lld, %lld) "
                              "and eligible_ts <= now(6) order by request_id, task_id limit %lld",
                              OB_AI_TASK_TABLE,
                              share::ObAiColumnSpec::TASK_STATE_PENDING,
                              share::ObAiColumnSpec::TASK_STATE_RETRY,
                              OB_AI_WORKER_TASK_LIMIT))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      int tmp_ret = OB_SUCCESS;
      if (OB_FAIL(conn.execute_read(sql.string(), res))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCCESS == (tmp_ret = result->next())) {
          ObAiWorkerTask task;
          ObString task_id;
          ObString rowkey_predicate;
          ObString request_id;
          int64_t table_id = 0;
          int64_t column_id = 0;
          int64_t state = 0;
          int64_t retry_count = 0;
          if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), task_id))
              || OB_FAIL(result->get_int(static_cast<int64_t>(1), table_id))
              || OB_FAIL(result->get_int(static_cast<int64_t>(2), column_id))
              || OB_FAIL(result->get_varchar(static_cast<int64_t>(3), rowkey_predicate))
              || OB_FAIL(result->get_varchar(static_cast<int64_t>(4), request_id))
              || OB_FAIL(result->get_int(static_cast<int64_t>(5), state))
              || OB_FAIL(result->get_int(static_cast<int64_t>(6), retry_count))) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          if (OB_FAIL(copy_result_string(allocator, task_id, task.task_id_))
              || OB_FAIL(copy_result_string(allocator, rowkey_predicate, task.rowkey_predicate_))
              || OB_FAIL(copy_result_string(allocator, request_id, task.request_id_))) {
            break;
          }
          task.table_id_ = static_cast<uint64_t>(table_id);
          task.column_id_ = static_cast<uint64_t>(column_id);
          task.state_ = state;
          task.retry_count_ = retry_count;
          if (OB_FAIL(tasks.push_back(task))) {
            break;
          }
        }
        if (OB_SUCC(ret) && OB_ITER_END != tmp_ret && OB_SUCCESS != tmp_ret) {
          ret = tmp_ret;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::handle_group(sqlclient::ObISQLConnection &conn,
                                    ObIAllocator &allocator,
                                    const ObSEArray<ObAiWorkerTask, 64> &tasks,
                                    int64_t begin,
                                    int64_t end)
{
  int ret = OB_SUCCESS;
  const ObAiWorkerTask &first = tasks.at(begin);
  const uint64_t table_id = first.table_id_;
  const ObString &request_id = first.request_id_;
  const ObString &rowkey_predicate = first.rowkey_predicate_;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObDatabaseSchema *database_schema = NULL;
  ObSEArray<const share::schema::ObColumnSchemaV2 *, 16> ai_columns;
  ObSEArray<const share::schema::ObColumnSchemaV2 *, 16> state_columns;
  ObSEArray<const share::schema::ObColumnSchemaV2 *, 16> request_columns;
  ObSEArray<const share::schema::ObColumnSchemaV2 *, 16> spec_columns;
  ObSEArray<ObAiWorkerValue, 8> values;
  int64_t max_retries = 0;
  // the failure of the model is retried, the other failures are permanent
  bool retryable = false;
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), database_schema))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::ObAiColumnUtil::collect_ai_columns(*table_schema,
                                                                ai_columns,
                                                                state_columns,
                                                                request_columns,
                                                                spec_columns))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // the spec of every AI column of the group is read first, so that the
    // number of retries of the group is known before the model is called
    ObSEArray<share::ObAiColumnSpec, 16> specs;
    for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
      const ObAiWorkerTask &task = tasks.at(i);
      int64_t index = -1;
      for (int64_t j = 0; j < ai_columns.count(); j++) {
        if (ai_columns.at(j)->get_column_id() == task.column_id_) {
          index = j;
          break;
        }
      }
      if (index < 0) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      share::ObAiColumnSpec spec;
      if (OB_FAIL(spec.parse(allocator, spec_columns.at(index)->get_comment()))) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      if (spec.get_max_retries() > max_retries) {
        max_retries = spec.get_max_retries();
      }
      if (OB_FAIL(specs.push_back(spec))) {
        break;
      }
    }
    for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
      const ObAiWorkerTask &task = tasks.at(i);
      int64_t index = -1;
      for (int64_t j = 0; j < ai_columns.count(); j++) {
        if (ai_columns.at(j)->get_column_id() == task.column_id_) {
          index = j;
          break;
        }
      }
      ObAiWorkerValue value;
      value.column_id_ = task.column_id_;
      if (OB_FAIL(compute_value(conn, allocator, guard, *table_schema, *database_schema,
                                 *ai_columns.at(index), specs.at(i - begin), task, value))) {
        // the model can not be reached, the task is tried again later
        retryable = true;
        break;
      }
      if (OB_FAIL(values.push_back(value))) {
        break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = write_back_group(conn, allocator, *table_schema, *database_schema, ai_columns,
                           state_columns, request_columns, tasks, begin, end, values,
                           request_id, rowkey_predicate);
  } else {
    // the group failed, the tasks are either tried again later or removed
    const bool terminal = (!retryable || first.retry_count_ + 1 > max_retries);
    LOG_WARN("[AI_COLUMN_WORKER] the task group failed",
             K(ret), K(retryable), K(max_retries), K(first.retry_count_),
             K(table_id), K(begin), K(end), K(request_id));
    int tmp_ret = handle_group_failure(conn, table_schema, database_schema, ai_columns,
                                       state_columns, request_columns, tasks, begin, end,
                                       request_id, max_retries, terminal);
    if (OB_FAIL(tmp_ret)) {
      LOG_WARN("[AI_COLUMN_WORKER] failed to handle the failure of the task group",
               K(tmp_ret), K(begin), K(end));
    }
    // the failure of one group does not stop the worker
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObAiColumnWorker::compute_value(sqlclient::ObISQLConnection &conn,
                                    ObIAllocator &allocator,
                                    share::schema::ObSchemaGetterGuard &guard,
                                    const share::schema::ObTableSchema &table_schema,
                                    const share::schema::ObDatabaseSchema &database_schema,
                                    const share::schema::ObColumnSchemaV2 &ai_column,
                                    const share::ObAiColumnSpec &spec,
                                    const ObAiWorkerTask &task,
                                    ObAiWorkerValue &value)
{
  int ret = OB_SUCCESS;
  ObString prompt;
  if (OB_FAIL(eval_prompt(conn, allocator, table_schema, database_schema, spec, task, prompt))) {
  } else {
    // the endpoint of the model and the model itself
    ObArenaAllocator endpoint_allocator;
    share::ObAiModelEndpointInfo endpoint;
    common::ObAIFuncExprInfo *info = NULL;
    if (OB_FAIL(share::ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(endpoint_allocator,
                                                                              spec.get_model_name(),
                                                                              endpoint))) {
      LOG_WARN("[AI_COLUMN_WORKER] failed to read the endpoint of the model",
               K(ret), K(spec.get_model_name()));
    } else if (OB_FAIL(common::ObAIFuncUtils::get_ai_func_info(allocator, spec.get_model_name(), guard, info))) {
      LOG_WARN("[AI_COLUMN_WORKER] failed to read the model", K(ret), K(spec.get_model_name()));
    } else {
      common::ObAIFuncModel model(allocator, *info, endpoint);
      ObString answer;
      ObString content = prompt;
      if (spec.is_embed()) {
        // the embedding model answers with the vector of the text
        if (OB_FAIL(model.call_dense_embedding(content, NULL, answer))) {
          LOG_WARN("[AI_COLUMN_WORKER] the embedding model failed", K(ret));
        }
      } else {
        // the system message tells the model the type of the value, the model
        // answers with a json value or with an integer for those types
        ObString system_prompt;
        const char *hint = share::ObAiColumnUtil::get_type_hint(ai_column);
        if (OB_NOT_NULL(hint)) {
          system_prompt.assign_ptr(hint, strlen(hint));
        }
        if (OB_FAIL(model.call_completion_with_system(content, system_prompt, NULL, answer))) {
          LOG_WARN("[AI_COLUMN_WORKER] the completion model failed", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        if (spec.is_embed()) {
          ret = build_vector_literal(allocator, answer, value.literal_);
        } else if (is_ai_integer_type(ai_column.get_data_type())) {
          ret = build_integer_literal(allocator, answer, value.literal_);
        } else {
          ret = build_string_literal(allocator, answer, value.literal_);
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::eval_prompt(sqlclient::ObISQLConnection &conn,
                                  ObIAllocator &allocator,
                                  const share::schema::ObTableSchema &table_schema,
                                  const share::schema::ObDatabaseSchema &database_schema,
                                  const share::ObAiColumnSpec &spec,
                                  const ObAiWorkerTask &task,
                                  ObString &prompt)
{
  int ret = OB_SUCCESS;
  const ObString &db_name = database_schema.get_database_name();
  const ObString &table_name = table_schema.get_table_name_str();
  const ObString &prompt_expr = spec.get_prompt_expr();
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt("select %.*s from `%.*s`.`%.*s` where %.*s",
                              prompt_expr.length(), prompt_expr.ptr(),
                              db_name.length(), db_name.ptr(),
                              table_name.length(), table_name.ptr(),
                              task.rowkey_predicate_.length(), task.rowkey_predicate_.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(conn.execute_read(sql.string(), res))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int tmp_ret = result->next();
        if (OB_ITER_END == tmp_ret) {
          // the row does not exist anymore
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(tmp_ret)) {
          ret = tmp_ret;
        } else {
          ObString value;
          if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), value))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(copy_result_string(allocator, value, prompt))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::write_back_group(sqlclient::ObISQLConnection &conn,
                                        ObIAllocator &allocator,
                                        const share::schema::ObTableSchema &table_schema,
                                        const share::schema::ObDatabaseSchema &database_schema,
                                        const ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns,
                                        const ObIArray<const share::schema::ObColumnSchemaV2 *> &state_columns,
                                        const ObIArray<const share::schema::ObColumnSchemaV2 *> &request_columns,
                                        const ObSEArray<ObAiWorkerTask, 64> &tasks,
                                        int64_t begin,
                                        int64_t end,
                                        const ObSEArray<ObAiWorkerValue, 8> &values,
                                        const ObString &request_id,
                                        const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  const ObString &db_name = database_schema.get_database_name();
  const ObString &table_name = table_schema.get_table_name_str();
  ObSqlString update_sql;
  ObSqlString delete_sql;
  if (OB_FAIL(update_sql.assign_fmt("update `%.*s`.`%.*s` set ",
                                     db_name.length(), db_name.ptr(),
                                     table_name.length(), table_name.ptr()))) {
  } else {
    // the values of all the AI columns of the group are written by one
    // statement, so that the columns of the row are never partly visible
    for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); i++) {
      const uint64_t column_id = values.at(i).column_id_;
      int64_t index = -1;
      for (int64_t j = 0; j < ai_columns.count(); j++) {
        if (ai_columns.at(j)->get_column_id() == column_id) {
          index = j;
          break;
        }
      }
      if (index < 0) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      if (i > 0 && OB_FAIL(update_sql.append(", "))) {
        break;
      }
      if (OB_FAIL(update_sql.append_fmt("`%.*s` = %.*s, `%.*s` = '%s'",
                                        static_cast<int>(ai_columns.at(index)->get_column_name_str().length()),
                                        ai_columns.at(index)->get_column_name_str().ptr(),
                                        static_cast<int>(values.at(i).literal_.length()),
                                        values.at(i).literal_.ptr(),
                                        static_cast<int>(state_columns.at(index)->get_column_name_str().length()),
                                        state_columns.at(index)->get_column_name_str().ptr(),
                                        share::ObAiColumnSpec::STATE_READY))) {
        break;
      }
    }
    // the request id of the row must be the one of the task, otherwise the
    // result is the result of an old request and it must be dropped
    if (OB_SUCC(ret) && OB_FAIL(update_sql.append_fmt(" where %.*s",
                                                      static_cast<int>(rowkey_predicate.length()),
                                                      rowkey_predicate.ptr()))) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); i++) {
      const uint64_t column_id = values.at(i).column_id_;
      int64_t index = -1;
      for (int64_t j = 0; j < request_columns.count(); j++) {
        if (ai_columns.at(j)->get_column_id() == column_id) {
          index = j;
          break;
        }
      }
      if (index < 0) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      if (OB_FAIL(update_sql.append_fmt(" and `%.*s` = '%.*s'",
                                        static_cast<int>(request_columns.at(index)->get_column_name_str().length()),
                                        request_columns.at(index)->get_column_name_str().ptr(),
                                        static_cast<int>(request_id.length()),
                                        request_id.ptr()))) {
        break;
      }
    }
  }
  // the tasks of the group are removed, the request id makes sure that the
  // tasks of a newer request are not removed
  if (OB_SUCC(ret)) {
    if (OB_FAIL(delete_sql.assign_fmt("delete from %s where task_id in (", OB_AI_TASK_TABLE))) {
    } else {
      for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
        if (i > begin && OB_FAIL(delete_sql.append(", "))) {
          break;
        }
        if (OB_FAIL(delete_sql.append_fmt("'%.*s'",
                                          static_cast<int>(tasks.at(i).task_id_.length()),
                                          tasks.at(i).task_id_.ptr()))) {
          break;
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(delete_sql.append_fmt(") and request_id = '%.*s'",
                                           static_cast<int>(request_id.length()),
                                           request_id.ptr()))) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  if (OB_SUCC(ret)) {
    int64_t affected_rows = 0;
    bool in_trans = false;
    if (OB_FAIL(conn.start_transaction())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      in_trans = true;
      if (OB_FAIL(conn.execute_write(update_sql.string(), affected_rows))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(conn.execute_write(delete_sql.string(), affected_rows))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (affected_rows != end - begin) {
        // the task was replaced by a newer request, the result is dropped
        if (OB_FAIL(conn.rollback())) {
          LOG_WARN("[AI_COLUMN_WORKER] failed to roll back the transaction", K(ret));
        }
        in_trans = false;
        ret = OB_SUCCESS;
        LOG_INFO("[AI_COLUMN_WORKER] the result of the AI column is stale, it is dropped");
      } else if (OB_FAIL(conn.commit())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        in_trans = false;
      }
    }
    if (OB_FAIL(ret) && in_trans) {
      if (OB_FAIL(conn.rollback())) {
        LOG_WARN("[AI_COLUMN_WORKER] failed to roll back the transaction", K(ret));
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::handle_group_failure(sqlclient::ObISQLConnection &conn,
                                            const share::schema::ObTableSchema *table_schema,
                                            const share::schema::ObDatabaseSchema *database_schema,
                                            const ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns,
                                            const ObIArray<const share::schema::ObColumnSchemaV2 *> &state_columns,
                                            const ObIArray<const share::schema::ObColumnSchemaV2 *> &request_columns,
                                            const ObSEArray<ObAiWorkerTask, 64> &tasks,
                                            int64_t begin,
                                            int64_t end,
                                            const ObString &request_id,
                                            int64_t max_retries,
                                            bool terminal)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSqlString state_sql;
  bool has_state_sql = false;
  if (terminal) {
    // The value of the AI column can not be computed.  The state of the row is
    // set to FAILED, so that the user can see that the value will not be
    // computed any more, and the task of the row is removed.
    if (OB_NOT_NULL(table_schema) && OB_NOT_NULL(database_schema)) {
      const ObString &db_name = database_schema->get_database_name();
      const ObString &table_name = table_schema->get_table_name_str();
      if (OB_FAIL(state_sql.assign_fmt("update `%.*s`.`%.*s` set ",
                                       db_name.length(), db_name.ptr(),
                                       table_name.length(), table_name.ptr()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
          const uint64_t column_id = tasks.at(i).column_id_;
          int64_t index = -1;
          for (int64_t j = 0; j < ai_columns.count(); j++) {
            if (ai_columns.at(j)->get_column_id() == column_id) {
              index = j;
              break;
            }
          }
          if (index < 0) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          if (i > begin && OB_FAIL(state_sql.append(", "))) {
            break;
          }
          if (OB_FAIL(state_sql.append_fmt("`%.*s` = '%s'",
                                            static_cast<int>(state_columns.at(index)->get_column_name_str().length()),
                                            state_columns.at(index)->get_column_name_str().ptr(),
                                            share::ObAiColumnSpec::STATE_FAILED))) {
            break;
          }
        }
        // The request id of the row must be the one of the task, otherwise the
        // row has been invalidated by a newer request and its state must not
        // be changed.  The condition belongs to the where clause, the state
        // of the row is set by the set clause.
        if (OB_SUCC(ret)
            && OB_FAIL(state_sql.append_fmt(" where %.*s",
                                             static_cast<int>(tasks.at(begin).rowkey_predicate_.length()),
                                             tasks.at(begin).rowkey_predicate_.ptr()))) {
          ret = OB_ERR_UNEXPECTED;
        }
        for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
          const uint64_t column_id = tasks.at(i).column_id_;
          int64_t index = -1;
          for (int64_t j = 0; j < request_columns.count(); j++) {
            if (ai_columns.at(j)->get_column_id() == column_id) {
              index = j;
              break;
            }
          }
          if (index < 0) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          if (OB_FAIL(state_sql.append_fmt(" and `%.*s` = '%.*s'",
                                            static_cast<int>(request_columns.at(index)->get_column_name_str().length()),
                                            request_columns.at(index)->get_column_name_str().ptr(),
                                            static_cast<int>(request_id.length()),
                                            request_id.ptr()))) {
            break;
          }
        }
        if (OB_SUCC(ret)) {
          has_state_sql = true;
        }
      }
    }
    if (OB_SUCC(ret)
        && OB_FAIL(sql.assign_fmt("delete from %s where task_id in (", OB_AI_TASK_TABLE))) {
    } else {
      for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
        if (i > begin && OB_FAIL(sql.append(", "))) {
          break;
        }
        if (OB_FAIL(sql.append_fmt("'%.*s'",
                                    static_cast<int>(tasks.at(i).task_id_.length()),
                                    tasks.at(i).task_id_.ptr()))) {
          break;
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(sql.append_fmt(") and request_id = '%.*s'",
                                    static_cast<int>(request_id.length()),
                                    request_id.ptr()))) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  } else {
    // the task is tried again later, the state of the task is committed, so
    // that the user can see that the task is not finished yet
    if (OB_FAIL(sql.assign_fmt("update %s set state = %lld, retry_count = retry_count + 1, "
                                "eligible_ts = now(6) + interval %lld second where task_id in (",
                                OB_AI_TASK_TABLE,
                                share::ObAiColumnSpec::TASK_STATE_RETRY,
                                OB_AI_WORKER_RETRY_DELAY_SEC))) {
    } else {
      for (int64_t i = begin; OB_SUCC(ret) && i < end; i++) {
        if (i > begin && OB_FAIL(sql.append(", "))) {
          break;
        }
        if (OB_FAIL(sql.append_fmt("'%.*s'",
                                    static_cast<int>(tasks.at(i).task_id_.length()),
                                    tasks.at(i).task_id_.ptr()))) {
          break;
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(sql.append_fmt(") and request_id = '%.*s'",
                                    static_cast<int>(request_id.length()),
                                    request_id.ptr()))) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  if (OB_SUCC(ret)) {
    int64_t affected_rows = 0;
    bool in_trans = false;
    if (has_state_sql) {
      // the state of the row and the removal of the task are one change, so
      // that the row never shows a failed task that does not exist any more
      if (OB_FAIL(conn.start_transaction())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        in_trans = true;
        if (OB_FAIL(conn.execute_write(state_sql.string(), affected_rows))) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(conn.execute_write(sql.string(), affected_rows))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("[AI_COLUMN_WORKER] failed to update the state of the task", K(ret));
    }
    if (OB_SUCC(ret) && in_trans) {
      if (OB_FAIL(conn.commit())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        in_trans = false;
      }
    }
    if (OB_FAIL(ret) && in_trans) {
      if (OB_FAIL(conn.rollback())) {
        LOG_WARN("[AI_COLUMN_WORKER] failed to roll back the transaction", K(ret));
      }
    }
  }
  return ret;
}

ObAiService::ObAiService()
  : is_inited_(false),
    ai_column_worker_(NULL)
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    void *mem = ob_malloc(sizeof(ObAiColumnWorker), ObMemAttr(ObModIds::OMT));
    if (OB_ISNULL(mem)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ai_column_worker_ = new (mem) ObAiColumnWorker();
      if (OB_FAIL(ai_column_worker_->init())) {
        ob_delete(ai_column_worker_);
        ai_column_worker_ = NULL;
      } else {
        is_inited_ = true;
      }
    }
  }
  return ret;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_NOT_NULL(ai_column_worker_) && OB_FAIL(ai_column_worker_->start())) {
    LOG_WARN("[AI_SERVICE] failed to start the AI column worker", K(ret));
  }
  return ret;
}

void ObAiService::stop()
{
  if (OB_NOT_NULL(ai_column_worker_)) {
    ai_column_worker_->stop();
  }
}

void ObAiService::wait()
{
  if (OB_NOT_NULL(ai_column_worker_)) {
    ai_column_worker_->wait();
  }
}

void ObAiService::destroy()
{
  if (OB_NOT_NULL(ai_column_worker_)) {
    ai_column_worker_->destroy();
    ob_delete(ai_column_worker_);
    ai_column_worker_ = NULL;
  }
  is_inited_ = false;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
