/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef OB_AI_COLUMN_TASK_UTIL_H_
#define OB_AI_COLUMN_TASK_UTIL_H_

#include "sql/ai_column/ob_ai_column_def.h"
#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
namespace sql
{

/**
 * DML side helpers of the AI COLUMN feature.
 *
 * All statements produced here are executed as inner SQL inside the *user*
 * transaction that triggers them, which gives exactly the semantics the tests
 * require:
 *   - the task row is visible inside the issuing transaction
 *     (ai_column_t1_dml: transaction_sees_task)
 *   - ROLLBACK drops the task together with the data change
 *     (ai_column_t1_dml: rollback_removes_task)
 *
 * rowkey_predicate format pinned by tests: "`id` = 2" (backquoted pk column,
 * space, '=', space, literal).
 */
class ObAiColumnTaskUtil
{
public:
  /**
   * Invalidation + task creation for one changed row:
   *   UPDATE <table> SET <result>=NULL, <state col>=PENDING, <request col>=<rid>
   *    WHERE <rowkey predicate>;
   *   INSERT INTO oceanbase.__all_ai_column_task(...) VALUES (...);
   * Tasks of all AI columns invalidated by the same row change share one
   * request_id (ai_column_worker: one_request_group_for_two_columns), which
   * the worker uses to publish grouped results atomically.
   */
  static int build_invalidate_sql(const share::schema::ObTableSchema &table_schema,
                                  const ObAiColumnMeta &meta,
                                  uint64_t request_id,
                                  const common::ObString &rowkey_predicate,
                                  common::ObSqlString &sql);

  /** Task insert fragment (one row per task). */
  static int build_task_insert_sql(uint64_t table_id,
                                   const ObAiColumnMeta &meta,
                                   uint64_t task_id,
                                   uint64_t request_id,
                                   const common::ObString &rowkey_predicate,
                                   common::ObSqlString &sql);

  /**
   * DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=... AND
   * rowkey_predicate IN (...): called on row DELETE so pending tasks of the
   * deleted rows disappear in the same transaction (delete_cleans_pending_tasks).
   */
  static int build_task_delete_sql(uint64_t table_id,
                                   const common::ObIArray<common::ObString> &rowkey_predicates,
                                   common::ObSqlString &sql);

  /**
   * Strict read check, executed once per referenced AI column when
   * @@ai_column_strict_read is ON:
   *   SELECT 1 FROM <table> WHERE <state col> != 1 LIMIT 1
   * Column-level semantics: the statement fails even when the matching rows
   * themselves are READY, as long as ANY row of the table is not
   * (ai_column_retry_strict lines "Strictness is column-wide").
   * Error: OB_AI_COLUMN_HAS_UNFINISHED_TASK with table_id / column_id.
   */
  static int build_strict_read_check_sql(const share::schema::ObTableSchema &table_schema,
                                         const ObAiColumnMeta &meta,
                                         common::ObSqlString &sql);

  /** Persist AI column metadata after CREATE/ALTER: __all_ai_column row. */
  static int build_meta_insert_sql(const ObAiColumnMeta &meta, common::ObSqlString &sql);

  /** "`id` = 2" — backquoted pk name, formatted literal. */
  static int format_rowkey_predicate(const common::ObString &pk_column_name,
                                     const common::ObString &pk_literal,
                                     common::ObSqlString &predicate);
};

} // namespace sql
} // namespace oceanbase

#endif /* OB_AI_COLUMN_TASK_UTIL_H_ */
