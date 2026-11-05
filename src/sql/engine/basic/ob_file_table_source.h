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

#ifndef OCEANBASE_BASIC_OB_FILE_TABLE_SOURCE_H_
#define OCEANBASE_BASIC_OB_FILE_TABLE_SOURCE_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "lib/allocator/page_arena.h"
#include "common/object/ob_object.h"
#include "sql/engine/basic/ob_parquet_reader.h"
#include <stdio.h>

namespace oceanbase
{
namespace sql
{

/**
 * The file table functions provided by the "file as database" feature.
 */
enum ObFileTableFuncType : int8_t
{
  OB_FILE_FUNC_READ_CSV = 0,
  OB_FILE_FUNC_READ_JSON = 1,
  OB_FILE_FUNC_READ_PARQUET = 2,
  OB_FILE_FUNC_GLOB = 3,
};

/**
 * The logical type inferred for one column of a file.
 */
enum ObFileTableColType : int8_t
{
  OB_FILE_COL_UNKNOWN = 0,
  OB_FILE_COL_BOOLEAN = 1,
  OB_FILE_COL_BIGINT = 2,
  OB_FILE_COL_DOUBLE = 3,
  OB_FILE_COL_DATE = 4,
  OB_FILE_COL_DATETIME = 5,
  OB_FILE_COL_VARCHAR = 6,
};

/**
 * One inferred column of a file.
 */
struct ObFileTableColumn
{
  ObFileTableColumn() : name_(), type_(OB_FILE_COL_UNKNOWN), nullable_(true), scale_(0) {}
  ObString name_;
  ObFileTableColType type_;
  // whether a sampled value of the column was NULL
  bool nullable_;
  // number of fractional digits, only meaningful for DATETIME columns
  int32_t scale_;
};

/**
 * The options of a file table function.
 */
struct ObFileTableOptions
{
  ObFileTableOptions()
      : func_type_(OB_FILE_FUNC_READ_CSV),
        header_(true),
        sample_size_(-1),
        format_(),
        delimiter_(","),
        nullstr_(),
        describe_(false) {}
  ObFileTableFuncType func_type_;
  // whether the first line of a CSV file holds the column names
  bool header_;
  // number of rows used for type detection, -1 means "all rows"
  int64_t sample_size_;
  // the value of the "format" named argument, may be empty
  ObString format_;
  // the field separator of a CSV file
  ObString delimiter_;
  // the text that represents a null value, empty means "an empty field"
  ObString nullstr_;
  // whether the table holds the description of the columns of the file
  bool describe_;
};

/**
 * Classifies a raw text value into one of the inferred column types.
 *
 * The order of the tests is significant: "true"/"false" are recognised as
 * booleans while plain "0"/"1" stay integers, a bare "YYYY-MM-DD" is a date
 * and "YYYY-MM-DD<sep>HH:MM:SS" is a timestamp.
 */
class ObFileTableTypeDetector
{
public:
  ObFileTableTypeDetector() : type_(OB_FILE_COL_UNKNOWN), scale_(0) {}

  void reset()
  {
    type_ = OB_FILE_COL_UNKNOWN;
    scale_ = 0;
  }

  // feeds one non null value into the detector
  void feed(const char *value, int64_t len);
  // merges the state of another detector of the same column
  void merge(const ObFileTableTypeDetector &other);

  ObFileTableColType get_type() const { return type_; }
  int32_t get_scale() const { return scale_; }
  bool is_known() const { return type_ != OB_FILE_COL_UNKNOWN; }

private:
  void merge_type(ObFileTableColType detected, int32_t scale);

  ObFileTableColType type_;
  int32_t scale_;
};

/**
 * One name/value pair of the current row.
 */
struct ObFileTableEntry
{
  ObFileTableEntry() : name_(), value_(), is_null_(false) {}
  ObString name_;
  ObString value_;
  bool is_null_;
};

/**
 * A streaming reader over one file.
 *
 * The reader never materializes the whole file: it keeps a fixed size read
 * buffer and produces one row at a time.
 */
class ObFileTableReader
{
public:
  ObFileTableReader();
  ~ObFileTableReader();

  /**
   * Opens the file.
   *
   * @param allocator     the allocator used for the read buffer
   * @param path          the absolute path of the file
   * @param options       the options of the table function
   * @param column_names  the names of the table columns, only used by the
   *                      formats that do not carry the names in the file
   */
  int open(ObIAllocator &allocator,
           const ObString &path,
           const ObFileTableOptions &options,
           const common::ObIArray<ObString> *column_names);  // reads the next row, returns OB_ITER_END once the file is exhausted
  int next_row();
  int64_t column_count() const { return column_count_; }
  // the value of one column of the current row
  int get_value(int64_t col_index, ObString &value, bool &is_null) const;
  // the entries of the current row, used to build the schema of a json file
  int64_t get_entry_count() const { return entries_.count(); }
  int get_entry(int64_t index, ObString &name, ObString &value, bool &is_null) const;
  // the names of the columns of a CSV file, only valid for CSV files
  int get_column_name(int64_t col_index, ObString &name) const;
  void close();

private:
  int fill_buffer();
  int read_csv_record();
  int read_jsonl_record();
  int next_glob_record();
  int next_describe_record();
  int build_describe_rows(ObIAllocator &allocator, const ObString &path, const ObFileTableOptions &options);
  int append_describe_value(ObIAllocator &allocator, const ObString &value);
  int append_entry(const ObString &name, const char *data, int64_t len, bool is_null);
  // remembers the size and the modification time of the file
  int record_file_stamp(ObIAllocator &allocator, const ObString &path);
  // fails the scan when the file has been changed since it was opened
  int check_file_unchanged();

private:
  FILE *fp_;
  ObIAllocator *allocator_;
  ObFileTableOptions options_;
  char *buf_;
  int64_t buf_size_;
  int64_t buf_pos_;
  int64_t buf_len_;
  bool eof_;
  // the current row, the values live in row_alloc_
  ObArenaAllocator row_alloc_;
  common::ObArray<ObFileTableEntry> entries_;
  common::ObArray<ObString> header_names_;
  common::ObArray<ObString> column_names_;
  // the scratch buffers used while a json record is parsed
  common::ObArray<ObString> json_keys_;
  common::ObArray<ObString> json_values_;
  common::ObArray<bool> json_nulls_;
  int64_t column_count_;
  // the files matched by a glob pattern
  common::ObArray<ObString> glob_files_;
  int64_t glob_pos_;
  // the reader of a parquet file, the rows of a parquet file are produced one
  // column at a time, the reader keeps the cursor of every column
  mutable ObParquetReader parquet_reader_;
  int64_t parquet_row_;
  // the description of the columns of the file, only used in the describe mode
  common::ObArray<ObString> describe_values_;
  int64_t describe_row_count_;
  int64_t describe_pos_;
  // the stamp of the file that is read, the scan fails when the file changes
  ObString file_path_;
  int64_t file_size_;
  int64_t file_mtime_;
};

/**
 * Infers the schema of a file.
 */
class ObFileTableSource
{
public:
  // infers the columns of a CSV file
  static int infer_csv(ObIAllocator &allocator,
                       const ObString &path,
                       const ObFileTableOptions &options,
                       common::ObIArray<ObFileTableColumn> &columns);
  // infers the columns of a newline delimited JSON file
  static int infer_json(ObIAllocator &allocator,
                        const ObString &path,
                        const ObFileTableOptions &options,
                        common::ObIArray<ObFileTableColumn> &columns);
  // infers the columns of a parquet file, the schema of the file is part of
  // the file itself
  static int infer_parquet(ObIAllocator &allocator,
                           const ObString &path,
                           const ObFileTableOptions &options,
                           common::ObIArray<ObFileTableColumn> &columns);
  // the schema of a glob pattern is always a single "file" column
  static int infer_glob(ObIAllocator &allocator,
                        const ObString &pattern,
                        common::ObIArray<ObFileTableColumn> &columns);
  // lists the files matched by a glob pattern
  static int list_glob_files(ObIAllocator &allocator,
                             const ObString &pattern,
                             common::ObArray<ObString> &files);
  // the printable name of an inferred column type, e.g. "BIGINT"
  static const char *col_type_name(ObFileTableColType type);
  // converts an inferred column into an OB data type
  static int to_data_type(const ObFileTableColumn &column, common::ObDataType &data_type);
  // makes the column names of a header row unique, a duplicated name "x"
  // becomes "x__2", "x__3" and so on
  static int make_unique_names(ObIAllocator &allocator,
                               common::ObArray<ObString> &names,
                               common::ObIArray<ObFileTableColumn> &columns);
  // encodes the path and the options of a file table function into one
  // string, the string is carried by the root column of the table so that the
  // physical operator knows which file it has to read
  static int encode_file_table_spec(ObIAllocator &allocator,
                                    const ObString &path,
                                    const ObFileTableOptions &options,
                                    ObString &spec);
  // decodes the string that encode_file_table_spec has built
  static int decode_file_table_spec(ObIAllocator &allocator,
                                    const ObString &spec,
                                    ObString &path,
                                    ObFileTableOptions &options);
  // the name of a file table function, e.g. "read_csv"
  static const char *func_type_name(ObFileTableFuncType type);
  // the number of the columns of the result of a describe statement
  static const int64_t DESCRIBE_COLUMN_COUNT = 6;
  // the name of one column of the result of a describe statement
  static const char *describe_column_name(int64_t index);
};

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_BASIC_OB_FILE_TABLE_SOURCE_H_
