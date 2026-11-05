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

} // namespace

/**
 * The unit test of the syntax of the file table functions:
 *
 * 1. the four table functions are parsed,
 * 2. the named arguments and the alias of a table function are parsed,
 * 3. the describe of a table function is parsed,
 * 4. the copy statement is parsed,
 * 5. a second positional argument is a syntax error, the table functions only
 *    take the path of the file as a positional argument.
 */
class ObFileSqlParserTest : public ::testing::Test
{
public:
  ObFileSqlParserTest() : allocator_(ObModIds::OB_SQL_PARSER), parser_(allocator_, SMO_DEFAULT) {}
  virtual ~ObFileSqlParserTest() = default;

  // parses one statement and frees the result
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

TEST_F(ObFileSqlParserTest, parse_the_file_table_functions)
{
  const char *sql[] = {
    "select * from read_csv('/tmp/data.csv')",
    "select * from read_json('/tmp/data.jsonl')",
    "select * from read_json('/tmp/data.jsonl', format = 'newline_delimited')",
    "select * from read_parquet('/tmp/data.parquet')",
    "select * from glob('/tmp/*.csv')",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sql) / sizeof(sql[0])); ++i) {
    ParseResult parse_result;
    ASSERT_EQ(OB_SUCCESS, parse(sql[i], parse_result))
        << "the statement " << sql[i] << " cannot be parsed";
    ASSERT_TRUE(has_node(parse_result.result_tree_, T_FILE_TABLE_FUNC))
        << "the statement " << sql[i] << " holds no file table function";
    free_result(parse_result);
  }
}

TEST_F(ObFileSqlParserTest, parse_the_named_arguments_and_the_alias)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("select t.c1 from read_csv('/tmp/data.csv', header = true, "
                              "sample_size = -1) t",
                              parse_result));
  ASSERT_TRUE(has_node(parse_result.result_tree_, T_FILE_TABLE_FUNC));
  ASSERT_TRUE(has_node(parse_result.result_tree_, T_FILE_FUNC_NAMED_ARG));
  free_result(parse_result);

  ParseResult alias_result;
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/tmp/data.csv') as t", alias_result));
  ASSERT_TRUE(has_node(alias_result.result_tree_, T_FILE_TABLE_FUNC));
  free_result(alias_result);
}

TEST_F(ObFileSqlParserTest, parse_the_describe_of_a_file_table_function)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("describe select * from read_csv('/tmp/data.csv')", parse_result));
  ASSERT_TRUE(has_node(parse_result.result_tree_, T_DESCRIBE_STMT));
  ASSERT_TRUE(has_node(parse_result.result_tree_, T_FILE_TABLE_FUNC));
  free_result(parse_result);
}

TEST_F(ObFileSqlParserTest, parse_the_copy_statement)
{
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("copy (select c1 from t1) to '/tmp/data.csv' "
                              "(format csv, header, nullstr 'NULL')",
                              parse_result));
  ASSERT_TRUE(has_node(parse_result.result_tree_, T_COPY_STMT));
  ASSERT_TRUE(has_node(parse_result.result_tree_, T_COPY_OPTION_LIST));
  free_result(parse_result);
}

TEST_F(ObFileSqlParserTest, a_second_positional_argument_is_a_syntax_error)
{
  ParseResult parse_result;
  ASSERT_NE(OB_SUCCESS, parse("select * from read_csv('/tmp/data.csv', 'xml')", parse_result));
  free_result(parse_result);
}

TEST_F(ObFileSqlParserTest, the_table_functions_can_be_used_as_an_identifier)
{
  // the names of the table functions are not reserved words
  ParseResult parse_result;
  ASSERT_EQ(OB_SUCCESS, parse("select read_csv from t1", parse_result));
  free_result(parse_result);
}

} // namespace sql
} // namespace oceanbase
