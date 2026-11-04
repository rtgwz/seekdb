/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef OB_AI_COLUMN_DEF_H_
#define OB_AI_COLUMN_DEF_H_

#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{

/**
 * Shared definitions for the AI COLUMN feature (seekdb competition task 3).
 *
 * Terminology
 *  - result column : the user visible column declared as
 *                    `<name> <type> AI COLUMN (AI_COMPLETE(...)|AI_EMBED(...)) WITH (...)`
 *  - hidden columns: three is_hidden=1 columns maintained per AI column
 *                    (state / request / result_spec), referenced from
 *                    oceanbase.__all_ai_column by column id.
 *  - task          : one row in oceanbase.__all_ai_column_task, created in the
 *                    same transaction that invalidates the result column.
 *
 * Semantics pinned by tools/deploy/mysql_test/test_suite/ai_column:
 *  - default read returns NULL while the row state is not READY
 *  - SET SESSION ai_column_strict_read=ON makes any statement that references
 *    an AI column fail with 11120 if ANY row of the table is not READY
 *    (column level check, not row level)
 *  - tasks of one row change share one request_id (grouped visibility)
 *  - a late model answer may never overwrite a newer result (stale guard)
 */

enum ObAiColumnFuncType
{
  AI_FUNC_COMPLETE = 0,
  AI_FUNC_EMBED = 1,
};

/** Row state persisted in the hidden state column of each AI column. */
enum ObAiColumnRowState
{
  AI_ROW_PENDING = 0,   // result is NULL, a task exists (or will exist)
  AI_ROW_READY = 1,     // result written back by the worker
  AI_ROW_FAILED = 2,    // terminal failure, retries exhausted, result stays NULL
};

/**
 * Task state persisted in oceanbase.__all_ai_column_task.state.
 * Visible states in tests: 3 = waiting for a scheduled retry (retry_count >= 1).
 * SUCCEEDED / terminal FAILED tasks are deleted from the task table.
 */
enum ObAiColumnTaskState
{
  AI_TASK_ELIGIBLE = 0, // eligible_ts reached, waiting to be claimed
  AI_TASK_RUNNING = 1,  // claimed by a worker
  AI_TASK_SUCCEEDED = 2,// transient; row deleted together with write back
  AI_TASK_RETRY_WAIT = 3,// failed attempt, retry scheduled (retry_count >= 1)
  AI_TASK_DEAD = 4,     // terminal failure, row about to be deleted
};

static const char *const AI_COLUMN_SYNC_MODE_ASYNC = "async";
static const int64_t AI_COLUMN_DEFAULT_MAX_RETRIES = 3;

/** Prefix of the three hidden columns of every AI column. */
static const char *const AI_COLUMN_HIDDEN_PREFIX = "__ai_";
static const char *const AI_COLUMN_STATE_SUFFIX = "_state";
static const char *const AI_COLUMN_REQUEST_SUFFIX = "_request";
static const char *const AI_COLUMN_RESULT_SPEC_SUFFIX = "_result_spec";

/** Name of the retry-only pseudo column (unquoted STATUS in RETRY ... WHERE). */
static const char *const AI_COLUMN_STATUS_PSEUDO_NAME = "status";

/**
 * Runtime metadata of one AI column. Mirrors oceanbase.__all_ai_column.
 */
struct ObAiColumnMeta
{
  ObAiColumnMeta()
      : table_id_(common::OB_INVALID_ID),
        column_id_(common::OB_INVALID_ID),
        state_column_id_(common::OB_INVALID_ID),
        request_column_id_(common::OB_INVALID_ID),
        result_spec_column_id_(common::OB_INVALID_ID),
        func_type_(AI_FUNC_COMPLETE),
        sync_mode_(0 /*async*/),
        max_retries_(AI_COLUMN_DEFAULT_MAX_RETRIES),
        vector_dim_(-1)
  {
  }
  uint64_t table_id_;
  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  common::ObString column_name_;
  common::ObString ai_model_name_;
  common::ObString dep_columns_;     // comma separated dependency columns
  common::ObString normalized_expr_; // typed prompt + user expression, persisted
  common::ObString expr_str_;        // raw expression text, e.g. AI_COMPLETE('m', content)
  int64_t func_type_;
  int64_t sync_mode_;
  int64_t max_retries_;
  int64_t vector_dim_; // -1 when the column is not a vector type

  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(ai_model_name),
               K_(func_type), K_(sync_mode), K_(max_retries), K_(vector_dim),
               K_(state_column_id), K_(request_column_id), K_(result_spec_column_id));
};

/**
 * Typed system prompts. The mock service matches on:
 *   json    -> "Return exactly one valid JSON value"
 *   integer -> "base-10 integer"
 * The JSON variant text is also matched by the DDL test against
 * __all_ai_column.normalized_expr.
 */
class ObAiColumnPrompt
{
public:
  static const char *json_prompt()
  {
    return "You are a data generator for a JSON column. "
           "Return exactly one valid JSON value and nothing else. "
           "Do not wrap it in markdown or add any explanation.";
  }
  static const char *integer_prompt()
  {
    return "You are a data generator for an integer column. "
           "Return exactly one base-10 integer and nothing else. "
           "Do not add any explanation.";
  }
  static const char *default_prompt()
  {
    return "You are a data generator for a text column. "
           "Return exactly one plain text value and nothing else.";
  }
  /** True when the prompt for the given result type must be the JSON one. */
  static bool is_json_prompt_needed(bool is_json_type) { return is_json_type; }
};

} // namespace sql
} // namespace oceanbase

#endif /* OB_AI_COLUMN_DEF_H_ */
