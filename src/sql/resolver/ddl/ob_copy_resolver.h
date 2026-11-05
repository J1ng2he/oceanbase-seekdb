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

#ifndef _OB_COPY_RESOLVER_H
#define _OB_COPY_RESOLVER_H
#include "sql/resolver/dml/ob_select_resolver.h"

namespace oceanbase
{
namespace sql
{
  /**
   * The resolver of a COPY statement.
   *
   * A COPY statement writes the result of a query into a file, it uses the
   * syntax of DuckDB:
   *
   *   copy (select c1 from t) to 'file.csv' (format csv, header, nullstr 'NULL')
   *
   * The statement is resolved into a select statement that writes its rows
   * into the file, the machinery of "select ... into outfile" is reused.
   */
  class ObCopyResolver : public ObSelectResolver
  {
  public:
    explicit ObCopyResolver(ObResolverParams &params)
      : ObSelectResolver(params)
    {}
    virtual ~ObCopyResolver() {}
    virtual int resolve(const ParseNode &parse_tree);

  private:
    // the options of a copy statement
    struct CopyOptions
    {
      CopyOptions() : format_(), header_(false), null_str_() {}
      // the format of the file, only "csv" is supported
      ObString format_;
      // whether the file holds a row with the names of the columns
      bool header_;
      // the text that is written for a null value
      ObString null_str_;
    };

    // reads the options of the statement
    int resolve_options(const ParseNode *option_list, CopyOptions &options);
    // builds the item that writes the rows of the statement into the file
    int build_into_item(const ParseNode *path_node,
                        const CopyOptions &options,
                        ObSelectIntoItem *&into_item);
    // the names of the columns of the statement, they are written into the
    // file when the file holds a header row
    int build_header_names(ObIAllocator &allocator, ObString &header_names);
  };
}
}
#endif
