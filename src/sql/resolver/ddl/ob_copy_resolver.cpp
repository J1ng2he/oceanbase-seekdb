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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_copy_resolver.h"
#include "sql/resolver/cmd/ob_load_data_stmt.h"
#include "sql/resolver/ob_resolver.h"

namespace oceanbase
{
namespace sql
{

// reads the options of the statement, the options are the options of DuckDB,
// e.g. "format csv, header, nullstr 'NULL'"
int ObCopyResolver::resolve_options(const ParseNode *option_list, CopyOptions &options)
{
  int ret = OB_SUCCESS;
  for (int32_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(option_list) && i < option_list->num_child_; ++i) {
    const ParseNode *option = option_list->children_[i];
    if (OB_ISNULL(option) || T_COPY_OPTION != option->type_ || option->num_child_ < 1
        || OB_ISNULL(option->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
      break;
    }
    const ParseNode *name_node = option->children_[0];
    const ParseNode *value_node = (option->num_child_ > 1) ? option->children_[1] : nullptr;
    const ObString name(name_node->str_len_, name_node->str_value_);
    if (0 == name.case_compare("format")) {
      if (OB_ISNULL(value_node)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "the format option");
      } else {
        options.format_.assign_ptr(value_node->str_value_, value_node->str_len_);
      }
    } else if (0 == name.case_compare("header")) {
      options.header_ = true;
    } else if (0 == name.case_compare("nullstr")) {
      if (OB_ISNULL(value_node)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "the nullstr option");
      } else {
        options.null_str_.assign_ptr(value_node->str_value_, value_node->str_len_);
      }
    } else {
      // an unknown option is not supported
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "the option");
    }
  }
  return ret;
}

// the names of the columns of the statement, they are written into the file
// when the file holds a header row
int ObCopyResolver::build_header_names(ObIAllocator &allocator, ObString &header_names)
{
  int ret = OB_SUCCESS;
  ObSelectStmt *select_stmt = get_select_stmt();
  if (OB_ISNULL(select_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObSqlString names;
    for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_select_item_size(); ++i) {
      const ObString &name = select_stmt->get_select_item(i).alias_name_;
      if (i > 0 && OB_FAIL(names.append(&ObExternalFileFormat::HEADER_NAME_SEPARATOR, 1))) {
        break;
      }
      if (OB_FAIL(names.append(name.ptr(), name.length()))) {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      ret = ob_write_string(allocator, names.string(), header_names);
    }
  }
  return ret;
}

// builds the item that writes the rows of the statement into the file
int ObCopyResolver::build_into_item(const ParseNode *path_node,
                                     const CopyOptions &options,
                                     ObSelectIntoItem *&into_item)
{
  int ret = OB_SUCCESS;
  ObSelectIntoItem *item = nullptr;
  ObExternalFileFormat format;
  ObString path;
  ObString header_names;
  ObCollationType file_cs_type = ObCharset::get_system_collation();
  if (!options.format_.empty() && 0 != options.format_.case_compare("csv")) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "the format option");
  } else if (OB_ISNULL(path_node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(item = static_cast<ObSelectIntoItem *>(allocator_->alloc(sizeof(ObSelectIntoItem))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    new (item) ObSelectIntoItem();
    item->into_type_ = T_INTO_OUTFILE;
    // the name of the file
    ObObj path_obj;
    path_obj.set_string(ObVarcharType, path_node->str_value_, path_node->str_len_);
    path_obj.set_collation_type(file_cs_type);
    item->outfile_name_ = path_obj;
    // the format of the file, the rows are written as a csv file
    format.format_type_ = ObExternalFileFormat::CSV_FORMAT;
    if (OB_FAIL(format.csv_format_.init_format(ObDataInFileStruct(), 0, file_cs_type))) {
      ret = OB_ERR_UNKNOWN_CHARSET;
    } else {
      format.csv_format_.field_term_str_ = ObString(",");
      format.csv_format_.line_term_str_ = ObString("\n");
      format.file_header_ = options.header_;
      format.null_str_ = options.null_str_;
      if (options.header_ && OB_FAIL(build_header_names(*allocator_, header_names))) {
      } else {
        format.header_names_ = header_names;
      }
    }
    if (OB_SUCC(ret)
        && OB_FAIL(format.to_string_with_alloc(item->external_properties_, *allocator_, true))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_SUCC(ret)) {
      into_item = item;
    }
  }
  return ret;
}

int ObCopyResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  CopyOptions options;
  ObSelectIntoItem *into_item = nullptr;
  if (T_COPY_STMT != parse_tree.type_ || 3 != parse_tree.num_child_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid COPY syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (OB_FAIL(resolve_options(parse_tree.children_[2], options))) {
  } else if (OB_FAIL(ObSelectResolver::resolve(*parse_tree.children_[0]))) {
  } else if (OB_FAIL(build_into_item(parse_tree.children_[1], options, into_item))) {
  } else if (OB_ISNULL(get_select_stmt())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // the rows of the statement are written into the file
    get_select_stmt()->set_select_into(into_item);
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
