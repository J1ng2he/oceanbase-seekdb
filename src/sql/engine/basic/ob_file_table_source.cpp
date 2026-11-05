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
#include "sql/engine/basic/ob_file_table_source.h"

#include "share/ob_errno.h"
#include "lib/utility/ob_platform_utils.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace oceanbase
{
namespace sql
{

static const int64_t FILE_TABLE_READ_BUF_SIZE = 64 * 1024;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

/**
 * Reads the size and the modification time of a file.
 *
 * The stamp is used to detect a file that is changed while it is read, such a
 * scan cannot produce a consistent result and has to fail.
 */
static int get_file_stamp(const char *path, int64_t &size, int64_t &mtime)
{
  int ret = OB_SUCCESS;
  ob_stat64_t st;
  if (OB_ISNULL(path) || 0 != ob_stat64(path, &st)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    size = static_cast<int64_t>(st.st_size);
#if defined(_WIN32)
    mtime = static_cast<int64_t>(st.st_mtime);
#elif defined(__APPLE__)
    mtime = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000
            + static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#else
    mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000
            + static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
  }
  return ret;
}

static inline bool is_digit_char(char c) { return c >= '0' && c <= '9'; }

static inline char to_lower_char(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool str_equal_ci(const char *s, int64_t len, const char *lit)
{
  const int64_t lit_len = (int64_t)strlen(lit);
  if (nullptr == s || len != lit_len) {
    return false;
  }
  for (int64_t i = 0; i < len; ++i) {
    if (to_lower_char(s[i]) != to_lower_char(lit[i])) {
      return false;
    }
  }
  return true;
}

// recognises an optionally signed decimal integer
static bool looks_like_integer(const char *s, int64_t len)
{
  if (nullptr == s || len <= 0) {
    return false;
  }
  int64_t i = 0;
  if (s[0] == '+' || s[0] == '-') {
    i = 1;
  }
  if (i >= len) {
    return false;
  }
  for (; i < len; ++i) {
    if (!is_digit_char(s[i])) {
      return false;
    }
  }
  return true;
}

// recognises a floating point literal
static bool looks_like_double(const char *s, int64_t len)
{
  if (nullptr == s || len <= 0) {
    return false;
  }
  int64_t i = 0;
  if (s[0] == '+' || s[0] == '-') {
    i = 1;
  }
  bool has_digit = false;
  bool has_dot = false;
  bool has_exp = false;
  for (; i < len; ++i) {
    const char c = s[i];
    if (is_digit_char(c)) {
      has_digit = true;
    } else if ('.' == c && !has_dot && !has_exp) {
      has_dot = true;
    } else if (('e' == c || 'E' == c) && has_digit && !has_exp) {
      has_exp = true;
      if (i + 1 < len && ('+' == s[i + 1] || '-' == s[i + 1])) {
        ++i;
      }
      if (i + 1 >= len) {
        return false;
      }
    } else {
      return false;
    }
  }
  return has_digit;
}

// "YYYY-MM-DD"
static bool looks_like_date(const char *s, int64_t len)
{
  if (nullptr == s || len != 10) {
    return false;
  }
  if (!is_digit_char(s[0]) || !is_digit_char(s[1]) || !is_digit_char(s[2]) || !is_digit_char(s[3])) {
    return false;
  }
  if ('-' != s[4]) {
    return false;
  }
  if (!is_digit_char(s[5]) || !is_digit_char(s[6])) {
    return false;
  }
  if ('-' != s[7]) {
    return false;
  }
  if (!is_digit_char(s[8]) || !is_digit_char(s[9])) {
    return false;
  }
  const int month = (s[5] - '0') * 10 + (s[6] - '0');
  const int day = (s[8] - '0') * 10 + (s[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// "YYYY-MM-DD<sep>HH:MM:SS[.fraction]", <sep> is a space or 'T'
static bool looks_like_datetime(const char *s, int64_t len, int32_t &scale)
{
  scale = 0;
  if (nullptr == s || len < 19) {
    return false;
  }
  if (!looks_like_date(s, 10)) {
    return false;
  }
  if (' ' != s[10] && 'T' != s[10]) {
    return false;
  }
  for (int i = 11; i < 19; ++i) {
    if (13 == i || 16 == i) {
      if (':' != s[i]) {
        return false;
      }
    } else if (!is_digit_char(s[i])) {
      return false;
    }
  }
  if (19 == len) {
    return true;
  }
  if ('.' != s[19]) {
    return false;
  }
  int64_t i = 20;
  while (i < len && is_digit_char(s[i])) {
    ++i;
  }
  if (20 == i || i != len) {
    return false;
  }
  scale = (int32_t)(len - 20);
  return true;
}

// ---------------------------------------------------------------------------
// ObFileTableTypeDetector
// ---------------------------------------------------------------------------

void ObFileTableTypeDetector::feed(const char *value, int64_t len)
{
  ObFileTableColType detected = OB_FILE_COL_VARCHAR;
  int32_t scale = 0;
  if (str_equal_ci(value, len, "true") || str_equal_ci(value, len, "false")) {
    detected = OB_FILE_COL_BOOLEAN;
  } else if (looks_like_integer(value, len)) {
    detected = OB_FILE_COL_BIGINT;
  } else if (looks_like_double(value, len)) {
    detected = OB_FILE_COL_DOUBLE;
  } else if (looks_like_date(value, len)) {
    detected = OB_FILE_COL_DATE;
  } else if (looks_like_datetime(value, len, scale)) {
    detected = OB_FILE_COL_DATETIME;
  }
  merge_type(detected, scale);
}

void ObFileTableTypeDetector::merge_type(ObFileTableColType detected, int32_t scale)
{
  if (!is_known()) {
    type_ = detected;
    scale_ = scale;
    return;
  }
  if (type_ == detected) {
    if (OB_FILE_COL_DATETIME == detected && scale > scale_) {
      scale_ = scale;
    }
    return;
  }
  if ((OB_FILE_COL_BIGINT == type_ && OB_FILE_COL_DOUBLE == detected)
      || (OB_FILE_COL_DOUBLE == type_ && OB_FILE_COL_BIGINT == detected)) {
    type_ = OB_FILE_COL_DOUBLE;
    return;
  }
  if ((OB_FILE_COL_DATE == type_ && OB_FILE_COL_DATETIME == detected)
      || (OB_FILE_COL_DATETIME == type_ && OB_FILE_COL_DATE == detected)) {
    type_ = OB_FILE_COL_DATETIME;
    if (OB_FILE_COL_DATETIME == detected && scale > scale_) {
      scale_ = scale;
    }
    return;
  }
  type_ = OB_FILE_COL_VARCHAR;
}

void ObFileTableTypeDetector::merge(const ObFileTableTypeDetector &other)
{
  if (!other.is_known()) {
    return;
  }
  merge_type(other.type_, other.scale_);
}

// ---------------------------------------------------------------------------
// a small flat JSON object parser, used for newline delimited JSON files
// ---------------------------------------------------------------------------

namespace
{
class ObFlatJsonParser
{
public:
  ObFlatJsonParser() : text_(NULL), len_(0), pos_(0) {}

  void reset(const char *text, int64_t len)
  {
    text_ = text;
    len_ = len;
    pos_ = 0;
  }

  // parses one flat JSON object, the values are returned as raw text
  int parse_object(ObIAllocator &allocator,
                   common::ObIArray<ObString> &keys,
                   common::ObIArray<ObString> &values,
                   common::ObIArray<bool> &nulls)
  {
    int ret = OB_SUCCESS;
    skip_space();
    if (pos_ >= len_ || '{' != text_[pos_]) {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_SUCC(ret)) {
      ++pos_;
      skip_space();
      while (OB_SUCC(ret) && pos_ < len_ && '}' != text_[pos_]) {
        ObString key;
        ObString value;
        bool is_null = false;
        if (OB_FAIL(parse_string(allocator, key))) {
        } else if (OB_FAIL(skip_colon())) {
        } else if (OB_FAIL(parse_value(allocator, value, is_null))) {
        } else if (OB_FAIL(keys.push_back(key))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(values.push_back(value))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(nulls.push_back(is_null))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
        if (OB_SUCC(ret)) {
          skip_space();
          if (pos_ < len_ && ',' == text_[pos_]) {
            ++pos_;
            skip_space();
          }
        }
      }
    }
    return ret;
  }

private:
  void skip_space()
  {
    while (pos_ < len_
           && (' ' == text_[pos_] || '\t' == text_[pos_] || '\r' == text_[pos_] || '\n' == text_[pos_])) {
      ++pos_;
    }
  }

  int skip_colon()
  {
    int ret = OB_SUCCESS;
    skip_space();
    if (pos_ >= len_ || ':' != text_[pos_]) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ++pos_;
      skip_space();
    }
    return ret;
  }

  int parse_string(ObIAllocator &allocator, ObString &out)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_ || '"' != text_[pos_]) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ++pos_;
      const int64_t start = pos_;
      bool escaped = false;
      while (pos_ < len_) {
        const char c = text_[pos_++];
        if (escaped) {
          escaped = false;
        } else if ('\\' == c) {
          escaped = true;
        } else if ('"' == c) {
          break;
        }
      }
      int64_t raw_len = pos_ - start;
      if (raw_len > 0 && '"' == text_[pos_ - 1]) {
        --raw_len;
      }
      if (raw_len < 0) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObString raw(raw_len, text_ + start);
        if (OB_FAIL(unescape(allocator, raw, out))) {
        }
      }
    }
    return ret;
  }

  int unescape(ObIAllocator &allocator, const ObString &raw, ObString &out)
  {
    int ret = OB_SUCCESS;
    if (NULL == raw.ptr()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (NULL == memchr(raw.ptr(), '\\', (size_t)raw.length())) {
      out = raw;
    } else {
      char *buf = static_cast<char *>(allocator.alloc(raw.length() > 0 ? raw.length() : 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t len = 0;
        for (int64_t i = 0; i < raw.length(); ++i) {
          char c = raw.ptr()[i];
          if ('\\' == c && i + 1 < raw.length()) {
            ++i;
            switch (raw.ptr()[i]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
              // only the ascii range is decoded, the rest is kept verbatim
              if (i + 4 < raw.length()) {
                char hex[5] = {raw.ptr()[i + 1], raw.ptr()[i + 2], raw.ptr()[i + 3], raw.ptr()[i + 4], 0};
                const long code = strtol(hex, NULL, 16);
                i += 4;
                if (code < 128) {
                  c = (char)code;
                } else {
                  buf[len++] = '?';
                  continue;
                }
              } else {
                c = raw.ptr()[i];
              }
              break;
            }
            default: c = raw.ptr()[i]; break;
            }
          }
          buf[len++] = c;
        }
        out.assign_ptr(buf, len);
      }
    }
    return ret;
  }

  int parse_value(ObIAllocator &allocator, ObString &out, bool &is_null)
  {
    int ret = OB_SUCCESS;
    is_null = false;
    if (pos_ >= len_) {
      ret = OB_ERR_UNEXPECTED;
    } else if ('"' == text_[pos_]) {
      // a string value
      ret = parse_string(allocator, out);
    } else if ('{' == text_[pos_] || '[' == text_[pos_]) {
      // nested structures are kept as their raw text
      const int64_t start = pos_;
      const char open = text_[pos_];
      const char close = ('{' == open) ? '}' : ']';
      int depth = 0;
      bool in_string = false;
      bool escaped = false;
      while (pos_ < len_) {
        const char c = text_[pos_++];
        if (in_string) {
          if (escaped) {
            escaped = false;
          } else if ('\\' == c) {
            escaped = true;
          } else if ('"' == c) {
            in_string = false;
          }
        } else if ('"' == c) {
          in_string = true;
        } else if (c == open) {
          ++depth;
        } else if (c == close) {
          --depth;
          if (0 == depth) {
            break;
          }
        }
      }
      out = ObString(pos_ - start, text_ + start);
    } else {
      const int64_t start = pos_;
      while (pos_ < len_ && ',' != text_[pos_] && '}' != text_[pos_] && ' ' != text_[pos_] && '\t' != text_[pos_]
             && '\r' != text_[pos_] && '\n' != text_[pos_]) {
        ++pos_;
      }
      out = ObString(pos_ - start, text_ + start);
      if (str_equal_ci(out.ptr(), out.length(), "null")) {
        is_null = true;
      }
    }
    return ret;
  }

private:
  const char *text_;
  int64_t len_;
  int64_t pos_;
};
} // namespace

// ---------------------------------------------------------------------------
// ObFileTableReader
// ---------------------------------------------------------------------------

ObFileTableReader::ObFileTableReader()
    : fp_(NULL),
      allocator_(NULL),
      options_(),
      buf_(NULL),
      buf_size_(0),
      buf_pos_(0),
      buf_len_(0),
      eof_(false),
      row_alloc_(),
      entries_(common::ObArray<ObFileTableEntry>()),
      header_names_(common::ObArray<ObString>()),
      column_names_(common::ObArray<ObString>()),
      json_keys_(common::ObArray<ObString>()),
      json_values_(common::ObArray<ObString>()),
      json_nulls_(common::ObArray<bool>()),
      column_count_(0),
      glob_files_(common::ObArray<ObString>()),
      glob_pos_(0),
      parquet_reader_(),
      parquet_row_(-1),
      describe_values_(common::ObArray<ObString>()),
      describe_row_count_(0),
      describe_pos_(0),
      path_copy_(NULL),
      file_name_()
{
}

ObFileTableReader::~ObFileTableReader()
{
  close();
  if (NULL != buf_ && NULL != allocator_) {
    allocator_->free(buf_);
    buf_ = NULL;
  }
}

// closes the file but keeps the read buffer, so that a rescan can reuse it
void ObFileTableReader::close()
{
  if (NULL != fp_) {
    fclose(fp_);
    fp_ = NULL;
  }
  buf_pos_ = 0;
  buf_len_ = 0;
  eof_ = false;
  glob_pos_ = 0;
  parquet_reader_.close();
  parquet_row_ = -1;
  file_path_ = ObString();
  file_size_ = 0;
  file_mtime_ = 0;
  path_copy_ = NULL;
  file_name_ = ObString();
}

int ObFileTableReader::open(ObIAllocator &allocator,
                            const ObString &path,
                            const ObFileTableOptions &options,
                            const common::ObIArray<ObString> *column_names)
{
  int ret = OB_SUCCESS;
  allocator_ = &allocator;
  options_ = options;
  entries_.reset();
  header_names_.reset();
  column_names_.reset();
  glob_files_.reset();
  glob_pos_ = 0;
  column_count_ = 0;
  describe_row_count_ = 0;
  describe_pos_ = 0;
  parquet_row_ = -1;
  if (OB_UNLIKELY(path.empty())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(OB_ISNULL(path_copy_ = static_cast<char *>(allocator.alloc(path.length() + 1))))) {
    // the path is copied, the file system needs a terminated name
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    memcpy(path_copy_, path.ptr(), path.length());
    path_copy_[path.length()] = '\0';
    file_name_ = ObString(path.length(), path_copy_);
  }
  if (OB_SUCC(ret) && OB_UNLIKELY(options_.describe_)) {
    // the rows of the table are the description of the columns of the file
    if (OB_FAIL(ret = build_describe_rows(allocator, file_name_, options_))) {
      LOG_WARN("cannot describe the file", K(ret), K(path));
    } else {
      column_count_ = ObFileTableSource::DESCRIBE_COLUMN_COUNT;
    }
  } else if (OB_FILE_FUNC_GLOB == options_.func_type_) {
    // a glob pattern does not open a single file, it expands to a list of files
    if (OB_FAIL(ret = ObFileTableSource::list_glob_files(allocator, file_name_, glob_files_))) {
      LOG_WARN("cannot expand the glob pattern", K(ret), K(file_name_));
    } else {
      column_count_ = 1;
    }
  } else if (OB_FILE_FUNC_READ_PARQUET == options_.func_type_) {
    // the schema of a parquet file is part of the file itself
    if (OB_FAIL(ret = parquet_reader_.open(allocator, file_name_))) {
      LOG_WARN("cannot open the parquet file", K(ret), K(file_name_));
    } else {
      column_count_ = parquet_reader_.get_column_count();
    }
  } else if (OB_ISNULL(fp_ = fopen(file_name_.ptr(), "rb"))) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("cannot open the file", K(ret), K(file_name_));
  } else if (OB_ISNULL(buf_) && OB_ISNULL(buf_ = static_cast<char *>(allocator.alloc(FILE_TABLE_READ_BUF_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ret = record_file_stamp(allocator, file_name_))) {
    // the stamp of the file is needed to detect a file that changes
    LOG_WARN("cannot remember the state of the file", K(ret), K(file_name_));
  } else {
    buf_size_ = FILE_TABLE_READ_BUF_SIZE;
    buf_pos_ = 0;
    buf_len_ = 0;
    eof_ = false;
  }
  if (OB_SUCC(ret) && NULL != column_names) {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_names->count(); ++i) {
      if (OB_FAIL(column_names_.push_back(column_names->at(i)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    if (OB_FILE_FUNC_GLOB != options_.func_type_
        && OB_FILE_FUNC_READ_PARQUET != options_.func_type_) {
      column_count_ = column_names->count();
    }
  }
  if (OB_SUCC(ret) && OB_FILE_FUNC_READ_CSV == options_.func_type_ && options_.header_) {
    // the first record of a CSV file holds the column names
    if (OB_SUCC(ret = read_csv_record())) {
      for (int64_t i = 0; OB_SUCC(ret) && i < entries_.count(); ++i) {
        if (OB_FAIL(header_names_.push_back(entries_.at(i).value_))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
      }
      column_count_ = header_names_.count();
    } else if (OB_ITER_END == ret) {
      // an empty file has no columns at all
      ret = OB_SUCCESS;
      column_count_ = 0;
    }
  }
  return ret;
}

int ObFileTableReader::fill_buffer()
{
  int ret = OB_SUCCESS;
  if (eof_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(ret = check_file_unchanged())) {
    // the file has been changed while it is read, the rows that have been
    // read cannot be trusted any more
    LOG_WARN("the file changed during the scan", K(ret), K(file_path_));
  } else {
    const size_t read_bytes = fread(buf_, 1, (size_t)buf_size_, fp_);
    if (0 == read_bytes) {
      eof_ = true;
      buf_pos_ = 0;
      buf_len_ = 0;
      ret = OB_ITER_END;
    } else {
      buf_pos_ = 0;
      buf_len_ = (int64_t)read_bytes;
    }
  }
  return ret;
}

int ObFileTableReader::record_file_stamp(ObIAllocator &allocator, const ObString &path)
{
  int ret = OB_SUCCESS;
  int64_t size = 0;
  int64_t mtime = 0;
  if (OB_FAIL(ret = get_file_stamp(path.ptr(), size, mtime))) {
    // the file cannot be read, the scan of the file fails
  } else {
    char *copy = static_cast<char *>(allocator.alloc(path.length() + 1));
    if (OB_ISNULL(copy)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      memcpy(copy, path.ptr(), path.length());
      copy[path.length()] = '\0';
      file_path_ = ObString(path.length(), copy);
      file_size_ = size;
      file_mtime_ = mtime;
    }
  }
  return ret;
}

int ObFileTableReader::check_file_unchanged()
{
  int ret = OB_SUCCESS;
  int64_t size = 0;
  int64_t mtime = 0;
  if (file_path_.empty()) {
    // the reader does not read a single file, there is nothing to check
  } else if (OB_FAIL(ret = get_file_stamp(file_path_.ptr(), size, mtime))) {
    // the file is gone while it is read
  } else if (size != file_size_ || mtime != file_mtime_) {
    ret = OB_CHECKSUM_ERROR;
  }
  return ret;
}

int ObFileTableReader::append_entry(const ObString &name, const char *data, int64_t len, bool is_null)
{
  int ret = OB_SUCCESS;
  ObFileTableEntry entry;
  entry.name_ = name;
  entry.is_null_ = is_null;
  if (!is_null
      && OB_FILE_FUNC_READ_CSV == options_.func_type_
      && options_.nullstr_.length() > 0
      && options_.nullstr_.length() == len
      && 0 == memcmp(options_.nullstr_.ptr(), data, (size_t)len)) {
    // the value is the text that stands for a null value
    entry.is_null_ = true;
  }
  if (!entry.is_null_) {
    char *buf = static_cast<char *>(row_alloc_.alloc(len > 0 ? len : 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len > 0) {
        memcpy(buf, data, (size_t)len);
      }
      entry.value_.assign_ptr(buf, len);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(entries_.push_back(entry))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

// parses one RFC 4180 style record out of the read buffer
int ObFileTableReader::read_csv_record()
{
  int ret = OB_SUCCESS;
  entries_.reset();
  row_alloc_.reuse();

  char *field_buf = NULL;
  int64_t field_len = 0;
  int64_t field_cap = 0;
  bool in_quotes = false;
  bool field_quoted = false;
  bool field_started = false;
  bool any_input = false;
  bool record_done = false;
  const char delimiter = (options_.delimiter_.length() > 0) ? options_.delimiter_.ptr()[0] : ',';

  field_cap = 256;
  field_buf = static_cast<char *>(row_alloc_.alloc(field_cap));
  if (OB_ISNULL(field_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }

  while (OB_SUCC(ret) && !record_done) {
    if (buf_pos_ >= buf_len_) {
      const int fill_ret = fill_buffer();
      if (OB_FAIL(fill_ret)) {
        if (OB_ITER_END == fill_ret) {
          if (!any_input) {
            ret = OB_ITER_END;
            break;
          }
          // the last record of the file has no trailing newline
          const bool is_null = (!field_quoted && 0 == field_len);
          ret = append_entry(ObString(), field_buf, field_len, is_null);
          record_done = true;
        } else {
          ret = fill_ret;
        }
        break;
      }
      continue;
    }
    const char c = buf_[buf_pos_++];
    any_input = true;
    if (in_quotes) {
      if ('"' == c) {
        if (buf_pos_ >= buf_len_) {
          const int fill_ret = fill_buffer();
          if (OB_FAIL(fill_ret)) {
            // the file ends right after the closing quote
            in_quotes = false;
            ret = append_entry(ObString(), field_buf, field_len, false);
            record_done = true;
            break;
          }
        }
        if ('"' == buf_[buf_pos_]) {
          // an escaped quote
          ++buf_pos_;
          if (field_len + 1 > field_cap) {
            char *new_buf = static_cast<char *>(row_alloc_.alloc(field_cap * 2));
            if (OB_ISNULL(new_buf)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
              break;
            }
            memcpy(new_buf, field_buf, (size_t)field_len);
            field_buf = new_buf;
            field_cap *= 2;
          }
          field_buf[field_len++] = '"';
        } else {
          in_quotes = false;
        }
      } else {
        if (field_len + 1 > field_cap) {
          char *new_buf = static_cast<char *>(row_alloc_.alloc(field_cap * 2));
          if (OB_ISNULL(new_buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            break;
          }
          memcpy(new_buf, field_buf, (size_t)field_len);
          field_buf = new_buf;
          field_cap *= 2;
        }
        field_buf[field_len++] = c;
      }
    } else if ('"' == c && !field_started) {
      in_quotes = true;
      field_quoted = true;
      field_started = true;
    } else if (delimiter == c) {
      const bool is_null = (!field_quoted && 0 == field_len);
      if (OB_FAIL(append_entry(ObString(), field_buf, field_len, is_null))) {
        break;
      }
      field_len = 0;
      field_quoted = false;
      field_started = false;
    } else if ('\n' == c) {
      const bool is_null = (!field_quoted && 0 == field_len);
      if (OB_FAIL(append_entry(ObString(), field_buf, field_len, is_null))) {
        break;
      }
      record_done = true;
    } else if ('\r' != c) {
      if (field_len + 1 > field_cap) {
        char *new_buf = static_cast<char *>(row_alloc_.alloc(field_cap * 2));
        if (OB_ISNULL(new_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }
        memcpy(new_buf, field_buf, (size_t)field_len);
        field_buf = new_buf;
        field_cap *= 2;
      }
      field_buf[field_len++] = c;
      field_started = true;
    }
  }
  return ret;
}

// reads one line and parses it as a flat JSON object
int ObFileTableReader::read_jsonl_record()
{
  int ret = OB_SUCCESS;
  entries_.reset();
  row_alloc_.reuse();

  char *line = NULL;
  int64_t line_len = 0;
  int64_t line_cap = 1024;
  bool any_input = false;
  line = static_cast<char *>(row_alloc_.alloc(line_cap));
  if (OB_ISNULL(line)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  while (OB_SUCC(ret)) {
    if (buf_pos_ >= buf_len_) {
      const int fill_ret = fill_buffer();
      if (OB_FAIL(fill_ret)) {
        if (OB_ITER_END == fill_ret) {
          if (!any_input) {
            ret = OB_ITER_END;
            break;
          }
        } else {
          ret = fill_ret;
          break;
        }
        break;
      }
      continue;
    }
    const char c = buf_[buf_pos_++];
    any_input = true;
    if ('\n' == c) {
      break;
    }
    if ('\r' == c) {
      continue;
    }
    if (line_len + 1 > line_cap) {
      char *new_buf = static_cast<char *>(row_alloc_.alloc(line_cap * 2));
      if (OB_ISNULL(new_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      memcpy(new_buf, line, (size_t)line_len);
      line = new_buf;
      line_cap *= 2;
    }
    line[line_len++] = c;
  }

  if (OB_SUCC(ret) && line_len > 0) {
    ObFlatJsonParser parser;
    json_keys_.reset();
    json_values_.reset();
    json_nulls_.reset();
    parser.reset(line, line_len);
    if (OB_FAIL(parser.parse_object(row_alloc_, json_keys_, json_values_, json_nulls_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cannot parse the json line", K(ret), K(line_len));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < json_keys_.count(); ++i) {
        if (OB_FAIL(append_entry(json_keys_.at(i), json_values_.at(i).ptr(), json_values_.at(i).length(),
                                json_nulls_.at(i)))) {
          break;
        }
      }
    }
  }
  return ret;
}

int ObFileTableReader::next_row()
{
  int ret = OB_SUCCESS;
  if (options_.describe_) {
    ret = next_describe_record();
  } else if (OB_FILE_FUNC_READ_JSON == options_.func_type_) {
    ret = read_jsonl_record();
  } else if (OB_FILE_FUNC_GLOB == options_.func_type_) {
    ret = next_glob_record();
  } else if (OB_FILE_FUNC_READ_PARQUET == options_.func_type_) {
    // the rows of a parquet file are produced by the reader of the file, the
    // reader keeps the cursor of every column of the file
    ++parquet_row_;
    if (parquet_row_ >= parquet_reader_.get_row_count()) {
      ret = OB_ITER_END;
    }
  } else {
    ret = read_csv_record();
  }
  return ret;
}

// one row of the describe mode holds the description of one column of the file
int ObFileTableReader::next_describe_record()
{
  int ret = OB_SUCCESS;
  entries_.reset();
  row_alloc_.reuse();
  if (describe_pos_ >= describe_row_count_) {
    ret = OB_ITER_END;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ObFileTableSource::DESCRIBE_COLUMN_COUNT; ++i) {
      const int64_t index = describe_pos_ * ObFileTableSource::DESCRIBE_COLUMN_COUNT + i;
      if (index >= describe_values_.count()) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const ObString &value = describe_values_.at(index);
      if (OB_FAIL(append_entry(ObString(), value.ptr(), value.length(), false))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    if (OB_SUCC(ret)) {
      ++describe_pos_;
    }
  }
  return ret;
}

int ObFileTableReader::append_describe_value(ObIAllocator &allocator, const ObString &value)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(value.length() > 0 ? value.length() : 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (value.length() > 0) {
      memcpy(buf, value.ptr(), (size_t)value.length());
    }
    if (OB_FAIL(describe_values_.push_back(ObString(value.length(), buf)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  return ret;
}

// detects the schema of the file and builds the description of its columns
int ObFileTableReader::build_describe_rows(ObIAllocator &allocator,
                                            const ObString &path,
                                            const ObFileTableOptions &options)
{
  int ret = OB_SUCCESS;
  common::ObArray<ObFileTableColumn> columns;
  // the schema of the file is detected by reading the file, the reader must not
  // build the description of the columns again
  ObFileTableOptions infer_options = options;
  infer_options.describe_ = false;
  if (OB_FILE_FUNC_READ_JSON == options.func_type_) {
    ret = ObFileTableSource::infer_json(allocator, path, infer_options, columns);
  } else if (OB_FILE_FUNC_READ_CSV == options.func_type_) {
    ret = ObFileTableSource::infer_csv(allocator, path, infer_options, columns);
  } else if (OB_FILE_FUNC_READ_PARQUET == options.func_type_) {
    ret = ObFileTableSource::infer_parquet(allocator, path, infer_options, columns);
  } else if (OB_FILE_FUNC_GLOB == options.func_type_) {
    // a glob pattern holds one column, the path of the matched file
    ret = ObFileTableSource::infer_glob(allocator, path, columns);
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      const ObFileTableColumn &column = columns.at(i);
      const char *type_name = ObFileTableSource::col_type_name(column.type_);
      if (OB_FAIL(ret = append_describe_value(allocator, column.name_))) {
      } else if (OB_FAIL(ret = append_describe_value(allocator, ObString(type_name)))) {
      } else if (OB_FAIL(ret = append_describe_value(allocator, ObString(column.nullable_ ? "YES" : "NO")))) {
      } else if (OB_FAIL(ret = append_describe_value(allocator, ObString("NULL")))) {
      } else if (OB_FAIL(ret = append_describe_value(allocator, ObString("NULL")))) {
      } else if (OB_FAIL(ret = append_describe_value(allocator, ObString("NULL")))) {
      } else {
        ++describe_row_count_;
      }
    }
  }
  return ret;
}

// a glob pattern expands to one row per matched file, the only column of the
// row holds the path of the file
int ObFileTableReader::next_glob_record()
{
  int ret = OB_SUCCESS;
  entries_.reset();
  row_alloc_.reuse();
  if (glob_pos_ >= glob_files_.count()) {
    ret = OB_ITER_END;
  } else {
    const ObString &file = glob_files_.at(glob_pos_);
    ++glob_pos_;
    if (OB_FAIL(append_entry(ObString(), file.ptr(), file.length(), false))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  return ret;
}

int ObFileTableReader::get_value(int64_t col_index, ObString &value, bool &is_null) const
{
  int ret = OB_SUCCESS;
  if (col_index < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FILE_FUNC_GLOB == options_.func_type_) {
    // the only column of a glob pattern holds the path of the matched file
    if (0 != col_index || entries_.empty()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      value = entries_.at(0).value_;
      is_null = entries_.at(0).is_null_;
    }
  } else if (OB_FILE_FUNC_READ_JSON == options_.func_type_) {
    // the columns of a json file are looked up by name
    if (col_index >= column_names_.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ObString &name = column_names_.at(col_index);
      bool found = false;
      for (int64_t i = 0; i < entries_.count(); ++i) {
        if (entries_.at(i).name_.case_compare(name) == 0) {
          value = entries_.at(i).value_;
          is_null = entries_.at(i).is_null_;
          found = true;
          break;
        }
      }
      if (!found) {
        // a missing field is a null value
        value = ObString();
        is_null = true;
      }
    }
  } else if (OB_FILE_FUNC_READ_PARQUET == options_.func_type_) {
    // the columns of a parquet file are read from the file itself
    ret = parquet_reader_.get_value(col_index, parquet_row_, value, is_null);
  } else {
    if (col_index >= entries_.count()) {
      // a missing trailing field is a null value
      value = ObString();
      is_null = true;
    } else {
      value = entries_.at(col_index).value_;
      is_null = entries_.at(col_index).is_null_;
    }
  }
  return ret;
}

int ObFileTableReader::get_entry(int64_t index, ObString &name, ObString &value, bool &is_null) const
{
  int ret = OB_SUCCESS;
  if (index < 0 || index >= entries_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    name = entries_.at(index).name_;
    value = entries_.at(index).value_;
    is_null = entries_.at(index).is_null_;
  }
  return ret;
}

int ObFileTableReader::get_column_name(int64_t col_index, ObString &name) const
{
  int ret = OB_SUCCESS;
  if (col_index < 0 || col_index >= header_names_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    name = header_names_.at(col_index);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// ObFileTableSource
// ---------------------------------------------------------------------------

const char *ObFileTableSource::col_type_name(ObFileTableColType type)
{
  const char *name = "VARCHAR";
  switch (type) {
  case OB_FILE_COL_BOOLEAN: name = "BOOLEAN"; break;
  case OB_FILE_COL_BIGINT: name = "BIGINT"; break;
  case OB_FILE_COL_DOUBLE: name = "DOUBLE"; break;
  case OB_FILE_COL_DATE: name = "DATE"; break;
  case OB_FILE_COL_DATETIME: name = "DATETIME"; break;
  case OB_FILE_COL_VARCHAR: name = "VARCHAR"; break;
  default: break;
  }
  return name;
}

int ObFileTableSource::to_data_type(const ObFileTableColumn &column, ObDataType &data_type)
{
  int ret = OB_SUCCESS;
  data_type.set_collation_level(CS_LEVEL_IMPLICIT);
  switch (column.type_) {
  case OB_FILE_COL_BOOLEAN: {
    // a boolean is stored as a tiny integer, 1 means true and 0 means false
    data_type.set_obj_type(ObTinyIntType);
    data_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObInt32Type]);
    break;
  }
  case OB_FILE_COL_BIGINT: {
    data_type.set_obj_type(ObIntType);
    data_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType]);
    break;
  }
  case OB_FILE_COL_DOUBLE: {
    data_type.set_obj_type(ObDoubleType);
    data_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObDoubleType]);
    break;
  }
  case OB_FILE_COL_DATE: {
    data_type.set_obj_type(ObDateType);
    data_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObDateType]);
    break;
  }
  case OB_FILE_COL_DATETIME: {
    // the number of the fractional digits of the values of the column decides
    // the scale of the column, the default scale of a datetime would print
    // six fractional digits
    data_type.set_obj_type(ObDateTimeType);
    ObAccuracy accuracy = ObAccuracy::DDL_DEFAULT_ACCURACY[ObDateTimeType];
    accuracy.set_scale(column.scale_);
    data_type.set_accuracy(accuracy);
    break;
  }
  default: {
    data_type.set_obj_type(ObVarcharType);
    data_type.set_accuracy(ObAccuracy::MAX_ACCURACY[ObVarcharType]);
    break;
  }
  }
  return ret;
}

// copies a string into the allocator of the table function. The buffer of the
// reader is reused by the next record, a name that outlives the record has to
// be copied.
static int copy_file_string(ObIAllocator &allocator, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(src.length() > 0 ? src.length() : 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (src.length() > 0 && OB_NOT_NULL(src.ptr())) {
      memcpy(buf, src.ptr(), static_cast<size_t>(src.length()));
    }
    dst.assign_ptr(buf, static_cast<int32_t>(src.length()));
  }
  return ret;
}

int ObFileTableSource::make_unique_names(ObIAllocator &allocator,
                                         common::ObArray<ObString> &names,
                                         common::ObIArray<ObFileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  common::ObArray<ObString> original_names;
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    if (OB_FAIL(original_names.push_back(names.at(i)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    ObString &name = names.at(i);
    int64_t occurrence = 1;
    bool duplicated = false;
    // the comparison always uses the original names, a renamed column must
    // not turn the next column into a duplicate
    for (int64_t j = 0; j < i; ++j) {
      if (original_names.at(j).case_compare(original_names.at(i)) == 0) {
        duplicated = true;
        ++occurrence;
      }
    }
    if (duplicated) {
      // "x" becomes "x__2", "x__3" and so on. The new name is built in the
      // allocator of the table function, a buffer on the stack would be far
      // too large, the stack of a worker thread is only a few hundred kilobytes.
      const int64_t new_len = name.length() + 32;
      char *new_name = static_cast<char *>(allocator.alloc(new_len + 1));
      if (OB_ISNULL(new_name)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (0 == name.length()) {
        const int written = snprintf(new_name, new_len + 1, "__%ld", occurrence);
        if (written < 0 || written > new_len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          name.assign_ptr(new_name, written);
        }
      } else {
        const int written = snprintf(new_name, new_len + 1, "%.*s__%ld",
                                     static_cast<int>(name.length()), name.ptr(), occurrence);
        if (written < 0 || written > new_len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          name.assign_ptr(new_name, written);
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (i < names.count()) {
      columns.at(i).name_ = names.at(i);
    }
  }
  return ret;
}

int ObFileTableSource::infer_csv(ObIAllocator &allocator,
                                 const ObString &path,
                                 const ObFileTableOptions &options,
                                 common::ObIArray<ObFileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  ObFileTableReader reader;
  if (OB_FAIL(ret = reader.open(allocator, path, options, NULL))) {
  } else if (0 == reader.column_count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the csv file has no header", K(ret), K(path));
  } else {
    const int64_t col_count = reader.column_count();
    common::ObArray<ObFileTableTypeDetector> detectors;
    common::ObArray<bool> nullables;
    common::ObArray<ObString> names;
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      ObString name;
      ObString name_copy;
      if (OB_FAIL(ret = reader.get_column_name(i, name))) {
        break;
      } else if (OB_FAIL(ret = copy_file_string(allocator, name, name_copy))) {
        break;
      } else if (OB_FAIL(ret = names.push_back(name_copy))) {
        break;
      } else if (OB_FAIL(ret = detectors.push_back(ObFileTableTypeDetector()))) {
        break;
      } else if (OB_FAIL(ret = nullables.push_back(false))) {
        break;
      }
    }
    int64_t row_count = 0;
    while (OB_SUCC(ret) && (options.sample_size_ < 0 || row_count < options.sample_size_)) {
      const int next_ret = reader.next_row();
      if (OB_ITER_END == next_ret) {
        break;
      } else if (OB_FAIL(next_ret)) {
        ret = next_ret;
        break;
      }
      ++row_count;
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
        ObString value;
        bool is_null = false;
        if (OB_FAIL(ret = reader.get_value(i, value, is_null))) {
          break;
        } else if (is_null) {
          nullables.at(i) = true;
        } else {
          detectors.at(i).feed(value.ptr(), value.length());
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      ObFileTableColumn column;
      column.name_ = names.at(i);
      column.type_ = detectors.at(i).is_known() ? detectors.at(i).get_type() : OB_FILE_COL_VARCHAR;
      column.nullable_ = nullables.at(i);
      column.scale_ = detectors.at(i).get_scale();
      if (OB_FAIL(ret = columns.push_back(column))) {
        break;
      }
    }
    // the column names of the header row have to be unique
    if (OB_SUCC(ret)) {
      ret = make_unique_names(allocator, names, columns);
    }
  }
  return ret;
}

int ObFileTableSource::infer_json(ObIAllocator &allocator,
                                  const ObString &path,
                                  const ObFileTableOptions &options,
                                  common::ObIArray<ObFileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  ObFileTableReader reader;
  if (OB_FAIL(ret = reader.open(allocator, path, options, NULL))) {
  } else {
    common::ObArray<ObString> names;
    common::ObArray<ObFileTableTypeDetector> detectors;
    common::ObArray<bool> nullables;
    int64_t row_count = 0;
    while (OB_SUCC(ret) && (options.sample_size_ < 0 || row_count < options.sample_size_)) {
      const int next_ret = reader.next_row();
      if (OB_ITER_END == next_ret) {
        break;
      } else if (OB_FAIL(next_ret)) {
        ret = next_ret;
        break;
      }
      ++row_count;
      // the schema of a json file is the union of the keys of all its rows
      const int64_t entry_count = reader.get_entry_count();
      for (int64_t i = 0; OB_SUCC(ret) && i < entry_count; ++i) {
        ObString key;
        ObString value;
        bool is_null = false;
        if (OB_FAIL(ret = reader.get_entry(i, key, value, is_null))) {
          break;
        }
        int64_t col_index = -1;
        for (int64_t j = 0; j < names.count(); ++j) {
          if (names.at(j).case_compare(key) == 0) {
            col_index = j;
            break;
          }
        }
        if (col_index < 0) {
          ObString key_copy;
          if (OB_FAIL(ret = copy_file_string(allocator, key, key_copy))) {
            break;
          } else if (OB_FAIL(ret = names.push_back(key_copy))) {
            break;
          } else if (OB_FAIL(ret = detectors.push_back(ObFileTableTypeDetector()))) {
            break;
          } else if (OB_FAIL(ret = nullables.push_back(false))) {
            break;
          }
          col_index = names.count() - 1;
        }
        if (is_null) {
          nullables.at(col_index) = true;
        } else {
          detectors.at(col_index).feed(value.ptr(), value.length());
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
      ObFileTableColumn column;
      column.name_ = names.at(i);
      column.type_ = detectors.at(i).is_known() ? detectors.at(i).get_type() : OB_FILE_COL_VARCHAR;
      column.nullable_ = nullables.at(i);
      column.scale_ = detectors.at(i).get_scale();
      if (OB_FAIL(ret = columns.push_back(column))) {
        break;
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// parquet support
// ---------------------------------------------------------------------------

// maps the physical type of a column of a parquet file into the type that the
// table function uses
static ObFileTableColType parquet_col_type(int32_t physical_type, int32_t converted_type)
{
  switch (converted_type) {
  case OB_PARQUET_CONVERTED_DATE:
    return OB_FILE_COL_DATE;
  case OB_PARQUET_CONVERTED_TIMESTAMP_MILLIS:
  case OB_PARQUET_CONVERTED_TIMESTAMP_MICROS:
    return OB_FILE_COL_DATETIME;
  default:
    break;
  }
  switch (physical_type) {
  case OB_PARQUET_TYPE_BOOLEAN:
    return OB_FILE_COL_BOOLEAN;
  case OB_PARQUET_TYPE_INT32:
  case OB_PARQUET_TYPE_INT64:
  case OB_PARQUET_TYPE_INT96:
    return OB_FILE_COL_BIGINT;
  case OB_PARQUET_TYPE_FLOAT:
  case OB_PARQUET_TYPE_DOUBLE:
    return OB_FILE_COL_DOUBLE;
  default:
    break;
  }
  return OB_FILE_COL_VARCHAR;
}

int ObFileTableSource::infer_parquet(ObIAllocator &allocator,
                                     const ObString &path,
                                     const ObFileTableOptions &options,
                                     common::ObIArray<ObFileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  ObParquetReader reader;
  if (OB_FAIL(ret = reader.open(allocator, path))) {
    LOG_WARN("cannot read the parquet file", K(ret), K(path));
  } else {
    const int64_t col_count = reader.get_column_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      ObFileTableColumn column;
      // the name of the column is part of the schema of the file, it is
      // already held by the allocator of the table function
      column.name_ = reader.get_column_name(i);
      column.type_ = parquet_col_type(reader.get_column_type(i), reader.get_column_converted_type(i));
      // a column of a parquet file may hold null values
      column.nullable_ = true;
      column.scale_ = 0;
      if (OB_FAIL(ret = columns.push_back(column))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
    }
  }
  UNUSED(options);
  return ret;
}

// ---------------------------------------------------------------------------
// glob support
// ---------------------------------------------------------------------------
// matches a name against a pattern that may hold the wildcards '*' and '?'
static bool wildcard_match(const char *pattern, int64_t pattern_len,
                            const char *name, int64_t name_len)
{
  int64_t p = 0;
  int64_t n = 0;
  int64_t star_p = -1;
  int64_t star_n = -1;
  while (n < name_len) {
    if (p < pattern_len && ('?' == pattern[p] || pattern[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < pattern_len && '*' == pattern[p]) {
      star_p = p++;
      star_n = n;
    } else if (star_p >= 0) {
      p = star_p + 1;
      n = ++star_n;
    } else {
      return false;
    }
  }
  while (p < pattern_len && '*' == pattern[p]) {
    ++p;
  }
  return p == pattern_len;
}

// splits a glob pattern into its directory part and its name pattern
static void split_glob_pattern(const ObString &pattern, ObString &dir, ObString &name_pattern)
{
  int64_t sep_pos = -1;
  for (int64_t i = pattern.length() - 1; i >= 0; --i) {
    const char c = pattern.ptr()[i];
    if ('/' == c || '\\' == c) {
      sep_pos = i;
      break;
    }
  }
  if (sep_pos < 0) {
    dir = ObString(".");
    name_pattern = pattern;
  } else {
    dir = ObString(sep_pos, pattern.ptr());
    name_pattern = ObString(pattern.length() - sep_pos - 1, pattern.ptr() + sep_pos + 1);
  }
}

int ObFileTableSource::list_glob_files(ObIAllocator &allocator,
                                        const ObString &pattern,
                                        common::ObArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  ObString dir;
  ObString name_pattern;
  common::ObArray<ObString> names;
  split_glob_pattern(pattern, dir, name_pattern);

#ifdef _WIN32
  char dir_buf[MAX_PATH];
  const int64_t dir_len = dir.length() < MAX_PATH - 1 ? dir.length() : MAX_PATH - 1;
  memcpy(dir_buf, dir.ptr(), (size_t)dir_len);
  dir_buf[dir_len] = '\0';
  WIN32_FIND_DATAA find_data;
  memset(&find_data, 0, sizeof(find_data));
  const std::string search = std::string(dir_buf) + "\\*";
  HANDLE handle = FindFirstFileA(search.c_str(), &find_data);
  if (INVALID_HANDLE_VALUE == handle) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("cannot list the directory", K(ret), K(dir));
  } else {
    do {
      if (0 == strcmp(".", find_data.cFileName) || 0 == strcmp("..", find_data.cFileName)) {
        continue;
      }
      // the buffer of the search is reused by the next call, the name has to
      // be copied before it is overwritten
      const int64_t name_len = static_cast<int64_t>(strlen(find_data.cFileName));
      char *name_copy = static_cast<char *>(allocator.alloc(name_len + 1));
      if (OB_ISNULL(name_copy)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      memcpy(name_copy, find_data.cFileName, static_cast<size_t>(name_len));
      name_copy[name_len] = '\0';
      if (OB_FAIL(names.push_back(ObString(name_len, name_copy)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
    } while (FindNextFileA(handle, &find_data));
    FindClose(handle);
  }
#else
  // the directory part of the pattern is a slice of the path of the statement,
  // it is not terminated, the directory has to be copied
  const std::string dir_name(dir.ptr(), dir.length());
  DIR *handle = opendir(dir_name.c_str());
  if (NULL == handle) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("cannot list the directory", K(ret), K(dir));
  } else {
    struct dirent *entry = NULL;
    while (NULL != (entry = readdir(handle))) {
      if (0 == strcmp(".", entry->d_name) || 0 == strcmp("..", entry->d_name)) {
        continue;
      }
      // the buffer of the directory entry is reused by the next call, the
      // name has to be copied before it is overwritten
      const int64_t name_len = static_cast<int64_t>(strlen(entry->d_name));
      char *name_copy = static_cast<char *>(allocator.alloc(name_len + 1));
      if (OB_ISNULL(name_copy)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      memcpy(name_copy, entry->d_name, static_cast<size_t>(name_len));
      name_copy[name_len] = '\0';
      if (OB_FAIL(names.push_back(ObString(name_len, name_copy)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
    }
    closedir(handle);
  }
#endif

  if (OB_SUCC(ret)) {
    // the matched files are returned in a stable order
    std::sort(names.begin(), names.end());
    for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
      const ObString &name = names.at(i);
      if (!wildcard_match(name_pattern.ptr(), name_pattern.length(), name.ptr(), name.length())) {
        continue;
      }
      const int64_t full_len = dir.length() + 1 + name.length();
      char *full = static_cast<char *>(allocator.alloc(full_len + 1));
      if (OB_ISNULL(full)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
      memcpy(full, dir.ptr(), (size_t)dir.length());
      full[dir.length()] = '/';
      memcpy(full + dir.length() + 1, name.ptr(), (size_t)name.length());
      full[full_len] = '\0';
      if (OB_FAIL(files.push_back(ObString(full_len, full)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        break;
      }
    }
  }
  return ret;
}

int ObFileTableSource::infer_glob(ObIAllocator &allocator,
                                   const ObString &pattern,
                                   common::ObIArray<ObFileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  common::ObArray<ObString> files;
  if (OB_FAIL(ret = list_glob_files(allocator, pattern, files))) {
  } else {
    ObFileTableColumn column;
    column.name_ = ObString("file");
    column.type_ = OB_FILE_COL_VARCHAR;
    column.nullable_ = false;
    if (OB_FAIL(ret = columns.push_back(column))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  return ret;
}

const char *ObFileTableSource::describe_column_name(int64_t index)
{
  static const char *const names[ObFileTableSource::DESCRIBE_COLUMN_COUNT] = {
    "column_name", "column_type", "null", "key", "default", "extra"
  };
  const char *name = "column_name";
  if (index >= 0 && index < ObFileTableSource::DESCRIBE_COLUMN_COUNT) {
    name = names[index];
  }
  return name;
}

const char *ObFileTableSource::func_type_name(ObFileTableFuncType type)
{
  const char *name = "read_csv";
  switch (type) {
  case OB_FILE_FUNC_READ_CSV: name = "read_csv"; break;
  case OB_FILE_FUNC_READ_JSON: name = "read_json"; break;
  case OB_FILE_FUNC_READ_PARQUET: name = "read_parquet"; break;
  case OB_FILE_FUNC_GLOB: name = "glob"; break;
  default: break;
  }
  return name;
}

// the fields of the encoded spec of a file table function are separated by
// this character, it never shows up in a path
static const char FILE_TABLE_SPEC_SEPARATOR = '\x01';

int ObFileTableSource::encode_file_table_spec(ObIAllocator &allocator,
                                               const ObString &path,
                                               const ObFileTableOptions &options,
                                               ObString &spec)
{
  int ret = OB_SUCCESS;
  const char header = options.header_ ? '1' : '0';
  const char describe = options.describe_ ? '1' : '0';
  ObString fields[7];
  fields[0] = path;
  fields[1] = ObString(func_type_name(options.func_type_));
  fields[2] = ObString(1, &header);
  fields[3] = options.delimiter_;
  fields[4] = options.nullstr_;
  fields[5] = options.format_;
  fields[6] = ObString(1, &describe);
  int64_t total = 0;
  for (int64_t i = 0; i < 7; ++i) {
    total += fields[i].length() + 1;
  }
  char *buf = static_cast<char *>(allocator.alloc(total + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = 0;
    for (int64_t i = 0; i < 7; ++i) {
      if (fields[i].length() > 0) {
        memcpy(buf + pos, fields[i].ptr(), (size_t)fields[i].length());
        pos += fields[i].length();
      }
      buf[pos++] = FILE_TABLE_SPEC_SEPARATOR;
    }
    buf[pos] = '\0';
    spec.assign_ptr(buf, pos);
  }
  return ret;
}

int ObFileTableSource::decode_file_table_spec(ObIAllocator &allocator,
                                               const ObString &spec,
                                               ObString &path,
                                               ObFileTableOptions &options)
{
  int ret = OB_SUCCESS;
  ObString fields[7];
  int64_t count = 0;
  int64_t start = 0;
  for (int64_t i = 0; i <= spec.length(); ++i) {
    if (i == spec.length() || FILE_TABLE_SPEC_SEPARATOR == spec.ptr()[i]) {
      if (count < 7) {
        fields[count].assign_ptr(spec.ptr() + start, i - start);
      }
      ++count;
      start = i + 1;
    }
  }
  if (count < 7) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the spec of the file table function is broken", K(ret), K(count));
  } else {
    path = fields[0];
    const ObString &func_name = fields[1];
    if (0 == func_name.case_compare("read_csv")) {
      options.func_type_ = OB_FILE_FUNC_READ_CSV;
    } else if (0 == func_name.case_compare("read_json")) {
      options.func_type_ = OB_FILE_FUNC_READ_JSON;
    } else if (0 == func_name.case_compare("read_parquet")) {
      options.func_type_ = OB_FILE_FUNC_READ_PARQUET;
    } else if (0 == func_name.case_compare("glob")) {
      options.func_type_ = OB_FILE_FUNC_GLOB;
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unknown file table function", K(ret), K(func_name));
    }
    options.header_ = (fields[2].length() > 0 && '1' == fields[2].ptr()[0]);
    options.delimiter_ = fields[3];
    options.nullstr_ = fields[4];
    options.format_ = fields[5];
    options.describe_ = (fields[6].length() > 0 && '1' == fields[6].ptr()[0]);
    if (options.delimiter_.empty()) {
      options.delimiter_ = ObString(",");
    }
  }
  UNUSED(allocator);
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
