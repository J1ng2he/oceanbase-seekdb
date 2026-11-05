/**
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

#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "share/ai_service/ob_ai_service_struct.h"

#include <string.h>

using namespace oceanbase::common;
using namespace oceanbase::share;

namespace oceanbase
{
namespace share
{

/**
 * The unit test of the metadata of the AI columns:
 *
 * 1. every AI column has three hidden companion columns, their names are built
 *    from the name of the AI column,
 * 2. the specification of an AI column is stored in the comment of the hidden
 *    column, it survives the round trip through the schema,
 * 3. the hidden companion columns have their own column id, the column id of
 *    the user column is not changed by them,
 * 4. the states of an AI column and the states of its task are fixed.
 */
class ObAiColumnSchemaTest : public ::testing::Test
{
public:
  ObAiColumnSchemaTest() = default;
  virtual ~ObAiColumnSchemaTest() = default;
};

TEST_F(ObAiColumnSchemaTest, the_hidden_companion_columns_of_an_ai_column)
{
  ObArenaAllocator allocator;
  ObString hidden_name;
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::hidden_column_name(allocator,
                                                           ObString("result"),
                                                           OB_AI_STATE_COLUMN_NAME_PREFIX,
                                                           hidden_name));
  ASSERT_EQ(0, hidden_name.case_compare("__ai_s_result"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::hidden_column_name(allocator,
                                                           ObString("result"),
                                                           OB_AI_REQUEST_COLUMN_NAME_PREFIX,
                                                           hidden_name));
  ASSERT_EQ(0, hidden_name.case_compare("__ai_r_result"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::hidden_column_name(allocator,
                                                           ObString("result"),
                                                           OB_AI_SPEC_COLUMN_NAME_PREFIX,
                                                           hidden_name));
  ASSERT_EQ(0, hidden_name.case_compare("__ai_p_result"));
}

TEST_F(ObAiColumnSchemaTest, the_specification_of_an_ai_column_is_stored_in_the_comment)
{
  ObArenaAllocator allocator;
  // the resolver stores the specification of the AI column in the comment of
  // the hidden column, the comment is a part of the schema
  ObColumnSchemaV2 spec_column;
  spec_column.reset();
  ASSERT_EQ(OB_SUCCESS, spec_column.set_column_name("__ai_p_result"));
  ASSERT_EQ(OB_SUCCESS, spec_column.set_comment(
      "v1;func=AI_COMPLETE;model=p0_model;max_retries=2;dim=-1;deps=content;"
      "normalized_expr=concat('Classify: ',content);prompt=concat('Classify: ',content)"));
  ASSERT_EQ(0, spec_column.get_column_name_str().case_compare("__ai_p_result"));

  // the specification is read back from the comment of the column
  ObAiColumnSpec spec;
  ASSERT_EQ(OB_SUCCESS, spec.parse(allocator, spec_column.get_comment_str()));
  ASSERT_TRUE(spec.is_valid());
  ASSERT_EQ(0, spec.get_func_name().case_compare("AI_COMPLETE"));
  ASSERT_EQ(0, spec.get_model_name().case_compare("p0_model"));
  ASSERT_EQ(2, spec.get_max_retries());
  ASSERT_TRUE(spec.depends_on(ObString("content")));
}

TEST_F(ObAiColumnSchemaTest, the_hidden_columns_have_their_own_column_id)
{
  ObArenaAllocator allocator;
  ObColumnSchemaV2 ai_column;
  ai_column.reset();
  ASSERT_EQ(OB_SUCCESS, ai_column.set_column_name("result"));
  ai_column.set_column_id(3);
  ai_column.set_is_hidden(false);

  // the three companion columns of the AI column are hidden and they have
  // their own column id
  ObColumnSchemaV2 state_column;
  state_column.reset();
  ASSERT_EQ(OB_SUCCESS, state_column.set_column_name("__ai_s_result"));
  state_column.set_column_id(4);
  state_column.set_is_hidden(true);

  ObColumnSchemaV2 request_column;
  request_column.reset();
  ASSERT_EQ(OB_SUCCESS, request_column.set_column_name("__ai_r_result"));
  request_column.set_column_id(5);
  request_column.set_is_hidden(true);

  ObColumnSchemaV2 spec_column;
  spec_column.reset();
  ASSERT_EQ(OB_SUCCESS, spec_column.set_column_name("__ai_p_result"));
  spec_column.set_column_id(6);
  spec_column.set_is_hidden(true);

  ASSERT_TRUE(ai_column.is_user_visible_column());
  ASSERT_FALSE(state_column.is_user_visible_column());
  ASSERT_FALSE(request_column.is_user_visible_column());
  ASSERT_FALSE(spec_column.is_user_visible_column());
  ASSERT_EQ(3, ai_column.get_column_id());
  ASSERT_EQ(4, state_column.get_column_id());
  ASSERT_EQ(5, request_column.get_column_id());
  ASSERT_EQ(6, spec_column.get_column_id());
}

TEST_F(ObAiColumnSchemaTest, a_string_is_escaped_for_a_sql_literal)
{
  ObArenaAllocator allocator;
  ObString escaped;
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::escape_sql_string(allocator, ObString("plain"), escaped));
  ASSERT_EQ(0, escaped.case_compare("plain"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::escape_sql_string(allocator, ObString("it's"), escaped));
  ASSERT_EQ(0, escaped.case_compare("it\\'s"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::escape_sql_string(allocator, ObString("a\\b"), escaped));
  ASSERT_EQ(0, escaped.case_compare("a\\\\b"));
}

TEST_F(ObAiColumnSchemaTest, the_states_of_an_ai_column_and_of_its_task)
{
  ASSERT_EQ(0, strcmp(ObAiColumnSpec::STATE_PENDING, "PENDING"));
  ASSERT_EQ(0, strcmp(ObAiColumnSpec::STATE_READY, "READY"));
  ASSERT_EQ(0, strcmp(ObAiColumnSpec::STATE_FAILED, "FAILED"));
  ASSERT_EQ(0, strcmp(ObAiColumnSpec::STATE_RETRY, "RETRY"));
  ASSERT_EQ(0, ObAiColumnSpec::TASK_STATE_PENDING);
  ASSERT_EQ(1, ObAiColumnSpec::TASK_STATE_RUNNING);
  ASSERT_EQ(3, ObAiColumnSpec::TASK_STATE_RETRY);
  ASSERT_EQ(4, ObAiColumnSpec::TASK_STATE_FAILED);
}

TEST_F(ObAiColumnSchemaTest, the_prompt_of_an_ai_column_holds_the_type_of_the_value)
{
  // the hint tells the model the type of the value the column holds
  ObColumnSchemaV2 column;
  column.reset();
  column.set_data_type(ObJsonType);
  ASSERT_NE(nullptr, ObAiColumnUtil::get_type_hint(column));
  column.reset();
  column.set_data_type(ObIntType);
  ASSERT_NE(nullptr, ObAiColumnUtil::get_type_hint(column));
  column.reset();
  // a text column needs no hint
  column.set_data_type(ObVarcharType);
  ASSERT_EQ(nullptr, ObAiColumnUtil::get_type_hint(column));
}

TEST_F(ObAiColumnSchemaTest, the_pseudo_column_of_a_retry_is_replaced)
{
  // the pseudo column "status" of the predicate of a retry is the state of the
  // AI column that is retried
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::rewrite_ai_status_pseudo_column(
      allocator, ObString("status = 'FAILED'"), ObString("__ai_s_result"), result));
  ASSERT_EQ(0, result.case_compare("`__ai_s_result` = 'FAILED'"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::rewrite_ai_status_pseudo_column(
      allocator, ObString("STATUS='READY' and category='news'"), ObString("__ai_s_result"), result));
  ASSERT_EQ(0, result.case_compare("`__ai_s_result`='READY' and category='news'"));
}

TEST_F(ObAiColumnSchemaTest, a_quoted_name_is_a_physical_column)
{
  ObArenaAllocator allocator;
  ObString result;
  // a quoted name is always a physical column of the table
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::rewrite_ai_status_pseudo_column(
      allocator, ObString("`status` = 'active'"), ObString("__ai_s_result"), result));
  ASSERT_EQ(0, result.case_compare("`status` = 'active'"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::rewrite_ai_status_pseudo_column(
      allocator, ObString("docs.status = 'active'"), ObString("__ai_s_result"), result));
  ASSERT_EQ(0, result.case_compare("docs.status = 'active'"));
  // the name inside a string literal is not replaced
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::rewrite_ai_status_pseudo_column(
      allocator, ObString("category = 'status'"), ObString("__ai_s_result"), result));
  ASSERT_EQ(0, result.case_compare("category = 'status'"));
  ASSERT_EQ(OB_SUCCESS, ObAiColumnUtil::rewrite_ai_status_pseudo_column(
      allocator, ObString("id in (1, 3)"), ObString("__ai_s_result"), result));
  ASSERT_EQ(0, result.case_compare("id in (1, 3)"));
}

} // namespace share
} // namespace oceanbase
