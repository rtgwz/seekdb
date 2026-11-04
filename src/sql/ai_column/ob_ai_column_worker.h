/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef OB_AI_COLUMN_WORKER_H_
#define OB_AI_COLUMN_WORKER_H_

#include "sql/ai_column/ob_ai_column_def.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/string/ob_sql_string.h"
#include "lib/thread/thread_pool.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace sql
{

struct ObAiColumnTaskRecord
{
  ObAiColumnTaskRecord()
      : table_id_(common::OB_INVALID_ID), task_id_(common::OB_INVALID_ID),
        column_id_(common::OB_INVALID_ID), request_id_(0),
        state_(AI_TASK_ELIGIBLE), retry_count_(0)
  {
  }
  uint64_t table_id_;
  uint64_t task_id_;
  uint64_t column_id_;
  uint64_t request_id_;
  common::ObString rowkey_predicate_;
  int64_t state_;
  int64_t retry_count_;
};

/**
 * Background worker of the AI COLUMN feature (scoring item 3 + 5).
 *
 * Lifecycle of one task:
 *
 *   user txn : invalidation + task insert (same transaction, see
 *              ObAiColumnTaskUtil) -> COMMIT
 *   worker   : poll eligible tasks (state=0 AND eligible_ts <= now(6))
 *              claim:  UPDATE ... SET state=1 WHERE task_id IN (...)  (atomic,
 *                      concurrent workers never share a task)
 *              group : all tasks with the same request_id are executed and
 *                      published together (grouped visibility:
 *                      ai_column_worker no_partial_visibility)
 *              run   : evaluate the stored expression per row
 *                      (SELECT <expr_str> FROM <table> WHERE <rowkey_predicate>)
 *                      and call the model endpoint of the column:
 *                        AI_COMPLETE -> /v1/chat/completions
 *                          {"messages":[{system: normalized_expr prompt},
 *                                       {user: evaluated prompt}],
 *                           "model": request_model_name}
 *                        AI_EMBED    -> /v1/embeddings {"input": <prompt>}
 *                      Authorization: Bearer <access_key>  (mock asserts this)
 *              write : UPDATE <table> SET <result>=<answer>, <state col>=READY
 *                      WHERE <rowkey> AND <request col>=<request_id>
 *                      -- the second condition is the stale guard: a late
 *                      answer of an outdated request must never overwrite a
 *                      newer result (ai_column_worker: late_old_result_cannot_
 *                      overwrite, mock 'slow-old' sleeps 1.2s)
 *              finish: DELETE tasks of the request_id; on HTTP failure either
 *                      UPDATE state=3 (retry scheduled, retry_count+1) when
 *                      retry_count < max_retries, or DELETE task and mark the
 *                      row state FAILED (terminal, result stays NULL,
 *                      ai_column_worker: terminal_failure_removes_task)
 *
 * Vector columns (scoring item 5): after invalidation the vector index must
 * drop the row; the write-back re-adds it. Integration point is
 * notify_vector_index_change() - wire it to the existing plugin vector index
 * async task infra (src/observer/vector_index/ob_plugin_vector_index_service.h)
 * which already maintains HNSW/VSAG indexes incrementally.
 *
 * Startup wiring TODO (single line): start one ObAiColumnWorker per observer
 * next to the vector index scheduler in src/observer/ob_server.cpp; the worker
 * is tenant agnostic and filters by table_id <-> tenant through the schema
 * service like other schedulers do.
 */
class ObAiColumnWorker : public lib::ThreadPool
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker();

  int init(common::ObISQLClient *sql_client, int64_t batch_size);
  int start();
  void stop();
  virtual void run1() override;

private:
  /** One iteration: claim -> execute -> publish. Returns rows processed. */
  int process_batch();
  /** Claim up to batch_size_ eligible tasks. */
  int claim_tasks(common::ObIArray<ObAiColumnTaskRecord> &tasks);
  /** Execute every task of one request_id and publish results atomically. */
  int execute_group(const common::ObIArray<ObAiColumnTaskRecord> &group);
  /**
   * Evaluate the column expression for the target row. Returns the model
   * input string (e.g. 'Classify: initial' in the tests).
   */
  int eval_model_input(const common::ObString &database_name,
                       const share::schema::ObTableSchema &table_schema,
                       const ObAiColumnMeta &meta,
                       const ObAiColumnTaskRecord &task,
                       common::ObString &input);
  /**
   * Call the endpoint configured for meta.ai_model_name_ via the existing
   * AI function infrastructure (ObAIFuncModel::call_completion / the embed
   * path used by ObExprAIEmbed), attaching the typed system prompt stored in
   * normalized_expr_.
   */
  int call_model(const ObAiColumnMeta &meta,
                 const common::ObString &input,
                 common::ObString &answer,
                 bool &transient_failure);
  /** Write the answer back with the stale guard; returns rows affected. */
  int write_back(const common::ObString &database_name,
                 const share::schema::ObTableSchema &table_schema,
                 const ObAiColumnMeta &meta,
                 const ObAiColumnTaskRecord &task,
                 const common::ObString &answer,
                 int64_t &affected_rows);
  /** Task finished (success or terminal failure): cleanup + row state. */
  int finish_group(const common::ObIArray<ObAiColumnTaskRecord> &group,
                   uint64_t table_id, bool all_success);
  /** Schedule a retry (state=3) or mark dead. */
  int handle_failure(const ObAiColumnTaskRecord &task,
                     const ObAiColumnMeta &meta,
                     bool transient_failure);
  /** Vector index maintenance hook, see class comment. */
  int notify_vector_index_change(const share::schema::ObTableSchema &table_schema,
                                 const ObAiColumnMeta &meta,
                                 bool row_added);

  bool inited_;
  volatile bool stopped_;
  int64_t batch_size_;
  common::ObISQLClient *sql_client_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorker);
};

} // namespace sql
} // namespace oceanbase

#endif /* OB_AI_COLUMN_WORKER_H_ */
