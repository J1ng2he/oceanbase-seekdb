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

#ifndef OCEANBASE_BASIC_OB_PARQUET_READER_H_
#define OCEANBASE_BASIC_OB_PARQUET_READER_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "lib/allocator/page_arena.h"
#include <stdio.h>

namespace oceanbase
{
namespace sql
{

// the reader of the compact protocol of Thrift, it is defined in the source
// file of the reader
class ObThriftReader;

/**
 * The physical types of the columns of a Parquet file.
 */
enum ObParquetType : int32_t
{
  OB_PARQUET_TYPE_BOOLEAN = 0,
  OB_PARQUET_TYPE_INT32 = 1,
  OB_PARQUET_TYPE_INT64 = 2,
  OB_PARQUET_TYPE_INT96 = 3,
  OB_PARQUET_TYPE_FLOAT = 4,
  OB_PARQUET_TYPE_DOUBLE = 5,
  OB_PARQUET_TYPE_BYTE_ARRAY = 6,
  OB_PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY = 7,
};

/**
 * The converted types of the Parquet format, they refine the physical type
 * of a column.
 */
enum ObParquetConvertedType : int32_t
{
  OB_PARQUET_CONVERTED_NONE = -1,
  OB_PARQUET_CONVERTED_UTF8 = 0,
  OB_PARQUET_CONVERTED_DECIMAL = 5,
  OB_PARQUET_CONVERTED_DATE = 6,
  OB_PARQUET_CONVERTED_TIME_MILLIS = 7,
  OB_PARQUET_CONVERTED_TIME_MICROS = 8,
  OB_PARQUET_CONVERTED_TIMESTAMP_MILLIS = 9,
  OB_PARQUET_CONVERTED_TIMESTAMP_MICROS = 10,
  OB_PARQUET_CONVERTED_UINT_8 = 11,
  OB_PARQUET_CONVERTED_UINT_16 = 12,
  OB_PARQUET_CONVERTED_UINT_32 = 13,
  OB_PARQUET_CONVERTED_UINT_64 = 14,
  OB_PARQUET_CONVERTED_INT_8 = 15,
  OB_PARQUET_CONVERTED_INT_16 = 16,
  OB_PARQUET_CONVERTED_INT_32 = 17,
  OB_PARQUET_CONVERTED_INT_64 = 18,
};

/**
 * The codecs of the Parquet format.
 */
enum ObParquetCodec : int32_t
{
  OB_PARQUET_CODEC_UNCOMPRESSED = 0,
  OB_PARQUET_CODEC_SNAPPY = 1,
  OB_PARQUET_CODEC_GZIP = 2,
  OB_PARQUET_CODEC_ZSTD = 6,
};

/**
 * The encodings of the values of a page.
 */
enum ObParquetEncoding : int32_t
{
  OB_PARQUET_ENCODING_PLAIN = 0,
  OB_PARQUET_ENCODING_PLAIN_DICTIONARY = 2,
  OB_PARQUET_ENCODING_RLE = 3,
  OB_PARQUET_ENCODING_BIT_PACKED = 4,
  OB_PARQUET_ENCODING_DELTA_BINARY_PACKED = 5,
  OB_PARQUET_ENCODING_RLE_DICTIONARY = 8,
  OB_PARQUET_ENCODING_BYTE_STREAM_SPLIT = 9,
};

/**
 * One value of a column of a Parquet file, the value is a text, the physical
 * layer converts the text into the type of the column.
 */
struct ObParquetValue
{
  ObParquetValue() : data_(nullptr), length_(0), is_null_(false) {}
  const char *data_;
  int32_t length_;
  bool is_null_;
};

/**
 * One column of a Parquet file.
 *
 * The values of a column are read page by page, the reader never holds more
 * than one page of a column in memory.
 */
class ObParquetColumn
{
public:
  ObParquetColumn();
  ~ObParquetColumn() {}

  const ObString &get_name() const { return name_; }
  int32_t get_type() const { return type_; }
  int32_t get_type_length() const { return type_length_; }
  int32_t get_converted_type() const { return converted_type_; }
  bool is_optional() const { return 1 == repetition_type_; }

  // the description of the column, it is filled while the footer is parsed
  ObString name_;
  int32_t type_;
  int32_t type_length_;
  int32_t converted_type_;
  int32_t repetition_type_;
  // the description of the chunk of the column
  int32_t codec_;
  int32_t encoding_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  int64_t total_compressed_size_;
  // the cursor of the column
  int64_t next_page_offset_;
  // the row index of the next value of the column
  int64_t next_row_;
  // the row index of the value that has been produced last, it is used to
  // answer a request for a row that has already been read
  int64_t current_row_;
  bool has_current_;
  ObParquetValue current_value_;
  // the values of the current page
  int64_t page_value_count_;
  int64_t page_value_index_;
  common::ObArray<ObParquetValue> page_values_;
  // the texts of the values of the current page
  char *page_text_;
  int64_t page_text_size_;
  int64_t page_text_used_;
  // the values of the dictionary of the column
  common::ObArray<ObParquetValue> dictionary_values_;
  char *dict_text_;
  int64_t dict_text_size_;
  int64_t dict_text_used_;
  bool dictionary_loaded_;
};

/**
 * The header of one page of a column of a Parquet file.
 */
struct ObParquetPageHeader
{
  ObParquetPageHeader()
      : type_(0),
        uncompressed_size_(0),
        compressed_size_(0),
        num_values_(0),
        encoding_(OB_PARQUET_ENCODING_PLAIN),
        def_level_byte_length_(0),
        rep_level_byte_length_(0),
        is_compressed_(true),
        dict_num_values_(0) {}
  // the type of the page, 0 is a data page, 2 is a dictionary page and 3 is
  // a data page of the second version of the format
  int32_t type_;
  int64_t uncompressed_size_;
  int64_t compressed_size_;
  int64_t num_values_;
  int32_t encoding_;
  int64_t def_level_byte_length_;
  int64_t rep_level_byte_length_;
  bool is_compressed_;
  int64_t dict_num_values_;
};

/**
 * A reader over one Parquet file.
 *
 * The reader decodes the footer of the file, it then reads the pages of the
 * columns one by one.  The values are produced as texts, the physical layer
 * converts them into the type of the column.
 */
class ObParquetReader
{
public:
  ObParquetReader();
  ~ObParquetReader();

  int open(ObIAllocator &allocator, const ObString &path);
  void close();

  int64_t get_row_count() const { return row_count_; }
  int64_t get_column_count() const { return columns_.count(); }
  const ObString &get_column_name(int64_t index) const;
  int32_t get_column_type(int64_t index) const;
  int32_t get_column_converted_type(int64_t index) const;
  // reads the value of one column of one row, the rows of a column have to be
  // read in increasing order
  int get_value(int64_t col_index, int64_t row_index, ObString &value, bool &is_null);

private:
  int read_footer();
  int parse_file_meta_data(const char *buf, int64_t len);
  int parse_schema(ObThriftReader &reader, uint8_t type);
  int parse_schema_element(ObThriftReader &reader, ObParquetColumn &column);
  int parse_row_groups(ObThriftReader &reader, uint8_t type);
  int parse_row_group(ObThriftReader &reader);
  int parse_column_chunks(ObThriftReader &reader, uint8_t type);
  int parse_column_chunk(ObThriftReader &reader, ObParquetColumn &column);
  int parse_column_meta_data(ObThriftReader &reader, uint8_t type, ObParquetColumn &column);
  int parse_page_header(ObThriftReader &reader, ObParquetPageHeader &header);
  int parse_data_page_header(ObThriftReader &reader, ObParquetPageHeader &header);
  int parse_data_page_header_v2(ObThriftReader &reader, ObParquetPageHeader &header);
  int parse_dictionary_page_header(ObThriftReader &reader, ObParquetPageHeader &header);
  int init_columns();
  int load_next_page(ObParquetColumn &column);
  int decode_values(ObParquetColumn &column, int32_t encoding, const char *data, int64_t len,
                    int64_t value_count, const int32_t *def_levels);
  int decode_plain_values(ObParquetColumn &column, const char *data, int64_t len,
                          int64_t value_count, const int32_t *def_levels);
  int decode_dictionary_values(ObParquetColumn &column, const char *data, int64_t len,
                               int64_t value_count, const int32_t *def_levels);
  int decode_dictionary_page(ObParquetColumn &column, const char *data, int64_t len,
                             int64_t value_count);
  int decompress_page(ObParquetColumn &column, const char *data, int64_t len,
                      int64_t expected_size, char *&out, int64_t &out_len);
  int read_value(ObParquetColumn &column, ObParquetValue &value);
  int append_text(ObParquetColumn &column, const char *data, int64_t len, ObParquetValue &value,
                  bool to_dictionary);
  int reserve_text(ObParquetColumn &column, int64_t size, bool to_dictionary);
  int append_int_text(ObParquetColumn &column, int64_t value, ObParquetValue &out);
  int append_double_text(ObParquetColumn &column, double value, ObParquetValue &out);
  int append_bool_text(ObParquetColumn &column, bool value, ObParquetValue &out);
  int read_range(int64_t offset, int64_t len, char *&buf);

private:
  FILE *fp_;
  ObIAllocator *allocator_;
  // the allocator of the buffers of the current page, it is reset while the
  // next page is read so that only one page of a column is held in memory
  ObArenaAllocator page_alloc_;
  int64_t file_size_;
  int64_t row_count_;
  common::ObArray<ObParquetColumn> columns_;
};

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_BASIC_OB_PARQUET_READER_H_
