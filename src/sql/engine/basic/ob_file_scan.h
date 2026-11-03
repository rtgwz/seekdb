/**
 * Copyright (c) 2026 OceanBase.
 * seekdb competition problem 2: File SQL
 * Standalone file scan utilities: CSV / JSONL / Parquet readers, schema
 * inference, glob file discovery and path security checks.
 *
 * This module is intentionally free of OceanBase dependencies so that it can
 * be unit-tested standalone (see unittest/sql/engine/basic/test_file_scan_utils.cpp)
 * and reused by the SQL text-rewrite layer (ob_file_sql_rewrite.*).
 */
#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_H_

#include <stdint.h>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

// ===========================================================================
// error codes (mapped to OB errors at the glue layer)
// ===========================================================================
enum FileSqlErr {
  FS_OK = 0,
  FS_ERR_NOT_SUPPORTED = 1,      // -> ER_NOT_SUPPORTED_YET (1235)
  FS_ERR_PARSE = 2,              // -> ER_PARSE_ERROR (1064)
  FS_ERR_FILE_NOT_EXIST = 3,     // file/dir missing or not readable
  FS_ERR_PATH_VIOLATION = 4,     // secure_file_priv violation / path escape
  FS_ERR_IO = 5,                 // generic IO failure
  FS_ERR_BAD_FORMAT = 6,         // malformed file content
  FS_ERR_MEM = 7,
  FS_ERR_ARG = 8,                // invalid function argument
};

const char *file_sql_err_str(int err);  // human readable message

// ===========================================================================
// column type model (DuckDB compatible inference)
// ===========================================================================
enum FileColType {
  FS_COL_NULL = 0,   // all values are NULL
  FS_COL_BOOLEAN = 1,
  FS_COL_BIGINT = 2,
  FS_COL_DOUBLE = 3,
  FS_COL_DATE = 4,
  FS_COL_DATETIME = 5,
  FS_COL_VARCHAR = 6,
};

const char *file_col_type_str(FileColType t);   // NULL/BOOLEAN/BIGINT/DOUBLE/DATE/DATETIME/VARCHAR

struct FileColumn {
  std::string name;
  FileColType type;
  bool nullable;      // at least one NULL observed
  FileColumn() : type(FS_COL_NULL), nullable(false) {}
  FileColumn(const std::string &n, FileColType t, bool nl)
      : name(n), type(t), nullable(nl) {}
};

// ===========================================================================
// scan options (DuckDB read_csv/read_json style)
// ===========================================================================
struct FileScanOptions {
  bool has_header;           // default true for csv
  std::string format;        // csv / newline_delimited / ndjson / auto
  int64_t sample_size;       // only -1 (scan everything) is allowed
  std::string null_str;      // literal meaning NULL (default: empty field)
  FileScanOptions()
      : has_header(true), format("csv"), sample_size(-1), null_str("") {}
};

// ===========================================================================
// typed value produced when scanning rows
// ===========================================================================
struct FileValue {
  bool is_null;
  FileColType type;   // declared column type (FS_COL_NULL for all-null col)
  std::string str_val;  // text representation (normalized: datetime 'T'->' ',
                        // booleans "1"/"0", numbers canonicalized)
  FileValue() : is_null(true), type(FS_COL_NULL) {}
};

typedef std::vector<FileValue> FileRow;

// row sink; return non-zero to abort the scan
typedef int (*FileRowSink)(void *ctx, const FileRow &row);

// ===========================================================================
// schema inference (full scan; sample_size must be -1)
// ===========================================================================
// infers schema of a csv file. On success fills `schema`.
int infer_csv_schema(const std::string &path, const FileScanOptions &opt,
                     std::vector<FileColumn> &schema);

// infers schema of a newline-delimited json file (union of keys, in first
// seen order; missing keys count as NULL).
int infer_jsonl_schema(const std::string &path, const FileScanOptions &opt,
                       std::vector<FileColumn> &schema);

// infers schema of a parquet file (minimal footer based reader).
int infer_parquet_schema(const std::string &path, std::vector<FileColumn> &schema);

// ===========================================================================
// row scanning (streaming, two passes: schema inference then data pass)
// ===========================================================================
int scan_csv(const std::string &path, const FileScanOptions &opt,
             const std::vector<FileColumn> &schema, FileRowSink sink, void *ctx);
int scan_jsonl(const std::string &path, const FileScanOptions &opt,
               const std::vector<FileColumn> &schema, FileRowSink sink, void *ctx);
int scan_parquet(const std::string &path, const std::vector<FileColumn> &schema,
                 FileRowSink sink, void *ctx);

// ===========================================================================
// glob file discovery: returns the `file` column values (full paths)
// ===========================================================================
int glob_files(const std::string &pattern, std::vector<std::string> &files);

// ===========================================================================
// path security: resolve to canonical path and check it lives under
// `secure_dir`. Empty `secure_dir` disables the check. Returns
// FS_ERR_PATH_VIOLATION when the canonical path escapes the base dir.
// ===========================================================================
int check_secure_path(const std::string &path, const std::string &secure_dir,
                      std::string &canonical);

// variant for write targets that need not exist yet
int check_secure_write_path(const std::string &path, const std::string &secure_dir,
                            std::string &canonical);

// does the file exist and is it a regular file
int check_readable_file(const std::string &path);

// ===========================================================================
// low level helpers exposed for unit tests
// ===========================================================================
// RFC4180 record splitter; consumes one logical record (may span lines).
// `in`  : next input chunk (a raw line WITHOUT trailing '\n')
// `eof` : no more input
// `done`: a full record was assembled into `fields`
// returns FS_OK or error
struct CsvParser {
  std::vector<std::string> fields;   // completed fields of current record
  std::string cur;                   // current field being assembled
  bool in_quotes;
  bool field_started;
  CsvParser() : in_quotes(false), field_started(false) {}
  // feed one line (without newline); returns true when a record is complete
  bool feed_line(const std::string &line);
  // flush at EOF (last record when file does not end with newline)
  bool flush_eof();
  void reset() { fields.clear(); cur.clear(); in_quotes = false; field_started = false; }
};

// classify one raw csv/json string cell into the type lattice; merges `into`
void merge_cell_type(const std::string &raw, FileColType &into, bool &nullable);
// normalize a raw cell into the canonical string for its final type
// (returns is_null=true for empty / nullstr)
FileValue normalize_cell(const std::string &raw, FileColType col_type,
                         const std::string &null_str);
// duplicate header dedup: id, id -> id, id__2; empty header -> columnN
std::vector<std::string> dedup_headers(const std::vector<std::string> &raw);

// parquet: minimal snappy block decompressor (enough for duckdb written files)
int snappy_decompress(const char *src, int64_t len, std::string &out);

} // end namespace filesql
} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_H_ */
