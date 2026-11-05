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
#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"
#include "oblib/common/sql_mode/ob_sql_mode.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_define.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

namespace
{

// looks for a node of the given type in the parse tree
bool has_node(ParseNode *node, ObItemType type)
{
  bool found = false;
  if (nullptr == node) {
    // an empty tree holds no node
  } else if (node->type_ == type) {
    found = true;
  } else {
    for (int32_t i = 0; i < node->num_child_ && !found; ++i) {
      found = has_node(node->children_[i], type);
    }
  }
  return found;
}

// counts the nodes of the given type in the parse tree
int64_t count_node(ParseNode *node, ObItemType type)
{
  int64_t count = 0;
  if (nullptr != node) {
    if (node->type_ == type) {
      count = 1;
    }
    for (int32_t i = 0; i < node->num_child_; ++i) {
      count += count_node(node->children_[i], type);
    }
  }
  return count;
}

} // namespace

/**
 * The unit test of the syntax of the AI columns:
 *
 * 1. the create table statement declares an AI column,
 * 2. the alter table statement adds an AI column,
 * 3. the alter table statement modifies an AI column,
 * 4. the alter table statement retries an AI column, with and without a
 *    predicate,
 * 5. the metadata of the AI columns is read from the system view.
 */
class ObAiColumnParserTest : public ::testing::Test
{
public:
  ObAiColumnParserTest() : allocator_(ObModIds::OB_SQL_PARSER), parser_(allocator_, SMO_DEFAULT) {}
  virtual ~ObAiColumnParserTest() = default;

  int parse(const char *sql, ParseResult &parse_result)
  {
    return parser_.parse(ObString(sql), parse_result);
  }

  void free_result(ParseResult &parse_result)
  {
    parser_.free_result(parse_result);
  }

protected:
  ObArenaAllocator allocator_;
  ObParser parser_;
};

TEST_F(ObAiColumnParserTest, create_a_table_with_an_ai_column)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("create table docs ("
                              "id bigint primary key, "
                              "content varchar(200), "
                              "tags json AI COLUMN ("
                              "AI_COMPLETE('p0_ddl_complete', concat('Classify: ', content))"
                              ") WITH (sync_mode='async', max_retries=2)"
                              ")",
                              parse_result));
  ASSERT_EQ(1, count_node(parse_result.result_tree_, T_AI_COLUMN_DEF));
  // the options of the AI column are parsed as well
  ASSERT_EQ(2, count_node(parse_result.result_tree_, T_AI_COLUMN_OPTION));
  free_result(parse_result);
}

TEST_F(ObAiColumnParserTest, create_a_table_with_an_ai_vector_column)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("create table docs ("
                              "id bigint primary key, "
                              "content varchar(200), "
                              "emb vector(4) AI COLUMN ("
                              "AI_EMBED('p0_ddl_embed', content)"
                              ") WITH (sync_mode='async', max_retries=3)"
                              ")",
                              parse_result));
  ASSERT_EQ(1, count_node(parse_result.result_tree_, T_AI_COLUMN_DEF));
  ASSERT_EQ(2, count_node(parse_result.result_tree_, T_AI_COLUMN_OPTION));
  free_result(parse_result);
}

TEST_F(ObAiColumnParserTest, an_ai_column_without_options)
{
  // the options of an AI column are optional
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("create table docs ("
                              "id bigint primary key, "
                              "tags json AI COLUMN (AI_COMPLETE('p0_ddl_complete', content))"
                              ")",
                              parse_result));
  ASSERT_EQ(1, count_node(parse_result.result_tree_, T_AI_COLUMN_DEF));
  ASSERT_EQ(0, count_node(parse_result.result_tree_, T_AI_COLUMN_OPTION));
  free_result(parse_result);
}

TEST_F(ObAiColumnParserTest, add_an_ai_column)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("alter table docs add column tags json AI COLUMN ("
                              "AI_COMPLETE('p0_ddl_complete', content)"
                              ") WITH (sync_mode='async', max_retries=1)",
                              parse_result));
  ASSERT_EQ(1, count_node(parse_result.result_tree_, T_AI_COLUMN_DEF));
  ASSERT_EQ(2, count_node(parse_result.result_tree_, T_AI_COLUMN_OPTION));
  free_result(parse_result);
}

TEST_F(ObAiColumnParserTest, modify_an_ai_column)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("alter table docs modify column tags json AI COLUMN ("
                              "AI_COMPLETE('p0_ddl_complete', content)"
                              ") WITH (sync_mode='async')",
                              parse_result));
  ASSERT_EQ(1, count_node(parse_result.result_tree_, T_AI_COLUMN_DEF));
  free_result(parse_result);
}

TEST_F(ObAiColumnParserTest, retry_an_ai_column)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("alter table docs retry ai column tags", parse_result));
  ASSERT_EQ(1, count_node(parse_result.result_tree_, T_AI_COLUMN_RETRY));
  free_result(parse_result);

  ParseResult predicate_result;
  ASSERT_EQ(OB_SUCCESS, parse("alter table docs retry ai column tags where status = 'FAILED'",
                              predicate_result));
  ASSERT_EQ(1, count_node(predicate_result.result_tree_, T_AI_COLUMN_RETRY));
  free_result(predicate_result);
}

TEST_F(ObAiColumnParserTest, read_the_metadata_of_the_ai_columns)
{
  // the metadata of the AI columns is exposed by a system view
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("select column_name, max_retries, state_column_id "
                              "from oceanbase.__all_ai_column",
                              parse_result));
  ASSERT_TRUE(has_node(parse_result.result_tree_, T_TABLE_REFERENCE));
  free_result(parse_result);
}

TEST_F(ObAiColumnParserTest, an_ai_column_needs_a_function)
{
  // the declaration of an AI column holds an expression
  ParseResult parse_result;
  ASSERT_NE(OB_SUCCESS, parse("create table docs (id bigint primary key, "
                               "tags json AI COLUMN)",
                               parse_result));
  free_result(parse_result);
}

} // namespace sql
} // namespace oceanbase
