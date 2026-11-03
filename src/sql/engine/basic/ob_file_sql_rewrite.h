/**
 * Copyright (c) 2026 OceanBase.
 * seekdb competition problem 2: File SQL - SQL text rewriter.
 *
 * Rewrites file-sql statements into plain MySQL/OceanBase SQL:
 *   - FROM read_csv('f' [, options])        -> FROM (SELECT ... UNION ALL ...) alias
 *   - FROM read_json('f', format=...)       -> idem (newline delimited json)
 *   - FROM read_ndjson('f')                 -> idem
 *   - FROM read_parquet('f')                -> idem
 *   - FROM glob('pattern')                  -> FROM (SELECT ... AS file ...) glob
 *   - DESCRIBE SELECT ... FROM read_csv(..) -> SELECT of schema metadata rows
 *   - COPY (query|table) TO 'path' (options)-> executes query, writes CSV
 *
 * Errors are reported as FileSqlErr codes so the caller can map them to
 * mysql error codes (1235 / 1064).
 *
 * Standalone (no OceanBase dependencies) for unit-testability.
 */
#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_REWRITE_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_REWRITE_H_

#include <stdint.h>
#include <string>
#include <vector>
#include "ob_file_scan.h"

namespace oceanbase
{
namespace sql
{
namespace filesql
{

struct FileSqlSessionCtx {
  bool enable_file_sql;
  std::string secure_file_priv;   // canonical secure dir; "" disables check
  FileSqlSessionCtx() : enable_file_sql(false) {}
};

// when a COPY statement is recognized the rewriter returns the inner query
// (already rewritten) plus export options; the glue layer executes the query
// and streams rows into the destination file.
struct CopyExportSpec {
  bool valid;
  std::string inner_query;
  std::string dest_path;
  std::string dest_canonical;
  bool with_header;
  std::string null_str;     // written for NULL values, default empty
  std::string delimiter;    // default ","
  std::string quote;        // default "\""
  CopyExportSpec() : valid(false), with_header(true), delimiter(","),
                     quote("\"") {}
};

struct RewriteResult {
  bool handled;             // false: statement untouched, run normally
  std::string sql;          // rewritten sql to execute (empty when only copy)
  CopyExportSpec copy;      // set when statement was COPY ... TO ...
  RewriteResult() : handled(false) {}
};

// main entry: returns FS_OK (check result.handled / result.sql / result.copy)
// or an error code from FileSqlErr.
int rewrite_file_sql(const std::string &input_sql, const FileSqlSessionCtx &ctx,
                     RewriteResult &result);

// ===========================================================================
// CSV writing helpers (used by COPY execution at the glue layer, exposed for
// unit tests)
// ===========================================================================
// escape one csv field: quote when needed (delimiter/quote/newline present),
// double embedded quotes
std::string csv_escape_field(const std::string &v, const std::string &delim,
                             const std::string &quote);

// format a double the way mysql/duckdb prints shortest round-trip style
// ("87" not "87.0"); fallback %.15g
std::string format_double_for_csv(double d);

// SQL literal escaping (single quote doubled, backslash escaped)
std::string sql_quote_string(const std::string &v);

// quote an identifier with backticks
std::string sql_quote_ident(const std::string &v);

// build the "(SELECT ... UNION ALL SELECT ...) alias" table expression for a
// scanned file; exposed for tests
int build_table_scan_sql(const std::string &func, const std::string &path,
                         const FileScanOptions &opt, const std::string &alias,
                         const std::string &secure_dir, std::string &out_sql);

int build_glob_sql(const std::string &pattern, const std::string &alias,
                   const std::string &secure_dir, std::string &out_sql);

int build_describe_sql(const std::string &func, const std::string &path,
                       const FileScanOptions &opt, const std::string &secure_dir,
                       std::string &out_sql);

} // end namespace filesql
} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_REWRITE_H_ */
