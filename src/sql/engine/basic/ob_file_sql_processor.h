/**
 * Copyright (c) 2026 OceanBase.
 * seekdb competition problem 2: File SQL - OceanBase glue layer.
 *
 * Bridges the standalone file-sql rewriter into ObSql: reads session
 * variables (enable_file_sql / secure_file_priv), rewrites the incoming
 * statement text and maps errors to OB error codes.
 */
#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_PROCESSOR_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_PROCESSOR_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/engine/basic/ob_file_sql_rewrite.h"

namespace oceanbase
{
namespace common
{
class ObNewRow;
}
namespace sql
{
class ObSQLSessionInfo;

class ObFileSqlProcessor
{
public:
  // Inspect stmt; when it is a file-sql statement:
  //  - rewritten=true, new_stmt holds the replacement text (allocated from
  //    `allocator`), is_copy=false; or
  //  - rewritten=true, is_copy=true, copy_spec carries the export
  //    specification + rewritten inner query (new_stmt empty).
  // On failure returns the mapped OB error (message already logged for the
  // user where applicable).
  static int rewrite_stmt(const common::ObString &stmt,
                          ObSQLSessionInfo &session,
                          common::ObIAllocator &allocator,
                          common::ObString &new_stmt,
                          bool &rewritten,
                          bool &is_copy,
                          filesql::CopyExportSpec &copy_spec);

  // Append one result row to the COPY destination file as CSV.
  // `file` is a FILE* opened for binary writing (kept as void* to avoid
  // stdio pollution in the header).
  static int write_copy_row(void *file, const common::ObNewRow *row,
                            ObSQLSessionInfo &session,
                            const filesql::CopyExportSpec &spec);

  // Map filesql::FileSqlErr to OB error codes.
  static int map_err(int file_sql_err);
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SQL_PROCESSOR_H_ */
