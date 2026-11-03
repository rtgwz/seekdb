/**
 * Copyright (c) 2026 OceanBase.
 * seekdb competition problem 2: File SQL - SQL text rewriter implementation.
 */
#include "ob_file_sql_rewrite.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

inline bool is_ident_char(char c)
{
  return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '#';
}

inline bool is_ident_start(char c)
{
  return isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '#';
}

inline bool ieq(const std::string &a, const char *b)
{
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// keywords that must never be captured as a bare table alias
static bool is_reserved_after_table(const std::string &w)
{
  static const char *kw[] = {
    "where", "group", "order", "having", "limit", "offset", "union", "except",
    "intersect", "join", "inner", "left", "right", "full", "cross", "natural",
    "straight_join", "on", "using", "as", "set", "for", "into", "procedure",
    "use", "index", "ignore", "force", "lock", "unlock", "window", "with",
    "fetch", "rows", "row", "is", "not", "and", "or", "when", "then", "else",
    "end", "from", "select", "by", "asc", "desc", "separator", "window",
    NULL
  };
  for (int i = 0; kw[i] != NULL; ++i) {
    if (ieq(w, kw[i])) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// lexer-ish helpers over the sql text
// ---------------------------------------------------------------------------
struct Scanner {
  const std::string &s;
  size_t p;
  explicit Scanner(const std::string &str) : s(str), p(0) {}

  bool eof() const { return p >= s.size(); }
  char cur() const { return p < s.size() ? s[p] : '\0'; }

  void skip_ws_and_comments()
  {
    while (p < s.size()) {
      char c = s[p];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++p; }
      else if (c == '-' && p + 1 < s.size() && s[p+1] == '-') {
        while (p < s.size() && s[p] != '\n') ++p;
      } else if (c == '#') {
        while (p < s.size() && s[p] != '\n') ++p;
      } else if (c == '/' && p + 1 < s.size() && s[p+1] == '*') {
        p += 2;
        while (p + 1 < s.size() && !(s[p] == '*' && s[p+1] == '/')) ++p;
        if (p + 1 < s.size()) p += 2; else p = s.size();
      } else break;
    }
  }

  bool read_ident(std::string &out)
  {
    if (p >= s.size() || !is_ident_start(s[p])) return false;
    size_t st = p;
    while (p < s.size() && is_ident_char(s[p])) ++p;
    out = s.substr(st, p - st);
    return true;
  }

  bool read_string_lit(std::string &out)
  {
    if (p >= s.size()) return false;
    char q = s[p];
    if (q != '\'' && q != '"') return false;
    ++p;
    out.clear();
    while (p < s.size()) {
      char c = s[p];
      if (c == '\\' && p + 1 < s.size() && q == '\'') {
        out += c; out += s[p+1]; p += 2; continue;
      }
      if (c == q) {
        if (p + 1 < s.size() && s[p+1] == q) { out += q; p += 2; continue; }
        ++p;
        return true;
      }
      out += c;
      ++p;
    }
    return false;  // unterminated
  }

  bool read_number(std::string &out)
  {
    size_t st = p;
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
    bool has_digit = false;
    while (p < s.size()) {
      char c = s[p];
      if (isdigit(static_cast<unsigned char>(c))) { has_digit = true; ++p; }
      else if (c == '.' || c == 'e' || c == 'E'
               || ((c == '+' || c == '-') && p > st && (s[p-1] == 'e' || s[p-1] == 'E'))) {
        ++p;
      } else break;
    }
    if (!has_digit) { p = st; return false; }
    out = s.substr(st, p - st);
    return true;
  }
};

static bool match_paren(const std::string &s, size_t lp, size_t &rp)
{
  int depth = 0;
  size_t p = lp;
  while (p < s.size()) {
    char c = s[p];
    if (c == '\'' || c == '"') {
      Scanner sc(s); sc.p = p;
      std::string tmp;
      if (!sc.read_string_lit(tmp)) return false;
      p = sc.p;
      continue;
    }
    if (c == '(') ++depth;
    else if (c == ')') {
      --depth;
      if (depth == 0) { rp = p; return true; }
    }
    ++p;
  }
  return false;
}

// ---------------------------------------------------------------------------
// function argument model
// ---------------------------------------------------------------------------
struct FuncArg {
  bool is_named;
  std::string key;      // lower-cased when named
  std::string value;    // raw text (unquoted for string literals)
  bool is_string;       // value was a quoted string
  bool is_number;
  FuncArg() : is_named(false), is_string(false), is_number(false) {}
};

static bool parse_bool_value(const std::string &v, bool &out)
{
  if (ieq(v, "true") || v == "1") { out = true; return true; }
  if (ieq(v, "false") || v == "0") { out = false; return true; }
  return false;
}

static std::string lower_copy(const std::string &v)
{
  std::string o = v;
  for (size_t i = 0; i < o.size(); ++i) {
    o[i] = static_cast<char>(tolower(static_cast<unsigned char>(o[i])));
  }
  return o;
}

// parse comma separated arguments: name = value | value
// value := string | number | ident
static int parse_args(const std::string &text, std::vector<FuncArg> &args)
{
  Scanner sc(text);
  while (true) {
    sc.skip_ws_and_comments();
    if (sc.eof()) break;
    // try "ident ="
    size_t save = sc.p;
    std::string tok;
    if (sc.read_ident(tok)) {
      sc.skip_ws_and_comments();
      if (sc.cur() == '=') {
        ++sc.p;
        FuncArg arg;
        arg.is_named = true;
        arg.key = lower_copy(tok);
        sc.skip_ws_and_comments();
        if (sc.cur() == '\'' || sc.cur() == '"') {
          if (!sc.read_string_lit(arg.value)) return FS_ERR_PARSE;
          arg.is_string = true;
        } else if (sc.cur() == '-' || isdigit(static_cast<unsigned char>(sc.cur()))
                   || sc.cur() == '+') {
          if (!sc.read_number(arg.value)) return FS_ERR_PARSE;
          arg.is_number = true;
        } else {
          std::string v;
          if (!sc.read_ident(v)) return FS_ERR_PARSE;
          arg.value = v;
        }
        args.push_back(arg);
      } else {
        FuncArg arg;
        arg.value = tok;   // bare identifier (true/false/null/keyword)
        args.push_back(arg);
      }
    } else if (sc.cur() == '\'' || sc.cur() == '"') {
      FuncArg arg;
      if (!sc.read_string_lit(arg.value)) return FS_ERR_PARSE;
      arg.is_string = true;
      args.push_back(arg);
    } else if (sc.cur() == '-' || isdigit(static_cast<unsigned char>(sc.cur()))
               || sc.cur() == '+') {
      FuncArg arg;
      if (!sc.read_number(arg.value)) return FS_ERR_PARSE;
      arg.is_number = true;
      args.push_back(arg);
    } else {
      return FS_ERR_PARSE;
    }
    sc.skip_ws_and_comments();
    if (sc.cur() == ',') { ++sc.p; }
    // both comma and plain whitespace separate arguments (duckdb style bare
    // option lists such as "format csv, header, nullstr 'NULL'")
    if (sc.eof()) break;
  }
  return FS_OK;
}

// ---------------------------------------------------------------------------
// sql generation
// ---------------------------------------------------------------------------
std::string sql_quote_string(const std::string &v)
{
  std::string out = "'";
  for (size_t i = 0; i < v.size(); ++i) {
    char c = v[i];
    if (c == '\'') out += "''";
    else if (c == '\\') out += "\\\\";
    else out += c;
  }
  out += "'";
  return out;
}

std::string sql_quote_ident(const std::string &v)
{
  std::string out = "`";
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '`') out += "``";
    else out += v[i];
  }
  out += "`";
  return out;
}

std::string format_double_for_csv(double d)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "%.15g", d);
  return buf;
}

static std::string cell_to_sql(const FileValue &v)
{
  if (v.is_null) return "NULL";
  switch (v.type) {
    case FS_COL_BOOLEAN:
    case FS_COL_BIGINT:
      return v.str_val;
    case FS_COL_DOUBLE:
      return "CAST(" + v.str_val + " AS DOUBLE)";
    case FS_COL_DATE:
      return "CAST(" + sql_quote_string(v.str_val) + " AS DATE)";
    case FS_COL_DATETIME:
      return "CAST(" + sql_quote_string(v.str_val) + " AS DATETIME)";
    default:
      return sql_quote_string(v.str_val);
  }
}

struct RowCollector {
  std::vector<FileRow> rows;
  int64_t limit;
  RowCollector() : limit(2000000) {}
};

static int collect_sink(void *ctx, const FileRow &row)
{
  RowCollector *rc = static_cast<RowCollector *>(ctx);
  if (static_cast<int64_t>(rc->rows.size()) >= rc->limit) return FS_ERR_NOT_SUPPORTED;
  rc->rows.push_back(row);
  return FS_OK;
}

static std::string type_cast_null(FileColType t)
{
  switch (t) {
    case FS_COL_BOOLEAN:
    case FS_COL_BIGINT: return "CAST(NULL AS SIGNED)";
    case FS_COL_DOUBLE: return "CAST(NULL AS DOUBLE)";
    case FS_COL_DATE: return "CAST(NULL AS DATE)";
    case FS_COL_DATETIME: return "CAST(NULL AS DATETIME)";
    default: return "NULL";
  }
}

static int build_values_subquery(const std::vector<FileColumn> &schema,
                                 const std::vector<FileRow> &rows,
                                 const std::string &alias, std::string &out)
{
  std::ostringstream os;
  os << "(";
  if (rows.empty()) {
    os << "SELECT ";
    for (size_t c = 0; c < schema.size(); ++c) {
      if (c) os << ", ";
      os << type_cast_null(schema[c].type) << " AS " << sql_quote_ident(schema[c].name);
    }
    if (schema.empty()) os << "1 AS dummy";
    os << " FROM DUAL WHERE 1 = 0";
  } else {
    for (size_t r = 0; r < rows.size(); ++r) {
      if (r) os << " UNION ALL ";
      os << "SELECT ";
      for (size_t c = 0; c < schema.size(); ++c) {
        if (c) os << ", ";
        if (r == 0) {
          os << cell_to_sql(rows[r][c]) << " AS " << sql_quote_ident(schema[c].name);
        } else {
          os << cell_to_sql(rows[r][c]);
        }
      }
    }
  }
  os << ") " << alias;
  out = os.str();
  return FS_OK;
}

static int sniff_and_collect(const std::string &func, const std::string &path,
                             const FileScanOptions &opt,
                             std::vector<FileColumn> &schema,
                             RowCollector &rc)
{
  int ret = FS_OK;
  if (ieq(func, "read_csv")) {
    ret = infer_csv_schema(path, opt, schema);
    if (ret == FS_OK) ret = scan_csv(path, opt, schema, collect_sink, &rc);
  } else if (ieq(func, "read_json") || ieq(func, "read_ndjson")) {
    ret = infer_jsonl_schema(path, opt, schema);
    if (ret == FS_OK) ret = scan_jsonl(path, opt, schema, collect_sink, &rc);
  } else if (ieq(func, "read_parquet")) {
    ret = infer_parquet_schema(path, schema);
    if (ret == FS_OK) ret = scan_parquet(path, schema, collect_sink, &rc);
  } else {
    return FS_ERR_ARG;
  }
  return ret;
}

int build_table_scan_sql(const std::string &func, const std::string &path,
                         const FileScanOptions &opt, const std::string &alias,
                         const std::string &secure_dir, std::string &out_sql)
{
  std::string canonical;
  int ret = check_secure_path(path, secure_dir, canonical);
  if (ret != FS_OK) return ret;
  ret = check_readable_file(canonical);
  if (ret != FS_OK) return ret;
  std::vector<FileColumn> schema;
  RowCollector rc;
  ret = sniff_and_collect(func, canonical, opt, schema, rc);
  if (ret != FS_OK) return ret;
  return build_values_subquery(schema, rc.rows, alias, out_sql);
}

int build_glob_sql(const std::string &pattern, const std::string &alias,
                   const std::string &secure_dir, std::string &out_sql)
{
  std::string dir_part = pattern;
  size_t pos = pattern.rfind('/');
  if (pos != std::string::npos) dir_part = pattern.substr(0, pos);
  else dir_part = ".";
  std::string canonical;
  int ret = check_secure_path(dir_part, secure_dir, canonical);
  if (ret != FS_OK) return ret;
  std::vector<std::string> files;
  ret = glob_files(pattern, files);
  if (ret != FS_OK) return ret;
  std::vector<FileColumn> schema;
  schema.push_back(FileColumn("file", FS_COL_VARCHAR, false));
  RowCollector rc;
  for (size_t i = 0; i < files.size(); ++i) {
    FileRow row;
    FileValue v;
    v.is_null = false;
    v.type = FS_COL_VARCHAR;
    v.str_val = files[i];
    row.push_back(v);
    rc.rows.push_back(row);
  }
  return build_values_subquery(schema, rc.rows, alias, out_sql);
}

int build_describe_sql(const std::string &func, const std::string &path,
                       const FileScanOptions &opt, const std::string &secure_dir,
                       std::string &out_sql)
{
  std::string canonical;
  int ret = check_secure_path(path, secure_dir, canonical);
  if (ret != FS_OK) return ret;
  ret = check_readable_file(canonical);
  if (ret != FS_OK) return ret;
  std::vector<FileColumn> schema;
  if (ieq(func, "read_csv")) {
    ret = infer_csv_schema(canonical, opt, schema);
  } else if (ieq(func, "read_json") || ieq(func, "read_ndjson")) {
    ret = infer_jsonl_schema(canonical, opt, schema);
  } else if (ieq(func, "read_parquet")) {
    ret = infer_parquet_schema(canonical, schema);
  } else {
    return FS_ERR_ARG;
  }
  if (ret != FS_OK) return ret;
  std::ostringstream os;
  os << "SELECT 'column_name' AS `column_name`, 'VARCHAR' AS `column_type`, "
        "'NO' AS `null`, NULL AS `key`, NULL AS `default`, NULL AS `extra` "
        "FROM DUAL WHERE 1 = 0";
  for (size_t i = 0; i < schema.size(); ++i) {
    os << " UNION ALL SELECT " << sql_quote_string(schema[i].name)
       << ", " << sql_quote_string(file_col_type_str(schema[i].type))
       << ", " << (schema[i].nullable ? "'YES'" : "'NO'")
       << ", NULL, NULL, NULL FROM DUAL";
  }
  out_sql = os.str();
  return FS_OK;
}

std::string csv_escape_field(const std::string &v, const std::string &delim,
                             const std::string &quote)
{
  if (v.empty()) return v;
  bool need = v.find(delim) != std::string::npos
           || v.find(quote) != std::string::npos
           || v.find('\n') != std::string::npos
           || v.find('\r') != std::string::npos;
  if (!need) return v;
  std::string out = quote;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v.compare(i, quote.size(), quote) == 0) {
      out += quote; out += quote;   // doubled
      i += quote.size() - 1;
    } else {
      out += v[i];
    }
  }
  out += quote;
  return out;
}

// ---------------------------------------------------------------------------
// function call rewriting
// ---------------------------------------------------------------------------
struct ReadFuncMatch {
  std::string func;
  std::string path;
  FileScanOptions opt;
  std::string alias;
  size_t begin;
  size_t end;
};

static bool is_file_func(const std::string &ident)
{
  return ieq(ident, "read_csv") || ieq(ident, "read_json")
      || ieq(ident, "read_ndjson") || ieq(ident, "read_parquet")
      || ieq(ident, "glob");
}

static bool read_alias_token(Scanner &sc, std::string &alias)
{
  sc.skip_ws_and_comments();
  if (sc.eof()) return false;
  char c = sc.cur();
  if (c == '`' || c == '"') {
    size_t st = sc.p;
    ++sc.p;
    while (sc.p < sc.s.size() && sc.s[sc.p] != c) ++sc.p;
    if (sc.p >= sc.s.size()) return false;
    ++sc.p;
    alias = sc.s.substr(st, sc.p - st);
    return true;
  }
  if (is_ident_start(c)) {
    size_t st = sc.p;
    while (sc.p < sc.s.size() && is_ident_char(sc.s[sc.p])) ++sc.p;
    alias = sc.s.substr(st, sc.p - st);
    return true;
  }
  return false;
}

static int interpret_call(const std::string &func, const std::vector<FuncArg> &args,
                          std::string &path, FileScanOptions &opt)
{
  if (args.empty()) return FS_ERR_PARSE;
  if (!args[0].is_string) return FS_ERR_PARSE;
  path = args[0].value;
  size_t positional = 1;
  if (args.size() > positional && !args[positional].is_named) {
    if (!args[positional].is_string) return FS_ERR_PARSE;
    std::string fmt = args[positional].value;
    if (ieq(func, "read_csv")) {
      if (!ieq(fmt, "csv") && !ieq(fmt, "auto")) return FS_ERR_PARSE;
    } else if (ieq(func, "read_json") || ieq(func, "read_ndjson")) {
      if (!ieq(fmt, "newline_delimited") && !ieq(fmt, "ndjson")
          && !ieq(fmt, "auto")) {
        if (ieq(fmt, "array")) return FS_ERR_NOT_SUPPORTED;
        return FS_ERR_PARSE;
      }
    } else {
      return FS_ERR_PARSE;
    }
    ++positional;
  }
  if (ieq(func, "glob")) {
    if (args.size() != 1) return FS_ERR_PARSE;
    return FS_OK;
  }
  for (size_t i = positional; i < args.size(); ++i) {
    const FuncArg &a = args[i];
    if (!a.is_named) return FS_ERR_PARSE;
    if (a.key == "format") {
      std::string fmt = a.value;
      if (ieq(func, "read_csv")) {
        if (!ieq(fmt, "csv") && !ieq(fmt, "auto")) return FS_ERR_PARSE;
      } else if (ieq(func, "read_json") || ieq(func, "read_ndjson")) {
        if (!ieq(fmt, "newline_delimited") && !ieq(fmt, "ndjson")
            && !ieq(fmt, "auto")) {
          if (ieq(fmt, "array")) return FS_ERR_NOT_SUPPORTED;
          return FS_ERR_PARSE;
        }
      } else if (ieq(func, "read_parquet")) {
        return FS_ERR_PARSE;
      }
      opt.format = fmt;
    } else if (a.key == "header") {
      bool b = false;
      if (!parse_bool_value(a.value, b)) return FS_ERR_PARSE;
      opt.has_header = b;
    } else if (a.key == "sample_size") {
      int64_t v = atoll(a.value.c_str());
      if (v != -1) return FS_ERR_NOT_SUPPORTED;
      opt.sample_size = -1;
    } else if (a.key == "nullstr" || a.key == "null") {
      opt.null_str = a.value;
    } else if (a.key == "delim" || a.key == "delimiter") {
      if (a.value.size() > 1) return FS_ERR_PARSE;
    } else if (a.key == "quote" || a.key == "escape" || a.key == "auto_detect"
               || a.key == "all_varchar" || a.key == "column_names"
               || a.key == "compression" || a.key == "union_by_name"
               || a.key == "columns" || a.key == "max_line_size"
               || a.key == "ignore_errors" || a.key == "filename"
               || a.key == "hive_partitioning" || a.key == "strict_mode") {
      // tolerated no-op options
    } else {
      return FS_ERR_PARSE;
    }
  }
  return FS_OK;
}

// find next file function call from scanner position; parses and validates it
static bool find_next_call(Scanner &sc, ReadFuncMatch &m, int &err)
{
  err = FS_OK;
  while (!sc.eof()) {
    size_t tok_start = sc.p;
    char c = sc.cur();
    if (c == '\'' || c == '"') {
      std::string tmp;
      if (!sc.read_string_lit(tmp)) { err = FS_ERR_PARSE; return false; }
      continue;
    }
    if (c == '`') {
      ++sc.p;
      while (sc.p < sc.s.size() && sc.s[sc.p] != '`') ++sc.p;
      if (sc.p < sc.s.size()) ++sc.p;
      continue;
    }
    if (is_ident_start(c)) {
      std::string ident;
      sc.read_ident(ident);
      if (is_file_func(ident)) {
        size_t save = sc.p;
        sc.skip_ws_and_comments();
        if (sc.cur() == '(') {
          size_t lp = sc.p;
          size_t rp = 0;
          if (!match_paren(sc.s, lp, rp)) { err = FS_ERR_PARSE; return false; }
          std::string argtext = sc.s.substr(lp + 1, rp - lp - 1);
          std::vector<FuncArg> args;
          int pret = parse_args(argtext, args);
          if (pret != FS_OK) { err = pret; return false; }
          m.func = ident;
          int iret = interpret_call(ident, args, m.path, m.opt);
          if (iret != FS_OK) { err = iret; return false; }
          sc.p = rp + 1;
          // alias capture (AS alias | bare alias | none)
          Scanner probe(sc.s);
          probe.p = sc.p;
          probe.skip_ws_and_comments();
          std::string alias;
          if (!probe.eof()) {
            size_t before_peek = probe.p;
            std::string maybe_as;
            if (probe.read_ident(maybe_as) && ieq(maybe_as, "as")) {
              probe.skip_ws_and_comments();
              if (!read_alias_token(probe, alias)) { err = FS_ERR_PARSE; return false; }
              sc.p = probe.p;
            } else {
              probe.p = before_peek;   // unread the identifier we just peeked
              if (is_ident_start(probe.cur()) || probe.cur() == '`') {
                std::string bare;
                Scanner p2(sc.s); p2.p = probe.p;
                if (read_alias_token(p2, bare) && !is_reserved_after_table(bare)) {
                  alias = bare;
                  sc.p = p2.p;
                }
              }
            }
          }
          if (alias.empty()) alias = ident;
          m.alias = alias;
          m.begin = tok_start;
          m.end = sc.p;
          return true;
        }
        sc.p = save;
      }
      continue;
    }
    ++sc.p;
  }
  return false;
}

// rewrite all file function calls inside sql text
static int rewrite_calls(const std::string &in_sql, const FileSqlSessionCtx &ctx,
                         bool &touched, std::string &out_sql)
{
  out_sql = in_sql;
  touched = false;
  int guard = 0;
  while (guard++ < 1000) {
    Scanner sc(out_sql);
    ReadFuncMatch m;
    int err = FS_OK;
    bool found = false;
    while (true) {
      found = find_next_call(sc, m, err);
      if (!found || err != FS_OK) break;
      std::string repl;
      int ret;
      if (ieq(m.func, "glob")) {
        ret = build_glob_sql(m.path, m.alias, ctx.secure_file_priv, repl);
      } else {
        ret = build_table_scan_sql(m.func, m.path, m.opt, m.alias,
                                   ctx.secure_file_priv, repl);
      }
      if (ret != FS_OK) return ret;
      out_sql = out_sql.substr(0, m.begin) + repl + out_sql.substr(m.end);
      touched = true;
      break;  // rescan from start (positions shifted)
    }
    if (err != FS_OK) return err;
    if (!found) break;
  }
  return FS_OK;
}

static bool starts_with_keyword(const std::string &sql, const char *kw)
{
  Scanner sc(sql);
  sc.skip_ws_and_comments();
  std::string ident;
  if (!sc.read_ident(ident)) return false;
  return ieq(ident, kw);
}

// does the sql contain any syntactically valid file function call?
// err carries validation errors that must abort processing
static bool probe_file_marker(const std::string &sql, int &err)
{
  err = FS_OK;
  Scanner probe(sql);
  ReadFuncMatch mm;
  while (true) {
    bool f = find_next_call(probe, mm, err);
    if (err == FS_ERR_PARSE) { err = FS_OK; return false; }  // let ob parser reject
    if (err != FS_OK) return false;
    if (!f) return false;
    return true;
  }
}

int rewrite_file_sql(const std::string &input_sql, const FileSqlSessionCtx &ctx,
                     RewriteResult &result)
{
  result.handled = false;
  result.sql.clear();
  result.copy = CopyExportSpec();
  std::string sql = input_sql;
  {
    size_t e = sql.size();
    while (e > 0 && (sql[e-1] == ';' || isspace(static_cast<unsigned char>(sql[e-1])))) --e;
    sql = sql.substr(0, e);
  }
  if (sql.empty()) return FS_OK;

  int probe_err = FS_OK;
  bool has_marker = probe_file_marker(sql, probe_err);
  if (probe_err != FS_OK) return probe_err;

  bool is_copy = starts_with_keyword(sql, "copy");
  bool is_describe = false;
  if (!is_copy) {
    Scanner sc(sql);
    sc.skip_ws_and_comments();
    std::string ident;
    if (sc.read_ident(ident) && (ieq(ident, "describe") || ieq(ident, "desc"))) {
      sc.skip_ws_and_comments();
      std::string nxt;
      if (sc.read_ident(nxt) && (ieq(nxt, "select") || ieq(nxt, "with"))) {
        is_describe = true;
      }
    }
  }

  if (!is_copy && !is_describe && !has_marker) return FS_OK;

  if (!ctx.enable_file_sql) {
    if (has_marker || is_copy) return FS_ERR_NOT_SUPPORTED;
    return FS_OK;
  }

  if (is_describe && has_marker) {
    Scanner sc(sql);
    ReadFuncMatch m;
    int err = FS_OK;
    while (find_next_call(sc, m, err)) {
      if (!ieq(m.func, "glob")) {
        result.handled = true;
        return build_describe_sql(m.func, m.path, m.opt, ctx.secure_file_priv,
                                  result.sql);
      }
    }
    if (err != FS_OK) return err;
  }

  if (is_copy) {
    Scanner sc(sql);
    sc.skip_ws_and_comments();
    std::string ident;
    if (!sc.read_ident(ident) || !ieq(ident, "copy")) return FS_ERR_PARSE;
    sc.skip_ws_and_comments();
    std::string inner;
    if (sc.cur() == '(') {
      size_t lp = sc.p, rp = 0;
      if (!match_paren(sc.s, lp, rp)) return FS_ERR_PARSE;
      inner = sql.substr(lp + 1, rp - lp - 1);
      sc.p = rp + 1;
    } else {
      std::string t1, t2;
      if (!sc.read_ident(t1)) return FS_ERR_PARSE;
      if (sc.cur() == '.') { ++sc.p; if (!sc.read_ident(t2)) return FS_ERR_PARSE; }
      inner = "SELECT * FROM " + t1 + (t2.empty() ? "" : "." + t2);
    }
    sc.skip_ws_and_comments();
    std::string kw;
    if (!sc.read_ident(kw) || !ieq(kw, "to")) return FS_ERR_PARSE;
    sc.skip_ws_and_comments();
    std::string path;
    if (!sc.read_string_lit(path)) return FS_ERR_PARSE;
    CopyExportSpec spec;
    spec.valid = true;
    spec.dest_path = path;
    spec.with_header = true;
    spec.null_str = "";
    spec.delimiter = ",";
    spec.quote = "\"";
    sc.skip_ws_and_comments();
    if (sc.cur() == '(') {
      size_t lp = sc.p, rp = 0;
      if (!match_paren(sc.s, lp, rp)) return FS_ERR_PARSE;
      std::string optext = sql.substr(lp + 1, rp - lp - 1);
      std::vector<FuncArg> oargs;
      int pret = parse_args(optext, oargs);
      if (pret != FS_OK) return pret;
      // support duckdb style bare words: (FORMAT CSV, HEADER, NULLSTR 'NULL')
      // and named style: (format = 'csv', header = true)
      //
      // first fold the token stream into effective key/value pairs:
      //  - a bare known key starts a new option (flushing a previous
      //    standalone `header` flag first)
      //  - a bare non-key identifier or a string literal completes the
      //    pending option as its value
      struct OptKey { const char *name; bool flag; };
      static const OptKey OPT_KEYS[] = {
        {"format", false}, {"nullstr", false}, {"null", false},
        {"delimiter", false}, {"delim", false}, {"quote", false},
        {"header", true},
      };
      std::vector<FuncArg> effective;
      std::string pending_key;
      bool has_pending = false;
      for (size_t i = 0; i <= oargs.size(); ++i) {
        bool at_end = (i == oargs.size());
        if (at_end) {
          if (has_pending) {
            if (pending_key != "header") return FS_ERR_PARSE;
            FuncArg fa; fa.is_named = true; fa.key = "header"; fa.value = "true";
            effective.push_back(fa);
          }
          break;
        }
        const FuncArg &a = oargs[i];
        if (a.is_named) {
          if (has_pending) {
            if (pending_key != "header") return FS_ERR_PARSE;
            FuncArg fa; fa.is_named = true; fa.key = "header"; fa.value = "true";
            effective.push_back(fa);
            has_pending = false;
          }
          effective.push_back(a);
          continue;
        }
        if (a.is_string) {
          if (!has_pending) return FS_ERR_PARSE;
          FuncArg fa; fa.is_named = true; fa.key = pending_key; fa.value = a.value;
          fa.is_string = true;
          effective.push_back(fa);
          has_pending = false;
          continue;
        }
        std::string ident = lower_copy(a.value);
        bool is_known = false;
        for (size_t k = 0; k < sizeof(OPT_KEYS)/sizeof(OPT_KEYS[0]); ++k) {
          if (ident == OPT_KEYS[k].name) { is_known = true; break; }
        }
        if (is_known) {
          if (has_pending) {
            if (pending_key != "header") return FS_ERR_PARSE;
            FuncArg fa; fa.is_named = true; fa.key = "header"; fa.value = "true";
            effective.push_back(fa);
          }
          pending_key = ident;
          has_pending = true;
        } else {
          if (!has_pending) return FS_ERR_PARSE;
          FuncArg fa; fa.is_named = true; fa.key = pending_key; fa.value = a.value;
          effective.push_back(fa);
          has_pending = false;
        }
      }
      for (size_t i = 0; i < effective.size(); ++i) {
        const FuncArg &a = effective[i];
        std::string key, val;
        bool is_string = false;
        key = a.key; val = a.value; is_string = a.is_string;
        if (key == "format") {
          if (!ieq(val, "csv")) {
            if (ieq(val, "json") || ieq(val, "parquet")) return FS_ERR_NOT_SUPPORTED;
            return FS_ERR_PARSE;
          }
        } else if (key == "header") {
          if (val.empty()) val = "true";
          bool b = true;
          if (!parse_bool_value(val, b)) return FS_ERR_PARSE;
          spec.with_header = b;
        } else if (key == "nullstr" || key == "null") {
          if (!is_string && !ieq(val, "null") && !val.empty()) return FS_ERR_PARSE;
          spec.null_str = val;
        } else if (key == "delimiter" || key == "delim") {
          if (!val.empty() && val.size() > 1) return FS_ERR_PARSE;
          spec.delimiter = val;
        } else if (key == "quote") {
          spec.quote = val;
        } else {
          return FS_ERR_PARSE;
        }
      }
      sc.p = rp + 1;
    }
    sc.skip_ws_and_comments();
    if (!sc.eof()) return FS_ERR_PARSE;
    std::string canonical;
    int ret = check_secure_write_path(spec.dest_path, ctx.secure_file_priv, canonical);
    if (ret != FS_OK) return ret;
    spec.dest_canonical = canonical;
    bool touched = false;
    std::string inner_rewritten;
    ret = rewrite_calls(inner, ctx, touched, inner_rewritten);
    if (ret != FS_OK) return ret;
    spec.inner_query = inner_rewritten;
    result.handled = true;
    result.copy = spec;
    return FS_OK;
  }

  {
    bool touched = false;
    std::string out;
    int ret = rewrite_calls(sql, ctx, touched, out);
    if (ret != FS_OK) return ret;
    if (touched) {
      result.handled = true;
      result.sql = out;
    }
    return FS_OK;
  }
}

}  // namespace filesql
}  // namespace sql
}  // namespace oceanbase
