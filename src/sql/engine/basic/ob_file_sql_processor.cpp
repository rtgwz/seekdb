/**
 * Copyright (c) 2026 OceanBase.
 * seekdb competition problem 2: File SQL - OceanBase glue implementation.
 */
#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_sql_processor.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "common/ob_field.h"
#include "common/object/ob_object.h"
#include "common/row/ob_row.h"
#include "common/timezone/ob_time_convert.h"
#include "common/timezone/ob_timezone_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{

using namespace common;

int ObFileSqlProcessor::map_err(int file_sql_err)
{
  int ret = OB_SUCCESS;
  switch (file_sql_err) {
    case filesql::FS_OK: ret = OB_SUCCESS; break;
    case filesql::FS_ERR_NOT_SUPPORTED: ret = OB_NOT_SUPPORTED; break;
    case filesql::FS_ERR_PARSE: ret = OB_ERR_PARSE_SQL; break;
    case filesql::FS_ERR_FILE_NOT_EXIST: ret = OB_FILE_NOT_EXIST; break;
    case filesql::FS_ERR_PATH_VIOLATION: ret = OB_FILE_NOT_EXIST; break;
    case filesql::FS_ERR_IO: ret = OB_IO_ERROR; break;
    case filesql::FS_ERR_BAD_FORMAT: ret = OB_ERR_WRONG_VALUE; break;
    case filesql::FS_ERR_MEM: ret = OB_ALLOCATE_MEMORY_FAILED; break;
    default: ret = OB_ERR_UNEXPECTED; break;
  }
  return ret;
}

int ObFileSqlProcessor::rewrite_stmt(const ObString &stmt,
                                     ObSQLSessionInfo &session,
                                     ObIAllocator &allocator,
                                     ObString &new_stmt,
                                     bool &rewritten,
                                     bool &is_copy,
                                     filesql::CopyExportSpec &copy_spec)
{
  int ret = OB_SUCCESS;
  new_stmt.reset();
  rewritten = false;
  is_copy = false;

  // cheap pre-check: most statements contain none of the markers and the
  // standalone rewriter itself bails out on the first scan, so just call it
  // unconditionally - it only allocates when a marker exists.
  filesql::FileSqlSessionCtx ctx;
  ctx.enable_file_sql = false;
  ctx.secure_file_priv = "";
  bool enable_file_sql = false;
  ObString secure_dir;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,
                                       enable_file_sql))) {
    LOG_WARN("get enable_file_sql failed", K(ret));
  } else if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV,
                                              secure_dir))) {
    LOG_WARN("get secure_file_priv failed", K(ret));
  } else {
    ctx.enable_file_sql = enable_file_sql;
    std::string secure(secure_dir.ptr(), secure_dir.length());
    // NULL/empty value disables restriction (mysql semantics: empty = no
    // limit). The shipped default is "/".
    ctx.secure_file_priv = secure;

    std::string input(stmt.ptr(), stmt.length());
    filesql::RewriteResult result;
    int fs_ret = filesql::rewrite_file_sql(input, ctx, result);
    if (fs_ret != filesql::FS_OK) {
      ret = map_err(fs_ret);
      if (OB_NOT_SUPPORTED == ret) {
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql");
      }
      LOG_WARN("file sql rewrite failed", K(fs_ret), K(ret));
    } else if (result.handled && result.copy.valid) {
      rewritten = true;
      is_copy = true;
      copy_spec = result.copy;
    } else if (result.handled && !result.sql.empty()) {
      char *buf = static_cast<char *>(allocator.alloc(result.sql.size() + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, result.sql.c_str(), result.sql.size());
        buf[result.sql.size()] = '\0';
        new_stmt.assign_ptr(buf, static_cast<ObString::obstr_size_t>(result.sql.size()));
        rewritten = true;
      }
    }
  }
  return ret;
}

int ObFileSqlProcessor::write_copy_row(void *file, const ObNewRow *row,
                                       ObSQLSessionInfo &session,
                                       const filesql::CopyExportSpec &spec)
{
  int ret = OB_SUCCESS;
  static const int64_t COPY_VALUE_BUF_LEN = 4096;
  if (OB_ISNULL(file) || OB_ISNULL(row)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    FILE *f = static_cast<FILE *>(file);
    const ObTimeZoneInfo *tz = session.get_timezone_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < row->get_count(); ++i) {
      const ObObj &obj = row->get_cell(i);
      std::string out;
      if (obj.is_null()) {
        out = spec.null_str;
      } else {
        ObObjType type = obj.get_type();
        char buf[COPY_VALUE_BUF_LEN];
        if (ob_is_int_tc(type)) {
          snprintf(buf, sizeof(buf), "%ld", static_cast<long>(obj.get_int()));
          out = buf;
        } else if (ob_is_uint_tc(type)) {
          snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(obj.get_uint64()));
          out = buf;
        } else if (ob_is_float_tc(type) || ob_is_double_tc(type)) {
          double d = obj.get_double();
          snprintf(buf, sizeof(buf), "%.15g", d);
          out = buf;
        } else if (ob_is_number_tc(type)) {
          int64_t n = obj.get_number().to_string(buf, sizeof(buf) - 1);
          if (n < 0 || n >= COPY_VALUE_BUF_LEN - 1) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            buf[n] = '\0';
            out = buf;
          }
        } else if (ob_is_datetime_tc(type)) {
          int64_t pos = 0;
          if (OB_FAIL(ObTimeConverter::datetime_to_str(obj.get_datetime(), tz,
                                                       0, buf, sizeof(buf) - 1, pos))) {
            LOG_WARN("datetime to str failed", K(ret));
          } else {
            buf[pos] = '\0';
            out = buf;
          }
        } else if (ob_is_date_tc(type)) {
          int64_t pos = 0;
          if (OB_FAIL(ObTimeConverter::date_to_str(obj.get_date(), buf,
                                                   sizeof(buf) - 1, pos))) {
            LOG_WARN("date to str failed", K(ret));
          } else {
            buf[pos] = '\0';
            out = buf;
          }
        } else if (ob_is_string_tc(type) && !obj.is_lob()) {
          const ObString &v = obj.get_varchar();
          out.assign(v.ptr(), v.length());
        } else {
          // bit/year/enum/set/geometry/json etc: best effort via varchar cast
          // is skipped; write empty
          out = "";
        }
      }
      if (OB_SUCC(ret)) {
        std::string escaped = filesql::csv_escape_field(out, spec.delimiter,
                                                        spec.quote);
        if (i > 0 && fwrite(spec.delimiter.data(), 1, spec.delimiter.size(), f)
                       != spec.delimiter.size()) {
          ret = OB_IO_ERROR;
        } else if (fwrite(escaped.data(), 1, escaped.size(), f)
                   != escaped.size()) {
          ret = OB_IO_ERROR;
        }
      }
    }
    if (OB_SUCC(ret)) {
      fputc('\n', f);
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
