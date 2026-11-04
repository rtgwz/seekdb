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
#include "share/schema/ob_ai_column_spec.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "query/session/ob_session_inner_sql.h"
#include "common/mysqlclient/ob_isql_client.h"

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
    // remember the dependency columns: UPDATE of any of them invalidates the
    // result (ai_column_t1_dml: unrelated column update must NOT trigger)
    if (OB_SUCC(ret) && deps.count() > 0) {
      common::ObSqlString dep_list;
      for (int64_t i = 0; OB_SUCC(ret) && i < deps.count(); ++i) {
        if (OB_FAIL(dep_list.append_fmt("%s%.*s", 0 == i ? "" : ",",
                static_cast<int32_t>(deps.at(i).length()), deps.at(i).ptr()))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ob_write_string(allocator, dep_list.string(),
                                         meta.dep_columns_))) {
        LOG_WARN("failed to persist dep columns", K(ret));
      }
    }

    // 3. dimension check for AI_EMBED(model, expr[, dim])
    // vector(N) is an extend type: the type info array carries the dimension
    // (e.g. {"vector", "4"} or {"vector(4)"} depending on the parse path).
    int64_t declared_dim = -1;
    bool is_vector = false;
    {
      const common::ObIArray<common::ObString> &ext_info = column.get_extended_type_info();
      char type_buf[OB_MAX_COLUMN_NAME_BUF_LENGTH + 8];
      int64_t pos = 0;
      for (int64_t e = 0; OB_SUCC(ret) && e < ext_info.count(); ++e) {
        if (OB_FAIL(databuff_printf(type_buf, sizeof(type_buf), pos, "%.*s ",
                static_cast<int32_t>(ext_info.at(e).length()), ext_info.at(e).ptr()))) {
        }
      }
      if (OB_SUCC(ret)) {
        common::ObString joined(static_cast<int32_t>(pos), type_buf);
        if (joined.length() >= 6
            && 0 == strncasecmp(joined.ptr(), "vector", 6)) {
          is_vector = true;
          const char *dim_str = nullptr;
          for (int64_t e = 0; e < ext_info.count(); ++e) {
            for (int64_t c = 0; c < ext_info.at(e).length(); ++c) {
              char ch = ext_info.at(e).ptr()[c];
              if (ch >= '0' && ch <= '9') { dim_str = ext_info.at(e).ptr() + c; break; }
            }
            if (OB_NOT_NULL(dim_str)) { break; }
          }
          if (OB_NOT_NULL(dim_str)) {
            char *end = nullptr;
            declared_dim = strtoll(dim_str, &end, 10);
          }
        }
      }
    }
    if (OB_SUCC(ret) && AI_FUNC_EMBED == meta.func_type_) {
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
    const common::ObIArray<uint64_t> &hidden_column_ids,
    common::ObIAllocator &allocator)
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
    // The full definition travels to the schema service as the comment of the
    // hidden result_spec column (see share::ObAiColumnSpecCodec), where
    // ObTableSqlService persists it into oceanbase.__all_ai_column.
    share::ObAiColumnSpec spec;
    common::ObString spec_comment;
    spec.column_name_ = meta.column_name_;
    spec.ai_model_name_ = meta.ai_model_name_;
    spec.dep_columns_ = meta.dep_columns_;
    spec.normalized_expr_ = meta.normalized_expr_;
    spec.expr_str_ = meta.expr_str_;
    spec.func_type_ = meta.func_type_;
    spec.sync_mode_ = meta.sync_mode_;
    spec.max_retries_ = meta.max_retries_;
    spec.vector_dim_ = meta.vector_dim_;
    if (OB_FAIL(share::ObAiColumnSpecCodec::serialize(spec, allocator, spec_comment))) {
      LOG_WARN("failed to serialize ai column spec", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
      share::schema::ObColumnSchemaV2 hidden_col;
      char buf[OB_MAX_COLUMN_NAME_BUF_LENGTH + 8];
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
        if (2 == i && OB_NOT_NULL(spec_comment.ptr())
            && OB_FAIL(hidden_col.set_comment(spec_comment))) {
          LOG_WARN("failed to set ai spec comment", K(ret));
        } else if (OB_FAIL(table_schema.add_column(hidden_col))) {
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


bool ObAiColumnResolverUtil::is_ai_result_column(
    const share::schema::ObTableSchema &table_schema,
    const share::schema::ObColumnSchemaV2 &column)
{
  bool is_ai = false;
  if (!column.is_hidden()) {
    char state_name[OB_MAX_COLUMN_NAME_BUF_LENGTH + 8];
    int64_t pos = 0;
    if (OB_SUCCESS == databuff_printf(state_name, sizeof(state_name), pos,
            "%s%.*s%s", AI_COLUMN_HIDDEN_PREFIX,
            static_cast<int32_t>(column.get_column_name_str().length()),
            column.get_column_name_str().ptr(), AI_COLUMN_STATE_SUFFIX)) {
      const share::schema::ObColumnSchemaV2 *state_col =
          table_schema.get_column_schema(state_name);
      is_ai = OB_NOT_NULL(state_col) && state_col->is_hidden();
    }
  }
  return is_ai;
}

int ObAiColumnResolverUtil::check_strict_read(
    const share::schema::ObTableSchema &table_schema,
    const share::schema::ObColumnSchemaV2 &result_col,
    ObSQLSessionInfo *session,
    share::schema::ObSchemaGetterGuard *schema_guard)
{
  int ret = OB_SUCCESS;
  bool strict = false;
  if (OB_ISNULL(session) || session->is_inner()) {
    // inner SQL must never recurse into the strict read check
  } else if (OB_FAIL(session->get_sys_variable(
          share::SYS_VAR_AI_COLUMN_STRICT_READ, strict))) {
    LOG_WARN("failed to read ai_column_strict_read", K(ret));
  } else if (!strict || !is_ai_result_column(table_schema, result_col)) {
    // fast path: check disabled or the column is not an AI column
  } else {
    const share::schema::ObDatabaseSchema *db_schema = nullptr;
    char state_name[OB_MAX_COLUMN_NAME_BUF_LENGTH + 8];
    int64_t pos = 0;
    if (OB_ISNULL(schema_guard)
        || OB_FAIL(schema_guard->get_database_schema(
                table_schema.get_database_id(), db_schema))
        || OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get database schema for strict read", K(ret));
    } else if (OB_FAIL(databuff_printf(state_name, sizeof(state_name), pos,
            "%s%.*s%s", AI_COLUMN_HIDDEN_PREFIX,
            static_cast<int32_t>(result_col.get_column_name_str().length()),
            result_col.get_column_name_str().ptr(), AI_COLUMN_STATE_SUFFIX))) {
    } else {
      common::ObSqlString sql;
      if (OB_FAIL(sql.assign_fmt(
              "SELECT 1 FROM `%.*s`.`%.*s` WHERE `%.*s` != %ld LIMIT 1",
              static_cast<int32_t>(db_schema->get_database_name_str().length()),
              db_schema->get_database_name_str().ptr(),
              static_cast<int32_t>(table_schema.get_table_name_str().length()),
              table_schema.get_table_name_str().ptr(),
              static_cast<int32_t>(strlen(state_name)), state_name,
              static_cast<int64_t>(AI_ROW_READY)))) {
        LOG_WARN("failed to build strict read check sql", K(ret));
      } else {
        query::ObSessionInnerSql inner_sql(session);
        common::ObISQLClient::ReadResult result;
        if (OB_FAIL(inner_sql.execute_read(sql, result))) {
          LOG_WARN("failed to run strict read check", K(ret), K(sql));
        } else {
          sqlclient::ObMySQLResult *res = result.mysql_result();
          if (OB_ISNULL(res)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            int cr = res->next();
            if (OB_SUCCESS == cr) {
              // at least one row is not READY: column level failure
              ret = OB_AI_COLUMN_HAS_UNFINISHED_TASK;
              LOG_USER_ERROR(OB_AI_COLUMN_HAS_UNFINISHED_TASK,
                             static_cast<long>(table_schema.get_table_id()),
                             static_cast<long>(result_col.get_column_id()));
              LOG_WARN("ai column strict read failed", K(ret),
                       "table_id", table_schema.get_table_id(),
                       "column_id", result_col.get_column_id());
            } else if (OB_ITER_END != cr) {
              ret = cr;
              LOG_WARN("failed to iterate strict read result", K(ret));
            } else {
              ret = OB_SUCCESS; // all rows READY
            }
          }
          int close_ret = result.close();
          if (OB_SUCCESS == ret && OB_SUCCESS != close_ret) {
            ret = close_ret;
          }
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
