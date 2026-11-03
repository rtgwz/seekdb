/**
 * Copyright (c) 2026 OceanBase.
 * seekdb competition problem 2: File SQL - file scan utilities.
 * Standalone implementation (no OceanBase dependencies).
 */
#include "ob_file_scan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

const char *file_sql_err_str(int err)
{
  switch (err) {
    case FS_OK: return "ok";
    case FS_ERR_NOT_SUPPORTED: return "not supported";
    case FS_ERR_PARSE: return "parse error";
    case FS_ERR_FILE_NOT_EXIST: return "file not exist";
    case FS_ERR_PATH_VIOLATION: return "path violates secure_file_priv";
    case FS_ERR_IO: return "io error";
    case FS_ERR_BAD_FORMAT: return "bad file format";
    case FS_ERR_MEM: return "out of memory";
    case FS_ERR_ARG: return "invalid argument";
    default: return "unknown error";
  }
}

const char *file_col_type_str(FileColType t)
{
  switch (t) {
    case FS_COL_NULL: return "NULL";
    case FS_COL_BOOLEAN: return "BOOLEAN";
    case FS_COL_BIGINT: return "BIGINT";
    case FS_COL_DOUBLE: return "DOUBLE";
    case FS_COL_DATE: return "DATE";
    case FS_COL_DATETIME: return "DATETIME";
    case FS_COL_VARCHAR: return "VARCHAR";
    default: return "VARCHAR";
  }
}

// ===========================================================================
// type lattice
// ===========================================================================
static bool is_true_false(const std::string &s)
{
  return s == "true" || s == "false" || s == "True" || s == "False"
      || s == "TRUE" || s == "FALSE";
}

static bool is_integer_str(const std::string &s, int64_t &out)
{
  if (s.empty() || s.size() > 20) return false;
  size_t i = 0;
  if (s[0] == '+' || s[0] == '-') i = 1;
  if (i >= s.size()) return false;
  for (size_t j = i; j < s.size(); ++j) {
    if (!isdigit(static_cast<unsigned char>(s[j]))) return false;
  }
  errno = 0;
  const char *p = s.c_str();
  char *end = NULL;
  long long v = strtoll(p, &end, 10);
  if (end == p || errno == ERANGE) {
    // huge integers fall back to double semantics at caller; treat as not int
    return false;
  }
  out = v;
  return true;
}

static bool is_double_str(const std::string &s, double &out)
{
  if (s.empty()) return false;
  // must look numeric: digits with optional single . eE sign
  size_t i = 0;
  if (s[0] == '+' || s[0] == '-') i = 1;
  bool has_digit = false, has_dot = false, has_exp = false;
  for (; i < s.size(); ++i) {
    char c = s[i];
    if (isdigit(static_cast<unsigned char>(c))) { has_digit = true; }
    else if (c == '.' && !has_dot && !has_exp) { has_dot = true; }
    else if ((c == 'e' || c == 'E') && has_digit && !has_exp) { has_exp = true; }
    else if ((c == '+' || c == '-') && i > 0 && (s[i-1]=='e' || s[i-1]=='E')) { }
    else { return false; }
  }
  if (!has_digit) return false;
  errno = 0;
  char *end = NULL;
  double d = strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size() || errno == ERANGE) return false;
  if (std::isnan(d) || std::isinf(d)) return false;
  out = d;
  return true;
}

static bool valid_date(int y, int m, int d)
{
  if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
  static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int md = mdays[m-1];
  if (m == 2 && ((y%4==0 && y%100!=0) || y%400==0)) md = 29;
  return d <= md;
}

// YYYY-MM-DD
static bool is_date_str(const std::string &s)
{
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  int y = atoi(s.substr(0,4).c_str()), m = atoi(s.substr(5,2).c_str()),
      d = atoi(s.substr(8,2).c_str());
  return valid_date(y, m, d);
}

// YYYY-MM-DD[T ]HH:MM:SS[.frac]
static bool is_datetime_str(const std::string &s, size_t &date_len)
{
  if (s.size() < 19) return false;
  if (!is_date_str(s.substr(0, 10))) return false;
  if (s[10] != 'T' && s[10] != ' ') return false;
  for (int i = 11; i < 19; ++i) {
    if (i == 13 || i == 16) {
      if (s[i] != ':') return false;
    } else if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  int hh = atoi(s.substr(11,2).c_str()), mi = atoi(s.substr(14,2).c_str()),
      ss = atoi(s.substr(17,2).c_str());
  if (hh > 23 || mi > 59 || ss > 61) return false;
  if (s.size() > 19) {
    if (s[19] != '.') return false;
    for (size_t i = 20; i < s.size(); ++i) {
      if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
  }
  date_len = s.size();
  return true;
}

static FileColType classify_cell(const std::string &raw)
{
  if (raw.empty()) return FS_COL_NULL;
  if (is_true_false(raw)) return FS_COL_BOOLEAN;
  int64_t iv = 0; double dv = 0;
  if (is_integer_str(raw, iv)) return FS_COL_BIGINT;
  if (is_double_str(raw, dv)) return FS_COL_DOUBLE;
  if (is_date_str(raw)) return FS_COL_DATE;
  size_t dl = 0;
  if (is_datetime_str(raw, dl)) return FS_COL_DATETIME;
  return FS_COL_VARCHAR;
}

void merge_cell_type(const std::string &raw, FileColType &into, bool &nullable)
{
  FileColType t = classify_cell(raw);
  if (t == FS_COL_NULL) { nullable = true; return; }
  if (into == FS_COL_NULL) { into = t; return; }
  if (into == t) return;
  if (into == FS_COL_VARCHAR || t == FS_COL_VARCHAR) { into = FS_COL_VARCHAR; return; }
  if ((into == FS_COL_BIGINT && t == FS_COL_DOUBLE)
      || (into == FS_COL_DOUBLE && t == FS_COL_BIGINT)) { into = FS_COL_DOUBLE; return; }
  if ((into == FS_COL_DATE && t == FS_COL_DATETIME)
      || (into == FS_COL_DATETIME && t == FS_COL_DATE)) { into = FS_COL_DATETIME; return; }
  into = FS_COL_VARCHAR;
}

FileValue normalize_cell(const std::string &raw, FileColType col_type,
                         const std::string &null_str)
{
  FileValue v;
  v.type = col_type;
  if (raw.empty() || raw == null_str) { v.is_null = true; return v; }
  v.is_null = false;
  switch (col_type) {
    case FS_COL_BOOLEAN: {
      const char *p = raw.c_str();
      v.str_val = (strcasecmp(p, "true") == 0 || raw == "1") ? "1" : "0";
      break;
    }
    case FS_COL_BIGINT: {
      int64_t iv = 0;
      if (is_integer_str(raw, iv)) {
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)iv);
        v.str_val = buf;
      } else {
        v.str_val = raw;  // should not happen
      }
      break;
    }
    case FS_COL_DOUBLE: {
      double dv = 0;
      if (is_double_str(raw, dv)) {
        char buf[64]; snprintf(buf, sizeof(buf), "%.15g", dv);
        v.str_val = buf;
      } else {
        v.str_val = raw;
      }
      break;
    }
    case FS_COL_DATE:
      v.str_val = raw;    // canonical already
      break;
    case FS_COL_DATETIME: {
      v.str_val = raw;
      if (v.str_val[10] == 'T') v.str_val[10] = ' ';
      // keep fractional seconds
      break;
    }
    case FS_COL_NULL:
      v.is_null = true;
      break;
    default:
      v.str_val = raw;
      break;
  }
  return v;
}

std::vector<std::string> dedup_headers(const std::vector<std::string> &raw)
{
  std::map<std::string, int> seen;
  std::vector<std::string> out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    std::string name = raw[i];
    if (name.empty()) {
      char buf[32]; snprintf(buf, sizeof(buf), "column%lu", (unsigned long)i);
      name = buf;
    }
    std::map<std::string,int>::iterator it = seen.find(name);
    if (it == seen.end()) {
      seen[name] = 1;
      out.push_back(name);
    } else {
      char buf[64]; snprintf(buf, sizeof(buf), "%s__%d", name.c_str(), it->second + 1);
      it->second += 1;
      out.push_back(buf);
    }
  }
  return out;
}

// ===========================================================================
// CSV parser (RFC4180 + multiline quotes)
// ===========================================================================
bool CsvParser::feed_line(const std::string &line)
{
  if (!in_quotes) {
    // skip totally empty line when no record in progress (blank line between
    // records / trailing newline)
    if (line.empty() && fields.empty() && !field_started) return false;
  }
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i+1] == '"') { cur += '"'; ++i; }
        else { in_quotes = false; }
      } else {
        cur += c;
      }
    } else {
      if (c == '"' && cur.empty() && !field_started) {
        in_quotes = true;
        field_started = true;
      } else if (c == ',') {
        fields.push_back(cur);
        cur.clear();
        field_started = false;
      } else {
        cur += c;
        field_started = true;
      }
    }
  }
  if (in_quotes) {
    cur += '\n';   // newline inside quoted field
    return false;
  }
  fields.push_back(cur);
  cur.clear();
  field_started = false;
  return true;
}

bool CsvParser::flush_eof()
{
  if (in_quotes || field_started || !cur.empty() || !fields.empty()) {
    // unterminated quote: treat what we have as the field content
    fields.push_back(cur);
    cur.clear();
    in_quotes = false;
    field_started = false;
    return true;
  }
  return false;
}

// stream csv records; cb returns FS_OK to continue
static int foreach_csv_record(const std::string &path, char delim,
                              int (*cb)(void *, const std::vector<std::string> &, bool &),
                              void *ctx)
{
  std::ifstream in(path.c_str(), std::ios::in);
  if (!in.is_open()) return FS_ERR_FILE_NOT_EXIST;
  CsvParser parser;
  std::string line;
  bool eof = false;
  int ret = FS_OK;
  while (!eof) {
    if (std::getline(in, line)) {
      // strip trailing '\r' of CRLF when not inside quotes
      if (!parser.in_quotes && !line.empty() && line[line.size()-1] == '\r') {
        line.erase(line.size()-1);
      }
      if (parser.feed_line(line)) {
        bool cont = true;
        ret = cb(ctx, parser.fields, cont);
        parser.reset();
        if (ret != FS_OK || !cont) { in.close(); return ret; }
      }
    } else {
      eof = true;
      if (parser.flush_eof()) {
        bool cont = true;
        ret = cb(ctx, parser.fields, cont);
        parser.reset();
      }
    }
  }
  (void)delim;
  in.close();
  return FS_OK;
}

struct CsvInferCtx {
  std::vector<FileColumn> *schema;
  const FileScanOptions *opt;
  bool first_record;
  int64_t row_count;
  CsvInferCtx() : schema(NULL), opt(NULL), first_record(true), row_count(0) {}
};

static int csv_infer_cb(void *p, const std::vector<std::string> &rec, bool &)
{
  CsvInferCtx *ctx = static_cast<CsvInferCtx *>(p);
  if (ctx->first_record && ctx->opt->has_header) {
    ctx->first_record = false;
    std::vector<std::string> names = dedup_headers(rec);
    for (size_t i = 0; i < names.size(); ++i) {
      FileColumn col(names[i], FS_COL_NULL, false);
      ctx->schema->push_back(col);
    }
    if (names.empty()) return FS_ERR_BAD_FORMAT;
    return FS_OK;
  }
  ctx->first_record = false;
  if (ctx->schema->empty()) {
    // no header: synthesize column names from first record width
    for (size_t i = 0; i < rec.size(); ++i) {
      char buf[32]; snprintf(buf, sizeof(buf), "column%lu", (unsigned long)i);
      FileColumn col(buf, FS_COL_NULL, false);
      ctx->schema->push_back(col);
    }
  }
  for (size_t i = 0; i < ctx->schema->size(); ++i) {
    const std::string &cell = i < rec.size() ? rec[i] : "";
    merge_cell_type(cell, (*ctx->schema)[i].type, (*ctx->schema)[i].nullable);
  }
  ++ctx->row_count;
  return FS_OK;
}

int infer_csv_schema(const std::string &path, const FileScanOptions &opt,
                     std::vector<FileColumn> &schema)
{
  schema.clear();
  CsvInferCtx ctx;
  ctx.schema = &schema;
  ctx.opt = &opt;
  int ret = foreach_csv_record(path, ',', csv_infer_cb, &ctx);
  if (ret != FS_OK) schema.clear();
  return ret;
}

static int csv_scan_cb(void *p, const std::vector<std::string> &rec, bool &)
{
  struct Ctx { const std::vector<FileColumn> *schema; FileRowSink sink; void *sctx;
               bool skip_first; };
  Ctx *ctx = static_cast<Ctx *>(p);
  if (ctx->skip_first) {
    ctx->skip_first = false;
    return FS_OK;   // header record already consumed during schema inference
  }
  FileRow row;
  row.reserve(ctx->schema->size());
  for (size_t i = 0; i < ctx->schema->size(); ++i) {
    const std::string &cell = i < rec.size() ? rec[i] : "";
    row.push_back(normalize_cell(cell, (*ctx->schema)[i].type, ""));
  }
  return ctx->sink(ctx->sctx, row);
}

int scan_csv(const std::string &path, const FileScanOptions &opt,
             const std::vector<FileColumn> &schema, FileRowSink sink, void *sctx)
{
  struct Ctx { const std::vector<FileColumn> *schema; FileRowSink sink; void *sctx;
               bool skip_first; };
  Ctx ctx; ctx.schema = &schema; ctx.sink = sink; ctx.sctx = sctx;
  ctx.skip_first = opt.has_header;
  return foreach_csv_record(path, ',', csv_scan_cb, &ctx);
}

// ===========================================================================
// minimal JSON parser (enough for flat newline-delimited json records)
// ===========================================================================
class MiniJson {
public:
  // value kinds
  enum Kind { J_NULL, J_BOOL, J_INT, J_DOUBLE, J_STRING, J_ARRAY, J_OBJECT };
  struct Value {
    Kind kind;
    bool b;
    int64_t i;
    double d;
    std::string s;                        // string value / raw text
    std::vector<std::pair<std::string, Value> > members; // object (ordered)
    Value() : kind(J_NULL), b(false), i(0), d(0) {}
  };
  static int parse(const std::string &text, Value &out)
  {
    MiniJson p;
    return p.parse_impl(text, out);
  }
  int parse_impl(const std::string &text, Value &out)
  {
    p_ = text.c_str(); end_ = p_ + text.size();
    int ret = parse_value(out);
    if (ret != 0) return ret;
    skip_ws();
    if (p_ != end_) return -1;   // trailing garbage
    return 0;
  }
private:
  const char *p_; const char *end_;
  void skip_ws() { while (p_ < end_ && (*p_==' '||*p_=='\t'||*p_=='\r'||*p_=='\n')) ++p_; }
  bool eat(char c) { skip_ws(); if (p_ < end_ && *p_ == c) { ++p_; return true; } return false; }
  int parse_value(Value &v)
  {
    skip_ws();
    if (p_ >= end_) return -1;
    char c = *p_;
    if (c == '{') return parse_object(v);
    if (c == '[') return parse_array(v);
    if (c == '"') { v.kind = J_STRING; return parse_string(v.s); }
    if (c == 't') { if (end_-p_>=4 && strncmp(p_,"true",4)==0) { p_+=4; v.kind=J_BOOL; v.b=true; return 0;} return -1; }
    if (c == 'f') { if (end_-p_>=5 && strncmp(p_,"false",5)==0) { p_+=5; v.kind=J_BOOL; v.b=false; return 0;} return -1; }
    if (c == 'n') { if (end_-p_>=4 && strncmp(p_,"null",4)==0) { p_+=4; v.kind=J_NULL; return 0;} return -1; }
    // number
    {
      const char *start = p_;
      if (*p_=='+'||*p_=='-'||*p_=='.') ++p_;
      bool isd = false;
      while (p_ < end_) {
        char ch = *p_;
        if (isdigit(static_cast<unsigned char>(ch))) { ++p_; isd = true; }
        else if (ch=='.'||ch=='e'||ch=='E'||((ch=='+'||ch=='-')&&(p_>start&&(p_[-1]=='e'||p_[-1]=='E')))) { ++p_; }
        else break;
      }
      if (!isd) return -1;
      std::string tok(start, p_ - start);
      errno = 0;
      char *e = NULL;
      if (tok.find('.') == std::string::npos && tok.find('e') == std::string::npos
          && tok.find('E') == std::string::npos) {
        long long iv = strtoll(tok.c_str(), &e, 10);
        if (e && *e == '\0' && errno != ERANGE) { v.kind = J_INT; v.i = iv; return 0; }
      }
      double dv = strtod(tok.c_str(), &e);
      if (e && *e == '\0') { v.kind = J_DOUBLE; v.d = dv; return 0; }
      return -1;
    }
  }
  int parse_string(std::string &out)
  {
    if (!eat('"')) return -1;
    out.clear();
    while (p_ < end_) {
      unsigned char c = *p_++;
      if (c == '"') return 0;
      if (c == '\\') {
        if (p_ >= end_) return -1;
        char e = *p_++;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            if (end_ - p_ < 4) return -1;
            unsigned int cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = *p_++; cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
              else return -1;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6
                && p_[0] == '\\' && p_[1] == 'u') {
              unsigned int lo = 0; p_ += 2;
              for (int k = 0; k < 4; ++k) {
                char h = *p_++; lo <<= 4;
                if (h >= '0' && h <= '9') lo |= (h - '0');
                else if (h >= 'a' && h <= 'f') lo |= (h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') lo |= (h - 'A' + 10);
                else { return -1; }
              }
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              }
            }
            // utf8 encode
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
              out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
              out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F));
              out += (char)(0x80 | (cp & 0x3F));
            } else {
              out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
              out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
            }
            break;
          }
          default: return -1;
        }
      } else {
        out += (char)c;
      }
    }
    return -1;
  }
  int parse_object(Value &v)
  {
    if (!eat('{')) return -1;
    v.kind = J_OBJECT;
    skip_ws();
    if (eat('}')) return 0;
    while (true) {
      std::string key;
      if (parse_string(key) != 0) return -1;
      if (!eat(':')) return -1;
      Value val;
      if (parse_value(val) != 0) return -1;
      v.members.push_back(std::make_pair(key, val));
      skip_ws();
      if (eat(',')) continue;
      if (eat('}')) return 0;
      return -1;
    }
  }
  int parse_array(Value &v)
  {
    if (!eat('[')) return -1;
    v.kind = J_ARRAY;
    skip_ws();
    if (eat(']')) return 0;
    while (true) {
      Value val;
      if (parse_value(val) != 0) return -1;
      v.members.push_back(std::make_pair("", val));  // store as pseudo members
      skip_ws();
      if (eat(',')) continue;
      if (eat(']')) return 0;
      return -1;
    }
  }
};

// convert a parsed json value to a raw text cell (for the type sniffer)
static void json_value_to_cell(const MiniJson::Value &v, bool &is_null, std::string &cell)
{
  is_null = false;
  cell.clear();
  switch (v.kind) {
    case MiniJson::J_NULL: is_null = true; return;
    case MiniJson::J_BOOL: cell = v.b ? "true" : "false"; return;
    case MiniJson::J_INT: {
      char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v.i); cell = buf; return;
    }
    case MiniJson::J_DOUBLE: {
      char buf[64]; snprintf(buf, sizeof(buf), "%.15g", v.d); cell = buf; return;
    }
    case MiniJson::J_STRING: cell = v.s; return;
    default:
      // array/object: keep a compact textual form (approximate: json text)
      cell = "<json>";
      return;
  }
}

struct JsonlInferCtx {
  std::vector<FileColumn> *schema;
  std::map<std::string, int> index;
  JsonlInferCtx() : schema(NULL) {}
};

static int jsonl_infer_line(void *p, const std::string &line, int64_t)
{
  JsonlInferCtx *ctx = static_cast<JsonlInferCtx *>(p);
  if (line.empty()) return FS_OK;  // tolerate blank lines
  MiniJson::Value root;
  if (MiniJson::parse(line, root) != 0 || root.kind != MiniJson::J_OBJECT) {
    return FS_ERR_BAD_FORMAT;
  }
  for (size_t i = 0; i < root.members.size(); ++i) {
    const std::string &key = root.members[i].first;
    bool is_null = false; std::string cell;
    json_value_to_cell(root.members[i].second, is_null, cell);
    std::map<std::string,int>::iterator it = ctx->index.find(key);
    FileColumn *col = NULL;
    if (it == ctx->index.end()) {
      FileColumn c(key, FS_COL_NULL, false);
      ctx->schema->push_back(c);
      ctx->index[key] = static_cast<int>(ctx->schema->size()) - 1;
      col = &(*ctx->schema)[ctx->schema->size() - 1];
    } else {
      col = &(*ctx->schema)[it->second];
    }
    if (is_null) col->nullable = true;
    else merge_cell_type(cell, col->type, col->nullable);
  }
  // keys absent from this record count as NULL for union-by-name semantics
  if (root.members.size() < ctx->schema->size()) {
    for (size_t i = 0; i < ctx->schema->size(); ++i) {
      bool present = false;
      for (size_t k = 0; k < root.members.size(); ++k) {
        if (root.members[k].first == (*ctx->schema)[i].name) { present = true; break; }
      }
      if (!present) (*ctx->schema)[i].nullable = true;
    }
  }
  return FS_OK;
}

int infer_jsonl_schema(const std::string &path, const FileScanOptions &,
                       std::vector<FileColumn> &schema)
{
  schema.clear();
  std::ifstream in(path.c_str());
  if (!in.is_open()) return FS_ERR_FILE_NOT_EXIST;
  JsonlInferCtx ctx; ctx.schema = &schema;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line[line.size()-1] == '\r') line.erase(line.size()-1);
    int ret = jsonl_infer_line(&ctx, line, 0);
    if (ret != FS_OK) { schema.clear(); return ret; }
  }
  return FS_OK;
}

int scan_jsonl(const std::string &path, const FileScanOptions &,
               const std::vector<FileColumn> &schema, FileRowSink sink, void *sctx)
{
  std::ifstream in(path.c_str());
  if (!in.is_open()) return FS_ERR_FILE_NOT_EXIST;
  std::map<std::string, size_t> index;
  for (size_t i = 0; i < schema.size(); ++i) index[schema[i].name] = i;
  std::string line;
  int ret = FS_OK;
  while (std::getline(in, line)) {
    if (!line.empty() && line[line.size()-1] == '\r') line.erase(line.size()-1);
    if (line.empty()) continue;
    MiniJson::Value root;
    if (MiniJson::parse(line, root) != 0 || root.kind != MiniJson::J_OBJECT) {
      return FS_ERR_BAD_FORMAT;
    }
    FileRow row(schema.size());
    for (size_t i = 0; i < schema.size(); ++i) { row[i].is_null = true; row[i].type = schema[i].type; }
    for (size_t i = 0; i < root.members.size(); ++i) {
      std::map<std::string,size_t>::iterator it = index.find(root.members[i].first);
      if (it == index.end()) continue;  // unknown key, ignore in data pass
      size_t idx = it->second;
      bool is_null = false; std::string cell;
      json_value_to_cell(root.members[i].second, is_null, cell);
      if (is_null) { row[idx].is_null = true; }
      else { row[idx] = normalize_cell(cell, schema[idx].type, ""); }
    }
    ret = sink(sctx, row);
    if (ret != FS_OK) break;
  }
  return ret == FS_OK ? FS_OK : ret;
}

// ===========================================================================
// snappy block decompressor
// ===========================================================================
int snappy_decompress(const char *src, int64_t len, std::string &out)
{
  int64_t i = 0;
  uint64_t ulen = 0;
  int shift = 0;
  while (true) {
    if (i >= len) return FS_ERR_BAD_FORMAT;
    unsigned char b = static_cast<unsigned char>(src[i++]);
    ulen |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80)) break;
    shift += 7;
    if (shift > 32) return FS_ERR_BAD_FORMAT;
  }
  if (ulen > (1ull << 30)) return FS_ERR_BAD_FORMAT;  // 1G safety cap
  out.clear();
  out.reserve(static_cast<size_t>(ulen));
  while (i < len) {
    unsigned char tag = static_cast<unsigned char>(src[i++]);
    unsigned int type = tag & 3;
    if (type == 0) {  // literal
      uint64_t ln = (tag >> 2) + 1;
      if (ln > 60) {
        uint64_t extra = ln - 60;
        if (i + static_cast<int64_t>(extra) > len) return FS_ERR_BAD_FORMAT;
        ln = 0;
        for (uint64_t k = 0; k < extra; ++k) {
          ln |= static_cast<uint64_t>(static_cast<unsigned char>(src[i + k])) << (8 * k);
        }
        i += static_cast<int64_t>(extra);
        ln += 1;
      }
      if (i + static_cast<int64_t>(ln) > len) return FS_ERR_BAD_FORMAT;
      out.append(src + i, static_cast<size_t>(ln));
      i += static_cast<int64_t>(ln);
    } else if (type == 1) {  // copy, 1-byte offset
      if (i >= len) return FS_ERR_BAD_FORMAT;
      uint64_t ln = 4 + ((tag >> 2) & 0x7);
      uint64_t off = (static_cast<uint64_t>(tag >> 5) << 8)
                   | static_cast<unsigned char>(src[i++]);
      if (off == 0 || off > out.size()) return FS_ERR_BAD_FORMAT;
      for (uint64_t k = 0; k < ln; ++k) {
        out += out[out.size() - off];
      }
    } else if (type == 2) {  // copy, 2-byte offset
      if (i + 2 > len) return FS_ERR_BAD_FORMAT;
      uint64_t ln = (tag >> 2) + 1;
      uint64_t off = static_cast<unsigned char>(src[i])
                   | (static_cast<uint64_t>(static_cast<unsigned char>(src[i+1])) << 8);
      i += 2;
      if (off == 0 || off > out.size()) return FS_ERR_BAD_FORMAT;
      for (uint64_t k = 0; k < ln; ++k) out += out[out.size() - off];
    } else {  // copy, 4-byte offset
      if (i + 4 > len) return FS_ERR_BAD_FORMAT;
      uint64_t ln = (tag >> 2) + 1;
      uint64_t off = 0;
      for (int k = 0; k < 4; ++k) {
        off |= static_cast<uint64_t>(static_cast<unsigned char>(src[i + k])) << (8 * k);
      }
      i += 4;
      if (off == 0 || off > out.size()) return FS_ERR_BAD_FORMAT;
      for (uint64_t k = 0; k < ln; ++k) out += out[out.size() - off];
    }
  }
  if (out.size() != ulen) return FS_ERR_BAD_FORMAT;
  return FS_OK;
}

// ===========================================================================
// parquet minimal reader
// ===========================================================================
namespace parquet {

struct ThriftReader {
  const char *b;
  int64_t len;
  int64_t i;
  ThriftReader(const char *buf, int64_t n) : b(buf), len(n), i(0) {}
  bool eof() const { return i >= len; }
  unsigned char byte() { return i < len ? static_cast<unsigned char>(b[i++]) : 0; }
  uint64_t uvarint() {
    uint64_t r = 0; int shift = 0;
    while (true) {
      if (i >= len) return r;
      unsigned char c = static_cast<unsigned char>(b[i++]);
      r |= static_cast<uint64_t>(c & 0x7f) << shift;
      if (!(c & 0x80)) return r;
      shift += 7;
      if (shift > 63) return r;
    }
  }
  int64_t zigzag() {
    uint64_t n = uvarint();
    return static_cast<int64_t>(n >> 1) ^ -static_cast<int64_t>(n & 1);
  }
  std::string binary() {
    uint64_t n = uvarint();
    if (i + static_cast<int64_t>(n) > len) return std::string();
    std::string s(b + i, static_cast<size_t>(n));
    i += static_cast<int64_t>(n);
    return s;
  }
};

// thrift compact type ids
static const int TT_TRUE = 1, TT_FALSE = 2, TT_I8 = 3, TT_I16 = 4, TT_I32 = 5,
                 TT_I64 = 6, TT_DOUBLE = 7, TT_BINARY = 8, TT_LIST = 9,
                 TT_SET = 10, TT_MAP = 11, TT_STRUCT = 12;

// generic struct field walker: calls back for each field; when the callback
// returns non-zero the field value was consumed by it, otherwise the value
// is skipped structurally.
struct FieldWalk {
  // returns 0 struct ended, non-zero error
  static int walk(ThriftReader &r, int (*cb)(void *, int16_t fid, int ttype,
                                             ThriftReader &, bool in_list, void *),
                  void *ctx)
  {
    int16_t last_fid = 0;
    while (true) {
      if (r.eof()) return -1;
      unsigned char h = r.byte();
      if (h == 0) return 0;  // STOP
      int ttype = h & 0xf;
      int16_t fid = last_fid;
      if (h >> 4) fid = static_cast<int16_t>(last_fid + (h >> 4));
      else fid = static_cast<int16_t>(r.zigzag());
      last_fid = fid;
      bool consumed = (cb != NULL && cb(ctx, fid, ttype, r, false, NULL) != 0);
      if (!consumed) {
        int ret = skip_or_cb(r, fid, ttype, NULL, NULL, true);
        if (ret != 0) return ret;
      }
    }
  }
  // structural skip of one value of the given thrift type; cb unused
  static int skip_or_cb(ThriftReader &r, int16_t fid, int ttype,
                        int (*cb)(void *, int16_t, int, ThriftReader &, bool, void *),
                        void *ctx, bool in_list)
  {
    (void)fid; (void)cb; (void)ctx; (void)in_list;
    switch (ttype) {
      case TT_TRUE:
      case TT_FALSE:
        break;
      case TT_I8: r.byte(); break;
      case TT_I16:
      case TT_I32:
      case TT_I64: r.zigzag(); break;
      case TT_DOUBLE: r.i += 8; break;
      case TT_BINARY: r.binary(); break;
      case TT_LIST:
      case TT_SET: {
        unsigned char h = r.byte();
        int et = h & 0xf;
        uint64_t cnt = (h >> 4) ? (h >> 4) : r.uvarint();
        for (uint64_t k = 0; k < cnt; ++k) {
          skip_or_cb(r, static_cast<int16_t>(-1), et, NULL, NULL, true);
        }
        break;
      }
      case TT_MAP: {
        uint64_t cnt = r.uvarint();
        if (cnt == 0) break;
        unsigned char tt = r.byte();
        int kt = tt & 0xf, vt = (tt >> 4) & 0xf;
        for (uint64_t k = 0; k < cnt; ++k) {
          skip_or_cb(r, -1, kt, NULL, NULL, true);
          skip_or_cb(r, -1, vt, NULL, NULL, true);
        }
        break;
      }
      case TT_STRUCT: {
        int ret = walk(r, NULL, NULL);
        if (ret != 0) return ret;
        break;
      }
      default:
        return -1;
    }
    return 0;
  }
};

// ---- parsed metadata model ------------------------------------------------
struct SchemaElement {
  int32_t type;            // physical type, -1 for root
  int32_t repetition;      // 0 required 1 optional 2 repeated, -1 unknown
  std::string name;
  int32_t num_children;
  int32_t converted_type;  // -1 none
  SchemaElement() : type(-1), repetition(-1), num_children(0), converted_type(-1) {}
};

struct ColumnMeta {
  int32_t type;
  std::vector<std::string> path;
  int32_t codec;
  int64_t num_values;
  int64_t total_compressed;
  int64_t data_page_offset;
  int64_t dictionary_page_offset;
  ColumnMeta() : type(-1), codec(0), num_values(0), total_compressed(0),
                 data_page_offset(-1), dictionary_page_offset(-1) {}
};

struct ColumnChunkInfo {
  int64_t file_offset;
  ColumnMeta meta;
  ColumnChunkInfo() : file_offset(0) {}
};

struct RowGroupInfo {
  std::vector<ColumnChunkInfo> chunks;
  int64_t num_rows;
  RowGroupInfo() : num_rows(0) {}
};

struct FileMeta {
  std::vector<SchemaElement> schema;
  int64_t num_rows;
  std::vector<RowGroupInfo> row_groups;
  FileMeta() : num_rows(0) {}
};

// schema element parse
struct SchemaCtx { FileMeta *fm; SchemaElement cur; bool active; };
static int schema_field_cb(void *p, int16_t fid, int ttype, ThriftReader &r, bool, void *)
{
  SchemaCtx *c = static_cast<SchemaCtx *>(p);
  switch (fid) {
    case 1: if (ttype == TT_I32) { c->cur.type = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 3: if (ttype == TT_I32) { c->cur.repetition = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 4: if (ttype == TT_BINARY) { c->cur.name = r.binary(); return 1; } break;
    case 5: if (ttype == TT_I32) { c->cur.num_children = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 6: if (ttype == TT_I32) { c->cur.converted_type = static_cast<int32_t>(r.zigzag()); return 1; } break;
    default: break;
  }
  return 0;
}

struct ListCtx {
  int elem_type;
  int (*on_elem)(void *, ThriftReader &);
  void *ctx;
  ThriftReader *outer;
};

// ColumnMetaData parse
struct ColMetaParse {
  ColumnMeta *cm;
};

static int colmeta_cb(void *p, int16_t fid, int ttype, ThriftReader &r, bool, void *)
{
  ColMetaParse *c = static_cast<ColMetaParse *>(p);
  switch (fid) {
    case 1: if (ttype == TT_I32) { c->cm->type = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 2: if (ttype == TT_LIST) {
      // encodings: list of i32, skip
      unsigned char h = r.byte();
      int et = h & 0xf;
      uint64_t cnt = (h >> 4) ? (h >> 4) : r.uvarint();
      for (uint64_t k = 0; k < cnt; ++k) {
        if (et == TT_I32 || et == TT_I64 || et == TT_I16) r.zigzag();
        else if (et == TT_I8) r.byte();
      }
      return 1;
    } break;
    case 3: if (ttype == TT_LIST) {
      unsigned char h = r.byte();
      int et = h & 0xf;
      uint64_t cnt = (h >> 4) ? (h >> 4) : r.uvarint();
      for (uint64_t k = 0; k < cnt; ++k) {
        if (et == TT_BINARY) c->cm->path.push_back(r.binary());
        else if (et == TT_I32 || et == TT_I64) r.zigzag();
      }
      return 1;
    } break;
    case 4: if (ttype == TT_I32) { c->cm->codec = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 5: if (ttype == TT_I64) { c->cm->num_values = r.zigzag(); return 1; } break;
    case 6: if (ttype == TT_I64) { c->cm->total_compressed = r.zigzag(); return 1; } break;
    case 9: if (ttype == TT_I64) { c->cm->data_page_offset = r.zigzag(); return 1; } break;
    case 11: if (ttype == TT_I64) { c->cm->dictionary_page_offset = r.zigzag(); return 1; } break;
    default: break;
  }
  return 0;
}

struct ChunkParse {
  ColumnChunkInfo *cc;
};
static int chunk_cb(void *p, int16_t fid, int ttype, ThriftReader &r, bool, void *)
{
  ChunkParse *c = static_cast<ChunkParse *>(p);
  if (fid == 2 && ttype == TT_I64) { c->cc->file_offset = r.zigzag(); return 1; }
  if (fid == 3 && ttype == TT_STRUCT) {
    ColMetaParse mp; mp.cm = &c->cc->meta;
    FieldWalk::walk(r, colmeta_cb, &mp);
    return 1;
  }
  return 0;
}

struct RowGroupParse {
  RowGroupInfo *rg;
  ColumnChunkInfo cur_chunk;
};
static int rg_cb(void *p, int16_t fid, int ttype, ThriftReader &r, bool, void *)
{
  RowGroupParse *c = static_cast<RowGroupParse *>(p);
  if (fid == 1 && ttype == TT_LIST) {
    unsigned char h = r.byte();
    int et = h & 0xf;
    uint64_t cnt = (h >> 4) ? (h >> 4) : r.uvarint();
    if (et != TT_STRUCT) { // unexpected
      for (uint64_t k = 0; k < cnt; ++k) FieldWalk::skip_or_cb(r, -1, et, NULL, NULL, true);
      return 1;
    }
    for (uint64_t k = 0; k < cnt; ++k) {
      c->cur_chunk = ColumnChunkInfo();
      ChunkParse cp; cp.cc = &c->cur_chunk;
      FieldWalk::walk(r, chunk_cb, &cp);
      c->rg->chunks.push_back(c->cur_chunk);
    }
    return 1;
  }
  if (fid == 3 && ttype == TT_I64) { c->rg->num_rows = r.zigzag(); return 1; }
  return 0;
}

struct FileMetaParse {
  FileMeta *fm;
};
static int fm_cb(void *p, int16_t fid, int ttype, ThriftReader &r, bool, void *)
{
  FileMetaParse *c = static_cast<FileMetaParse *>(p);
  if (fid == 2 && ttype == TT_LIST) {
    unsigned char h = r.byte();
    int et = h & 0xf;
    uint64_t cnt = (h >> 4) ? (h >> 4) : r.uvarint();
    if (et != TT_STRUCT) {
      for (uint64_t k = 0; k < cnt; ++k) FieldWalk::skip_or_cb(r, -1, et, NULL, NULL, true);
      return 1;
    }
    for (uint64_t k = 0; k < cnt; ++k) {
      SchemaCtx sc; sc.fm = c->fm; sc.active = true;
      FieldWalk::walk(r, schema_field_cb, &sc);
      c->fm->schema.push_back(sc.cur);
    }
    return 1;
  }
  if (fid == 3 && ttype == TT_I64) { c->fm->num_rows = r.zigzag(); return 1; }
  if (fid == 4 && ttype == TT_LIST) {
    unsigned char h = r.byte();
    int et = h & 0xf;
    uint64_t cnt = (h >> 4) ? (h >> 4) : r.uvarint();
    if (et != TT_STRUCT) {
      for (uint64_t k = 0; k < cnt; ++k) FieldWalk::skip_or_cb(r, -1, et, NULL, NULL, true);
      return 1;
    }
    for (uint64_t k = 0; k < cnt; ++k) {
      c->fm->row_groups.push_back(RowGroupInfo());
      RowGroupParse rp; rp.rg = &(*c->fm->row_groups.rbegin());
      FieldWalk::walk(r, rg_cb, &rp);
    }
    return 1;
  }
  return 0;
}

// page header model
struct PageHeaderInfo {
  int32_t type;              // 0 dict 1 data 2 index 3 data_v2
  int32_t uncompressed_size;
  int32_t compressed_size;
  // v1
  int64_t v1_num_values;
  int32_t v1_encoding;
  // v2
  int32_t v2_num_values;
  int32_t v2_num_nulls;
  int32_t v2_num_rows;
  int32_t v2_encoding;
  int32_t v2_def_bytes;
  int32_t v2_rep_bytes;
  bool v2_compressed;
  PageHeaderInfo()
      : type(-1), uncompressed_size(0), compressed_size(0), v1_num_values(0),
        v1_encoding(0), v2_num_values(0), v2_num_nulls(0), v2_num_rows(0),
        v2_encoding(0), v2_def_bytes(0), v2_rep_bytes(0), v2_compressed(true) {}
};

struct PageHeaderParse {
  PageHeaderInfo *ph;
};
static int ph_cb(void *p, int16_t fid, int ttype, ThriftReader &r, bool, void *)
{
  PageHeaderParse *c = static_cast<PageHeaderParse *>(p);
  PageHeaderInfo *ph = c->ph;
  switch (fid) {
    case 1: if (ttype == TT_I32) { ph->type = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 2: if (ttype == TT_I32) { ph->uncompressed_size = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 3: if (ttype == TT_I32) { ph->compressed_size = static_cast<int32_t>(r.zigzag()); return 1; } break;
    case 5: {  // DataPageHeader
      if (ttype != TT_STRUCT) break;
      // walk sub struct manually
      int16_t last = 0;
      while (true) {
        if (r.eof()) return -1;
        unsigned char h = r.byte();
        if (h == 0) break;
        int tt = h & 0xf;
        int16_t f2 = (h >> 4) ? static_cast<int16_t>(last + (h >> 4))
                              : static_cast<int16_t>(r.zigzag());
        last = f2;
        if (f2 == 1 && tt == TT_I32) ph->v1_num_values = r.zigzag();
        else if (f2 == 2 && tt == TT_I32) ph->v1_encoding = static_cast<int32_t>(r.zigzag());
        else FieldWalk::skip_or_cb(r, f2, tt, NULL, NULL, false);
      }
      return 1;
    }
    case 8: {  // DataPageHeaderV2
      if (ttype != TT_STRUCT) break;
      int16_t last = 0;
      while (true) {
        if (r.eof()) return -1;
        unsigned char h = r.byte();
        if (h == 0) break;
        int tt = h & 0xf;
        int16_t f2 = (h >> 4) ? static_cast<int16_t>(last + (h >> 4))
                              : static_cast<int16_t>(r.zigzag());
        last = f2;
        if (f2 == 1 && tt == TT_I32) ph->v2_num_values = static_cast<int32_t>(r.zigzag());
        else if (f2 == 2 && tt == TT_I32) ph->v2_num_nulls = static_cast<int32_t>(r.zigzag());
        else if (f2 == 4 && tt == TT_I32) ph->v2_encoding = static_cast<int32_t>(r.zigzag());
        else if (f2 == 5 && tt == TT_I32) ph->v2_def_bytes = static_cast<int32_t>(r.zigzag());
        else if (f2 == 6 && tt == TT_I32) ph->v2_rep_bytes = static_cast<int32_t>(r.zigzag());
        else if (f2 == 7 && (tt == TT_TRUE || tt == TT_FALSE)) ph->v2_compressed = (tt == TT_TRUE);
        else FieldWalk::skip_or_cb(r, f2, tt, NULL, NULL, false);
      }
      return 1;
    }
    default: break;
  }
  return 0;
}

// RLE / bit-packed hybrid decoder (for def levels and dictionary indices)
struct RleDecoder {
  const char *b;
  int64_t len;
  int64_t i;
  int bit_width;
  // current run
  bool rle_run;
  uint64_t run_count;     // remaining values in run
  uint64_t rle_value;
  uint64_t bit_group_count; // for bitpacked: remaining groups (8 values each)
  uint64_t bit_buf;
  int bit_buf_bits;
  RleDecoder(const char *buf, int64_t n, int bw)
      : b(buf), len(n), i(0), bit_width(bw), rle_run(false), run_count(0),
        rle_value(0), bit_group_count(0), bit_buf(0), bit_buf_bits(0) {}
  // returns 1 and value; 0 when no more; -1 error
  int next(uint32_t &v)
  {
    if (bit_width == 0) { v = 0; return 1; }
    if (rle_run) {
      if (run_count > 0) { --run_count; v = static_cast<uint32_t>(rle_value); return 1; }
      rle_run = false;
    }
    if (bit_group_count > 0) {
      v = read_bits();
      // bitpacked run counts in values: each group has 8; decrement handled below
      return 1;
    }
    if (i >= len) return 0;
    uint64_t header = read_uvarint();
    if (header & 1) {
      // bit packed run: (header>>1) groups of 8 values
      uint64_t groups = header >> 1;
      bit_group_count = groups * 8;
      v = read_bits();
      return 1;
    } else {
      uint64_t count = header >> 1;
      if (i >= len) return -1;
      rle_value = read_int_bytes((bit_width + 7) / 8);
      rle_run = true;
      run_count = count;
      --run_count;
      v = static_cast<uint32_t>(rle_value);
      return 1;
    }
  }
  uint64_t read_uvarint() {
    uint64_t r = 0; int shift = 0;
    while (i < len) {
      unsigned char c = static_cast<unsigned char>(b[i++]);
      r |= static_cast<uint64_t>(c & 0x7f) << shift;
      if (!(c & 0x80)) break;
      shift += 7;
    }
    return r;
  }
  uint64_t read_int_bytes(int n) {
    uint64_t r = 0;
    for (int k = 0; k < n && i < len; ++k) {
      r |= static_cast<uint64_t>(static_cast<unsigned char>(b[i++])) << (8 * k);
    }
    return r;
  }
  uint32_t read_bits() {
    while (bit_buf_bits < bit_width && i < len) {
      bit_buf |= static_cast<uint64_t>(static_cast<unsigned char>(b[i++])) << bit_buf_bits;
      bit_buf_bits += 8;
    }
    uint32_t v = static_cast<uint32_t>(bit_buf & ((1u << bit_width) - 1));
    bit_buf >>= bit_width;
    bit_buf_bits -= bit_width;
    --bit_group_count;
    return v;
  }
};

} // end namespace parquet

// parquet reader facade -------------------------------------------------------
struct ParquetFile {
  std::ifstream in;
  int64_t file_size;
  parquet::FileMeta meta;
  // flattened leaf columns
  struct LeafCol {
    std::string name;
    int32_t physical_type;
    int32_t converted_type;
    int max_def_level;
    LeafCol() : physical_type(-1), converted_type(-1), max_def_level(0) {}
  };
  std::vector<LeafCol> leaves;

  int open(const std::string &path);
  int read_at(int64_t off, int64_t n, std::string &buf);
};

int ParquetFile::open(const std::string &path)
{
  in.open(path.c_str(), std::ios::in | std::ios::binary);
  if (!in.is_open()) return FS_ERR_FILE_NOT_EXIST;
  in.seekg(0, std::ios::end);
  file_size = in.tellg();
  if (file_size < 12) return FS_ERR_BAD_FORMAT;
  std::string tail(8, '\0');
  in.seekg(file_size - 8);
  in.read(&tail[0], 8);
  if (tail[4] != 'P' || tail[5] != 'A' || tail[6] != 'R' || tail[7] != '1') {
    return FS_ERR_BAD_FORMAT;
  }
  uint32_t footer_len = static_cast<unsigned char>(tail[0])
      | (static_cast<unsigned char>(tail[1]) << 8)
      | (static_cast<unsigned char>(tail[2]) << 16)
      | (static_cast<unsigned char>(tail[3]) << 24);
  if (footer_len <= 0 || static_cast<int64_t>(footer_len) + 12 > file_size) {
    return FS_ERR_BAD_FORMAT;
  }
  std::string footer(footer_len, '\0');
  in.seekg(file_size - 8 - footer_len);
  in.read(&footer[0], footer_len);
  parquet::ThriftReader r(footer.c_str(), footer_len);
  parquet::FileMetaParse fp; fp.fm = &meta;
  if (parquet::FieldWalk::walk(r, parquet::fm_cb, &fp) != 0) return FS_ERR_BAD_FORMAT;

  // flatten schema: root first
  if (meta.schema.empty()) return FS_ERR_BAD_FORMAT;
  // compute leaves via DFS
  struct DFS {
    std::vector<parquet::SchemaElement> &schema;
    std::vector<ParquetFile::LeafCol> &leaves;
    size_t pos;
    DFS(std::vector<parquet::SchemaElement> &s, std::vector<ParquetFile::LeafCol> &l)
        : schema(s), leaves(l), pos(1) {}
    void run(int children, int def_level) {
      for (int c = 0; c < children && pos < schema.size(); ++c) {
        const parquet::SchemaElement &e = schema[pos];
        int dl = def_level + (e.repetition == 1 ? 1 : 0);
        if (e.num_children > 0) {
          ++pos;
          run(e.num_children, dl);
        } else {
          LeafCol lc;
          lc.name = e.name;
          lc.physical_type = e.type;
          lc.converted_type = e.converted_type;
          lc.max_def_level = dl;
          leaves.push_back(lc);
          ++pos;
        }
      }
    }
  };
  int root_children = meta.schema[0].num_children;
  if (root_children <= 0) root_children = static_cast<int>(meta.schema.size()) - 1;
  DFS dfs(meta.schema, leaves);
  if (meta.schema[0].num_children > 0 || meta.schema.size() > 1) {
    dfs.run(root_children, 0);
  }
  return FS_OK;
}

int ParquetFile::read_at(int64_t off, int64_t n, std::string &buf)
{
  buf.assign(static_cast<size_t>(n), '\0');
  in.clear();
  in.seekg(off);
  in.read(&buf[0], n);
  if (in.gcount() != n) return FS_ERR_IO;
  return FS_OK;
}

static FileColType parquet_leaf_col_type(const ParquetFile::LeafCol &lc)
{
  switch (lc.physical_type) {
    case 0: return FS_COL_BOOLEAN;                 // BOOLEAN
    case 1:
    case 2:                                        // INT32 INT64
      if (lc.converted_type == 6) return FS_COL_DATE;
      return FS_COL_BIGINT;
    case 4:
    case 5: return FS_COL_DOUBLE;                  // FLOAT DOUBLE
    case 3: return FS_COL_VARCHAR;                 // INT96 (legacy ts) treat text
    case 6:
    case 7:                                        // BYTE_ARRAY / FIXED
      if (lc.converted_type == 6) return FS_COL_DATE;
      return FS_COL_VARCHAR;
    default: return FS_COL_VARCHAR;
  }
}

// decode one page body of values into string values (non-null positions only)
struct PageValCtx {
  int32_t physical_type;
  std::vector<std::string> *out;   // appended per non-null value
};

static int decode_plain_values(const char *data, int64_t len, int32_t ptype,
                               int64_t expect, std::vector<std::string> &out)
{
  int64_t i = 0;
  for (int64_t k = 0; k < expect; ++k) {
    switch (ptype) {
      case 1: {  // INT32
        if (i + 4 > len) return FS_ERR_BAD_FORMAT;
        int32_t v; memcpy(&v, data + i, 4); i += 4;
        char buf[24]; snprintf(buf, sizeof(buf), "%d", v);
        out.push_back(buf);
        break;
      }
      case 2: {  // INT64
        if (i + 8 > len) return FS_ERR_BAD_FORMAT;
        int64_t v; memcpy(&v, data + i, 8); i += 8;
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
        out.push_back(buf);
        break;
      }
      case 4: {  // FLOAT
        if (i + 4 > len) return FS_ERR_BAD_FORMAT;
        float v; memcpy(&v, data + i, 4); i += 4;
        char buf[64]; snprintf(buf, sizeof(buf), "%.15g", static_cast<double>(v));
        out.push_back(buf);
        break;
      }
      case 5: {  // DOUBLE
        if (i + 8 > len) return FS_ERR_BAD_FORMAT;
        double v; memcpy(&v, data + i, 8); i += 8;
        char buf[64]; snprintf(buf, sizeof(buf), "%.15g", v);
        out.push_back(buf);
        break;
      }
      case 0: {  // BOOLEAN: bit packed LSB first
        {
          int64_t bit = 0;
          for (; k < expect; ++k) {
            int64_t byte_idx = i + (bit / 8);
            if (byte_idx >= len) return FS_ERR_BAD_FORMAT;
            int b = (static_cast<unsigned char>(data[byte_idx]) >> (bit % 8)) & 1;
            out.push_back(b ? "true" : "false");
            ++bit;
          }
          i += (bit + 7) / 8;
          return FS_OK;
        }
      }
      case 6: {  // BYTE_ARRAY
        if (i + 4 > len) return FS_ERR_BAD_FORMAT;
        uint32_t n; memcpy(&n, data + i, 4); i += 4;
        if (i + static_cast<int64_t>(n) > len) return FS_ERR_BAD_FORMAT;
        out.push_back(std::string(data + i, n));
        i += n;
        break;
      }
      case 7: {  // FIXED_LEN_BYTE_ARRAY unsupported here
        return FS_ERR_NOT_SUPPORTED;
      }
      default:
        return FS_ERR_NOT_SUPPORTED;
    }
  }
  return FS_OK;
}

// scan one column chunk, produce values for rows (with nulls from def levels)
static int scan_parquet_column(ParquetFile &pf,
                               const parquet::ColumnChunkInfo &chunk,
                               const ParquetFile::LeafCol &leaf,
                               int64_t chunk_rows,
                               std::vector<FileValue> &vals)
{
  const parquet::ColumnMeta &cm = chunk.meta;
  if (cm.path.empty()) return FS_ERR_BAD_FORMAT;
  int64_t start = cm.data_page_offset;
  if (cm.dictionary_page_offset > 0 && cm.dictionary_page_offset < start) {
    start = cm.dictionary_page_offset;
  }
  int64_t end = chunk.file_offset > start ? chunk.file_offset
                                          : start + cm.total_compressed;
  std::vector<std::string> dict;    // dictionary page values (plain)
  int64_t got = 0;
  int64_t off = start;
  while (off < end && got < cm.num_values) {
    std::string hdr;
    // read page header: thrift structs are small; read incrementally
    // read up to 512b for header then trim by parsed length
    int64_t try_len = end - off; if (try_len > 512) try_len = 512;
    if (FS_OK != pf.read_at(off, try_len, hdr)) return FS_ERR_IO;
    parquet::ThriftReader hr(hdr.c_str(), try_len);
    parquet::PageHeaderInfo ph;
    parquet::PageHeaderParse pp; pp.ph = &ph;
    if (parquet::FieldWalk::walk(hr, parquet::ph_cb, &pp) != 0) return FS_ERR_BAD_FORMAT;
    int64_t hdr_len = hr.i;
    int64_t body_off = off + hdr_len;
    int64_t body_len = ph.compressed_size;
    if (ph.type == 2 /* index page */) { off = body_off + body_len; continue; }
    std::string body;
    if (FS_OK != pf.read_at(body_off, body_len, body)) return FS_ERR_IO;

    if (ph.type == 0) {  // dictionary page (v1 header fields)
      std::string raw;
      if (cm.codec != 0) {
        if (FS_OK != snappy_decompress(body.data(), body.size(), raw)) return FS_ERR_BAD_FORMAT;
      } else raw = body;
      // DataPageHeader at field 7 of PageHeader was not parsed; dict values = all
      // num_values unknown -> decode greedily until exhausted
      std::vector<std::string> dvals;
      decode_plain_values(raw.data(), raw.size(), cm.type,
                          static_cast<int64_t>(raw.size() / (cm.type == 2 ? 8 : 4)) + 1,
                          dvals);  // best effort; trimmed below
      dict.swap(dvals);
      off = body_off + body_len;
      continue;
    }

    // data page v1 or v2
    int64_t num_values = 0;
    std::string def_raw;      // def levels bytes (already outside compression for v2)
    std::string val_raw;      // decompressed values region
    int32_t encoding = 0;
    if (ph.type == 3) {  // v2
      num_values = ph.v2_num_values;
      int64_t p = 0;
      int64_t rep_len = ph.v2_rep_bytes;
      int64_t def_len = ph.v2_def_bytes;
      p += rep_len;  // skip rep levels (flat schema: 0 anyway)
      if (leaf.max_def_level > 0) {
        def_raw.assign(body, static_cast<size_t>(p), static_cast<size_t>(def_len));
      }
      p += def_len;
      std::string vals_comp(body, static_cast<size_t>(p),
                            static_cast<size_t>(body.size() - p));
      if (cm.codec != 0 && ph.v2_compressed) {
        if (FS_OK != snappy_decompress(vals_comp.data(), vals_comp.size(), val_raw)) {
          return FS_ERR_BAD_FORMAT;
        }
      } else {
        val_raw.swap(vals_comp);
      }
      encoding = ph.v2_encoding;
    } else {  // v1
      num_values = ph.v1_num_values;
      std::string raw;
      if (cm.codec != 0) {
        if (FS_OK != snappy_decompress(body.data(), body.size(), raw)) {
          return FS_ERR_BAD_FORMAT;
        }
      } else raw = body;
      // def levels (RLE, 4-byte length prefix) when max_def>0
      int64_t p = 0;
      if (leaf.max_def_level > 0) {
        if (raw.size() < 4) return FS_ERR_BAD_FORMAT;
        uint32_t dl; memcpy(&dl, raw.data(), 4);
        p = 4;
        if (p + static_cast<int64_t>(dl) > static_cast<int64_t>(raw.size())) {
          return FS_ERR_BAD_FORMAT;
        }
        def_raw.assign(raw, 4, dl);
        p += dl;
      }
      val_raw.assign(raw, static_cast<size_t>(p), raw.size() - static_cast<size_t>(p));
      encoding = ph.v1_encoding;
    }

    // def levels -> null bitmap
    std::vector<bool> non_null;
    non_null.reserve(static_cast<size_t>(num_values));
    if (leaf.max_def_level > 0) {
      int bw = 0;
      { int m = leaf.max_def_level; while (m > 0) { ++bw; m >>= 1; } }
      parquet::RleDecoder rd(def_raw.data(), static_cast<int64_t>(def_raw.size()), bw);
      for (int64_t k = 0; k < num_values; ++k) {
        uint32_t lvl = 0;
        int st = rd.next(lvl);
        if (st <= 0) return FS_ERR_BAD_FORMAT;
        non_null.push_back(lvl >= static_cast<uint32_t>(leaf.max_def_level));
      }
    } else {
      non_null.assign(static_cast<size_t>(num_values), true);
    }

    // values
    std::vector<std::string> vstrs;
    int64_t nn = 0;
    for (int64_t k = 0; k < num_values; ++k) if (non_null[static_cast<size_t>(k)]) ++nn;
    if (encoding == 0) {  // PLAIN
      int ret = decode_plain_values(val_raw.data(), static_cast<int64_t>(val_raw.size()),
                                    cm.type, nn, vstrs);
      if (ret != FS_OK) return ret;
    } else if (encoding == 2 || encoding == 1) {  // RLE_DICTIONARY / PLAIN_DICTIONARY
      if (dict.empty()) return FS_ERR_BAD_FORMAT;
      int bit_width = val_raw.empty() ? 0 : static_cast<unsigned char>(val_raw[0]);
      parquet::RleDecoder rd(val_raw.data() + 1,
                             static_cast<int64_t>(val_raw.size()) - 1, bit_width);
      for (int64_t k = 0; k < nn; ++k) {
        uint32_t idx = 0;
        if (rd.next(idx) <= 0 || idx >= dict.size()) return FS_ERR_BAD_FORMAT;
        vstrs.push_back(dict[idx]);
      }
    } else {
      return FS_ERR_NOT_SUPPORTED;  // DELTA_*, BYTE_STREAM_SPLIT etc.
    }
    size_t vi = 0;
    for (int64_t k = 0; k < num_values && got < chunk.meta.num_values; ++k, ++got) {
      if (non_null[static_cast<size_t>(k)]) {
        FileValue fv;
        fv.is_null = false;
        fv.type = parquet_leaf_col_type(leaf);
        fv.str_val = vstrs[vi++];
        // datetime conversion for converted types handled by normalize
        if (fv.type == FS_COL_BOOLEAN) { fv.str_val = (fv.str_val == "true") ? "1" : "0"; }
        else if (fv.type == FS_COL_DOUBLE || fv.type == FS_COL_BIGINT) { /* canonical already */ }
        else if (leaf.converted_type == 6) {
          // days since epoch -> date string
          int64_t days = atoll(fv.str_val.c_str());
          // convert via days-from-civil inverse
          int64_t z = days + 719468;
          int64_t era = (z >= 0 ? z : z - 146096) / 146097;
          int64_t doe = z - era * 146097;
          int64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
          int64_t y = yoe + era * 400;
          int64_t doy = doe - (365*yoe + yoe/4 - yoe/100);
          int64_t mp = (5*doy + 2)/153;
          int64_t d = doy - (153*mp+2)/5 + 1;
          int64_t m = mp + (mp < 10 ? 3 : -9);
          y += (m <= 2);
          char buf[24]; snprintf(buf, sizeof(buf), "%04d-%02d-%02d", (int)y, (int)m, (int)d);
          fv.str_val = buf;
        }
        vals.push_back(fv);
      } else {
        FileValue fv;
        fv.is_null = true;
        fv.type = parquet_leaf_col_type(leaf);
        vals.push_back(fv);
      }
    }
    off = body_off + body_len;
  }
  while (got < chunk_rows) {   // pad missing (should not happen)
    FileValue fv; fv.is_null = true; fv.type = parquet_leaf_col_type(leaf);
    vals.push_back(fv); ++got;
  }
  return FS_OK;
}

int infer_parquet_schema(const std::string &path, std::vector<FileColumn> &schema)
{
  schema.clear();
  ParquetFile pf;
  int ret = pf.open(path);
  if (ret != FS_OK) return ret;
  for (size_t i = 0; i < pf.leaves.size(); ++i) {
    FileColumn col(pf.leaves[i].name, parquet_leaf_col_type(pf.leaves[i]), false);
    schema.push_back(col);
  }
  return FS_OK;
}

struct ParquetScanCtx {
  FileRowSink sink;
  void *sctx;
  std::vector<std::vector<FileValue> > columns;  // column-major buffer per row group
  int64_t rows;
};

static int parquet_emit(ParquetScanCtx &ctx)
{
  for (int64_t r = 0; r < ctx.rows; ++r) {
    FileRow row;
    row.reserve(ctx.columns.size());
    for (size_t c = 0; c < ctx.columns.size(); ++c) {
      row.push_back(r < static_cast<int64_t>(ctx.columns[c].size())
                    ? ctx.columns[c][static_cast<size_t>(r)] : FileValue());
    }
    int ret = ctx.sink(ctx.sctx, row);
    if (ret != FS_OK) return ret;
  }
  return FS_OK;
}

int scan_parquet(const std::string &path, const std::vector<FileColumn> &schema,
                 FileRowSink sink, void *sctx)
{
  ParquetFile pf;
  int ret = pf.open(path);
  if (ret != FS_OK) return ret;
  (void)schema;
  ParquetScanCtx ctx;
  ctx.sink = sink; ctx.sctx = sctx; ctx.rows = 0;
  for (size_t g = 0; g < pf.meta.row_groups.size(); ++g) {
    parquet::RowGroupInfo &rg = pf.meta.row_groups[g];
    ctx.columns.assign(pf.leaves.size(), std::vector<FileValue>());
    ctx.rows = rg.num_rows;
    for (size_t c = 0; c < rg.chunks.size() && c < pf.leaves.size(); ++c) {
      ret = scan_parquet_column(pf, rg.chunks[c], pf.leaves[c], rg.num_rows,
                                ctx.columns[c]);
      if (ret != FS_OK) return ret;
    }
    ret = parquet_emit(ctx);
    if (ret != FS_OK) return ret;
  }
  return FS_OK;
}

// ===========================================================================
// glob
// ===========================================================================
static bool glob_match(const char *pat, const char *s)
{
  // supports * and ? (no character classes)
  const char *p = pat, *str = s;
  const char *star = NULL, *ss = NULL;
  while (*str) {
    if (*p == '?' || *p == *str) { ++p; ++str; }
    else if (*p == '*') { star = p++; ss = str; }
    else if (star) { p = star + 1; str = ++ss; }
    else return false;
  }
  while (*p == '*') ++p;
  return *p == '\0';
}

static bool has_wildcard(const std::string &s)
{
  return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

int glob_files(const std::string &pattern, std::vector<std::string> &files)
{
  files.clear();
  if (pattern.empty()) return FS_ERR_ARG;
  if (!has_wildcard(pattern)) {
    struct stat st;
    if (::stat(pattern.c_str(), &st) == 0) files.push_back(pattern);
    return FS_OK;
  }
  std::string dir = ".";
  std::string rest = pattern;
  // split on last '/'
  size_t pos = pattern.rfind('/');
  std::string base_pat;
  if (pos != std::string::npos) {
    if (pos == 0) dir = "/";
    else dir = pattern.substr(0, pos);
    rest = pattern.substr(pos + 1);
    // wildcard may be in dir part too: expand greedily level by level
  }
  base_pat = rest;
  // if wildcard in dir part, walk: expand directories recursively
  std::vector<std::string> dirs;
  if (has_wildcard(dir)) {
    std::string ddir = ".";
    std::string drest = dir;
    size_t p2 = dir.rfind('/');
    if (p2 != std::string::npos) {
      ddir = p2 == 0 ? "/" : dir.substr(0, p2);
      drest = dir.substr(p2 + 1);
    }
    if (has_wildcard(ddir)) return FS_ERR_NOT_SUPPORTED;  // one level enough for tests
    DIR *d = ::opendir(ddir.c_str());
    if (d == NULL) return FS_OK;
    struct dirent *e;
    while ((e = ::readdir(d)) != NULL) {
      if (glob_match(drest.c_str(), e->d_name)) {
        std::string full = (ddir == "/") ? "/" + std::string(e->d_name)
                                         : ddir + "/" + e->d_name;
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) dirs.push_back(full);
      }
    }
    ::closedir(d);
  } else {
    dirs.push_back(dir);
  }
  for (size_t i = 0; i < dirs.size(); ++i) {
    DIR *d = ::opendir(dirs[i].c_str());
    if (d == NULL) continue;
    struct dirent *e;
    while ((e = ::readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      if (glob_match(base_pat.c_str(), e->d_name)) {
        std::string full = (dirs[i] == "/") ? "/" + std::string(e->d_name)
                                            : dirs[i] + "/" + e->d_name;
        struct stat st;
        if (::stat(full.c_str(), &st) == 0) files.push_back(full);
      }
    }
    ::closedir(d);
  }
  // deterministic order
  for (size_t i = 0; i + 1 < files.size(); ++i) {
    for (size_t j = i + 1; j < files.size(); ++j) {
      if (files[j] < files[i]) files[i].swap(files[j]);
    }
  }
  return FS_OK;
}

// ===========================================================================
// path security
// ===========================================================================
static int canonicalize_path(const std::string &path, bool must_exist,
                             std::string &out)
{
  char buf[PATH_MAX];
  if (::realpath(path.c_str(), buf) != NULL) {
    out = buf;
    return FS_OK;
  }
  if (must_exist) return FS_ERR_FILE_NOT_EXIST;
  // for not-yet-existing target: canonicalize parent + basename
  size_t pos = path.rfind('/');
  std::string parent = (pos == std::string::npos) ? std::string(".")
                    : (pos == 0 ? std::string("/") : path.substr(0, pos));
  std::string base = (pos == std::string::npos) ? path : path.substr(pos + 1);
  if (base.empty() || base == "." || base == "..") return FS_ERR_ARG;
  char pbuf[PATH_MAX];
  if (::realpath(parent.c_str(), pbuf) == NULL) return FS_ERR_FILE_NOT_EXIST;
  out = std::string(pbuf) + "/" + base;
  return FS_OK;
}

int check_secure_path(const std::string &path, const std::string &secure_dir,
                      std::string &canonical)
{
  // reject explicit parent references pre-normalization (defense in depth;
  // realpath already resolves them, this also guards non-existent paths)
  if (path.find("..") != std::string::npos) {
    // allowed only if it stays inside after canonicalization; simplest: reject
    // any ".." component outright
    size_t s = 0;
    while (s <= path.size()) {
      size_t e = path.find('/', s);
      if (e == std::string::npos) e = path.size();
      if (e - s == 2 && path[s] == '.' && path[s+1] == '.') return FS_ERR_PATH_VIOLATION;
      if (e == path.size()) break;
      s = e + 1;
    }
  }
  int ret = canonicalize_path(path, true, canonical);
  if (ret != FS_OK) return ret;
  if (secure_dir.empty()) return FS_OK;
  std::string sec;
  ret = canonicalize_path(secure_dir, true, sec);
  if (ret != FS_OK) return FS_ERR_PATH_VIOLATION;
  if (canonical == sec) return FS_OK;
  if (canonical.compare(0, sec.size() + 1, sec + "/") == 0) return FS_OK;
  return FS_ERR_PATH_VIOLATION;
}

int check_secure_write_path(const std::string &path, const std::string &secure_dir,
                            std::string &canonical)
{
  int ret = canonicalize_path(path, false, canonical);
  if (ret != FS_OK) return ret;
  if (secure_dir.empty()) return FS_OK;
  std::string sec;
  ret = canonicalize_path(secure_dir, true, sec);
  if (ret != FS_OK) return FS_ERR_PATH_VIOLATION;
  if (canonical == sec) return FS_OK;
  if (canonical.compare(0, sec.size() + 1, sec + "/") == 0) return FS_OK;
  return FS_ERR_PATH_VIOLATION;
}

int check_readable_file(const std::string &path)
{
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return FS_ERR_FILE_NOT_EXIST;
  if (!S_ISREG(st.st_mode)) return FS_ERR_ARG;
  if (::access(path.c_str(), R_OK) != 0) return FS_ERR_IO;
  return FS_OK;
}

} // end namespace filesql
} // end namespace sql
} // end namespace oceanbase
