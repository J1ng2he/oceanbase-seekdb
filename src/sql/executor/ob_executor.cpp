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

#define USING_LOG_PREFIX SQL_EXE

#include "ob_executor.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_smart_var.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/session/ob_sql_session_info.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

// Whether all the rows of the table have a ready value of the AI column.  The
// state of the AI column of a row is stored in its hidden state column, a row
// whose state is not READY has no value yet, or its value is not valid any
// more.  The check is made for the whole table, the rows that the statement
// reads are not the rows that are checked.
int is_ai_column_ready(ObExecContext &ctx,
                       oceanbase::share::schema::ObSchemaGetterGuard &schema_guard,
                       const ObAiColumnRef &ai_column_ref,
                       bool &is_ready)
{
  int ret = OB_SUCCESS;
  is_ready = true;
  const oceanbase::share::schema::ObTableSchema *table_schema = NULL;
  const oceanbase::share::schema::ObDatabaseSchema *database_schema = NULL;
  ObString state_column_name;
  ObSqlString sql;
  sqlclient::ObISQLConnection *conn = NULL;
  sqlclient::ObISQLConnectionGuard conn_guard;
  ObSQLSessionInfo *session = ctx.get_my_session();
  ObIAllocator &allocator = ctx.get_allocator();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard.get_table_schema(ai_column_ref.table_id_, table_schema))) {
    // the schema of the table is not available, the AI column is not checked
    SQL_EXE_LOG(WARN, "failed to get the schema of the table of an AI column", K(ret));
    ret = OB_SUCCESS;
  } else if (OB_ISNULL(table_schema)) {
    // the table is gone, the AI column does not exist any more
    ret = OB_SUCCESS;
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(),
                                                      database_schema))) {
    SQL_EXE_LOG(WARN, "failed to get the schema of the database of an AI column", K(ret));
    ret = OB_SUCCESS;
  } else if (OB_ISNULL(database_schema)) {
    // the database is gone, the AI column does not exist any more
    ret = OB_SUCCESS;
  } else {
    const oceanbase::share::schema::ObColumnSchemaV2 *column_schema = table_schema->get_column_schema(ai_column_ref.column_id_);
    if (OB_ISNULL(column_schema)) {
      // the AI column is gone, it is not checked any more
      ret = OB_SUCCESS;
    } else if (OB_FAIL(oceanbase::share::ObAiColumnUtil::hidden_column_name(allocator,
                                                                 column_schema->get_column_name_str(),
                                                                 OB_AI_STATE_COLUMN_NAME_PREFIX,
                                                                 state_column_name))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.assign_fmt("select 1 from `%.*s`.`%.*s` "
                                       "where `%.*s` is null or `%.*s` <> '%s' limit 1",
                                       static_cast<int>(database_schema->get_database_name_str().length()),
                                       database_schema->get_database_name_str().ptr(),
                                       static_cast<int>(table_schema->get_table_name_str().length()),
                                       table_schema->get_table_name_str().ptr(),
                                       static_cast<int>(state_column_name.length()),
                                       state_column_name.ptr(),
                                       static_cast<int>(state_column_name.length()),
                                       state_column_name.ptr(),
                                       oceanbase::share::ObAiColumnSpec::STATE_READY))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(oceanbase::query::ObInnerSQLConnectionAccess::create_connection_with_owned_session(
               conn_guard))
               || OB_ISNULL(conn = conn_guard.get_ptr())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // The probe runs on a session of its own, so it has a transaction of its
      // own.  The transaction of the user is not touched, the probe can not
      // break the statement of the user.  The rows that the user has changed
      // but not committed are not seen by the probe.
      bool has_unfinished_row = false;
      SMART_VAR(ObISQLClient::ReadResult, res) {
        if (OB_FAIL(ret = conn->execute_read(sql.string(), res, false /*is_user_sql*/))) {
        } else {
          sqlclient::ObMySQLResult *result = res.get_result();
          if (OB_ISNULL(result)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const int next_ret = result->next();
            if (OB_ITER_END == next_ret) {
              // every row is ready
              has_unfinished_row = false;
            } else if (OB_SUCC(next_ret)) {
              has_unfinished_row = true;
            } else {
              ret = next_ret;
            }
            // the result set of the probe must be closed, the statement of the
            // probe ends here
            const int close_ret = result->close();
            if (OB_SUCCESS != close_ret) {
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        is_ready = !has_unfinished_row;
      }
    }
  }
  return ret;
}

// The strict read of the AI columns rejects a statement that uses an AI column
// whose value is not ready yet.  Only the AI columns that the statement really
// uses are checked, a statement that does not use an AI column is never
// rejected.
int check_ai_column_strict_read(ObExecContext &ctx, ObPhysicalPlan *phy_plan)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (OB_ISNULL(session) || OB_ISNULL(phy_plan) || 0 == phy_plan->get_ai_column_ref_count()) {
    // the statement does not use an AI column
    return OB_SUCCESS;
  }
  if (session->is_inner()) {
    // The statements of the AI column itself are not checked.  The worker
    // writes the value of the AI column and the DML service invalidates it,
    // they are the statements of an inner session.
    return OB_SUCCESS;
  }
  bool strict_read = false;
  if (OB_FAIL(session->get_sys_variable(oceanbase::share::SYS_VAR_AI_COLUMN_STRICT_READ, strict_read))) {
    SQL_EXE_LOG(WARN, "failed to get the value of ai_column_strict_read", K(ret));
    ret = OB_SUCCESS;
  } else if (strict_read) {
    oceanbase::share::schema::ObSchemaGetterGuard schema_guard;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
      SQL_EXE_LOG(WARN, "failed to get the schema guard", K(ret));
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ObFixedArray<ObAiColumnRef, ObIAllocator> &ai_column_refs = phy_plan->get_ai_column_refs();
      for (int64_t i = 0; OB_SUCC(ret) && i < ai_column_refs.count(); ++i) {
        const ObAiColumnRef &ai_column_ref = ai_column_refs.at(i);
        bool is_ready = false;
        if (OB_FAIL(is_ai_column_ready(ctx, schema_guard, ai_column_ref, is_ready))) {
          SQL_EXE_LOG(WARN, "failed to check the state of an AI column", K(ret),
                      K(ai_column_ref.table_id_), K(ai_column_ref.column_id_));
        } else if (!is_ready) {
          ret = OB_AI_COLUMN_HAS_UNFINISHED_TASKS;
          LOG_USER_ERROR(OB_AI_COLUMN_HAS_UNFINISHED_TASKS,
                         static_cast<int64_t>(ai_column_ref.table_id_),
                         static_cast<uint64_t>(ai_column_ref.column_id_));
        }
      }
    }
  }
  return ret;
}
} // namespace

int ObExecutor::init(ObPhysicalPlan *plan)
{
  int ret = OB_SUCCESS;
  if (true == inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(plan)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    phy_plan_ = plan;
    inited_ = true;
  }
  return ret;
}

void ObExecutor::reset()
{
  inited_ = false;
  phy_plan_ = NULL;
  execution_id_ = OB_INVALID_ID;
}

int ObExecutor::execute_plan(ObExecContext &ctx)
{
  NG_TRACE(exec_plan_begin);
  int ret = OB_SUCCESS;
  ObSqlExecutorCtx &task_exec_ctx = ctx.get_sql_exec_ctx();
  ObExecuteResult &exec_result = task_exec_ctx.get_execute_result();
  ObSQLSessionInfo *session_info = ctx.get_my_session();
  ObPhysicalPlanCtx *plan_ctx = ctx.get_physical_plan_ctx();
  int64_t batched_stmt_cnt = ctx.get_sql_ctx()->multi_stmt_item_.get_batched_stmt_cnt();
  ctx.set_use_temp_expr_ctx_cache(true);
  // If the batch execution is rewritten by insert multi values, there is no need to repack multiple times
  if (ctx.get_sql_ctx()->is_do_insert_batch_opt()) {
    batched_stmt_cnt = 0;
  }
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(session_info)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(phy_plan_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(check_ai_column_strict_read(ctx, phy_plan_))) {
    // the statement uses an AI column whose value is not ready yet, the
    // statement is rejected before it writes or reads any row
  } else if (OB_FAIL(session_info->set_cur_phy_plan(phy_plan_))) {
  } else if (OB_FAIL(phy_plan_->get_expr_frame_info()
                                 .pre_alloc_exec_memory(ctx))) {
  } else if (batched_stmt_cnt > 0
      && OB_FAIL(plan_ctx->create_implicit_cursor_infos(batched_stmt_cnt))) {
  } else {
    ObPhyPlanType execute_type = phy_plan_->get_plan_type();
    // Some parallel plans do not require the PX scheduler.
    if (execute_type != OB_PHY_PLAN_LOCAL && phy_plan_->is_require_local_execution()) {
      execute_type = OB_PHY_PLAN_LOCAL;
      LOG_TRACE("change the plan execution type",
          "fact", execute_type, K(phy_plan_->get_plan_type()));
    }
    switch (execute_type) {
      case OB_PHY_PLAN_LOCAL: {
        ObOperator *op = NULL;
        if (OB_FAIL(phy_plan_->get_root_op_spec()->create_operator(ctx, op))) {
        } else if (OB_ISNULL(op)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          exec_result.set_static_engine_root(op);
        }
        break;
      }
      case OB_PHY_PLAN_DISTRIBUTED:
        // PX special path
        // PX mode, scheduling work is handled by the ObPxCoord operator
        ret = execute_static_cg_px_plan(ctx);
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
        break;
    }
  }
  NG_TRACE(exec_plan_end);
  return ret;
}

int ObExecutor::execute_static_cg_px_plan(ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  ObOperator *op = NULL;
  if (OB_FAIL(phy_plan_->get_root_op_spec()->create_op_input(ctx))) {
  } else if (OB_FAIL(phy_plan_->get_root_op_spec()->create_operator(ctx, op))) {
  } else if (OB_ISNULL(op)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ctx.get_sql_executor_ctx()
        ->get_execute_result()
        .set_static_engine_root(op);
  }
  return ret;
}

int ObExecutor::close(ObExecContext &ctx)
{
  // close function should be designed to be callable at any time, therefore regardless of the value of inited_
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session_info = ctx.get_my_session();
  if (OB_LIKELY(NULL != session_info)) {
    // Reset cur_phy_plan_ in session to NULL
    session_info->reset_cur_phy_plan_to_null();
  }
  return ret;
}
