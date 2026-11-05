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

#ifndef OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_
#define OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "common/json_type/ob_json_base.h"
#include "lib/container/ob_se_array.h"
#include "share/schema/ob_schema_struct.h"
#include "share/ai_service/ob_ai_model_info.h"

namespace oceanbase
{
namespace share
{

// Layer-neutral AI service endpoint info from user side.
class ObAiModelEndpointInfo
{
  friend class ObAiServiceProxy;
public:
  ObAiModelEndpointInfo() { reset(); }
  ~ObAiModelEndpointInfo() = default;

  void reset()
  {
    name_.reset();
    scope_ = DEFAULT_SCOPE;
    ai_model_name_.reset();
    url_.reset();
    access_key_.reset();
    provider_.reset();
    request_model_name_.reset();
    parameters_.reset();
    request_transform_fn_.reset();
    response_transform_fn_.reset();
    endpoint_id_ = OB_INVALID_ID;
  }

  int parse_from_json_base(common::ObArenaAllocator &allocator,const ObString &name, const common::ObIJsonBase &params_jbase);
  int merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_endpoint);
  int deep_copy(common::ObIAllocator &allocator, const ObAiModelEndpointInfo &other);
  int check_valid() const;

  const ObString &get_name() const { return name_; }
  const ObString &get_scope() const { return scope_; }
  const ObString &get_url() const { return url_; }
  const ObString &get_encrypted_access_key() const { return access_key_; }
  int get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const;
  const ObString &get_ai_model_name() const { return ai_model_name_; }
  const ObString &get_request_model_name() const { return request_model_name_; }
  const ObString &get_provider() const { return provider_; }
  const ObString &get_parameters() const { return parameters_; }
  const ObString &get_request_transform_fn() const { return request_transform_fn_; }
  const ObString &get_response_transform_fn() const { return response_transform_fn_; }
  uint64_t get_endpoint_id() const { return endpoint_id_; }
  void set_endpoint_id(uint64_t endpoint_id) { endpoint_id_ = endpoint_id; }

  TO_STRING_KV(K_(name),
               K_(scope),
               K_(endpoint_id),
               K_(url),
               K_(access_key),
               K_(ai_model_name),
               K_(provider),
               K_(request_model_name),
               K_(parameters),
               K_(request_transform_fn),
               K_(response_transform_fn));
private:
  static const ObString DEFAULT_SCOPE;
  static bool is_valid_provider(const ObString &provider);
  static bool is_access_key_optional(const ObString &provider);
  int encrypt_access_key_(common::ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key);
  int decrypt_access_key_(common::ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const;
private:
  uint64_t endpoint_id_;
  ObString name_;
  ObString scope_;
  ObString ai_model_name_;
  ObString url_;
  ObString access_key_;
  ObString provider_;
  ObString request_model_name_;
  ObString parameters_;
  ObString request_transform_fn_;
  ObString response_transform_fn_;
};

// The runtime metadata of one AI column.  It is parsed from the spec text
// that the DDL resolver stored in the comment of the hidden spec column
// (`__ai_p_<column>`).  The spec text is a flat `key=value` list, see
// `ObDDLResolver::gen_ai_column_spec_text`.
class ObAiColumnSpec
{
public:
  ObAiColumnSpec() { reset(); }
  ~ObAiColumnSpec() = default;

  void reset()
  {
    valid_ = false;
    func_name_.reset();
    model_name_.reset();
    prompt_expr_.reset();
    normalized_expr_.reset();
    max_retries_ = 0;
    dim_ = -1;
    deps_text_.reset();
  }

  // Parse the spec text.  The text is copied into the given allocator, so it
  // stays valid after the schema cache entry is gone.
  int parse(common::ObIAllocator &allocator, const ObString &spec_text);

  bool is_valid() const { return valid_; }
  const ObString &get_func_name() const { return func_name_; }
  const ObString &get_model_name() const { return model_name_; }
  const ObString &get_prompt_expr() const { return prompt_expr_; }
  const ObString &get_normalized_expr() const { return normalized_expr_; }
  int64_t get_max_retries() const { return max_retries_; }
  int64_t get_dim() const { return dim_; }
  // The raw `deps=` field, a comma separated list of the user columns that the
  // prompt expression refers to.
  const ObString &get_deps_text() const { return deps_text_; }
  // Whether the prompt expression refers to the given column.
  bool depends_on(const ObString &column_name) const;
  bool is_embed() const { return 0 == func_name_.case_compare("AI_EMBED"); }

  // The state of an AI column, it is stored in the hidden state column.
  static const char *const STATE_PENDING;
  static const char *const STATE_READY;
  static const char *const STATE_FAILED;
  // The state of a row whose AI column is going to be computed again.
  static const char *const STATE_RETRY;

  // The task states of `oceanbase.__all_ai_column_task`.
  static const int64_t TASK_STATE_PENDING;
  static const int64_t TASK_STATE_RUNNING;
  static const int64_t TASK_STATE_RETRY;
  static const int64_t TASK_STATE_FAILED;

private:
  bool valid_;
  ObString func_name_;
  ObString model_name_;
  ObString prompt_expr_;
  ObString normalized_expr_;
  int64_t max_retries_;
  int64_t dim_;
  ObString deps_text_;
};

// Helpers that work on the AI columns of a table.
class ObAiColumnUtil
{
public:
  // Collect the AI columns of a table and their hidden companion columns.
  // Every array is indexed the same way, so `ai_columns[i]` belongs to
  // `state_columns[i]`, `request_columns[i]` and `spec_columns[i]`.
  static int collect_ai_columns(const schema::ObTableSchema &table_schema,
                                common::ObIArray<const schema::ObColumnSchemaV2 *> &ai_columns,
                                common::ObIArray<const schema::ObColumnSchemaV2 *> &state_columns,
                                common::ObIArray<const schema::ObColumnSchemaV2 *> &request_columns,
                                common::ObIArray<const schema::ObColumnSchemaV2 *> &spec_columns);

  // Build the row key predicate of a row, e.g. "`id` = 2".  The primary key
  // columns are taken from the table schema, the values from the given
  // `column_name` / `column_value` pairs.
  static int build_rowkey_predicate(common::ObIAllocator &allocator,
                                    const schema::ObTableSchema &table_schema,
                                    const common::ObIArray<ObString> &column_names,
                                    const common::ObIArray<ObString> &column_values,
                                    ObString &predicate);

  // Escape a string so that it can be embedded in a single quoted SQL literal.
  static int escape_sql_string(common::ObIAllocator &allocator,
                                const ObString &value,
                                ObString &escaped);

  // The name of the hidden companion column of an AI column.
  static int hidden_column_name(common::ObIAllocator &allocator,
                                 const ObString &ai_column_name,
                                 const char *prefix,
                                 ObString &hidden_name);

  // The system message that tells the model the type of the value of an AI
  // column.  It is empty when the type of the column needs no hint.
  static const char *get_type_hint(const schema::ObColumnSchemaV2 &column);

  // Rewrite the pseudo column `status` of a retry predicate into the hidden
  // state column of the AI column that is retried.  The pseudo column is only
  // the status of the AI column that is retried, a quoted name is always a
  // physical column of the table.
  static int rewrite_ai_status_pseudo_column(common::ObIAllocator &allocator,
                                              const ObString &predicate,
                                              const ObString &state_column_name,
                                              ObString &rewritten);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_