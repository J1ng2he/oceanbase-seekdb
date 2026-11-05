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
#include "sql/resolver/ddl/ob_ddl_resolver.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

namespace
{

// looks for the first node of the given type in the parse tree
ParseNode *find_node(ParseNode *node, ObItemType type)
{
  ParseNode *found = nullptr;
  if (nullptr == node) {
    // an empty tree holds no node
  } else if (node->type_ == type) {
    found = node;
  } else {
    for (int32_t i = 0; i < node->num_child_ && nullptr == found; ++i) {
      found = find_node(node->children_[i], type);
    }
  }
  return found;
}

// looks for the option of the given name in the parse tree, the options of an
// AI column are the nodes of the type T_AI_COLUMN_OPTION
ParseNode *find_option(ParseNode *node, const char *name)
{
  ParseNode *found = nullptr;
  if (nullptr == node) {
    // an empty tree holds no option
  } else if (node->type_ == T_AI_COLUMN_OPTION && node->num_child_ == 2) {
    ObString option_name(node->children_[0]->str_len_, node->children_[0]->str_value_);
    if (0 == option_name.case_compare(name)) {
      found = node;
    }
  } else {
    for (int32_t i = 0; i < node->num_child_ && nullptr == found; ++i) {
      found = find_option(node->children_[i], name);
    }
  }
  return found;
}

// the value of an option of an AI column
ObString option_value(ParseNode *option)
{
  return ObString(option->children_[1]->str_len_, option->children_[1]->str_value_);
}

} // namespace

/**
 * The unit test of the resolution of an AI column:
 *
 * 1. the attribute of an AI column holds the expression of the model and the
 *    options of the column,
 * 2. the options of an AI column are read from the option list of the attribute,
 * 3. the columns the expression of the AI column depends on are collected, the
 *    names of the functions and the names inside the string literals are not
 *    column references,
 * 4. the specification of an AI column has the default values.
 */
class ObAiColumnResolverTest : public ::testing::Test
{
public:
  ObAiColumnResolverTest() : allocator_(ObModIds::OB_SQL_PARSER), parser_(allocator_, SMO_DEFAULT) {}
  virtual ~ObAiColumnResolverTest() = default;

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

TEST_F(ObAiColumnResolverTest, the_expression_of_an_ai_column_is_resolved)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("create table docs ("
                              "id bigint primary key, "
                              "content varchar(200), "
                              "tags json AI COLUMN ("
                              "AI_COMPLETE('p0_model', concat('Classify: ', content))"
                              ") WITH (sync_mode='async', max_retries=2))",
                              parse_result));
  ParseNode *attr = find_node(parse_result.result_tree_, T_AI_COLUMN_DEF);
  ASSERT_NE(nullptr, attr);
  // the attribute holds the expression of the column and its options
  ASSERT_EQ(2, attr->num_child_);
  ParseNode *expr = attr->children_[0];
  ASSERT_NE(nullptr, expr);
  // the expression is the call of the model
  ObString expr_text(expr->str_len_, expr->str_value_);
  ASSERT_TRUE(0 == expr_text.case_compare("AI_COMPLETE('p0_model', concat('Classify: ', content))"));
  free_result(parse_result);
}

TEST_F(ObAiColumnResolverTest, the_options_of_an_ai_column_are_resolved)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("create table docs ("
                              "id bigint primary key, "
                              "tags json AI COLUMN (AI_COMPLETE('p0_model', content))"
                              ") WITH (sync_mode='async', max_retries=2))",
                              parse_result));
  // the options of the AI column are the nodes of the type T_AI_COLUMN_OPTION
  ParseNode *sync_mode = find_option(parse_result.result_tree_, "sync_mode");
  ASSERT_NE(nullptr, sync_mode);
  ASSERT_EQ(0, option_value(sync_mode).case_compare("async"));
  ParseNode *max_retries = find_option(parse_result.result_tree_, "max_retries");
  ASSERT_NE(nullptr, max_retries);
  ASSERT_EQ(0, option_value(max_retries).case_compare("2"));
  free_result(parse_result);
}

TEST_F(ObAiColumnResolverTest, an_ai_column_without_options)
{
  // the options of an AI column are optional
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("create table docs ("
                              "id bigint primary key, "
                              "tags json AI COLUMN (AI_COMPLETE('p0_model', content))"
                              ")",
                              parse_result));
  ParseNode *attr = find_node(parse_result.result_tree_, T_AI_COLUMN_DEF);
  ASSERT_NE(nullptr, attr);
  ASSERT_EQ(1, attr->num_child_);
  ASSERT_EQ(nullptr, find_option(parse_result.result_tree_, "sync_mode"));
  free_result(parse_result);
}

TEST_F(ObAiColumnResolverTest, the_columns_the_expression_depends_on_are_collected)
{
  ObSEArray<ObString, 8> deps;
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::collect_ai_column_deps(
      ObString("concat('Classify: ', content)"), deps));
  ASSERT_EQ(1, deps.count());
  ASSERT_EQ(0, deps.at(0).case_compare("content"));

  // the name of a function is not a column reference
  ObSEArray<ObString, 8> function_deps;
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::collect_ai_column_deps(
      ObString("upper(title)"), function_deps));
  ASSERT_EQ(1, function_deps.count());
  ASSERT_EQ(0, function_deps.at(0).case_compare("title"));

  // a name inside a string literal is not a column reference
  ObSEArray<ObString, 8> literal_deps;
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::collect_ai_column_deps(
      ObString("'a constant'"), literal_deps));
  ASSERT_EQ(0, literal_deps.count());

  // only the last part of a qualified name is a column reference
  ObSEArray<ObString, 8> qualified_deps;
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::collect_ai_column_deps(
      ObString("docs.category"), qualified_deps));
  ASSERT_EQ(1, qualified_deps.count());
  ASSERT_EQ(0, qualified_deps.at(0).case_compare("category"));
}

TEST_F(ObAiColumnResolverTest, the_specification_of_an_ai_column_has_the_default_values)
{
  // the specification is built by the resolver, it holds no value before the
  // attribute of the column is resolved
  ObAiColumnSpec spec;
  ASSERT_EQ(ObAiColumnSpec::AI_FUNC_UNKNOWN, spec.func_type_);
  ASSERT_EQ(0, spec.max_retries_);
  ASSERT_EQ(-1, spec.dim_);
  // the function of an AI column is a completion or an embedding
  ASSERT_EQ(1, static_cast<int64_t>(ObAiColumnSpec::AI_FUNC_COMPLETE));
  ASSERT_EQ(2, static_cast<int64_t>(ObAiColumnSpec::AI_FUNC_EMBED));
  // every AI column has three hidden companion columns
  ASSERT_EQ(0, static_cast<int64_t>(ObAiColumnSpec::AI_HIDDEN_KIND_STATE));
  ASSERT_EQ(1, static_cast<int64_t>(ObAiColumnSpec::AI_HIDDEN_KIND_REQUEST));
  ASSERT_EQ(2, static_cast<int64_t>(ObAiColumnSpec::AI_HIDDEN_KIND_SPEC));
}

} // namespace sql
} // namespace oceanbase
