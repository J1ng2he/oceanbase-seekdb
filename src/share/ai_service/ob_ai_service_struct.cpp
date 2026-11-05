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

#include "share/ai_service/ob_ai_service_struct.h"
#include "lib/json/ob_json.h"
#include "lib/string/ob_sql_string.h"


#define USING_LOG_PREFIX SHARE

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::json;

namespace oceanbase
{
namespace share
{
const ObString ObAiModelEndpointInfo::DEFAULT_SCOPE = "ALL";

const char *VALID_PROVIDERS[] = {
  "ALIYUN-OPENAI",
  "ALIYUN-DASHSCOPE",
  "DEEPSEEK",
  "SILICONFLOW",
  "COHERE",
  "HUNYUAN-OPENAI",
  "OPENAI",
  "OLLAMA"
};

#define EXTRACT_JSON_ELEM_STR(json_key, member) \
  EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, "void")

#define EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, post_process) \
      if (elem.first.case_compare(json_key) == 0) { \
        if (elem.second->json_type() != ObJsonNodeType::J_STRING) { \
          ret = OB_AI_FUNC_PARAM_VALUE_INVALID; \
          LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, elem.first.length(), elem.first.ptr()); \
          LOG_WARN("invalid json type", K(ret), K(elem.first), K(elem.second->json_type())); \
        } else { \
          member = ObString(elem.second->get_data_length(), elem.second->get_data()); \
          post_process; \
        } \
      } else

#define EXTRACT_JSON_ELEM_END() \
  { \
    ret = OB_AI_FUNC_PARAM_INVALID; \
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, elem.first.length(), elem.first.ptr()); \
    LOG_WARN("unknown json key param", K(ret), K(elem.first)); \
  }

int ObAiModelEndpointInfo::parse_from_json_base(common::ObArenaAllocator &allocator, const ObString &name, const ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  if (OB_FAIL(merge_delta_endpoint(allocator, params_jbase))) {
  }
  LOG_INFO("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::deep_copy(
    common::ObIAllocator &allocator,
    const ObAiModelEndpointInfo &other)
{
  int ret = OB_SUCCESS;
  reset();
  endpoint_id_ = other.endpoint_id_;
  if (OB_FAIL(ob_write_string(allocator, other.name_, name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.scope_, scope_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.ai_model_name_, ai_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.url_, url_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.access_key_, access_key_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.provider_, provider_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_model_name_, request_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.parameters_, parameters_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_transform_fn_, request_transform_fn_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.response_transform_fn_, response_transform_fn_))) {
  }
  return ret;
}

int ObAiModelEndpointInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (scope_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("scope"), "scope");
  } else if (scope_.case_compare(DEFAULT_SCOPE) != 0) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("scope"), "scope");
  } else if (ai_model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("ai_model_name"), "ai_model_name");
  } else if (url_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("url"), "url");
  } else if (access_key_.empty() && !is_access_key_optional(provider_)) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("access_key"), "access_key");
    LOG_WARN("access_key is empty", K(ret), K(*this));
  } else if (provider_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("provider"), "provider");
  } else if (!is_valid_provider(provider_)) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("provider"), "provider");
  } else if (!parameters_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("parameters"), "parameters");
  } else if (!request_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("request_transform_fn"), "request_transform_fn");
  } else if (!response_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("response_transform_fn"), "response_transform_fn");
  }
  return ret;
}

bool ObAiModelEndpointInfo::is_valid_provider(const ObString &provider)
{
  bool is_valid = false;
  for (int i = 0; i < ARRAYSIZEOF(VALID_PROVIDERS); i++) {
    if (provider.case_compare(VALID_PROVIDERS[i]) == 0) {
      is_valid = true;
      break;
    }
  }
  return is_valid;
}

bool ObAiModelEndpointInfo::is_access_key_optional(const ObString &provider)
{
  // Ollama 是本地部署的推理服务，不需要 API Key，允许 access_key 为空
  return provider.case_compare("OLLAMA") == 0;
}

int ObAiModelEndpointInfo::merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_jbase)
{
  int ret = OB_SUCCESS;
  JsonObjectIterator iter = delta_jbase.object_iterator();
  bool has_api_key = false;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("scope", scope_)
      EXTRACT_JSON_ELEM_STR("ai_model_name", ai_model_name_)
      EXTRACT_JSON_ELEM_STR("url", url_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("access_key", access_key_, has_api_key = true)
      EXTRACT_JSON_ELEM_STR("provider", provider_)
      EXTRACT_JSON_ELEM_STR("request_model_name", request_model_name_)
      EXTRACT_JSON_ELEM_STR("parameters", parameters_)
      EXTRACT_JSON_ELEM_STR("request_transform_fn", request_transform_fn_)
      EXTRACT_JSON_ELEM_STR("response_transform_fn", response_transform_fn_)
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    if (has_api_key && !access_key_.empty() && OB_FAIL(encrypt_access_key_(allocator, access_key_, access_key_))) {
      LOG_WARN("failed to encrypt access key", K(ret));
    } else if (OB_FAIL(check_valid())) {
    }
  }

  LOG_INFO("merge delta endpoint", K(ret), K(delta_jbase), K(delta_jbase.json_type()), K(delta_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::encrypt_access_key_(ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, access_key, encrypted_access_key))) {
  }
  return ret;
}

int ObAiModelEndpointInfo::decrypt_access_key_(ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, encrypted_access_key, unencrypted_access_key))) {
  }
  return ret;
}

const char *EndpointType::ENDPOINT_TYPE_STR[] = {
  "DENSE_EMBEDDING",
  "SPARSE_EMBEDDING",
  "COMPLETION",
  "RERANK",
};

EndpointType::TYPE EndpointType::str_to_endpoint_type(const ObString &type_str)
{
  STATIC_ASSERT(static_cast<int64_t>(EndpointType::MAX_TYPE) == ARRAYSIZEOF(ENDPOINT_TYPE_STR) + 1, "endpoint type str len is mismatch");
  EndpointType::TYPE endpoint_type = EndpointType::INVALID_TYPE;
  bool is_found = false;
  for (int i = 1; i < EndpointType::MAX_TYPE && !is_found; i++) {
    if (type_str.case_compare(ENDPOINT_TYPE_STR[i-1]) == 0) {
      endpoint_type = static_cast<EndpointType::TYPE>(i);
      is_found = true;
    }
  }
  return endpoint_type;
}

int ObAiModelEndpointInfo::get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(decrypt_access_key_(allocator, access_key_, unencrypted_access_key))) {
  }
  return ret;
}

int ObAiServiceModelInfo::parse_from_json_base(const ObString &name, const common::ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  JsonObjectIterator iter = params_jbase.object_iterator();
  ObString type_str;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("model_name", model_name_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("type", type_str, type_ = EndpointType::str_to_endpoint_type(type_str))
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(check_valid())) {
    }
  }

  LOG_TRACE("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiServiceModelInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("model_name"), "model_name");
  } else if (type_ == EndpointType::MAX_TYPE) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("type"), "type");
  } else if (type_ == EndpointType::INVALID_TYPE) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("type"), "type");
  }
  return ret;
}

OB_SERIALIZE_MEMBER(ObAiServiceModelInfo, name_, type_, model_name_);

const char *const ObAiColumnSpec::STATE_PENDING = "PENDING";
const char *const ObAiColumnSpec::STATE_READY = "READY";
const char *const ObAiColumnSpec::STATE_FAILED = "FAILED";
// The state of a row whose AI column is going to be computed again.  The
// marker is written by `ALTER TABLE ... RETRY AI COLUMN`, the DML service
// reads it and invalidates the AI column of the row.
const char *const ObAiColumnSpec::STATE_RETRY = "RETRY";

const int64_t ObAiColumnSpec::TASK_STATE_PENDING = 0;
const int64_t ObAiColumnSpec::TASK_STATE_RUNNING = 1;
const int64_t ObAiColumnSpec::TASK_STATE_RETRY = 3;
const int64_t ObAiColumnSpec::TASK_STATE_FAILED = 4;

namespace
{
// The spec text is a flat list of `key=value` fields separated by `;`, the
// `prompt` field is the last one and may contain any character.
int find_field(const ObString &spec, const char *key, int64_t &value_begin, int64_t &value_end)
{
  int ret = OB_SUCCESS;
  const int64_t key_len = strlen(key);
  int64_t pos = 0;
  value_begin = -1;
  value_end = -1;
  while (pos < spec.length()) {
    int64_t field_begin = pos;
    int64_t field_end = spec.length();
    for (int64_t i = pos; i < spec.length(); i++) {
      if (';' == spec.ptr()[i]) {
        field_end = i;
        break;
      }
    }
    if (field_end - field_begin > key_len
        && 0 == strncmp(spec.ptr() + field_begin, key, key_len)
        && '=' == spec.ptr()[field_begin + key_len]) {
      value_begin = field_begin + key_len + 1;
      value_end = field_end;
      break;
    }
    pos = field_end + 1;
  }
  return ret;
}

int dup_field(common::ObIAllocator &allocator,
              const ObString &spec,
              const char *key,
              ObString &value)
{
  int ret = OB_SUCCESS;
  int64_t begin = -1;
  int64_t end = -1;
  if (OB_FAIL(find_field(spec, key, begin, end))) {
  } else if (begin < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the AI column spec has no field", K(key), K(spec));
  } else {
    char *buf = static_cast<char *>(allocator.alloc(end - begin + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      memcpy(buf, spec.ptr() + begin, end - begin);
      buf[end - begin] = '\0';
      value.assign_ptr(buf, static_cast<int32_t>(end - begin));
    }
  }
  return ret;
}
} // namespace

int ObAiColumnSpec::parse(common::ObIAllocator &allocator, const ObString &spec_text)
{
  int ret = OB_SUCCESS;
  reset();
  ObString max_retries_text;
  ObString dim_text;
  if (OB_FAIL(dup_field(allocator, spec_text, "func", func_name_))
      || OB_FAIL(dup_field(allocator, spec_text, "model", model_name_))
      || OB_FAIL(dup_field(allocator, spec_text, "max_retries", max_retries_text))
      || OB_FAIL(dup_field(allocator, spec_text, "dim", dim_text))
      || OB_FAIL(dup_field(allocator, spec_text, "deps", deps_text_))
      || OB_FAIL(dup_field(allocator, spec_text, "normalized_expr", normalized_expr_))
      || OB_FAIL(dup_field(allocator, spec_text, "prompt", prompt_expr_))) {
    LOG_WARN("failed to parse the AI column spec", K(ret), K(spec_text));
  } else {
    max_retries_ = strtoll(max_retries_text.ptr(), NULL, 10);
    dim_ = strtoll(dim_text.ptr(), NULL, 10);
    valid_ = func_name_.length() > 0 && model_name_.length() > 0;
    if (!valid_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the AI column spec is incomplete", K(spec_text));
    }
  }
  return ret;
}

bool ObAiColumnSpec::depends_on(const ObString &column_name) const
{
  bool found = false;
  int64_t pos = 0;
  while (!found && pos < deps_text_.length()) {
    int64_t item_begin = pos;
    int64_t item_end = deps_text_.length();
    for (int64_t i = pos; i < deps_text_.length(); i++) {
      if (',' == deps_text_.ptr()[i]) {
        item_end = i;
        break;
      }
    }
    if (item_end - item_begin == column_name.length()
        && 0 == strncasecmp(deps_text_.ptr() + item_begin, column_name.ptr(), column_name.length())) {
      found = true;
    }
    pos = item_end + 1;
  }
  return found;
}

namespace
{
// The hints that tell the model the type of the value of an AI column, they
// are sent to the model as the system message of the request.
const char *const OB_AI_JSON_TYPE_HINT = "Return exactly one valid JSON value.";
const char *const OB_AI_INT_TYPE_HINT = "Answer with a single base-10 integer.";

// All the integer types are rendered as a base-10 integer by the model.
bool is_ai_integer_type(ObObjType data_type)
{
  return (data_type >= ObTinyIntType && data_type <= ObIntType)
         || (data_type >= ObUTinyIntType && data_type <= ObUInt64Type);
}

// Whether the hidden column is the companion column of the given AI column.
// The name of a companion column is the prefix followed by the name of the AI
// column, the name is truncated when it is too long.
bool is_ai_companion_column(const ObString &hidden_name,
                            const char *prefix,
                            const ObString &ai_column_name)
{
  bool matched = false;
  const int64_t prefix_len = strlen(prefix);
  if (hidden_name.length() > prefix_len && 0 == strncmp(hidden_name.ptr(), prefix, prefix_len)) {
    int64_t expect_len = ai_column_name.length();
    if (prefix_len + expect_len > OB_AI_MAX_COLUMN_NAME_LENGTH) {
      expect_len = OB_AI_MAX_COLUMN_NAME_LENGTH - prefix_len;
    }
    if (hidden_name.length() == prefix_len + expect_len) {
      matched = 0 == strncmp(hidden_name.ptr() + prefix_len, ai_column_name.ptr(), expect_len);
    }
  }
  return matched;
}
} // namespace

int ObAiColumnUtil::collect_ai_columns(const schema::ObTableSchema &table_schema,
                                       common::ObIArray<const schema::ObColumnSchemaV2 *> &ai_columns,
                                       common::ObIArray<const schema::ObColumnSchemaV2 *> &state_columns,
                                       common::ObIArray<const schema::ObColumnSchemaV2 *> &request_columns,
                                       common::ObIArray<const schema::ObColumnSchemaV2 *> &spec_columns)
{
  int ret = OB_SUCCESS;
  ai_columns.reset();
  state_columns.reset();
  request_columns.reset();
  spec_columns.reset();
  ObTableSchema::const_column_iterator iter = table_schema.column_begin();
  for (; OB_SUCC(ret) && iter != table_schema.column_end(); ++iter) {
    const schema::ObColumnSchemaV2 *column = *iter;
    if (!column->is_ai_column()) {
      continue;
    }
    const ObString &column_name = column->get_column_name_str();
    const schema::ObColumnSchemaV2 *state_col = NULL;
    const schema::ObColumnSchemaV2 *request_col = NULL;
    const schema::ObColumnSchemaV2 *spec_col = NULL;
    for (ObTableSchema::const_column_iterator it = table_schema.column_begin();
         OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
      const schema::ObColumnSchemaV2 *other = *it;
      const ObString &other_name = other->get_column_name_str();
      if (is_ai_companion_column(other_name, OB_AI_STATE_COLUMN_NAME_PREFIX, column_name)) {
        state_col = other;
      } else if (is_ai_companion_column(other_name, OB_AI_REQUEST_COLUMN_NAME_PREFIX, column_name)) {
        request_col = other;
      } else if (is_ai_companion_column(other_name, OB_AI_SPEC_COLUMN_NAME_PREFIX, column_name)) {
        spec_col = other;
      }
    }
    if (OB_ISNULL(state_col) || OB_ISNULL(request_col) || OB_ISNULL(spec_col)) {
      LOG_WARN("the AI column has no hidden companion columns", K(column_name));
      continue;
    }
    if (OB_FAIL(ai_columns.push_back(column))
        || OB_FAIL(state_columns.push_back(state_col))
        || OB_FAIL(request_columns.push_back(request_col))
        || OB_FAIL(spec_columns.push_back(spec_col))) {
      break;
    }
  }
  return ret;
}

int ObAiColumnUtil::escape_sql_string(common::ObIAllocator &allocator,
                                       const ObString &value,
                                       ObString &escaped)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(value.length() * 2 + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t j = 0;
    for (int64_t i = 0; i < value.length(); i++) {
      const char c = value.ptr()[i];
      if ('\'' == c || '\\' == c) {
        buf[j++] = '\\';
      }
      buf[j++] = c;
    }
    buf[j] = '\0';
    escaped.assign_ptr(buf, static_cast<int32_t>(j));
  }
  return ret;
}

int ObAiColumnUtil::build_rowkey_predicate(common::ObIAllocator &allocator,
                                            const schema::ObTableSchema &table_schema,
                                            const common::ObIArray<ObString> &column_names,
                                            const common::ObIArray<ObString> &column_values,
                                            ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_str;
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  const int64_t rowkey_cnt = rowkey_info.get_size();
  if (0 == rowkey_cnt) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the table has no primary key", K(table_schema.get_table_id()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; i++) {
      const ObRowkeyColumn *rowkey_column = rowkey_info.get_column(i);
      const schema::ObColumnSchemaV2 *column = table_schema.get_column_schema(rowkey_column->column_id_);
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      int64_t index = -1;
      for (int64_t j = 0; index < 0 && j < column_names.count(); j++) {
        if (column_names.at(j).case_compare(column->get_column_name_str()) == 0) {
          index = j;
        }
      }
      if (index < 0 || index >= column_values.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the primary key column is not in the row", K(column->get_column_name()));
        break;
      }
      ObString escaped_name;
      ObString escaped_value;
      if (OB_FAIL(escape_sql_string(allocator, column->get_column_name_str(), escaped_name))
          || OB_FAIL(escape_sql_string(allocator, column_values.at(index), escaped_value))) {
        break;
      }
      if (i > 0 && OB_FAIL(sql_str.append(" AND "))) {
        break;
      }
      if (OB_FAIL(sql_str.append_fmt("`%.*s` = '%.*s'",
                                     static_cast<int32_t>(escaped_name.length()), escaped_name.ptr(),
                                     static_cast<int32_t>(escaped_value.length()), escaped_value.ptr()))) {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      char *buf = static_cast<char *>(allocator.alloc(sql_str.length() + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        memcpy(buf, sql_str.ptr(), sql_str.length());
        buf[sql_str.length()] = '\0';
        predicate.assign_ptr(buf, static_cast<int32_t>(sql_str.length()));
      }
    }
  }
  return ret;
}

int ObAiColumnUtil::hidden_column_name(common::ObIAllocator &allocator,
                                        const ObString &ai_column_name,
                                        const char *prefix,
                                        ObString &hidden_name)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = strlen(prefix);
  const int64_t name_len = ai_column_name.length();
  char *buf = static_cast<char *>(allocator.alloc(prefix_len + name_len + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    memcpy(buf, prefix, prefix_len);
    memcpy(buf + prefix_len, ai_column_name.ptr(), name_len);
    buf[prefix_len + name_len] = '\0';
    hidden_name.assign_ptr(buf, static_cast<int32_t>(prefix_len + name_len));
  }
  return ret;
}

const char *ObAiColumnUtil::get_type_hint(const schema::ObColumnSchemaV2 &column)
{
  const char *hint = NULL;
  if (column.is_json()) {
    hint = OB_AI_JSON_TYPE_HINT;
  } else if (is_ai_integer_type(column.get_data_type())) {
    hint = OB_AI_INT_TYPE_HINT;
  }
  return hint;
}

namespace
{
// Whether the character can be a part of an unquoted identifier.
bool ai_is_identifier_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
         || '_' == c || '$' == c;
}

// Compare two strings, the case of the letters is ignored.
bool ai_equal_ignore_case(const char *left, int64_t left_len, const char *right, int64_t right_len)
{
  if (left_len != right_len) {
    return false;
  }
  for (int64_t i = 0; i < left_len; i++) {
    char c1 = left[i];
    char c2 = right[i];
    if (c1 >= 'A' && c1 <= 'Z') { c1 = c1 - 'A' + 'a'; }
    if (c2 >= 'A' && c2 <= 'Z') { c2 = c2 - 'A' + 'a'; }
    if (c1 != c2) {
      return false;
    }
  }
  return true;
}
} // namespace

int ObAiColumnUtil::rewrite_ai_status_pseudo_column(common::ObIAllocator &allocator,
                                                    const ObString &predicate,
                                                    const ObString &state_column_name,
                                                    ObString &rewritten)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_str;
  const char *src = predicate.ptr();
  const int64_t len = predicate.length();
  int64_t i = 0;
  if (OB_ISNULL(src) || len <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    while (i < len) {
      const char c = src[i];
      if ('\'' == c || '"' == c) {
        // a string literal, it is copied as it is
        const char quote = c;
        if (OB_FAIL(sql_str.append(src + i, 1))) {
          break;
        }
        i++;
        while (i < len) {
          if ('\\' == src[i] && i + 1 < len) {
            if (OB_FAIL(sql_str.append(src + i, 2))) {
              break;
            }
            i += 2;
            continue;
          }
          if (OB_FAIL(sql_str.append(src + i, 1))) {
            break;
          }
          if (quote == src[i]) {
            i++;
            break;
          }
          i++;
        }
      } else if ('`' == c) {
        // a quoted identifier is always a physical column of the table
        if (OB_FAIL(sql_str.append(src + i, 1))) {
          break;
        }
        i++;
        while (i < len) {
          if (OB_FAIL(sql_str.append(src + i, 1))) {
            break;
          }
          if ('`' == src[i]) {
            i++;
            if (i < len && '`' == src[i]) {
              i++;
              continue;
            }
            break;
          }
          i++;
        }
      } else if ('-' == c && i + 1 < len && '-' == src[i + 1]) {
        // a comment, it is copied as it is
        while (i < len && '\n' != src[i]) {
          if (OB_FAIL(sql_str.append(src + i, 1))) {
            break;
          }
          i++;
        }
      } else if ('/' == c && i + 1 < len && '*' == src[i + 1]) {
        if (OB_FAIL(sql_str.append(src + i, 2))) {
          break;
        }
        i += 2;
        while (i + 1 < len && !('*' == src[i] && '/' == src[i + 1])) {
          if (OB_FAIL(sql_str.append(src + i, 1))) {
            break;
          }
          i++;
        }
        if (i + 1 < len) {
          if (OB_FAIL(sql_str.append(src + i, 2))) {
            break;
          }
          i += 2;
        }
      } else if (ai_is_identifier_char(c)) {
        const int64_t begin = i;
        while (i < len && ai_is_identifier_char(src[i])) {
          i++;
        }
        const int64_t id_len = i - begin;
        // a qualified name refers to a physical column of the table
        const bool is_qualified = (i < len && '.' == src[i]) || (begin > 0 && '.' == src[begin - 1]);
        if (!is_qualified && ai_equal_ignore_case(src + begin, id_len, "status", 6)) {
          if (OB_FAIL(sql_str.append_fmt("`%.*s`",
                                        static_cast<int>(state_column_name.length()),
                                        state_column_name.ptr()))) {
            break;
          }
        } else if (OB_FAIL(sql_str.append(src + begin, id_len))) {
          break;
        }
      } else {
        if (OB_FAIL(sql_str.append(src + i, 1))) {
          break;
        }
        i++;
      }
    }
  }
  if (OB_SUCC(ret)) {
    char *buf = static_cast<char *>(allocator.alloc(sql_str.length() + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (sql_str.length() > 0) {
        memcpy(buf, sql_str.ptr(), sql_str.length());
      }
      buf[sql_str.length()] = '\0';
      rewritten.assign_ptr(buf, static_cast<int32_t>(sql_str.length()));
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
