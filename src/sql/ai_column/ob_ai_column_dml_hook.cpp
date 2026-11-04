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
#include "sql/ai_column/ob_ai_column_dml_hook.h"

#include "lib/allocator/ob_arena_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_utils.h"
#include "share/schema/ob_ai_column_spec.h"
#include "sql/ai_column/ob_ai_column_def.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "query/session/ob_session_inner_sql.h"

#include <atomic>

namespace oceanbase
{
namespace sql
{

using namespace common;

namespace
{

const int64_t AI_HOOK_MAX_COLS = 8;

struct AiColRuntime
{
  uint64_t result_column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  ObString result_name_;
  ObString state_name_;
  ObString request_name_;
  uint64_t dep_column_ids_[AI_HOOK_MAX_COLS];
  int64_t dep_cnt_;
  bool is_vector_;
};

struct AiTableCtx
{
  uint64_t table_id_;        // raw table id
  uint64_t extract_table_id_;// id written into the task table
  ObString database_name_;
  ObString table_name_;
  AiColRuntime cols_[AI_HOOK_MAX_COLS];
  int64_t col_cnt_;
  ObString rowkey_names_[AI_HOOK_MAX_COLS];
  int64_t rowkey_cnt_;
};

struct AiRowEvent
{
  int64_t ctx_idx_;
  ObString rowkey_predicate_;      // NEW rowkey, target of the invalidation
  ObString old_rowkey_predicate_;  // non empty when the rowkey changed
  bool is_delete_;
  uint64_t invalid_cols_[AI_HOOK_MAX_COLS]; // result column ids to invalidate
  int64_t invalid_cnt_;
};

/** Thread local pending state, reset by flush_pending(). */
struct AiPendingState
{
  AiPendingState() : arena_(), ctxs_(), events_() {}
  ObArenaAllocator arena_;
  ObSEArray<AiTableCtx, 4> ctxs_;
  ObSEArray<AiRowEvent, 64> events_;
};

AiPendingState &get_pending_state()
{
  static thread_local AiPendingState state;
  return state;
}

int append_quoted_literal(ObSqlString &sql, const ObDatum &datum)
{
  int ret = OB_SUCCESS;
  ObString v = datum.get_string();
  if (OB_FAIL(sql.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < v.length(); ++i) {
      if ('\'' == v.ptr()[i] && OB_FAIL(sql.append("''"))) {
      } else if ('\'' != v.ptr()[i] && OB_FAIL(sql.append(v.ptr()[i]))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
    }
  }
  return ret;
}

int format_datum_literal(const ObObjType type, const ObDatum &datum,
                         ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (ob_is_integer_tc(type) || ob_is_decimal_int_tc(type)) {
    ret = sql.append_fmt("%ld", datum.get_int());
  } else if (ob_is_uint_tc(type)) {
    ret = sql.append_fmt("%lu", datum.get_uint64());
  } else if (ob_is_float_tc(type) || ob_is_double_tc(type)) {
    ret = sql.append_fmt("%f", datum.get_double());
  } else if (ob_is_string_tc(type) || ob_is_text_tc(type)) {
    ret = append_quoted_literal(sql, datum);
  } else if (ob_is_datetime_tc(type)) {
    ret = sql.append_fmt("%ld", datum.get_int());
  } else if (datum.is_null()) {
    ret = sql.append("NULL");
  } else {
    // fallback: treat as string
    ret = append_quoted_literal(sql, datum);
  }
  return ret;
}

/** "`id` = 2" (AND-joined for multi column rowkeys), test pinned format. */
int build_rowkey_predicate(const AiTableCtx &ctx,
                           const common::ObIArray<uint64_t> &column_ids,
                           const common::ObIArray<ObExpr *> &row,
                           ObEvalCtx &eval_ctx,
                           ObSqlString &pred)
{
  int ret = OB_SUCCESS;
  pred.reset();
  for (int64_t rk = 0; OB_SUCC(ret) && rk < ctx.rowkey_cnt_; ++rk) {
    // rowkey columns are the first rowkey_cnt_ entries of the DML row
    if (OB_UNLIKELY(rk >= row.count() || rk >= column_ids.count())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum *datum = nullptr;
      const ObExpr *expr = row.at(rk);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(expr->eval(eval_ctx, datum))) {
        LOG_WARN("failed to eval rowkey datum", K(ret));
      } else if (OB_ISNULL(datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        if (rk > 0 && OB_FAIL(pred.append(" AND "))) {
        } else if (OB_FAIL(pred.append_fmt("`%.*s` = ",
                static_cast<int32_t>(ctx.rowkey_names_[rk].length()),
                ctx.rowkey_names_[rk].ptr()))) {
        } else if (OB_FAIL(format_datum_literal(expr->type_.get_type(),
                *datum, pred))) {
          LOG_WARN("failed to format rowkey literal", K(ret));
        }
      }
    }
  }
  return ret;
}

inline int64_t spec_dep_len(const ObString &deps, int64_t start, int64_t end)
{
  return end - start;
}

bool name_matches_ai_hidden(const ObString &name, const char *suffix,
                            ObString &ai_col_name)
{
  bool match = false;
  const int64_t pre = strlen(AI_COLUMN_HIDDEN_PREFIX);
  const int64_t suf = strlen(suffix);
  if (name.length() > pre + suf
      && 0 == STRNCASECMP(name.ptr(), AI_COLUMN_HIDDEN_PREFIX, pre)
      && 0 == STRNCASECMP(name.ptr() + name.length() - suf, suffix, suf)) {
    ai_col_name.assign_ptr(name.ptr() + pre,
                           static_cast<int32_t>(name.length() - pre - suf));
    match = true;
  }
  return match;
}

int build_table_ctx(ObExecContext &exec_ctx, uint64_t table_id,
                    AiTableCtx &ctx, bool &table_interesting)
{
  int ret = OB_SUCCESS;
  table_interesting = false;
  const share::schema::ObTableSchema *table_schema = nullptr;
  share::schema::ObSchemaGetterGuard *guard = nullptr;
  if (OB_ISNULL(guard = exec_ctx.get_sql_ctx()->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(guard->get_table_schema(table_id, table_schema))) {
    LOG_WARN("failed to get table schema", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    // schema gone; nothing to do
  } else if (OB_INVALID_ID == table_schema->get_database_id()) {
    // system table
  } else {
    const share::schema::ObDatabaseSchema *db_schema = nullptr;
    if (OB_FAIL(guard->get_database_schema(table_schema->get_database_id(),
                                           db_schema))
        || OB_ISNULL(db_schema)) {
      LOG_WARN("failed to get database schema", K(ret));
    } else {
      ctx.table_id_ = table_id;
      ctx.extract_table_id_ =
          share::schema::ObSchemaUtils::get_extract_schema_id(table_id);
      ctx.database_name_ = db_schema->get_database_name_str();
      ctx.table_name_ = table_schema->get_table_name_str();
      ctx.col_cnt_ = 0;
      ctx.rowkey_cnt_ = 0;
      // rowkey column names
      const share::schema::ObRowkeyInfo &rk_info = table_schema->get_rowkey_info();
      for (int64_t i = 0;
           OB_SUCC(ret) && i < rk_info.get_size() && i < AI_HOOK_MAX_COLS;
           ++i) {
        const share::schema::ObRowkeyColumn *rk_col = rk_info.get_column(i);
        const share::schema::ObColumnSchemaV2 *col =
            OB_NOT_NULL(rk_col)
                ? table_schema->get_column_schema(rk_col->column_id_) : nullptr;
        if (OB_ISNULL(col)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ctx.rowkey_names_[i] = col->get_column_name_str();
          ctx.rowkey_cnt_ = i + 1;
        }
      }
      // AI columns: state hidden column defines the triple
      for (share::schema::ObTableSchema::const_column_iterator iter =
               table_schema->column_begin();
           OB_SUCC(ret) && iter != table_schema->column_end() && ctx.col_cnt_ < AI_HOOK_MAX_COLS;
           ++iter) {
        const share::schema::ObColumnSchemaV2 *col =
            OB_NOT_NULL(iter) ? *iter : nullptr;
        ObString ai_name;
        if (OB_ISNULL(col) || !col->is_hidden()) {
        } else if (!name_matches_ai_hidden(col->get_column_name_str(),
                                           AI_COLUMN_STATE_SUFFIX, ai_name)) {
        } else {
          char request_name[OB_MAX_COLUMN_NAME_BUF_LENGTH + 8];
          char spec_name[OB_MAX_COLUMN_NAME_BUF_LENGTH + 8];
          int64_t pos = 0;
          const share::schema::ObColumnSchemaV2 *result_col =
              table_schema->get_column_schema(ai_name);
          const share::schema::ObColumnSchemaV2 *request_col = nullptr;
          const share::schema::ObColumnSchemaV2 *spec_col = nullptr;
          if (OB_FAIL(databuff_printf(request_name, sizeof(request_name), pos,
                  "%s%.*s%s", AI_COLUMN_HIDDEN_PREFIX,
                  static_cast<int32_t>(ai_name.length()), ai_name.ptr(),
                  AI_COLUMN_REQUEST_SUFFIX))) {
          } else if (FALSE_IT(pos = 0)) {
          } else if (OB_FAIL(databuff_printf(spec_name, sizeof(spec_name), pos,
                  "%s%.*s%s", AI_COLUMN_HIDDEN_PREFIX,
                  static_cast<int32_t>(ai_name.length()), ai_name.ptr(),
                  AI_COLUMN_RESULT_SPEC_SUFFIX))) {
          } else if (OB_ISNULL(result_col)
                     || OB_ISNULL(request_col = table_schema->get_column_schema(request_name))
                     || OB_ISNULL(spec_col = table_schema->get_column_schema(spec_name))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("incomplete ai column schema", K(ret), K(ai_name));
          } else {
            AiColRuntime &col_rt = ctx.cols_[ctx.col_cnt_];
            col_rt.result_column_id_ = result_col->get_column_id();
            col_rt.state_column_id_ = col->get_column_id();
            col_rt.request_column_id_ = request_col->get_column_id();
            col_rt.result_name_ = result_col->get_column_name_str();
            col_rt.state_name_ = col->get_column_name_str();
            col_rt.request_name_ = request_col->get_column_name_str();
            col_rt.dep_cnt_ = 0;
            col_rt.is_vector_ = false;
            // dependencies travel in the spec comment of the result_spec column
            share::ObAiColumnSpec spec;
            if (share::ObAiColumnSpecCodec::is_spec_comment(
                    spec_col->get_comment_str())
                && OB_FAIL(share::ObAiColumnSpecCodec::deserialize(
                    spec_col->get_comment_str(), get_pending_state().arena_,
                    spec))) {
              LOG_WARN("failed to deserialize ai spec, treating as no deps",
                       K(ret));
              ret = OB_SUCCESS; // tolerate a broken comment
            } else {
              col_rt.is_vector_ = (spec.vector_dim_ >= 0);
              if (spec.dep_columns_.length() <= 0) {
                // no dependency columns recorded
              }
              // map dependency names to column ids
              int64_t start = 0;
              for (int64_t p = 0;
                   OB_SUCC(ret) && p <= spec.dep_columns_.length()
                   && col_rt.dep_cnt_ < AI_HOOK_MAX_COLS;
                   ++p) {
                if (p == spec.dep_columns_.length()
                    || ',' == spec.dep_columns_.ptr()[p]) {
                  ObString dep_name(spec_dep_len(spec.dep_columns_, start, p),
                                    spec.dep_columns_.ptr() + start);
                  const share::schema::ObColumnSchemaV2 *dep_col =
                      table_schema->get_column_schema(dep_name);
                  if (OB_NOT_NULL(dep_col)) {
                    col_rt.dep_column_ids_[col_rt.dep_cnt_++] =
                        dep_col->get_column_id();
                  }
                  start = p + 1;
                }
              }
            }
            ++ctx.col_cnt_;
          }
        }
      }
      table_interesting = ctx.col_cnt_ > 0;
    }
  }
  return ret;
}

int get_table_ctx_idx(ObExecContext &exec_ctx, uint64_t table_id,
                      int64_t &ctx_idx)
{
  int ret = OB_SUCCESS;
  AiPendingState &state = get_pending_state();
  ctx_idx = -1;
  for (int64_t i = 0; OB_SUCC(ret) && i < state.ctxs_.count(); ++i) {
    if (state.ctxs_.at(i).table_id_ == table_id) {
      ctx_idx = i;
    }
  }
  if (OB_SUCC(ret) && ctx_idx < 0) {
    AiTableCtx ctx;
    bool interesting = false;
    if (OB_FAIL(build_table_ctx(exec_ctx, table_id, ctx, interesting))) {
      LOG_WARN("failed to build ai table ctx", K(ret), K(table_id));
    } else if (OB_FAIL(state.ctxs_.push_back(ctx))) {
      LOG_WARN("failed to push ai table ctx", K(ret));
    } else {
      // a ctx with col_cnt_ == 0 acts as the "not an ai table" marker so the
      // schema is scanned at most once per table per statement
      ctx_idx = state.ctxs_.count() - 1;
    }
  } else if (ctx_idx >= 0 && state.ctxs_.at(ctx_idx).col_cnt_ == 0) {
    // marker entry: table has no AI column
    ctx_idx = -1;
  }
  return ret;
}

} // anonymous namespace

int ObAiColumnDmlHook::on_insert_row(ObExecContext &exec_ctx,
                                     uint64_t table_id,
                                     const common::ObIArray<uint64_t> &column_ids,
                                     int64_t rowkey_cnt,
                                     const common::ObIArray<ObExpr *> &new_row,
                                     ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  int64_t ctx_idx = -1;
  if (OB_FAIL(get_table_ctx_idx(exec_ctx, table_id, ctx_idx))) {
    LOG_WARN("failed to get ai table ctx", K(ret));
  } else if (ctx_idx < 0) {
    // not an AI column table
  } else {
    AiPendingState &state = get_pending_state();
    const AiTableCtx &ctx = state.ctxs_.at(ctx_idx);
    AiRowEvent event;
    ObSqlString pred;
    event.ctx_idx_ = ctx_idx;
    event.is_delete_ = false;
    event.old_rowkey_predicate_.reset();
    event.invalid_cnt_ = 0;
    for (int64_t c = 0; c < ctx.col_cnt_; ++c) {
      event.invalid_cols_[event.invalid_cnt_++] = ctx.cols_[c].result_column_id_;
    }
    if (OB_FAIL(build_rowkey_predicate(ctx, column_ids, new_row, eval_ctx, pred))) {
      LOG_WARN("failed to build rowkey predicate", K(ret));
    } else if (OB_FAIL(ob_write_string(state.arena_, pred.string(),
                                       event.rowkey_predicate_))) {
      LOG_WARN("failed to persist predicate", K(ret));
    } else if (OB_FAIL(state.events_.push_back(event))) {
      LOG_WARN("failed to push ai row event", K(ret));
    }
  }
  return ret;
}

int ObAiColumnDmlHook::on_update_row(ObExecContext &exec_ctx,
                                     uint64_t table_id,
                                     const common::ObIArray<uint64_t> &column_ids,
                                     int64_t rowkey_cnt,
                                     const common::ObIArray<ObExpr *> &old_row,
                                     const common::ObIArray<ObExpr *> &new_row,
                                     const common::ObIArray<uint64_t> &updated_column_ids,
                                     ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  int64_t ctx_idx = -1;
  if (OB_FAIL(get_table_ctx_idx(exec_ctx, table_id, ctx_idx))) {
    LOG_WARN("failed to get ai table ctx", K(ret));
  } else if (ctx_idx < 0) {
    // not an AI column table
  } else {
    AiPendingState &state = get_pending_state();
    const AiTableCtx &ctx = state.ctxs_.at(ctx_idx);
    bool rowkey_changed = false;
    AiRowEvent event;
    event.ctx_idx_ = ctx_idx;
    event.is_delete_ = false;
    event.old_rowkey_predicate_.reset();
    event.invalid_cnt_ = 0;
    // 1. did the assignment touch a rowkey column?
    for (int64_t u = 0; OB_SUCC(ret) && !rowkey_changed
         && u < updated_column_ids.count(); ++u) {
      for (int64_t rk = 0; rk < ctx.rowkey_cnt_; ++rk) {
        // rowkey column ids are the leading entries of column_ids
        if (rk < column_ids.count()
            && updated_column_ids.at(u) == column_ids.at(rk)) {
          rowkey_changed = true;
        }
      }
    }
    // 2. which AI columns lost a dependency?
    for (int64_t c = 0; OB_SUCC(ret) && c < ctx.col_cnt_; ++c) {
      const AiColRuntime &col = ctx.cols_[c];
      bool hit = rowkey_changed;
      for (int64_t d = 0; !hit && d < col.dep_cnt_; ++d) {
        for (int64_t u = 0; u < updated_column_ids.count(); ++u) {
          if (col.dep_column_ids_[d] == updated_column_ids.at(u)) {
            hit = true;
          }
        }
      }
      if (hit) {
        event.invalid_cols_[event.invalid_cnt_++] = col.result_column_id_;
      }
    }
    if (OB_SUCC(ret) && event.invalid_cnt_ > 0) {
      ObSqlString pred;
      ObSqlString old_pred;
      if (OB_FAIL(build_rowkey_predicate(ctx, column_ids, new_row, eval_ctx, pred))) {
        LOG_WARN("failed to build new rowkey predicate", K(ret));
      } else if (rowkey_changed
                 && OB_FAIL(build_rowkey_predicate(ctx, column_ids, old_row,
                                                   eval_ctx, old_pred))) {
        LOG_WARN("failed to build old rowkey predicate", K(ret));
      } else if (OB_FAIL(ob_write_string(state.arena_, pred.string(),
                                         event.rowkey_predicate_))) {
      } else if (rowkey_changed
                 && OB_FAIL(ob_write_string(state.arena_, old_pred.string(),
                                            event.old_rowkey_predicate_))) {
      } else if (OB_FAIL(state.events_.push_back(event))) {
        LOG_WARN("failed to push ai row event", K(ret));
      }
    }
  }
  return ret;
}

int ObAiColumnDmlHook::on_delete_row(ObExecContext &exec_ctx,
                                     uint64_t table_id,
                                     const common::ObIArray<uint64_t> &column_ids,
                                     int64_t rowkey_cnt,
                                     const common::ObIArray<ObExpr *> &old_row,
                                     ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  int64_t ctx_idx = -1;
  if (OB_FAIL(get_table_ctx_idx(exec_ctx, table_id, ctx_idx))) {
    LOG_WARN("failed to get ai table ctx", K(ret));
  } else if (ctx_idx < 0) {
    // not an AI column table
  } else {
    AiPendingState &state = get_pending_state();
    const AiTableCtx &ctx = state.ctxs_.at(ctx_idx);
    AiRowEvent event;
    ObSqlString pred;
    event.ctx_idx_ = ctx_idx;
    event.is_delete_ = true;
    event.invalid_cnt_ = 0;
    event.rowkey_predicate_.reset();
    event.old_rowkey_predicate_.reset();
    if (OB_FAIL(build_rowkey_predicate(ctx, column_ids, old_row, eval_ctx, pred))) {
      LOG_WARN("failed to build rowkey predicate", K(ret));
    } else if (OB_FAIL(ob_write_string(state.arena_, pred.string(),
                                       event.rowkey_predicate_))) {
    } else if (OB_FAIL(state.events_.push_back(event))) {
      LOG_WARN("failed to push ai row event", K(ret));
    }
  }
  return ret;
}

bool ObAiColumnDmlHook::has_pending()
{
  return get_pending_state().events_.count() > 0;
}

int ObAiColumnDmlHook::flush_pending(ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  AiPendingState &state = get_pending_state();
  if (0 == state.events_.count()) {
    // fast path for statements without AI columns
  } else if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    static std::atomic<uint64_t> request_seq(1);
    static std::atomic<uint64_t> task_seq(1);
    const uint64_t now_us = ObTimeUtility::current_time();
    query::ObSessionInnerSql inner_sql(session);
    for (int64_t e = 0; OB_SUCC(ret) && e < state.events_.count(); ++e) {
      const AiRowEvent &event = state.events_.at(e);
      const AiTableCtx &ctx = state.ctxs_.at(event.ctx_idx_);
      // one request_id per row change: shared by every AI column of the row
      // (grouped publish, ai_column_worker: one_request_group_for_two_columns)
      const uint64_t request_id = (now_us << 8) | (request_seq.fetch_add(1) & 0xFF);
      if (event.is_delete_) {
        // delete only cleans the pending tasks of the row
        ObSqlString sql;
        if (OB_FAIL(sql.assign_fmt(
                "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu "
                "AND rowkey_predicate = '%.*s'",
                ctx.extract_table_id_,
                static_cast<int32_t>(event.rowkey_predicate_.length()),
                event.rowkey_predicate_.ptr()))) {
        } else {
          int64_t affected = 0;
          if (OB_FAIL(inner_sql.execute_write(sql, affected))) {
            LOG_WARN("failed to clean ai tasks of deleted row", K(ret));
          }
        }
      } else {
        // PK update: drop the tasks bound to the old rowkey first
        if (event.old_rowkey_predicate_.length() > 0) {
          ObSqlString sql;
          if (OB_FAIL(sql.assign_fmt(
                  "DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu "
                  "AND rowkey_predicate = '%.*s'",
                  ctx.extract_table_id_,
                  static_cast<int32_t>(event.old_rowkey_predicate_.length()),
                  event.old_rowkey_predicate_.ptr()))) {
          } else {
            int64_t affected = 0;
            if (OB_FAIL(inner_sql.execute_write(sql, affected))) {
              LOG_WARN("failed to clean ai tasks of old rowkey", K(ret));
            }
          }
        }
        for (int64_t c = 0; OB_SUCC(ret) && c < event.invalid_cnt_; ++c) {
          const AiColRuntime *col = nullptr;
          for (int64_t k = 0; k < ctx.col_cnt_; ++k) {
            if (ctx.cols_[k].result_column_id_ == event.invalid_cols_[c]) {
              col = &ctx.cols_[k];
            }
          }
          if (OB_ISNULL(col)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const uint64_t task_id =
                (now_us << 16) | (task_seq.fetch_add(1) & 0xFFFF);
            ObSqlString invalidate_sql;
            ObSqlString task_sql;
            if (OB_FAIL(invalidate_sql.assign_fmt(
                    "UPDATE `%.*s`.`%.*s` SET `%.*s` = NULL, `%.*s` = %ld, "
                    "`%.*s` = %lu WHERE %.*s",
                    static_cast<int32_t>(ctx.database_name_.length()),
                    ctx.database_name_.ptr(),
                    static_cast<int32_t>(ctx.table_name_.length()),
                    ctx.table_name_.ptr(),
                    static_cast<int32_t>(col->result_name_.length()),
                    col->result_name_.ptr(),
                    static_cast<int32_t>(col->state_name_.length()),
                    col->state_name_.ptr(),
                    static_cast<int64_t>(AI_ROW_PENDING),
                    static_cast<int32_t>(col->request_name_.length()),
                    col->request_name_.ptr(),
                    request_id,
                    static_cast<int32_t>(event.rowkey_predicate_.length()),
                    event.rowkey_predicate_.ptr()))) {
            } else if (OB_FAIL(task_sql.assign_fmt(
                    "INSERT INTO oceanbase.__all_ai_column_task "
                    "(table_id, task_id, column_id, request_id, "
                    "rowkey_predicate, eligible_ts, state, retry_count) "
                    "VALUES (%lu, %lu, %lu, %lu, '%.*s', now(6), %ld, 0)",
                    ctx.extract_table_id_, task_id, col->result_column_id_,
                    request_id,
                    static_cast<int32_t>(event.rowkey_predicate_.length()),
                    event.rowkey_predicate_.ptr(),
                    static_cast<int64_t>(AI_TASK_ELIGIBLE))) {
            } else {
              int64_t affected = 0;
              if (OB_FAIL(inner_sql.execute_write(invalidate_sql, affected))) {
                LOG_WARN("failed to invalidate ai column", K(ret));
              } else if (OB_FAIL(inner_sql.execute_write(task_sql, affected))) {
                LOG_WARN("failed to insert ai column task", K(ret));
              }
            }
          }
        }
      }
    }
    state.events_.reset();
    state.ctxs_.reset();
    state.arena_.reset();
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
