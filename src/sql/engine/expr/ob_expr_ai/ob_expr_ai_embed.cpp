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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "query/api/query/engine/expr/ob_expr_lob_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  if (expr.is_batch_result() && ctx.get_batch_size() > 1) {
    // The expression is evaluated row by row inside the generic batch callback of
    // the parent expression (for example json_length(ai_embed(...))). Collect the
    // whole batch here so that all the rows of the batch share a single request.
    ret = eval_ai_embed_in_batch(expr, ctx);
    if (OB_SUCC(ret)) {
      // The batch callback does not know which rows the parent expression really
      // needs, so the empty input is reported by the row callback of this row.
      ObDatum *arg_model_id = nullptr;
      ObDatum *arg_content = nullptr;
      ObDatum *arg_dim = nullptr;
      if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                             : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
      } else if (!arg_content->is_null() && arg_content->get_string().empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
        res.set_null();
      }
    }
  } else {
    ret = eval_ai_embed_single_row(expr, ctx, res);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_in_batch(const ObExpr &expr, ObEvalCtx &ctx)
{
  int ret = OB_SUCCESS;
  const int64_t batch_size = ctx.get_batch_size();
  // Every row of the batch takes part in the request, so the skip vector is all false.
  void *mem = ctx.exec_ctx_.get_allocator().alloc(ObBitVector::memory_size(batch_size));
  if (OB_ISNULL(mem)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObBitVector *skip = to_bit_vector(mem);
    skip->init(batch_size);
    ret = eval_ai_embed_batch_ex(expr, ctx, *skip, batch_size, true/*defer_empty_input_error*/);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_single_row(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    // 内容为空的行不参与请求，结果为 NULL
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                        const ObBitVector &skip, const int64_t batch_size)
{
  return eval_ai_embed_batch_ex(expr, ctx, skip, batch_size, false/*defer_empty_input_error*/);
}

int ObExprAIEmbed::eval_ai_embed_batch_ex(const ObExpr &expr, ObEvalCtx &ctx,
                                           const ObBitVector &skip, const int64_t batch_size,
                                           const bool defer_empty_input_error)
{
  INIT_SUCC(ret);
  ObDatumVector res_datum = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObDatumVector model_vec;
  ObDatumVector content_vec;
  ObDatumVector dim_vec;
  ObArray<ObString> contents;
  ObArray<int64_t> row_indexes;
  ObArray<ObString> results;
  ObString model_id;
  int64_t dim = 0;
  ObJsonObject *config = nullptr;
  ObJsonInt *dim_json = nullptr;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();

  if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, model_vec, content_vec, dim_vec))) {
  } else if (OB_ISNULL(model_vec.datums_) || OB_ISNULL(content_vec.datums_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (model_vec.at(0)->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
  } else {
    model_id = model_vec.at(0)->get_string();
    if (model_id.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
    }
    // 收集本批需要发起请求的行；内容为 NULL 的行不进入请求，结果直接置为 NULL
    for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; j++) {
      if (skip.at(j) || eval_flags.at(j)) {
        continue;
      }
      ObDatum *content_datum = content_vec.at(j);
      if (content_datum->is_null()) {
        eval_flags.set(j);
        res_datum.at(j)->set_null();
      } else if (content_datum->get_string().empty()) {
        if (defer_empty_input_error) {
          // 本行的空串由行级回调报错，这里先置 NULL，并且不标记为已求值，
          // 这样父表达式的行级回调还会再次进入该行，从而给出与单行求值一致的错误。
          res_datum.at(j)->set_null();
        } else {
          eval_flags.set(j);
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
        }
      } else {
        eval_flags.set(j);
        if (OB_FAIL(contents.push_back(content_datum->get_string()))) {
        } else if (OB_FAIL(row_indexes.push_back(j))) {
        }
      }
    }

    if (OB_SUCC(ret) && !contents.empty()) {
      if (expr.arg_cnt_ == 3 && OB_NOT_NULL(dim_vec.datums_)) {
        ObDatum *dim_datum = dim_vec.at(0);
        if (dim_datum->is_null()) {
          // 未指定维度时按模型默认维度处理
        } else {
          dim = dim_datum->get_int();
          if (dim <= 0) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          }
        }
      }
      if (OB_SUCC(ret) && dim > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
        } else if (OB_ISNULL(endpoint_resolver)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                       model_id, temp_allocator, resolved_endpoint))) {
        } else {
          ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
          // 多行合并为一次请求，NULL 行不参与，避免请求数随行数线性增长
          if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
          } else if (results.count() != contents.count()) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            for (int64_t k = 0; OB_SUCC(ret) && k < results.count(); k++) {
              int64_t row = row_indexes.at(k);
              ObString &result = results.at(k);
              ObTextStringDatumResult output_result(expr.datum_meta_.type_, &expr, &ctx, res_datum.at(row));
              if (OB_FAIL(output_result.init_with_batch_idx(result.length(), row))) {
              } else if (OB_FAIL(output_result.append(result))) {
              } else {
                output_result.set_result();
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
