/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef OB_AI_COLUMN_DML_HOOK_H_
#define OB_AI_COLUMN_DML_HOOK_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

class ObExecContext;
class ObSQLSessionInfo;
class ObEvalCtx;
class ObExpr;
template <class T, int MAX>
class ObSEArray;
template <typename T>
class ObIArray;

/**
 * Row level hooks of the AI COLUMN feature (scoring item 2, DML linkage).
 *
 * Called from ObDMLService::process_{insert,update,delete}_row for every
 * changed row of a user table. Events accumulate in thread local storage and
 * are published by flush_pending() at ObTableModifyOp::inner_close() — still
 * inside the user transaction, via the session bound inner connection
 * (query::ObSessionInnerSql). Running the invalidation UPDATE + task INSERT
 * inside the user transaction gives exactly the tested semantics:
 *
 *   - the task row is visible to the issuing transaction
 *   - ROLLBACK drops the task together with the data change
 *   - DELETE removes pending tasks of the deleted rows immediately
 *
 * Semantics per event type:
 *   INSERT : invalidate every AI column of the row (state=PENDING,
 *            request=<rid shared by all AI columns of the row>) + one task
 *   UPDATE : invalidate only AI columns whose dependency columns are touched
 *            by the assignment; a rowkey (PK) update additionally deletes the
 *            pending tasks of the OLD rowkey and invalidates the NEW one
 *            (task rowkey_predicate = "`id` = <new pk>")
 *   DELETE : no invalidation (the row is gone), only task cleanup
 */
class ObAiColumnDmlHook
{
public:
  static int on_insert_row(ObExecContext &exec_ctx,
                           uint64_t table_id,
                           const common::ObIArray<uint64_t> &column_ids,
                           int64_t rowkey_cnt,
                           const common::ObIArray<ObExpr *> &new_row,
                           ObEvalCtx &eval_ctx);

  static int on_update_row(ObExecContext &exec_ctx,
                           uint64_t table_id,
                           const common::ObIArray<uint64_t> &column_ids,
                           int64_t rowkey_cnt,
                           const common::ObIArray<ObExpr *> &old_row,
                           const common::ObIArray<ObExpr *> &new_row,
                           const common::ObIArray<uint64_t> &updated_column_ids,
                           ObEvalCtx &eval_ctx);

  static int on_delete_row(ObExecContext &exec_ctx,
                           uint64_t table_id,
                           const common::ObIArray<uint64_t> &column_ids,
                           int64_t rowkey_cnt,
                           const common::ObIArray<ObExpr *> &old_row,
                           ObEvalCtx &eval_ctx);

  /** Publish accumulated events inside the user transaction. */
  static int flush_pending(ObSQLSessionInfo *session);

  static bool has_pending();
};

} // namespace sql
} // namespace oceanbase

#endif /* OB_AI_COLUMN_DML_HOOK_H_ */
