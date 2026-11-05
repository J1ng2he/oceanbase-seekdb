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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_parquet_reader.h"
#include "lib/ob_errno.h"
#include "share/ob_errno.h"
#include "lib/compress/zlib/ob_zlib_compressor.h"

#include <string.h>
#include <string>

namespace oceanbase
{
namespace sql
{

using namespace oceanbase::common;

// ---------------------------------------------------------------------------
// the compact protocol of Thrift, the metadata of a Parquet file is encoded
// with it
//
// the metadata is read field by field, the reader of the protocol skips the
// fields that are not needed, the metadata of a file is never held in memory
// as a tree
// ---------------------------------------------------------------------------

class ObThriftReader
{
public:
  ObThriftReader(const char *buf, int64_t len) : buf_(buf), len_(len), pos_(0) {}

  int64_t pos() const { return pos_; }
  int64_t left() const { return len_ - pos_; }

  int read_byte(uint8_t &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      value = static_cast<uint8_t>(buf_[pos_++]);
    }
    return ret;
  }

  int read_varint(uint64_t &value)
  {
    int ret = OB_SUCCESS;
    uint64_t result = 0;
    int shift = 0;
    bool done = false;
    while (!done) {
      uint8_t byte = 0;
      if (OB_FAIL(ret = read_byte(byte))) {
        break;
      }
      result |= (static_cast<uint64_t>(byte & 0x7F) << shift);
      done = (0 == (byte & 0x80));
      shift += 7;
      if (shift > 70) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
    }
    value = result;
    return ret;
  }

  // the integers of the protocol are encoded with the zigzag encoding
  static int64_t to_zigzag(uint64_t raw) { return static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1)); }

  int read_int(int64_t &value)
  {
    int ret = OB_SUCCESS;
    uint64_t raw = 0;
    if (OB_FAIL(ret = read_varint(raw))) {
    } else {
      value = to_zigzag(raw);
    }
    return ret;
  }

  int read_double(double &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ + 8 > len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      uint64_t raw = 0;
      for (int i = 0; i < 8; ++i) {
        raw |= (static_cast<uint64_t>(static_cast<uint8_t>(buf_[pos_ + i])) << (8 * i));
      }
      pos_ += 8;
      memcpy(&value, &raw, sizeof(value));
    }
    return ret;
  }

  int read_binary(const char *&data, int64_t &len)
  {
    int ret = OB_SUCCESS;
    uint64_t size = 0;
    if (OB_FAIL(ret = read_varint(size))) {
    } else if (pos_ + static_cast<int64_t>(size) > len_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      data = buf_ + pos_;
      len = static_cast<int64_t>(size);
      pos_ += static_cast<int64_t>(size);
    }
    return ret;
  }

  /**
   * Reads the header of the next field of a struct.
   *
   * The function returns OB_ITER_END when the struct ends.
   */
  int read_field_header(int16_t &last_id, int16_t &field_id, uint8_t &type)
  {
    int ret = OB_SUCCESS;
    uint8_t header = 0;
    if (OB_FAIL(ret = read_byte(header))) {
    } else if (0 == header) {
      // the end of the struct
      ret = OB_ITER_END;
    } else {
      const uint8_t delta = static_cast<uint8_t>(header >> 4);
      type = static_cast<uint8_t>(header & 0x0F);
      if (0 == delta) {
        uint64_t raw = 0;
        if (OB_FAIL(ret = read_varint(raw))) {
          return ret;
        }
        field_id = static_cast<int16_t>(to_zigzag(raw));
      } else {
        field_id = static_cast<int16_t>(last_id + delta);
      }
      last_id = field_id;
    }
    return ret;
  }

  /**
   * Reads the header of a list, the number of the elements of the list and
   * the type of the elements.
   */
  int read_list_header(uint64_t &size, uint8_t &elem_type)
  {
    int ret = OB_SUCCESS;
    uint8_t header = 0;
    if (OB_FAIL(ret = read_byte(header))) {
      return ret;
    }
    const uint8_t raw_size = static_cast<uint8_t>(header >> 4);
    elem_type = static_cast<uint8_t>(header & 0x0F);
    size = raw_size;
    if (15 == raw_size) {
      if (OB_FAIL(ret = read_varint(size))) {
        return ret;
      }
    }
    return ret;
  }

  // skips the value of a field
  int skip_value(uint8_t type);
  // skips the elements of a list
  int skip_list(uint8_t elem_type, uint64_t size);
  // skips the fields of a struct
  int skip_struct();

private:
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

int ObThriftReader::skip_value(uint8_t type)
{
  int ret = OB_SUCCESS;
  switch (type) {
  case 1:   // the boolean true
  case 2: { // the boolean false, the value of a boolean is part of the type
    break;
  }
  case 3: { // a byte
    uint8_t value = 0;
    ret = read_byte(value);
    break;
  }
  case 4:   // a 16 bit integer
  case 5:   // a 32 bit integer
  case 6: { // a 64 bit integer
    int64_t value = 0;
    ret = read_int(value);
    break;
  }
  case 7: { // a double
    double value = 0;
    ret = read_double(value);
    break;
  }
  case 8: { // a binary
    const char *data = nullptr;
    int64_t len = 0;
    ret = read_binary(data, len);
    break;
  }
  case 9:   // a list
  case 10: { // a set
    uint64_t size = 0;
    uint8_t elem_type = 0;
    if (OB_SUCC(ret = read_list_header(size, elem_type))) {
      ret = skip_list(elem_type, size);
    }
    break;
  }
  case 11: { // a map
    uint64_t size = 0;
    if (OB_SUCC(ret = read_varint(size))) {
      uint8_t types = 0;
      if (OB_SUCC(ret = read_byte(types))) {
        const uint8_t key_type = static_cast<uint8_t>(types >> 4);
        const uint8_t value_type = static_cast<uint8_t>(types & 0x0F);
        for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          if (OB_FAIL(ret = skip_value(key_type))) {
            break;
          }
          ret = skip_value(value_type);
        }
      }
    }
    break;
  }
  case 12: { // a struct
    ret = skip_struct();
    break;
  }
  default:
    ret = OB_ERR_UNEXPECTED;
    break;
  }
  return ret;
}

int ObThriftReader::skip_list(uint8_t elem_type, uint64_t size)
{
  int ret = OB_SUCCESS;
  for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    ret = skip_value(elem_type);
  }
  return ret;
}

int ObThriftReader::skip_struct()
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = read_field_header(last_id, field_id, type))) {
    ret = skip_value(type);
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the decoder of the Snappy block format, the Parquet format uses the block
// format without the stream framing
// ---------------------------------------------------------------------------

static int snappy_decompress(const char *src, int64_t src_len, char *dst, int64_t dst_len,
                              int64_t &out_len)
{
  int ret = OB_SUCCESS;
  int64_t src_pos = 0;
  int64_t dst_pos = 0;
  uint64_t expected = 0;
  {
    uint64_t result = 0;
    int shift = 0;
    bool done = false;
    while (!done) {
      if (src_pos >= src_len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const uint8_t byte = static_cast<uint8_t>(src[src_pos++]);
      result |= (static_cast<uint64_t>(byte & 0x7F) << shift);
      done = (0 == (byte & 0x80));
      shift += 7;
      if (shift > 70) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
    }
    if (OB_SUCC(ret)) {
      expected = result;
    }
  }
  if (OB_SUCC(ret) && expected > static_cast<uint64_t>(dst_len)) {
    ret = OB_ERR_UNEXPECTED;
  }
  while (OB_SUCC(ret) && src_pos < src_len) {
    const uint8_t tag = static_cast<uint8_t>(src[src_pos++]);
    const uint8_t kind = static_cast<uint8_t>(tag & 0x03);
    if (0 == kind) {
      // a literal
      uint32_t len = static_cast<uint32_t>(tag >> 2);
      if (len >= 60) {
        const uint32_t extra = len - 59;
        if (src_pos + extra > src_len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        uint64_t value = 0;
        for (uint32_t i = 0; i < extra; ++i) {
          value |= (static_cast<uint64_t>(static_cast<uint8_t>(src[src_pos + i])) << (8 * i));
        }
        src_pos += extra;
        len = static_cast<uint32_t>(value);
      }
      ++len;
      if (src_pos + len > src_len || dst_pos + len > dst_len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      memcpy(dst + dst_pos, src + src_pos, len);
      src_pos += len;
      dst_pos += len;
    } else {
      // a copy
      uint32_t len = 0;
      uint32_t offset = 0;
      if (1 == kind) {
        if (src_pos >= src_len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        len = 4 + ((static_cast<uint32_t>(tag) >> 2) & 0x07);
        offset = (static_cast<uint32_t>(tag & 0xE0) << 3) | static_cast<uint8_t>(src[src_pos]);
        src_pos += 1;
      } else if (2 == kind) {
        if (src_pos + 2 > src_len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        len = 1 + (static_cast<uint32_t>(tag) >> 2);
        offset = static_cast<uint32_t>(static_cast<uint8_t>(src[src_pos]))
                 | (static_cast<uint32_t>(static_cast<uint8_t>(src[src_pos + 1])) << 8);
        src_pos += 2;
      } else {
        if (src_pos + 4 > src_len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        len = 1 + (static_cast<uint32_t>(tag) >> 2);
        offset = static_cast<uint32_t>(static_cast<uint8_t>(src[src_pos]))
                 | (static_cast<uint32_t>(static_cast<uint8_t>(src[src_pos + 1])) << 8)
                 | (static_cast<uint32_t>(static_cast<uint8_t>(src[src_pos + 2])) << 16)
                 | (static_cast<uint32_t>(static_cast<uint8_t>(src[src_pos + 3])) << 24);
        src_pos += 4;
      }
      if (0 == offset || offset > static_cast<uint32_t>(dst_pos) || dst_pos + len > dst_len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      // the copy may overlap, the bytes have to be copied one by one
      for (uint32_t i = 0; i < len; ++i) {
        dst[dst_pos + i] = dst[dst_pos - offset + i];
      }
      dst_pos += len;
    }
  }
  if (OB_SUCC(ret)) {
    out_len = dst_pos;
    if (static_cast<uint64_t>(dst_pos) != expected) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the decoder of the RLE and the bit packing hybrid of the Parquet format
// ---------------------------------------------------------------------------

/**
 * Decodes a run of the hybrid encoding.
 *
 * @param bit_width  the number of the bits of a value, zero means that every
 *                   value of the run is zero
 * @param out        the buffer that receives the values
 */
static int decode_rle_levels(const char *data, int64_t len, int32_t bit_width,
                              int64_t value_count, int32_t *out)
{
  int ret = OB_SUCCESS;
  if (bit_width < 0 || bit_width > 32) {
    ret = OB_ERR_UNEXPECTED;
    return ret;
  }
  if (0 == bit_width) {
    // every value of the run is zero, the run holds no data at all
    for (int64_t i = 0; i < value_count; ++i) {
      out[i] = 0;
    }
    return ret;
  }
  int64_t pos = 0;
  int64_t count = 0;
  while (OB_SUCC(ret) && count < value_count) {
    // the header of a run
    uint64_t header = 0;
    int shift = 0;
    bool done = false;
    while (!done) {
      if (pos >= len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const uint8_t byte = static_cast<uint8_t>(data[pos++]);
      header |= (static_cast<uint64_t>(byte & 0x7F) << shift);
      done = (0 == (byte & 0x80));
      shift += 7;
      if (shift > 70) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
    }
    if (OB_FAIL(ret)) {
      break;
    }
    if (0 == (header & 1)) {
      // a run of the same value
      const int64_t run_len = static_cast<int64_t>(header >> 1);
      const int64_t value_bytes = (bit_width + 7) / 8;
      if (pos + value_bytes > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      uint64_t value = 0;
      for (int64_t i = 0; i < value_bytes; ++i) {
        value |= (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i));
      }
      pos += value_bytes;
      for (int64_t i = 0; OB_SUCC(ret) && i < run_len && count < value_count; ++i) {
        out[count++] = static_cast<int32_t>(value);
      }
    } else {
      // a run of the bit packed values
      const int64_t group_count = static_cast<int64_t>(header >> 1);
      const int64_t byte_count = group_count * bit_width;
      if (pos + byte_count > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      for (int64_t group = 0; OB_SUCC(ret) && group < group_count; ++group) {
        for (int i = 0; OB_SUCC(ret) && i < 8 && count < value_count; ++i) {
          uint64_t value = 0;
          for (int bit = 0; bit < bit_width; ++bit) {
            const int64_t bit_index = group * 8 * bit_width + i * bit_width + bit;
            const uint8_t byte = static_cast<uint8_t>(data[pos + bit_index / 8]);
            const uint8_t one = static_cast<uint8_t>((byte >> (bit_index % 8)) & 1);
            value |= (static_cast<uint64_t>(one) << bit);
          }
          out[count++] = static_cast<int32_t>(value);
        }
      }
      pos += byte_count;
    }
  }
  if (OB_SUCC(ret) && count != value_count) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the conversion of a day or of a timestamp into a text
// ---------------------------------------------------------------------------

static void civil_from_days(int64_t days, int32_t &year, int32_t &month, int32_t &day)
{
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  year = static_cast<int32_t>(y + (m <= 2 ? 1 : 0));
  month = static_cast<int32_t>(m);
  day = static_cast<int32_t>(d);
}

// the number of the values of a page that are not null
static int64_t count_present_values(const int32_t *def_levels, int64_t value_count)
{
  if (OB_ISNULL(def_levels)) {
    return value_count;
  }
  int64_t present = 0;
  for (int64_t i = 0; i < value_count; ++i) {
    if (0 != def_levels[i]) {
      ++present;
    }
  }
  return present;
}

// copies a string into the allocator of the reader, the buffer of the footer
// of the file is released as soon as the first page is read
static int copy_parquet_string(ObIAllocator &allocator, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(src.length() > 0 ? src.length() : 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (src.length() > 0 && OB_NOT_NULL(src.ptr())) {
      memcpy(buf, src.ptr(), static_cast<size_t>(src.length()));
    }
    dst.assign_ptr(buf, src.length());
  }
  return ret;
}

// ---------------------------------------------------------------------------
// ObParquetColumn
// ---------------------------------------------------------------------------

ObParquetColumn::ObParquetColumn()
    : name_(),
      type_(OB_PARQUET_TYPE_INT32),
      type_length_(0),
      converted_type_(OB_PARQUET_CONVERTED_NONE),
      repetition_type_(0),
      codec_(OB_PARQUET_CODEC_UNCOMPRESSED),
      encoding_(OB_PARQUET_ENCODING_PLAIN),
      num_values_(0),
      data_page_offset_(0),
      dictionary_page_offset_(-1),
      total_compressed_size_(0),
      next_page_offset_(0),
      next_row_(0),
      current_row_(-1),
      has_current_(false),
      current_value_(),
      page_value_count_(0),
      page_value_index_(0),
      page_values_(),
      page_text_(nullptr),
      page_text_size_(0),
      page_text_used_(0),
      dictionary_values_(),
      dict_text_(nullptr),
      dict_text_size_(0),
      dict_text_used_(0),
      dictionary_loaded_(false)
{}

// ---------------------------------------------------------------------------
// ObParquetReader
// ---------------------------------------------------------------------------

ObParquetReader::ObParquetReader()
    : fp_(nullptr),
      allocator_(nullptr),
      page_alloc_(common::ObModIds::OB_MODULE_PAGE_ALLOCATOR, 4096),
      file_size_(0),
      row_count_(0),
      columns_()
{}

ObParquetReader::~ObParquetReader()
{
  close();
}

void ObParquetReader::close()
{
  if (OB_NOT_NULL(fp_)) {
    fclose(fp_);
    fp_ = nullptr;
  }
  page_alloc_.reset();
}

// the path of a file is a slice of the path of the statement, it is not
// terminated, the file system needs a terminated name
static std::string terminated_path(const ObString &path)
{
  return std::string(path.ptr(), path.length());
}

int ObParquetReader::open(ObIAllocator &allocator, const ObString &path)
{
  int ret = OB_SUCCESS;
  close();
  allocator_ = &allocator;
  columns_.reset();
  row_count_ = 0;
  file_size_ = 0;
  if (path.empty()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(fp_ = fopen(terminated_path(path).c_str(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("cannot open the parquet file", K(ret), K(path));
  } else if (OB_FAIL(ret = fseek(fp_, 0, SEEK_END))) {  } else {
    const long size = ftell(fp_);
    if (size < 12) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      file_size_ = static_cast<int64_t>(size);
    }
  }
  if (OB_SUCC(ret)) {
    ret = read_footer();
  }
  if (OB_SUCC(ret)) {
    ret = init_columns();
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

const ObString &ObParquetReader::get_column_name(int64_t index) const
{
  static const ObString empty;
  if (index < 0 || index >= columns_.count()) {
    return empty;
  }
  return columns_.at(index).get_name();
}

int32_t ObParquetReader::get_column_type(int64_t index) const
{
  if (index < 0 || index >= columns_.count()) {
    return OB_PARQUET_TYPE_INT32;
  }
  return columns_.at(index).get_type();
}

int32_t ObParquetReader::get_column_converted_type(int64_t index) const
{
  if (index < 0 || index >= columns_.count()) {
    return OB_PARQUET_CONVERTED_NONE;
  }
  return columns_.at(index).get_converted_type();
}

int ObParquetReader::read_range(int64_t offset, int64_t len, char *&buf)
{
  int ret = OB_SUCCESS;
  if (offset < 0 || len < 0 || offset + len > file_size_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buf = static_cast<char *>(page_alloc_.alloc(len > 0 ? len : 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ret = fseek(fp_, static_cast<long>(offset), SEEK_SET))) {
  } else if (len > 0 && 1 != fread(buf, static_cast<size_t>(len), 1, fp_)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the footer of the file
// ---------------------------------------------------------------------------

int ObParquetReader::read_footer()
{
  int ret = OB_SUCCESS;
  char magic[4] = {0};
  char *footer = nullptr;
  uint32_t footer_len = 0;
  // the file begins and ends with the magic of the format
  if (OB_FAIL(ret = fseek(fp_, 0, SEEK_SET))) {
  } else if (1 != fread(magic, 4, 1, fp_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (0 != memcmp(magic, "PAR1", 4)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ret = fseek(fp_, static_cast<long>(file_size_ - 4), SEEK_SET))) {
  } else if (1 != fread(magic, 4, 1, fp_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (0 != memcmp(magic, "PAR1", 4)) {
    // the file does not end with the magic of the Parquet format
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ret = fseek(fp_, static_cast<long>(file_size_ - 8), SEEK_SET))) {
  } else if (1 != fread(&footer_len, sizeof(footer_len), 1, fp_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (footer_len + 12 > static_cast<uint32_t>(file_size_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ret = read_range(file_size_ - 8 - footer_len, footer_len, footer))) {
  } else {
    ret = parse_file_meta_data(footer, footer_len);
  }
  return ret;
}

int ObParquetReader::parse_file_meta_data(const char *buf, int64_t len)
{
  int ret = OB_SUCCESS;
  ObThriftReader reader(buf, len);
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    switch (field_id) {
    case 3: { // the number of the rows of the file
      int64_t number = 0;
      if (OB_SUCC(ret = reader.read_int(number))) {
        row_count_ = number;
      }
      break;
    }
    case 2: { // the schema of the file
      ret = parse_schema(reader, type);
      break;
    }
    case 4: { // the row groups of the file
      ret = parse_row_groups(reader, type);
      break;
    }
    default:
      ret = reader.skip_value(type);
      break;
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::parse_schema(ObThriftReader &reader, uint8_t type)
{
  int ret = OB_SUCCESS;
  uint64_t size = 0;
  uint8_t elem_type = 0;
  UNUSED(type);
  if (OB_FAIL(ret = reader.read_list_header(size, elem_type))) {
    return ret;
  }
  if (12 != elem_type) {
    ret = OB_ERR_UNEXPECTED;
    return ret;
  }
  for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    if (0 == i) {
      // the root of the schema, the columns are its children
      ret = reader.skip_struct();
      continue;
    }
    ObParquetColumn column;
    if (OB_SUCC(ret = parse_schema_element(reader, column))) {
      ret = columns_.push_back(column);
      if (OB_FAIL(ret)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  return ret;
}

int ObParquetReader::parse_schema_element(ObThriftReader &reader, ObParquetColumn &column)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  ObString name;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    int64_t number = 0;
    switch (field_id) {
    case 1: // the physical type of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.type_ = static_cast<int32_t>(number);
      }
      break;
    case 2: // the length of the values of a fixed length byte array
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.type_length_ = static_cast<int32_t>(number);
      }
      break;
    case 3: // the repetition type of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.repetition_type_ = static_cast<int32_t>(number);
      }
      break;
    case 4: { // the name of the column
      const char *data = nullptr;
      int64_t data_len = 0;
      if (OB_SUCC(ret = reader.read_binary(data, data_len))) {
        name.assign_ptr(data, static_cast<int32_t>(data_len));
      }
      break;
    }
    case 6: // the converted type of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.converted_type_ = static_cast<int32_t>(number);
      }
      break;
    default:
      ret = reader.skip_value(type);
      break;
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  if (OB_SUCC(ret)) {
    // the buffer of the footer is released as soon as the first page is read,
    // the name of the column has to be copied
    ret = copy_parquet_string(*allocator_, name, column.name_);
  }
  return ret;
}

int ObParquetReader::parse_row_groups(ObThriftReader &reader, uint8_t type)
{
  int ret = OB_SUCCESS;
  uint64_t size = 0;
  uint8_t elem_type = 0;
  UNUSED(type);
  if (OB_FAIL(ret = reader.read_list_header(size, elem_type))) {
    return ret;
  }
  if (12 != elem_type) {
    ret = OB_ERR_UNEXPECTED;
    return ret;
  }
  for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    if (0 == i) {
      // only the first row group is read, the reader does not hold more than
      // one page of a column in memory
      ret = parse_row_group(reader);
    } else {
      ret = reader.skip_struct();
    }
  }
  return ret;
}

int ObParquetReader::parse_row_group(ObThriftReader &reader)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    if (1 == field_id) {
      // the chunks of the columns of the row group
      ret = parse_column_chunks(reader, type);
    } else {
      ret = reader.skip_value(type);
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::parse_column_chunks(ObThriftReader &reader, uint8_t type)
{
  int ret = OB_SUCCESS;
  uint64_t size = 0;
  uint8_t elem_type = 0;
  UNUSED(type);
  if (OB_FAIL(ret = reader.read_list_header(size, elem_type))) {
    return ret;
  }
  if (12 != elem_type) {
    ret = OB_ERR_UNEXPECTED;
    return ret;
  }
  for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    if (i < static_cast<uint64_t>(columns_.count())) {
      ret = parse_column_chunk(reader, columns_.at(i));
    } else {
      ret = reader.skip_struct();
    }
  }
  return ret;
}

int ObParquetReader::parse_column_chunk(ObThriftReader &reader, ObParquetColumn &column)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    if (3 == field_id) {
      // the description of the chunk of the column
      ret = parse_column_meta_data(reader, type, column);
    } else {
      ret = reader.skip_value(type);
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::parse_column_meta_data(ObThriftReader &reader, uint8_t type, ObParquetColumn &column)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  UNUSED(type);
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    int64_t number = 0;
    switch (field_id) {
    case 1: // the physical type of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.type_ = static_cast<int32_t>(number);
      }
      break;
    case 2: { // the encodings of the values of the column
      uint64_t size = 0;
      uint8_t elem_type = 0;
      if (OB_FAIL(ret = reader.read_list_header(size, elem_type))) {
        break;
      }
      for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        if (OB_FAIL(ret = reader.read_int(number))) {
          break;
        }
        if (OB_PARQUET_ENCODING_RLE_DICTIONARY == number
            || OB_PARQUET_ENCODING_PLAIN_DICTIONARY == number) {
          column.encoding_ = OB_PARQUET_ENCODING_RLE_DICTIONARY;
        }
      }
      break;
    }
    case 4: // the codec of the pages of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.codec_ = static_cast<int32_t>(number);
      }
      break;
    case 5: // the number of the values of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.num_values_ = number;
      }
      break;
    case 9: // the offset of the first page of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.data_page_offset_ = number;
      }
      break;
    case 11: // the offset of the dictionary page of the column
      if (OB_SUCC(ret = reader.read_int(number))) {
        column.dictionary_page_offset_ = number;
      }
      break;
    default:
      ret = reader.skip_value(type);
      break;
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::init_columns()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    ObParquetColumn &column = columns_.at(i);
    column.next_page_offset_ = (column.dictionary_page_offset_ >= 0)
                                ? column.dictionary_page_offset_
                                : column.data_page_offset_;
    column.next_row_ = 0;
    column.current_row_ = -1;
    column.has_current_ = false;
    column.current_value_ = ObParquetValue();
    column.page_value_count_ = 0;
    column.page_value_index_ = 0;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the text of the values
// ---------------------------------------------------------------------------

/**
 * Makes sure that the buffer of the texts is large enough.
 *
 * The buffer may have to be moved, the values that have already been decoded
 * point into it, their pointers have to be fixed.
 */
int ObParquetReader::reserve_text(ObParquetColumn &column, int64_t size, bool to_dictionary)
{
  int ret = OB_SUCCESS;
  char *&buf = to_dictionary ? column.dict_text_ : column.page_text_;
  int64_t &buf_size = to_dictionary ? column.dict_text_size_ : column.page_text_size_;
  if (size <= buf_size) {
    return ret;
  }
  char *new_buf = static_cast<char *>(allocator_->alloc(static_cast<uint32_t>(size)));
  if (OB_ISNULL(new_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    char *old_buf = buf;
    const int64_t old_size = buf_size;
    const int64_t used = to_dictionary ? column.dict_text_used_ : column.page_text_used_;
    if (OB_NOT_NULL(old_buf) && used > 0) {
      memcpy(new_buf, old_buf, static_cast<size_t>(used));
    }
    buf = new_buf;
    buf_size = size;
    if (OB_NOT_NULL(old_buf) && old_buf != new_buf) {
      // the values that have already been decoded point into the old buffer
      common::ObArray<ObParquetValue> &values = to_dictionary ? column.dictionary_values_ : column.page_values_;
      for (int64_t i = 0; i < values.count(); ++i) {
        ObParquetValue &item = values.at(i);
        if (OB_NOT_NULL(item.data_) && item.data_ >= old_buf && item.data_ < old_buf + old_size) {
          item.data_ = new_buf + (item.data_ - old_buf);
        }
      }
      if (column.has_current_ && OB_NOT_NULL(column.current_value_.data_)
          && column.current_value_.data_ >= old_buf && column.current_value_.data_ < old_buf + old_size) {
        column.current_value_.data_ = new_buf + (column.current_value_.data_ - old_buf);
      }
    }
  }
  return ret;
}

int ObParquetReader::append_text(ObParquetColumn &column, const char *data, int64_t len,
                                 ObParquetValue &value, bool to_dictionary)
{
  int ret = OB_SUCCESS;
  int64_t &used = to_dictionary ? column.dict_text_used_ : column.page_text_used_;
  if (OB_FAIL(ret = reserve_text(column, used + len + 1, to_dictionary))) {
    return ret;
  }
  char *buf = to_dictionary ? column.dict_text_ : column.page_text_;
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    return ret;
  }
  if (len > 0 && OB_NOT_NULL(data)) {
    memcpy(buf + used, data, static_cast<size_t>(len));
  }
  value.data_ = buf + used;
  value.length_ = static_cast<int32_t>(len);
  value.is_null_ = false;
  used += len;
  return ret;
}

int ObParquetReader::append_int_text(ObParquetColumn &column, int64_t number, ObParquetValue &out)
{
  int ret = OB_SUCCESS;
  char buf[32];
  const int len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number));
  if (len < 0 || len >= static_cast<int>(sizeof(buf))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = append_text(column, buf, len, out, false);
  }
  return ret;
}

int ObParquetReader::append_double_text(ObParquetColumn &column, double number, ObParquetValue &out)
{
  int ret = OB_SUCCESS;
  char buf[64];
  const int len = snprintf(buf, sizeof(buf), "%.15g", number);
  if (len < 0 || len >= static_cast<int>(sizeof(buf))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = append_text(column, buf, len, out, false);
  }
  return ret;
}

int ObParquetReader::append_bool_text(ObParquetColumn &column, bool number, ObParquetValue &out)
{
  int ret = OB_SUCCESS;
  if (number) {
    ret = append_text(column, "true", 4, out, false);
  } else {
    ret = append_text(column, "false", 5, out, false);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the decoder of the pages
// ---------------------------------------------------------------------------

int ObParquetReader::decode_plain_values(ObParquetColumn &column, const char *data, int64_t len,
                                         int64_t value_count,
                                         const int32_t *def_levels)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const bool has_levels = (OB_NOT_NULL(def_levels));
  int64_t present_index = 0;
  // the texts of the values of the page are held in one buffer, the buffer is
  // large enough for most of the pages
  if (OB_FAIL(ret = reserve_text(column, 64 + value_count * 24, false))) {
    return ret;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
    ObParquetValue value;
    const bool is_null = has_levels && (0 == def_levels[i]);
    if (is_null) {
      value.is_null_ = true;
    } else {
      switch (column.get_type()) {
      case OB_PARQUET_TYPE_BOOLEAN: {
        // the booleans are stored as a bit packed run, the bit of the index of
        // the value is the value itself
        const int64_t bit_index = present_index;
        if ((bit_index + 8) / 8 > len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        const uint8_t byte = static_cast<uint8_t>(data[bit_index / 8]);
        const bool number = (0 != ((byte >> (bit_index % 8)) & 1));
        ret = append_bool_text(column, number, value);
        break;
      }
      case OB_PARQUET_TYPE_INT32: {
        if (pos + 4 > len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        const int32_t number = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint8_t>(data[pos]))
                                                     | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 1])) << 8)
                                                     | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 2])) << 16)
                                                     | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 3])) << 24));
        pos += 4;
        if (OB_PARQUET_CONVERTED_DATE == column.get_converted_type()) {
          int32_t year = 0;
          int32_t month = 0;
          int32_t day = 0;
          civil_from_days(number, year, month, day);
          char buf[32];
          const int written = snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
          ret = append_text(column, buf, written, value, false);
        } else {
          ret = append_int_text(column, number, value);
        }
        break;
      }
      case OB_PARQUET_TYPE_INT64: {
        if (pos + 8 > len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
          raw |= (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i));
        }
        pos += 8;
        ret = append_int_text(column, static_cast<int64_t>(raw), value);
        break;
      }
      case OB_PARQUET_TYPE_INT96: {
        // the value is a number of days and a number of nanoseconds
        if (pos + 12 > len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        const uint64_t nanos = static_cast<uint64_t>(static_cast<uint8_t>(data[pos]))
                               | (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 1])) << 8)
                               | (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 2])) << 16)
                               | (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + 3])) << 24);
        const int32_t days = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 4]))
                                                   | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 5])) << 8)
                                                   | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 6])) << 16)
                                                   | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 7])) << 24));
        pos += 12;
        int32_t year = 0;
        int32_t month = 0;
        int32_t day = 0;
        civil_from_days(days, year, month, day);
        char buf[64];
        const int written = snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02u:%02u:%02u.%09u",
                                     year, month, day,
                                     static_cast<unsigned>(nanos / 3600000000000ULL),
                                     static_cast<unsigned>((nanos / 60000000000ULL) % 60),
                                     static_cast<unsigned>((nanos / 1000000000ULL) % 60),
                                     static_cast<unsigned>(nanos % 1000000000ULL));
        ret = append_text(column, buf, written, value, false);
        break;
      }
      case OB_PARQUET_TYPE_FLOAT: {
        if (pos + 4 > len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        uint32_t raw = 0;
        for (int i = 0; i < 4; ++i) {
          raw |= (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i));
        }
        pos += 4;
        float number = 0;
        memcpy(&number, &raw, sizeof(number));
        ret = append_double_text(column, static_cast<double>(number), value);
        break;
      }
      case OB_PARQUET_TYPE_DOUBLE: {
        if (pos + 8 > len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
          raw |= (static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i));
        }
        pos += 8;
        double number = 0;
        memcpy(&number, &raw, sizeof(number));
        ret = append_double_text(column, number, value);
        break;
      }
      case OB_PARQUET_TYPE_BYTE_ARRAY:
      case OB_PARQUET_TYPE_FIXED_LEN_BYTE_ARRAY: {
        int64_t value_len = 0;
        if (OB_PARQUET_TYPE_BYTE_ARRAY == column.get_type()) {
          if (pos + 4 > len) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          value_len = static_cast<int64_t>(static_cast<uint32_t>(static_cast<uint8_t>(data[pos]))
                        | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 1])) << 8)
                        | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 2])) << 16)
                        | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 3])) << 24));
          pos += 4;
        } else {
          value_len = column.get_type_length();
        }
        if (pos + value_len > len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        ret = append_text(column, data + pos, value_len, value, false);
        pos += value_len;
        break;
      }
      default:
        ret = OB_NOT_SUPPORTED;
        break;
      }
    }
    if (OB_SUCC(ret)) {
      if (!is_null) {
        ++present_index;
      }
      if (OB_FAIL(ret = column.page_values_.push_back(value))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
    }
  }
  return ret;
}

int ObParquetReader::decode_dictionary_values(ObParquetColumn &column, const char *data, int64_t len,
                                              int64_t value_count,
                                              const int32_t *def_levels)
{
  int ret = OB_SUCCESS;
  if (!column.dictionary_loaded_) {
    ret = OB_ERR_UNEXPECTED;
    return ret;
  }
  if (len < 1) {
    ret = OB_ERR_UNEXPECTED;
    return ret;
  }
  // the first byte of the data holds the number of the bits of an index
  const int32_t bit_width = static_cast<int8_t>(data[0]);
  const int64_t present_count = count_present_values(def_levels, value_count);
  int32_t *indices = nullptr;
  if (present_count > 0) {
    indices = static_cast<int32_t *>(page_alloc_.alloc(sizeof(int32_t) * present_count));
    if (OB_ISNULL(indices)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      return ret;
    }
    if (OB_FAIL(ret = decode_rle_levels(data + 1, len - 1, bit_width, present_count, indices))) {
      return ret;
    }
  }
  int64_t index = 0;
  const bool has_levels = (OB_NOT_NULL(def_levels));
  for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
    ObParquetValue value;
    if (has_levels && 0 == def_levels[i]) {
      value.is_null_ = true;
    } else {
      if (index >= present_count) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const int32_t dict_index = indices[index++];
      if (dict_index < 0 || dict_index >= column.dictionary_values_.count()) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const ObParquetValue &item = column.dictionary_values_.at(dict_index);
      value.data_ = item.data_;
      value.length_ = item.length_;
      value.is_null_ = false;
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ret = column.page_values_.push_back(value))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
    }
  }
  return ret;
}

int ObParquetReader::decode_values(ObParquetColumn &column, int32_t encoding, const char *data,
                                   int64_t len, int64_t value_count,
                                   const int32_t *def_levels)
{
  int ret = OB_SUCCESS;
  if (OB_PARQUET_ENCODING_RLE_DICTIONARY == encoding
      || OB_PARQUET_ENCODING_PLAIN_DICTIONARY == encoding) {
    ret = decode_dictionary_values(column, data, len, value_count, def_levels);
  } else if (OB_PARQUET_ENCODING_PLAIN == encoding
             || OB_PARQUET_ENCODING_RLE == encoding
             || OB_PARQUET_ENCODING_BIT_PACKED == encoding) {
    ret = decode_plain_values(column, data, len, value_count, def_levels);
  } else {
    // the delta encodings and the split encoding are not supported
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("the encoding of the page is not supported", K(ret), K(encoding));
  }
  if (OB_SUCC(ret)) {
    // the number of the values of the page, the reader produces them one by
    // one, the count has to be known before the first value is asked for
    column.page_value_count_ = column.page_values_.count();
  }
  return ret;
}

int ObParquetReader::decode_dictionary_page(ObParquetColumn &column, const char *data, int64_t len,
                                            int64_t value_count)
{
  int ret = OB_SUCCESS;
  char *plain = nullptr;
  const int64_t plain_size = len;
  if (OB_ISNULL(plain = static_cast<char *>(page_alloc_.alloc(plain_size > 0 ? plain_size : 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    return ret;
  }
  int64_t plain_len = 0;
  if (OB_FAIL(ret = decompress_page(column, data, len, plain_size, plain, plain_len))) {
    return ret;
  }
  column.dictionary_values_.reset();
  column.dict_text_used_ = 0;
  int64_t pos = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
    ObParquetValue value;
    int64_t value_len = 0;
    if (OB_PARQUET_TYPE_BOOLEAN == column.get_type()) {
      // the booleans of the dictionary are stored as a bit packed run
      if ((i + 8) / 8 > plain_len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const uint8_t byte = static_cast<uint8_t>(plain[i / 8]);
      const bool number = (0 != ((byte >> (i % 8)) & 1));
      if (OB_FAIL(ret = append_text(column, number ? "true" : "false", number ? 4 : 5, value, true))) {
        break;
      }
      if (OB_FAIL(ret = column.dictionary_values_.push_back(value))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      continue;
    }
    if (OB_PARQUET_TYPE_BYTE_ARRAY == column.get_type()) {
      if (pos + 4 > plain_len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      value_len = static_cast<int64_t>(static_cast<uint32_t>(static_cast<uint8_t>(plain[pos]))
                    | (static_cast<uint32_t>(static_cast<uint8_t>(plain[pos + 1])) << 8)
                    | (static_cast<uint32_t>(static_cast<uint8_t>(plain[pos + 2])) << 16)
                    | (static_cast<uint32_t>(static_cast<uint8_t>(plain[pos + 3])) << 24));
      pos += 4;
    } else {
      value_len = column.get_type_length();
    }
    if (pos + value_len > plain_len) {
      ret = OB_ERR_UNEXPECTED;
      break;
    }
    if (OB_FAIL(ret = append_text(column, plain + pos, value_len, value, true))) {
      break;
    }
    pos += value_len;
    if (OB_FAIL(ret = column.dictionary_values_.push_back(value))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      break;
    }
  }
  if (OB_SUCC(ret)) {
    column.dictionary_loaded_ = true;
  }
  return ret;
}

int ObParquetReader::decompress_page(ObParquetColumn &column, const char *data, int64_t len,
                                      int64_t expected_size, char *&out, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  out_len = 0;
  if (OB_PARQUET_CODEC_UNCOMPRESSED == column.codec_) {
    if (len > expected_size) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      memcpy(out, data, static_cast<size_t>(len));
      out_len = len;
    }
  } else if (OB_PARQUET_CODEC_SNAPPY == column.codec_) {
    ret = snappy_decompress(data, len, out, expected_size, out_len);
  } else if (OB_PARQUET_CODEC_GZIP == column.codec_) {
    ObZlibCompressor decompressor;
    int64_t size = 0;
    ret = decompressor.decompress(data, len, out, expected_size, size);
    out_len = size;
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("the codec of the page is not supported", K(ret), K(column.codec_));
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the pages of a column
// ---------------------------------------------------------------------------

int ObParquetReader::parse_page_header(ObThriftReader &reader, ObParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    int64_t number = 0;
    switch (field_id) {
    case 1: // the type of the page
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.type_ = static_cast<int32_t>(number);
      }
      break;
    case 2: // the size of the page before it is compressed
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.uncompressed_size_ = number;
      }
      break;
    case 3: // the size of the page after it is compressed
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.compressed_size_ = number;
      }
      break;
    case 5: // the header of a data page of the first version of the format
      ret = parse_data_page_header(reader, header);
      break;
    case 7: // the header of a dictionary page
      ret = parse_dictionary_page_header(reader, header);
      break;
    case 8: // the header of a data page of the second version of the format
      ret = parse_data_page_header_v2(reader, header);
      break;
    default:
      ret = reader.skip_value(type);
      break;
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::parse_data_page_header(ObThriftReader &reader, ObParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    int64_t number = 0;
    switch (field_id) {
    case 1: // the number of the values of the page
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.num_values_ = number;
      }
      break;
    case 2: // the encoding of the values of the page
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.encoding_ = static_cast<int32_t>(number);
      }
      break;
    default:
      ret = reader.skip_value(type);
      break;
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::parse_data_page_header_v2(ObThriftReader &reader, ObParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    int64_t number = 0;
    switch (field_id) {
    case 1: // the number of the values of the page
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.num_values_ = number;
      }
      break;
    case 4: // the encoding of the values of the page
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.encoding_ = static_cast<int32_t>(number);
      }
      break;
    case 5: // the number of the bytes of the definition levels
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.def_level_byte_length_ = number;
      }
      break;
    case 6: // the number of the bytes of the repetition levels
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.rep_level_byte_length_ = number;
      }
      break;
    case 7: // whether the values of the page are compressed, the value of a
             // boolean is part of the type of the field
      header.is_compressed_ = (1 == type);
      break;
    default:
      ret = reader.skip_value(type);
      break;
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::parse_dictionary_page_header(ObThriftReader &reader, ObParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  int16_t field_id = 0;
  uint8_t type = 0;
  while (OB_SUCC(ret = reader.read_field_header(last_id, field_id, type))) {
    int64_t number = 0;
    if (1 == field_id) {
      // the number of the values of the dictionary
      if (OB_SUCC(ret = reader.read_int(number))) {
        header.dict_num_values_ = number;
      }
    } else {
      ret = reader.skip_value(type);
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObParquetReader::load_next_page(ObParquetColumn &column)
{
  int ret = OB_SUCCESS;
  column.page_value_count_ = 0;
  column.page_value_index_ = 0;
  column.page_values_.reset();
  column.page_text_used_ = 0;
  // only the data of the current page is held in memory, the buffer of the
  // previous page is released
  page_alloc_.reset();
  if (column.next_page_offset_ <= 0 || column.next_page_offset_ >= file_size_) {
    return OB_SUCCESS;
  }
  char *header_buf = nullptr;
  int64_t header_size = 0;
  int64_t body_len = 0;
  int64_t value_count = 0;
  int32_t encoding = column.encoding_;
  ObParquetPageHeader header;
  {
    const int64_t max_header_size = 4096;
    const int64_t available = file_size_ - column.next_page_offset_;
    const int64_t read_size = available < max_header_size ? available : max_header_size;
    if (OB_FAIL(ret = read_range(column.next_page_offset_, read_size, header_buf))) {
      return ret;
    }
    ObThriftReader reader(header_buf, read_size);
    if (OB_FAIL(ret = parse_page_header(reader, header))) {
      return ret;
    }
    header_size = reader.pos();
    value_count = header.num_values_;
    encoding = header.encoding_;
  }
  const int64_t body_offset = column.next_page_offset_ + header_size;
  column.next_page_offset_ = body_offset + header.compressed_size_;
  if (body_offset + header.compressed_size_ > file_size_) {
    ret = OB_ERR_UNEXPECTED;
    return ret;
  }
  char *body = nullptr;
  if (OB_FAIL(ret = read_range(body_offset, header.compressed_size_, body))) {
    return ret;
  }
  if (2 == header.type_) {
    // a dictionary page, the values of the column are in the pages that
    // follow, a column chunk holds at most one dictionary page
    if (OB_SUCC(ret = decode_dictionary_page(column, body, header.compressed_size_,
                                             header.dict_num_values_))) {
      ret = load_next_page(column);
    }
    return ret;
  }
  if (0 != header.type_ && 3 != header.type_) {
    ret = OB_NOT_SUPPORTED;
    return ret;
  }
  if (3 == header.type_) {
    // the levels of a data page of the second version of the format are not
    // compressed, only the values are
    const int64_t level_size = header.rep_level_byte_length_ + header.def_level_byte_length_;
    if (level_size > header.compressed_size_) {
      ret = OB_ERR_UNEXPECTED;
      return ret;
    }
    const int64_t value_size = header.compressed_size_ - level_size;
    const int64_t expected_size = header.uncompressed_size_ - level_size;
    char *values = nullptr;
    int64_t values_len = 0;
    if (expected_size > 0) {
      if (OB_ISNULL(values = static_cast<char *>(page_alloc_.alloc(expected_size)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
      }
      if (header.is_compressed_) {
        if (OB_FAIL(ret = decompress_page(column, body + level_size, value_size, expected_size,
                                          values, values_len))) {
          return ret;
        }
      } else {
        memcpy(values, body + level_size, static_cast<size_t>(value_size));
        values_len = value_size;
      }
    }
    // the definition levels of the page
    int32_t *def_levels = nullptr;
    if (column.is_optional() && header.def_level_byte_length_ > 0) {
      def_levels = static_cast<int32_t *>(page_alloc_.alloc(sizeof(int32_t) * value_count));
      if (OB_ISNULL(def_levels)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
      }
      if (OB_FAIL(ret = decode_rle_levels(body + header.rep_level_byte_length_,
                                          header.def_level_byte_length_, 1,
                                          value_count, def_levels))) {
        return ret;
      }
    }
    ret = decode_values(column, encoding, values, values_len, value_count, def_levels);
    return ret;
  }
  // a data page of the first version of the format
  char *plain = nullptr;
  const int64_t plain_size = header.uncompressed_size_ > 0 ? header.uncompressed_size_ : 1;
  if (OB_ISNULL(plain = static_cast<char *>(page_alloc_.alloc(plain_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    return ret;
  }
  if (OB_FAIL(ret = decompress_page(column, body, header.compressed_size_, header.uncompressed_size_,
                                    plain, body_len))) {
    return ret;
  }
  const int64_t def_length = column.is_optional()
                               ? static_cast<int64_t>(static_cast<uint8_t>(plain[0]))
                                 | (static_cast<int64_t>(static_cast<uint8_t>(plain[1])) << 8)
                                 | (static_cast<int64_t>(static_cast<uint8_t>(plain[2])) << 16)
                                 | (static_cast<int64_t>(static_cast<uint8_t>(plain[3])) << 24)
                               : 0;
  int32_t *def_levels = nullptr;
  if (column.is_optional()) {
    if (4 + def_length > body_len) {
      ret = OB_ERR_UNEXPECTED;
      return ret;
    }
    def_levels = static_cast<int32_t *>(page_alloc_.alloc(sizeof(int32_t) * value_count));
    if (OB_ISNULL(def_levels)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      return ret;
    }
    if (OB_FAIL(ret = decode_rle_levels(plain + 4, def_length, 1, value_count, def_levels))) {
      return ret;
    }
  }
  ret = decode_values(column, encoding, plain + 4 + def_length, body_len - 4 - def_length, value_count,
                      def_levels);
  return ret;
}

int ObParquetReader::read_value(ObParquetColumn &column, ObParquetValue &value)
{
  int ret = OB_SUCCESS;
  if (column.num_values_ > 0 && column.next_row_ >= column.num_values_) {
    // the chunk of the column holds no more values, the reader must not read
    // beyond the last page of the chunk
    ret = OB_ITER_END;
    return ret;
  }
  if (column.page_value_index_ >= column.page_value_count_) {
    if (OB_FAIL(ret = load_next_page(column))) {
      return ret;
    }
    if (0 == column.page_value_count_) {
      // the file holds no more values
      ret = OB_ITER_END;
      return ret;
    }
  }
  value = column.page_values_.at(column.page_value_index_);
  ++column.page_value_index_;
  ++column.next_row_;
  return ret;
}

int ObParquetReader::get_value(int64_t col_index, int64_t row_index, ObString &value, bool &is_null)
{
  int ret = OB_SUCCESS;
  if (col_index < 0 || col_index >= columns_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObParquetColumn &column = columns_.at(col_index);
    if (column.has_current_ && column.current_row_ == row_index) {
      // the value of the row has already been produced
      is_null = column.current_value_.is_null_;
      value = column.current_value_.is_null_
                ? ObString()
                : ObString(column.current_value_.length_, column.current_value_.data_);
    } else if (row_index < column.next_row_) {
      // the rows of a column have to be read in increasing order
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rows of a column have to be read in increasing order", K(ret), K(row_index));
    } else {
      // the rows that are not needed are skipped
      while (OB_SUCC(ret) && column.next_row_ < row_index) {
        ObParquetValue skipped;
        const int read_ret = read_value(column, skipped);
        if (OB_ITER_END == read_ret) {
          // the file holds fewer rows than the row that is asked for
          break;
        } else if (OB_FAIL(read_ret)) {
          ret = read_ret;
          break;
        }
      }
      if (OB_SUCC(ret) && column.next_row_ == row_index) {
        ObParquetValue current;
        const int read_ret = read_value(column, current);
        if (OB_ITER_END == read_ret) {
          // the file holds fewer rows than the row that is asked for
          is_null = true;
          value = ObString();
        } else if (OB_FAIL(read_ret)) {
          ret = read_ret;
        } else {
          column.current_value_ = current;
          column.current_row_ = row_index;
          column.has_current_ = true;
          is_null = current.is_null_;
          value = current.is_null_ ? ObString() : ObString(current.length_, current.data_);
        }
      } else {
        // the file holds fewer rows than the row that is asked for
        is_null = true;
        value = ObString();
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
