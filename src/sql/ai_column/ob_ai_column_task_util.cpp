/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#define USING_LOG_PREFIX SQL
#include "sql/ai_column/ob_ai_column_task_util.h"

namespace oceanbase
{
namespace sql
{

int ObAiColumnTaskUtil::format_rowkey_predicate(const common::ObString &pk_column_name,
                                                const common::ObString &pk_literal,
                                                common::ObSqlString &predicate)
{
  // Tests match exactly: task.rowkey_predicate = '`id` = 2'
  return predicate.assign_fmt("`%.*s` = %.*s",
                              static_cast<int32_t>(pk_column_name.length()),
                              pk_column_name.ptr(),
                              static_cast<int32_t>(pk_literal.length()),
                              pk_literal.ptr());
}

int ObAiColumnTaskUtil::build_invalidate_sql(const share::schema::ObTableSchema &table_schema,
                                             const ObAiColumnMeta &meta,
                                             uint64_t request_id,
                                             const common::ObString &rowkey_predicate,
                                             common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *state_col =
      table_schema.get_column_schema(meta.state_column_id_);
  const share::schema::ObColumnSchemaV2 *request_col =
      table_schema.get_column_schema(meta.request_column_id_);
  const common::ObString &db_name = table_schema.get_database_name_str();
  const common::ObString &tb_name = table_schema.get_table_name_str();
  if (OB_ISNULL(state_col) || OB_ISNULL(request_col)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hidden ai columns missing", K(ret), K(meta));
  } else {
    // hidden columns are addressed by id so the statement is unambiguous
    sql.reset();
    if (OB_FAIL(sql.assign_fmt(
            "UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%.*s` = %ld, `%.*s` = %lu "
            "WHERE %.*s",
            static_cast<int32_t>(db_name.length()), db_name.ptr(),
            static_cast<int32_t>(tb_name.length()), tb_name.ptr(),
            static_cast<int32_t>(meta.column_name_.length()), meta.column_name_.ptr(),
            static_cast<int32_t>(state_col->get_column_name_str().length()),
            state_col->get_column_name_str().ptr(),
            static_cast<int64_t>(AI_ROW_PENDING),
            static_cast<int32_t>(request_col->get_column_name_str().length()),
            request_col->get_column_name_str().ptr(),
            request_id,
            static_cast<int32_t>(rowkey_predicate.length()),
            rowkey_predicate.ptr()))) {
      LOG_WARN("failed to build invalidate sql", K(ret));
    }
  }
  return ret;
}

int ObAiColumnTaskUtil::build_task_insert_sql(uint64_t table_id,
                                              const ObAiColumnMeta &meta,
                                              uint64_t task_id,
                                              uint64_t request_id,
                                              const common::ObString &rowkey_predicate,
                                              common::ObSqlString &sql)
{
  // eligible_ts is set to NOW(6) so the task is immediately eligible
  // (ai_column_t1_dml: insert_task_eligible_immediately checks
  //  max(task.eligible_ts) <= now(6)).
  return sql.assign_fmt(
      "INSERT INTO oceanbase.__all_ai_column_task "
      "(table_id, task_id, column_id, request_id, rowkey_predicate, eligible_ts, "
      " state, retry_count) VALUES "
      "(%lu, %lu, %lu, %lu, '%.*s', now(6), %ld, 0)",
      table_id, task_id, meta.column_id_, request_id,
      static_cast<int32_t>(rowkey_predicate.length()), rowkey_predicate.ptr(),
      static_cast<int64_t>(AI_TASK_ELIGIBLE));
}

int ObAiColumnTaskUtil::build_task_delete_sql(
    uint64_t table_id,
    const common::ObIArray<common::ObString> &rowkey_predicates,
    common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(rowkey_predicates.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    sql.reset();
    if (OB_FAIL(sql.assign_fmt(
            "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu "
            "AND rowkey_predicate IN (", table_id))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_predicates.count(); ++i) {
      if (OB_FAIL(sql.append_fmt("%s'%.*s'", 0 == i ? "" : ", ",
                                 static_cast<int32_t>(rowkey_predicates.at(i).length()),
                                 rowkey_predicates.at(i).ptr()))) {
        LOG_WARN("failed to append predicate", K(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(")"))) {
    }
  }
  return ret;
}

int ObAiColumnTaskUtil::build_strict_read_check_sql(
    const share::schema::ObTableSchema &table_schema,
    const ObAiColumnMeta &meta,
    common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *state_col =
      table_schema.get_column_schema(meta.state_column_id_);
  const common::ObString &db_name = table_schema.get_database_name_str();
  const common::ObString &tb_name = table_schema.get_table_name_str();
  if (OB_ISNULL(state_col)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("hidden state column missing", K(ret), K(meta));
  } else {
    ret = sql.assign_fmt(
        "SELECT 1 FROM `%.*s`.`%.*s` WHERE `%.*s` != %ld LIMIT 1",
        static_cast<int32_t>(db_name.length()), db_name.ptr(),
        static_cast<int32_t>(tb_name.length()), tb_name.ptr(),
        static_cast<int32_t>(state_col->get_column_name_str().length()),
        state_col->get_column_name_str().ptr(),
        static_cast<int64_t>(AI_ROW_READY));
  }
  return ret;
}

int ObAiColumnTaskUtil::build_meta_insert_sql(const ObAiColumnMeta &meta,
                                              common::ObSqlString &sql)
{
  char dim_buf[32];
  dim_buf[0] = '\0';
  if (meta.vector_dim_ >= 0) {
    snprintf(dim_buf, sizeof(dim_buf), "%ld", meta.vector_dim_);
  }
  return sql.assign_fmt(
      "INSERT INTO oceanbase.__all_ai_column "
      "(table_id, column_id, column_name, ai_model_name, func_type, sync_mode, "
      " max_retries, normalized_expr, expr_str, state_column_id, "
      " request_column_id, result_spec_column_id, vector_dim) VALUES "
      "(%lu, %lu, '%.*s', '%.*s', %ld, %ld, %ld, '%.*s', '%.*s', %lu, %lu, %lu, %s)",
      meta.table_id_, meta.column_id_,
      static_cast<int32_t>(meta.column_name_.length()), meta.column_name_.ptr(),
      static_cast<int32_t>(meta.ai_model_name_.length()), meta.ai_model_name_.ptr(),
      meta.func_type_, meta.sync_mode_, meta.max_retries_,
      static_cast<int32_t>(meta.normalized_expr_.length()), meta.normalized_expr_.ptr(),
      static_cast<int32_t>(meta.expr_str_.length()), meta.expr_str_.ptr(),
      meta.state_column_id_, meta.request_column_id_, meta.result_spec_column_id_,
      meta.vector_dim_ >= 0 ? dim_buf : "NULL");
}

} // namespace sql
} // namespace oceanbase
