/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ob_ai_column_resolver_util.h"
#include "lib/string/ob_sql_string.h"
#include "common/object/ob_obj_type.h"

namespace oceanbase
{
namespace sql
{

int ObAiColumnResolverUtil::get_column_name(const ParseNode &ai_node,
                                            common::ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(T_AI_COLUMN_DEF != ai_node.type_ || 5 != ai_node.num_child_
                  || OB_ISNULL(ai_node.children_) || OB_ISNULL(ai_node.children_[0]))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column def node", K(ret), K(ai_node.type_), K(ai_node.num_child_));
  } else {
    const ParseNode *name_ref = ai_node.children_[0];
    const ParseNode *name_node = nullptr;
    if (T_COLUMN_REF == name_ref->type_ && name_ref->num_child_ > 2) {
      name_node = name_ref->children_[2];
    } else {
      name_node = name_ref; // tolerate a bare name node
    }
    if (OB_ISNULL(name_node) || 0 == name_node->str_len_) {
      ret = OB_WRONG_COLUMN_NAME;
    } else {
      name.assign_ptr(name_node->str_value_,
                      static_cast<int32_t>(name_node->str_len_));
    }
  }
  return ret;
}

bool ObAiColumnResolverUtil::is_ai_func_expr(const ParseNode &expr_node)
{
  return T_FUN_SYS_AI_COMPLETE == expr_node.type_
      || T_FUN_SYS_AI_EMBED == expr_node.type_;
}

int ObAiColumnResolverUtil::collect_deps(const ParseNode *node,
                                         common::ObIArray<common::ObString> &deps)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    // do nothing
  } else if (T_COLUMN_REF == node->type_ && OB_NOT_NULL(node->children_)
             && node->num_child_ >= 3 && OB_NOT_NULL(node->children_[2])) {
    // last child of a qualified column ref is the column name
    const ParseNode *col = node->children_[node->num_child_ - 1];
    if (OB_FAIL(add_var_to_array_no_dup(deps,
            common::ObString(col->str_len_, col->str_value_)))) {
      LOG_WARN("failed to push dependency", K(ret));
    }
  } else if (OB_NOT_NULL(node->children_)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(collect_deps(node->children_[i], deps))) {
        LOG_WARN("failed to collect dependencies", K(ret));
      }
    }
  }
  return ret;
}

int ObAiColumnResolverUtil::parse_options(const ParseNode *options, ObAiColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  const ParseNode *cur = options;
  while (OB_SUCC(ret) && OB_NOT_NULL(cur)) {
    if (T_LINK_NODE == cur->type_) {
      if (OB_UNLIKELY(2 != cur->num_child_ || OB_ISNULL(cur->children_))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        // right-most option first, then walk the left chain
        if (OB_FAIL(parse_options(cur->children_[1], meta))) {
        } else {
          cur = cur->children_[0];
        }
      }
    } else if (T_AI_COLUMN_OPTION == cur->type_) {
      if (OB_UNLIKELY(2 != cur->num_child_ || OB_ISNULL(cur->children_)
                      || OB_ISNULL(cur->children_[0]) || OB_ISNULL(cur->children_[1]))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const ParseNode *key = cur->children_[0];
        const ParseNode *value = cur->children_[1];
        common::ObString key_str(key->str_len_, key->str_value_);
        if (0 == key_str.case_compare("sync_mode")) {
          common::ObString mode(value->str_len_, value->str_value_);
          if (0 != mode.case_compare(AI_COLUMN_SYNC_MODE_ASYNC)) {
            // Only the async mode is part of the competition spec.
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "ai column sync_mode other than async");
          }
        } else if (0 == key_str.case_compare("max_retries")) {
          meta.max_retries_ = value->value_;
          if (meta.max_retries_ < 0) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai column max_retries must be >= 0");
          }
        } else {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unknown ai column option");
        }
      }
      cur = nullptr;
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected node in ai column option list", K(ret), K(cur->type_));
    }
  }
  return ret;
}

int ObAiColumnResolverUtil::resolve_ai_column_def(
    const ParseNode &ai_node,
    share::schema::ObColumnSchemaV2 &column,
    const common::ObIArray<common::ObString> &ai_column_names,
    common::ObIAllocator &allocator,
    ObAiColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  const ParseNode *expr_node = nullptr;
  if (OB_UNLIKELY(T_AI_COLUMN_DEF != ai_node.type_ || 5 != ai_node.num_child_
                  || OB_ISNULL(ai_node.children_))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column def node", K(ret));
  } else if (OB_FAIL(get_column_name(ai_node, meta.column_name_))) {
    LOG_WARN("failed to get ai column name", K(ret));
  } else if (OB_ISNULL(expr_node = ai_node.children_[2])
             || !is_ai_func_expr(*expr_node)) {
    // The grammar only allows a function call here; anything else is a syntax
    // error which the parser already rejects. Defensive check kept for safety.
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "ai column expression must be AI_COMPLETE or AI_EMBED");
  } else if (OB_UNLIKELY(expr_node->num_child_ < 2 || OB_ISNULL(expr_node->children_)
                         || OB_ISNULL(expr_node->children_[0]))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // 1. function type and model name
    const ParseNode *model_node = expr_node->children_[0];
    meta.func_type_ = (T_FUN_SYS_AI_EMBED == expr_node->type_)
                      ? AI_FUNC_EMBED : AI_FUNC_COMPLETE;
    meta.ai_model_name_.assign_ptr(model_node->str_value_,
                                   static_cast<int32_t>(model_node->str_len_));

    // 2. dependency check: an AI column must not depend on another AI column
    common::ObSEArray<common::ObString, 4> deps;
    if (OB_FAIL(collect_deps(expr_node, deps))) {
      LOG_WARN("failed to collect dependencies", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < deps.count(); ++i) {
      for (int64_t j = 0; OB_SUCC(ret) && j < ai_column_names.count(); ++j) {
        if (0 == deps.at(i).case_compare(ai_column_names.at(j))) {
          // test ai_column_ddl: --error 1235 with default message
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("ai column cannot depend on another ai column",
                   K(ret), K(deps.at(i)));
        }
      }
    }

    // 3. dimension check for AI_EMBED(model, expr[, dim])
    const bool is_vector = (ObVectorType == column.get_data_type());
    if (OB_SUCC(ret) && AI_FUNC_EMBED == meta.func_type_) {
      const int64_t declared_dim = column.get_accuracy().get_length();
      if (expr_node->num_child_ >= 3 && OB_NOT_NULL(expr_node->children_[2])
          && T_INT == expr_node->children_[2]->type_) {
        const int64_t embed_dim = expr_node->children_[2]->value_;
        if (is_vector && embed_dim != declared_dim) {
          // test ai_column_ddl: --error 7600
          // "inconsistent dimension: expected %u got %u"
          ret = OB_ERR_INVALID_VECTOR_DIM;
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM,
                         static_cast<uint64_t>(declared_dim),
                         static_cast<uint64_t>(embed_dim));
        }
      }
      if (OB_SUCC(ret) && is_vector) {
        meta.vector_dim_ = declared_dim;
      }
    }

    // 4. WITH options
    if (OB_SUCC(ret) && OB_NOT_NULL(ai_node.children_[3])) {
      if (OB_FAIL(parse_options(ai_node.children_[3], meta))) {
        LOG_WARN("failed to parse ai column options", K(ret));
      }
    }

    // 5. normalized expression: typed system prompt + raw expression text.
    // The DDL test matches '%Return exactly one valid JSON value%' for a JSON
    // column; the worker sends the same text as the system message so the mock
    // can produce type-correct answers ("base-10 integer" -> "7").
    if (OB_SUCC(ret)) {
      const char *prompt = nullptr;
      if (common::ObJsonType == column.get_data_type()) {
        prompt = ObAiColumnPrompt::json_prompt();
      } else if (ob_is_int_tc(column.get_data_type())) {
        prompt = ObAiColumnPrompt::integer_prompt();
      } else {
        prompt = ObAiColumnPrompt::default_prompt();
      }
      common::ObSqlString normalized;
      int64_t expr_len = OB_NOT_NULL(expr_node->str_value_)
                         ? static_cast<int64_t>(expr_node->str_len_) : 0;
      const char *expr_str = OB_NOT_NULL(expr_node->str_value_)
                             ? expr_node->str_value_ : "";
      if (OB_FAIL(normalized.append(prompt))) {
      } else if (OB_FAIL(normalized.append_fmt(" | expr=%.*s",
              static_cast<int32_t>(expr_len), expr_str))) {
      } else if (OB_FAIL(normalized.append_fmt(" | target=%.*s",
              static_cast<int32_t>(meta.column_name_.length()),
              meta.column_name_.ptr()))) {
      } else if (OB_FAIL(ob_write_string(allocator, normalized.string(),
                                         meta.normalized_expr_))) {
        LOG_WARN("failed to persist normalized expr", K(ret));
      } else {
        meta.expr_str_.assign_ptr(expr_str, static_cast<int32_t>(expr_len));
      }
    }
  }
  return ret;
}

int ObAiColumnResolverUtil::add_hidden_columns(
    share::schema::ObTableSchema &table_schema,
    const ObAiColumnMeta &meta,
    const common::ObIArray<uint64_t> &hidden_column_ids)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(3 != hidden_column_ids.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("three hidden column ids expected", K(ret), K(hidden_column_ids.count()));
  } else {
    static const char *suffixes[3] = {AI_COLUMN_STATE_SUFFIX,
                                      AI_COLUMN_REQUEST_SUFFIX,
                                      AI_COLUMN_RESULT_SPEC_SUFFIX};
    static const common::ObObjType types[3] = {common::ObIntType,
                                               common::ObIntType,
                                               common::ObVarcharType};
    for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
      share::schema::ObColumnSchemaV2 hidden_col;
      char buf[OB_MAX_COLUMN_NAME_BUFFER_LENGTH + 1];
      int64_t pos = 0;
      if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%s%.*s%s",
                  AI_COLUMN_HIDDEN_PREFIX,
                  static_cast<int32_t>(meta.column_name_.length()),
                  meta.column_name_.ptr(), suffixes[i]))) {
        LOG_WARN("failed to build hidden column name", K(ret));
      } else if (OB_FAIL(hidden_col.set_column_name(buf))) {
      } else {
        hidden_col.set_column_id(hidden_column_ids.at(i));
        hidden_col.set_data_type(types[i]);
        hidden_col.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
        hidden_col.set_nullable(true);
        hidden_col.set_is_hidden(true); // is_hidden = 1 in __all_column
        if (OB_FAIL(table_schema.add_column(hidden_col))) {
          LOG_WARN("failed to add hidden ai column", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObAiColumnResolverUtil::validate_retry_target(
    const share::schema::ObTableSchema &table_schema,
    const common::ObIArray<ObAiColumnMeta> &ai_metas,
    const common::ObString &column_name)
{
  int ret = OB_SUCCESS;
  bool found = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_metas.count(); ++i) {
    if (0 == column_name.case_compare(ai_metas.at(i).column_name_)) {
      found = true;
    }
  }
  if (OB_SUCC(ret) && !found) {
    // test ai_column_retry_strict:
    //   ERROR 0A000: retrying a column that is not an AI column not supported
    ret = OB_AI_COLUMN_RETRY_NOT_AI_COLUMN;
    LOG_WARN("retry target is not an ai column", K(ret), K(column_name));
  }
  return ret;
}

bool ObAiColumnResolverUtil::status_refers_to_pseudo_column(
    const common::ObString &col_name,
    bool is_backquoted,
    const share::schema::ObTableSchema &table_schema,
    const ObAiColumnMeta &meta)
{
  bool is_pseudo = false;
  if (0 == col_name.case_compare(AI_COLUMN_STATUS_PSEUDO_NAME)) {
    const share::schema::ObColumnSchemaV2 *physical =
        table_schema.get_column_schema(col_name);
    // `status` (backquoted) -> physical business column when it exists;
    //  status (unquoted)   -> pseudo status column of the retried AI column
    is_pseudo = !(is_backquoted && OB_NOT_NULL(physical));
  }
  return is_pseudo;
}

} // namespace sql
} // namespace oceanbase
