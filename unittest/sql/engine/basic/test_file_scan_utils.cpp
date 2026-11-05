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
#include "lib/encode/ob_base64_encode.h"
#include "lib/string/ob_string.h"
#include "sql/engine/basic/ob_file_table_source.h"
#include "sql/engine/basic/ob_parquet_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

namespace
{

const char *const CSV_FILE_NAME = "test_file_scan_utils.csv";
const char *const JSON_FILE_NAME = "test_file_scan_utils.jsonl";
const char *const PARQUET_FILE_NAME = "test_file_scan_utils.parquet";

// the file has a column of every type of the inference, the score column
// holds an integer and a double, the day and the created column hold a null
const char *const CSV_CONTENT =
    "id,name,score,active,day,created\n"
    "1,alpha,1.5,true,2026-08-03,2026-08-03T12:13:14\n"
    "2,beta,,false,,\n"
    "3,gamma,3,true,2026-08-05,2026-08-05T01:02:03\n";

// the keys of the rows are different, the schema of the file is the union of
// the keys and the fields that a row does not have are null
const char *const JSON_CONTENT =
    "{\"a\":1,\"b\":\"x\"}\n"
    "{\"b\":\"y\",\"c\":true}\n"
    "{\"a\":2}\n";

// the parquet file of the baseline of the topic, the file holds one INT32
// column with seven rows and the values of the column use the snappy
// compression
const char *const PARQUET_BASE64 =
    "UEFSMRUGFTwVQFwVDhUAFQ4VABUEFQARHDYAKAQGAAAAGAQAAAAAAAAAA38cbAAAAAABAAAAAgAA"
    "AAMAAAAEAAAABQAAAAYAAAAmlAEcFQIZJQYAGRgCYzEVAhYOFogBFowBJgg8NgAoBAYAAAAYBAAA"
    "AAAAAAAVBBksSARyb290FQIAFQIlAhgCYzEAFg4ZHBkcJpQBHBUCGSUGABkYAmMxFQIWDhaIARaM"
    "ASYIPDYAKAQGAAAAGAQAAAAAAAAAFowBFg4AGRwYDEFSUk9XOnNjaGVtYRi8AS8vLy8vNFFBQUFB"
    "UUFBQUFBQUFLQUE0QURBQUxBQVFBQ2dBQUFCUUFBQUFBQUFBQkJBQUtBQXdBQUFBSUFBUUFDZ0FB"
    "QUFnQUFBQUlBQUFBQUFBQUFBRUFBQUFVQUFBQUVBQVVBQkFBRGdBUEFBUUFBQUFJQUJBQUFBQVlB"
    "QUFBSUFBQUFBQUFBUUljQUFBQUNBQU1BQVFBQ3dBSUFBQUFJQUFBQUFBQUFBRUFBQUFBQWdBQUFH"
    "TXhBQUE9ABgsQXJyb3cyIC0gTmF0aXZlIFJ1c3QgaW1wbGVtZW50YXRpb24gb2YgQXJyb3cATgEA"
    "AFBBUjE=";

int write_file(const char *name, const char *content, int64_t length)
{
  int ret = OB_SUCCESS;
  FILE *fp = fopen(name, "wb");
  if (NULL == fp) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    if (length > 0 && 1 != fwrite(content, 1, length, fp)) {
      ret = OB_ERR_UNEXPECTED;
    }
    fclose(fp);
  }
  return ret;
}

// writes a csv file that is larger than the read buffer of the reader, the
// scan of such a file needs more than one read
int write_large_csv_file(int64_t row_count)
{
  int ret = OB_SUCCESS;
  FILE *fp = fopen(CSV_FILE_NAME, "wb");
  if (NULL == fp) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = OB_SUCCESS;
    if (0 != fputs("id,value\n", fp)) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < row_count; ++i) {
      char line[64];
      const int written = snprintf(line, sizeof(line), "%ld,%ld\n",
                                   static_cast<long>(i), static_cast<long>(i * 2));
      if (written <= 0 || written >= static_cast<int>(sizeof(line))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (1 != fwrite(line, 1, written, fp)) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    fclose(fp);
  }
  return ret;
}

int write_parquet_file()
{
  int ret = OB_SUCCESS;
  const int64_t length = strlen(PARQUET_BASE64);
  ObArenaAllocator allocator;
  uint8_t *buffer = static_cast<uint8_t *>(allocator.alloc(length));
  if (OB_ISNULL(buffer)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = 0;
    if (OB_FAIL(ret = ObBase64Encoder::decode(PARQUET_BASE64, length, buffer, length, pos))) {
      // the baseline file is broken
    } else {
      ret = write_file(PARQUET_FILE_NAME, reinterpret_cast<const char *>(buffer), pos);
    }
  }
  return ret;
}

void remove_files()
{
  remove(CSV_FILE_NAME);
  remove(JSON_FILE_NAME);
  remove(PARQUET_FILE_NAME);
}

} // namespace

/**
 * The unit test of the file scan of the "file as database" feature:
 *
 * 1. the type of every column of a csv file is inferred,
 * 2. the options of the table functions are honoured,
 * 3. the schema of a jsonl file is the union of the keys of all its rows,
 * 4. a file that changes while it is read fails the scan,
 * 5. the parquet file of the baseline is read, the file uses snappy.
 */
class ObFileScanUtilsTest : public ::testing::Test
{
public:
  ObFileScanUtilsTest() = default;
  virtual ~ObFileScanUtilsTest() = default;

  virtual void SetUp()
  {
    remove_files();
  }

  virtual void TearDown()
  {
    remove_files();
  }
};

TEST_F(ObFileScanUtilsTest, infer_the_types_of_a_csv_file)
{
  ASSERT_EQ(OB_SUCCESS, write_file(CSV_FILE_NAME, CSV_CONTENT, strlen(CSV_CONTENT)));
  ObArenaAllocator allocator;
  ObFileTableOptions options;
  options.func_type_ = OB_FILE_FUNC_READ_CSV;
  options.header_ = true;
  options.sample_size_ = -1;
  ObSEArray<ObFileTableColumn, 16> columns;
  ASSERT_EQ(OB_SUCCESS, ObFileTableSource::infer_csv(allocator, ObString(CSV_FILE_NAME),
                                                     options, columns));
  ASSERT_EQ(6, columns.count());
  ASSERT_EQ(0, columns.at(0).name_.case_compare("id"));
  ASSERT_EQ(OB_FILE_COL_BIGINT, columns.at(0).type_);
  ASSERT_EQ(0, columns.at(1).name_.case_compare("name"));
  ASSERT_EQ(OB_FILE_COL_VARCHAR, columns.at(1).type_);
  // an integer and a double in the same column widen the column to a double
  ASSERT_EQ(0, columns.at(2).name_.case_compare("score"));
  ASSERT_EQ(OB_FILE_COL_DOUBLE, columns.at(2).type_);
  ASSERT_EQ(0, columns.at(3).name_.case_compare("active"));
  ASSERT_EQ(OB_FILE_COL_BOOLEAN, columns.at(3).type_);
  ASSERT_EQ(0, columns.at(4).name_.case_compare("day"));
  ASSERT_EQ(OB_FILE_COL_DATE, columns.at(4).type_);
  ASSERT_TRUE(columns.at(4).nullable_);
  ASSERT_EQ(0, columns.at(5).name_.case_compare("created"));
  ASSERT_EQ(OB_FILE_COL_DATETIME, columns.at(5).type_);
  ASSERT_TRUE(columns.at(5).nullable_);
  ASSERT_STREQ("BIGINT", ObFileTableSource::col_type_name(columns.at(0).type_));
  ASSERT_STREQ("DOUBLE", ObFileTableSource::col_type_name(columns.at(2).type_));
  ASSERT_STREQ("BOOLEAN", ObFileTableSource::col_type_name(columns.at(3).type_));
}

TEST_F(ObFileScanUtilsTest, read_the_rows_of_a_csv_file)
{
  ASSERT_EQ(OB_SUCCESS, write_file(CSV_FILE_NAME, CSV_CONTENT, strlen(CSV_CONTENT)));
  ObArenaAllocator allocator;
  ObFileTableOptions options;
  options.func_type_ = OB_FILE_FUNC_READ_CSV;
  options.header_ = true;
  options.sample_size_ = -1;
  ObFileTableReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(allocator, ObString(CSV_FILE_NAME), options, NULL));
  ASSERT_EQ(6, reader.column_count());

  ObString name;
  ASSERT_EQ(OB_SUCCESS, reader.get_column_name(0, name));
  ASSERT_EQ(0, name.case_compare("id"));

  ObString value;
  bool is_null = false;
  ASSERT_EQ(OB_SUCCESS, reader.next_row());
  ASSERT_EQ(OB_SUCCESS, reader.get_value(0, value, is_null));
  ASSERT_FALSE(is_null);
  ASSERT_EQ(0, value.case_compare("1"));
  // the second row has no score
  ASSERT_EQ(OB_SUCCESS, reader.next_row());
  ASSERT_EQ(OB_SUCCESS, reader.get_value(2, value, is_null));
  ASSERT_TRUE(is_null);
  ASSERT_EQ(OB_SUCCESS, reader.next_row());
  ASSERT_EQ(OB_SUCCESS, reader.get_value(2, value, is_null));
  ASSERT_FALSE(is_null);
  ASSERT_EQ(0, value.case_compare("3"));
  // the file has three rows
  ASSERT_EQ(OB_ITER_END, reader.next_row());
  reader.close();
}

TEST_F(ObFileScanUtilsTest, the_schema_of_a_json_file_is_the_union_of_the_keys)
{
  ASSERT_EQ(OB_SUCCESS, write_file(JSON_FILE_NAME, JSON_CONTENT, strlen(JSON_CONTENT)));
  ObArenaAllocator allocator;
  ObFileTableOptions options;
  options.func_type_ = OB_FILE_FUNC_READ_JSON;
  options.format_ = ObString("newline_delimited");
  options.sample_size_ = -1;
  ObSEArray<ObFileTableColumn, 16> columns;
  ASSERT_EQ(OB_SUCCESS, ObFileTableSource::infer_json(allocator, ObString(JSON_FILE_NAME),
                                                      options, columns));
  ASSERT_EQ(3, columns.count());
  ASSERT_EQ(0, columns.at(0).name_.case_compare("a"));
  ASSERT_EQ(0, columns.at(1).name_.case_compare("b"));
  ASSERT_EQ(0, columns.at(2).name_.case_compare("c"));
  ASSERT_EQ(OB_FILE_COL_BIGINT, columns.at(0).type_);
  ASSERT_EQ(OB_FILE_COL_VARCHAR, columns.at(1).type_);
  ASSERT_EQ(OB_FILE_COL_BOOLEAN, columns.at(2).type_);
  // the field "c" is missing in the first two rows
  ASSERT_TRUE(columns.at(2).nullable_);

  ObFileTableReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(allocator, ObString(JSON_FILE_NAME), options, NULL));
  // the first row holds the keys "a" and "b"
  ASSERT_EQ(OB_SUCCESS, reader.next_row());
  ASSERT_EQ(2, reader.get_entry_count());
  // the second row holds the keys "b" and "c"
  ASSERT_EQ(OB_SUCCESS, reader.next_row());
  ASSERT_EQ(2, reader.get_entry_count());
  // the third row holds the key "a" only
  ASSERT_EQ(OB_SUCCESS, reader.next_row());
  ASSERT_EQ(1, reader.get_entry_count());
  ASSERT_EQ(OB_ITER_END, reader.next_row());
  reader.close();
}

TEST_F(ObFileScanUtilsTest, a_file_that_changes_during_the_scan_fails)
{
  // the file is larger than the read buffer of the reader, the scan has to
  // read the file more than once
  ASSERT_EQ(OB_SUCCESS, write_large_csv_file(100000));
  ObArenaAllocator allocator;
  ObFileTableOptions options;
  options.func_type_ = OB_FILE_FUNC_READ_CSV;
  options.header_ = true;
  options.sample_size_ = -1;
  ObFileTableReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(allocator, ObString(CSV_FILE_NAME), options, NULL));
  // read the first rows of the file
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < 100; ++i) {
    ret = reader.next_row();
  }
  ASSERT_EQ(OB_SUCCESS, ret);
  // the file is changed while it is read
  ASSERT_EQ(OB_SUCCESS, write_file(CSV_FILE_NAME, "id,value\n1,2\n", 13));
  // the scan has to fail, the rows that have been read cannot be trusted
  bool failed = false;
  for (int64_t i = 0; i < 100000 && !failed; ++i) {
    const int next_ret = reader.next_row();
    if (OB_ITER_END == next_ret) {
      break;
    } else if (OB_FAIL(next_ret)) {
      failed = true;
      ASSERT_EQ(OB_CHECKSUM_ERROR, next_ret);
    }
  }
  ASSERT_TRUE(failed);
  reader.close();
}

TEST_F(ObFileScanUtilsTest, read_the_parquet_file_of_the_baseline)
{
  ASSERT_EQ(OB_SUCCESS, write_parquet_file());
  ObArenaAllocator allocator;
  ObParquetReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(allocator, ObString(PARQUET_FILE_NAME)));
  ASSERT_EQ(7, reader.get_row_count());
  ASSERT_EQ(1, reader.get_column_count());
  ASSERT_EQ(0, reader.get_column_name(0).case_compare("c1"));
  // the column of the baseline is an int32
  ASSERT_EQ(OB_PARQUET_TYPE_INT32, reader.get_column_type(0));
  int64_t sum = 0;
  for (int64_t row = 0; row < reader.get_row_count(); ++row) {
    ObString value;
    bool is_null = false;
    ASSERT_EQ(OB_SUCCESS, reader.get_value(0, row, value, is_null));
    ASSERT_FALSE(is_null);
    sum += strtol(value.ptr(), NULL, 10);
  }
  // the values of the baseline are 0, 1, 2, 3, 4, 5 and 6
  ASSERT_EQ(21, sum);
  reader.close();
}

} // namespace sql
} // namespace oceanbase
