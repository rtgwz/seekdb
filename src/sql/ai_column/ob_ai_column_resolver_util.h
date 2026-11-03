/**
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef OB_AI_COLUMN_RESOLVER_UTIL_H_
#define OB_AI_COLUMN_RESOLVER_UTIL_H_

#include "sql/ai_column/ob_ai_column_def.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{

class ObSQLSessionInfo;

/**
 * DDL side helpers of the AI COLUMN feature.
 *
 * Grammar (see sql_parser_mysql_mode.y):
 *   col data_type AI COLUMN '(' AI_COMPLETE('model', expr[, dim]) ')'
 *        [WITH '(' sync_mode EQ STRING | max_retries EQ INT ',' ... ')']
 *
 * Parse node layout (T_AI_COLUMN_DEF):
 *   children_[0] : T_COLUMN_REF  column name
 *   children_[1] : data type node (same as T_COLUMN_DEFINITION.children_[1])
 *   children_[2] : AI_COMPLETE / AI_EMBED function call expression
 *   children_[3] : T_LINK_NODE chain of T_AI_COLUMN_OPTION or NULL
 *   children_[4] : opt_position_column (may be NULL)
 */
class ObAiColumnResolverUtil
{
public:
  /**
   * Validate one AI column definition and fill `meta`.
   * `column` must already carry name / data type / accuracy resolved by the
   * normal column path; this function only handles AI specifics:
   *   1. expression root must be AI_COMPLETE or AI_EMBED
   *   2. dependencies may not reference another AI column (OB_NOT_SUPPORTED,
   *      test expects `ERROR 0A000: Not supported feature or function`)
   *   3. AI_EMBED third argument (dimension) must match vector(N)
   *      (OB_ERR_INVALID_VECTOR_DIM, test expects error 7600
   *       "inconsistent dimension: expected 4 got 3")
   *   4. sync_mode must be 'async', max_retries must be a non negative int
   *   5. normalized_expr = typed system prompt + expression text
   */
  static int resolve_ai_column_def(const ParseNode &ai_node,
                                   share::schema::ObColumnSchemaV2 &column,
                                   const common::ObIArray<common::ObString> &ai_column_names,
                                   common::ObIAllocator &allocator,
                                   ObAiColumnMeta &meta);

  /**
   * Append the three hidden columns (state / request / result_spec) of one AI
   * column to the table schema. Column ids are taken from the id generator of
   * the resolver; hidden columns are invisible to user SQL but readable by the
   * internal worker, and their ids are persisted in __all_ai_column.
   */
  static int add_hidden_columns(share::schema::ObTableSchema &table_schema,
                                const ObAiColumnMeta &meta,
                                const common::ObIArray<uint64_t> &hidden_column_ids);

  /** Extract the column name of a T_AI_COLUMN_DEF node. */
  static int get_column_name(const ParseNode &ai_node, common::ObString &name);

  /** True when `expr_node` is AI_COMPLETE(...) or AI_EMBED(...). */
  static bool is_ai_func_expr(const ParseNode &expr_node);

  /**
   * ALTER TABLE ... RETRY AI COLUMN <col> [WHERE ...] validation.
   * Returns OB_AI_COLUMN_RETRY_NOT_AI_COLUMN (11121) when the target column is
   * not an AI column, per test expectation:
   *   ERROR 0A000: retrying a column that is not an AI column not supported
   */
  static int validate_retry_target(const share::schema::ObTableSchema &table_schema,
                                   const common::ObIArray<ObAiColumnMeta> &ai_metas,
                                   const common::ObString &column_name);

  /**
   * The unquoted STATUS keyword inside RETRY ... WHERE addresses the pseudo
   * status column (PENDING / READY / FAILED) of the retried AI column, while
   * `status` in backticks addresses the physical business column. The parser
   * hands us a T_COLUMN_REF without backticks for the pseudo column, so the
   * rewrite decision is made here by comparing against the physical columns:
   * if a physical column of the same name exists AND the reference is quoted,
   * it stays physical; otherwise it is rewritten to the hidden state column.
   */
  static bool status_refers_to_pseudo_column(const common::ObString &col_name,
                                             bool is_backquoted,
                                             const share::schema::ObTableSchema &table_schema,
                                             const ObAiColumnMeta &meta);

private:
  static int collect_deps(const ParseNode *node, common::ObIArray<common::ObString> &deps);
  static int parse_options(const ParseNode *options, ObAiColumnMeta &meta);
};

} // namespace sql
} // namespace oceanbase

#endif /* OB_AI_COLUMN_RESOLVER_UTIL_H_ */
