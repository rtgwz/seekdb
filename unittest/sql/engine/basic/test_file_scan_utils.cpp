/**
 * Copyright (c) 2026 OceanBase.
 * seekdb competition problem 2: standalone unit tests for file scan utils
 * and the file-sql rewriter. Build without any OceanBase dependency:
 *   g++ -std=c++11 -g -O0 -o test_file_scan_utils \
 *       test_file_scan_utils.cpp ob_file_scan.cpp ob_file_sql_rewrite.cpp
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <cstdlib>

#include "ob_file_scan.h"
#include "ob_file_sql_rewrite.h"

using namespace oceanbase::sql::filesql;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (cond) { ++g_pass; }                                                \
    else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static std::string g_dir;   // fixture dir

static void write_file(const std::string &path, const std::string &content)
{
  std::ofstream f(path.c_str(), std::ios::binary);
  f << content;
}

// ---------------------------------------------------------------------------
static void test_csv_basic()
{
  printf("== test_csv_basic\n");
  std::string p = g_dir + "/basic.csv";
  write_file(p,
    "id,name,score,active,day,created,note,id\n"
    "1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12,\"hello,\n"
    "world\",101\n"
    "2,Bob,87,false,,,plain,102\n"
    "3,Chen,90.25,true,2026-08-03,2026-08-03T12:13:14,\"say \"\"hi\"\"\",103\n");
  std::vector<FileColumn> schema;
  FileScanOptions opt;
  int ret = infer_csv_schema(p, opt, schema);
  CHECK(ret == FS_OK);
  CHECK(schema.size() == 8);
  CHECK(schema[0].name == "id" && schema[0].type == FS_COL_BIGINT && !schema[0].nullable);
  CHECK(schema[1].name == "name" && schema[1].type == FS_COL_VARCHAR);
  CHECK(schema[2].name == "score" && schema[2].type == FS_COL_DOUBLE);
  CHECK(schema[3].name == "active" && schema[3].type == FS_COL_BOOLEAN);
  CHECK(schema[4].name == "day" && schema[4].type == FS_COL_DATE && schema[4].nullable);
  CHECK(schema[5].name == "created" && schema[5].type == FS_COL_DATETIME
        && schema[5].nullable);
  CHECK(schema[6].name == "note" && schema[6].type == FS_COL_VARCHAR);
  CHECK(schema[7].name == "id__2" && schema[7].type == FS_COL_BIGINT);
}

// portable row sink
struct CollectCtx { std::vector<FileRow> rows; };
static int collect(void *c, const FileRow &r)
{
  CollectCtx *ctx = static_cast<CollectCtx *>(c);
  ctx->rows.push_back(r);
  return FS_OK;
}

static void test_csv_rows()
{
  printf("== test_csv_rows\n");
  std::string p = g_dir + "/basic.csv";
  std::vector<FileColumn> schema;
  FileScanOptions opt;
  CHECK(infer_csv_schema(p, opt, schema) == FS_OK);
  CollectCtx ctx;
  CHECK(scan_csv(p, opt, schema, collect, &ctx) == FS_OK);
  CHECK(ctx.rows.size() == 3);
  // row 0
  CHECK(!ctx.rows[0][0].is_null && ctx.rows[0][0].str_val == "1");
  CHECK(ctx.rows[0][6].str_val == "hello,\nworld");
  CHECK(ctx.rows[0][2].str_val == "98.5");
  CHECK(ctx.rows[0][5].str_val == "2026-08-01 10:11:12");
  // row 1 nulls
  CHECK(ctx.rows[1][4].is_null && ctx.rows[1][5].is_null);
  CHECK(ctx.rows[1][3].str_val == "0");
  // row 2 T separator normalized, escaped quote
  CHECK(ctx.rows[2][5].str_val == "2026-08-03 12:13:14");
  CHECK(ctx.rows[2][6].str_val == "say \"hi\"");
  CHECK(ctx.rows[2][0].str_val == "3");
}

static void test_type_lattice()
{
  printf("== test_type_lattice\n");
  FileColType t = FS_COL_NULL; bool nl = false;
  merge_cell_type("1", t, nl); CHECK(t == FS_COL_BIGINT && !nl);
  merge_cell_type("2.5", t, nl); CHECK(t == FS_COL_DOUBLE);
  merge_cell_type("", t, nl); CHECK(t == FS_COL_DOUBLE && nl);
  t = FS_COL_NULL; nl = false;
  merge_cell_type("true", t, nl); CHECK(t == FS_COL_BOOLEAN);
  merge_cell_type("false", t, nl); CHECK(t == FS_COL_BOOLEAN);
  merge_cell_type("1", t, nl); CHECK(t == FS_COL_VARCHAR);  // bool+int -> varchar
  t = FS_COL_NULL; nl = false;
  merge_cell_type("2026-08-01", t, nl); CHECK(t == FS_COL_DATE);
  merge_cell_type("2026-08-02 10:00:00", t, nl); CHECK(t == FS_COL_DATETIME);
  merge_cell_type("2026-08-03", t, nl); CHECK(t == FS_COL_DATETIME);
  t = FS_COL_NULL; nl = false;
  merge_cell_type("", t, nl); merge_cell_type("", t, nl);
  CHECK(t == FS_COL_NULL && nl);   // all null stays NULL type
  t = FS_COL_NULL; nl = false;
  merge_cell_type("007", t, nl); CHECK(t == FS_COL_BIGINT);
  merge_cell_type("abc", t, nl); CHECK(t == FS_COL_VARCHAR);
  t = FS_COL_NULL; nl = false;
  merge_cell_type("2026-13-01", t, nl); CHECK(t == FS_COL_VARCHAR); // invalid date
}

static void test_jsonl()
{
  printf("== test_jsonl\n");
  std::string p = g_dir + "/basic.jsonl";
  write_file(p,
    "{\"id\":1,\"name\":\"Alice\",\"score\":98.5,\"active\":true}\n"
    "{\"id\":2,\"name\":\"Bob\",\"active\":false,\"day\":\"2026-08-02\"}\n"
    "{\"id\":3,\"name\":\"Chen\",\"score\":90,\"active\":true,\"note\":\"ok\"}\n");
  std::vector<FileColumn> schema;
  FileScanOptions opt;
  opt.format = "newline_delimited";
  CHECK(infer_jsonl_schema(p, opt, schema) == FS_OK);
  CHECK(schema.size() == 6);
  CHECK(schema[0].name == "id" && schema[0].type == FS_COL_BIGINT);
  CHECK(schema[1].name == "name" && schema[1].type == FS_COL_VARCHAR);
  CHECK(schema[2].name == "score" && schema[2].type == FS_COL_DOUBLE
        && schema[2].nullable);
  CHECK(schema[3].name == "active" && schema[3].type == FS_COL_BOOLEAN);
  CHECK(schema[4].name == "day" && schema[4].type == FS_COL_DATE);
  CHECK(schema[5].name == "note" && schema[5].type == FS_COL_VARCHAR);
  CollectCtx ctx;
  CHECK(scan_jsonl(p, opt, schema, collect, &ctx) == FS_OK);
  CHECK(ctx.rows.size() == 3);
  CHECK(ctx.rows[0][2].str_val == "98.5" && !ctx.rows[0][2].is_null);
  CHECK(ctx.rows[1][2].is_null);
  CHECK(ctx.rows[1][4].str_val == "2026-08-02");
  CHECK(ctx.rows[2][5].str_val == "ok");
}

static void test_parquet()
{
  printf("== test_parquet\n");
  // decode the fixture shipped with the repo test suite
  std::string src = g_dir + "/simple.parquet.b64";
  std::ifstream f(src.c_str());
  if (!f.good()) {
    printf("  (no b64 fixture, skip)\n");
    return;
  }
  std::string b64((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  // strip whitespace
  std::string b64c;
  for (size_t i = 0; i < b64.size(); ++i) {
    if (!isspace(static_cast<unsigned char>(b64[i]))) b64c += b64[i];
  }
  static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string bin;
  int val = 0, valb = -8;
  for (size_t i = 0; i < b64c.size(); i++) {
    if (b64c[i] == '=') break;
    const char *pp = strchr(tbl, b64c[i]);
    if (!pp) continue;
    val = (val << 6) + static_cast<int>(pp - tbl);
    valb += 6;
    if (valb >= 0) {
      bin += static_cast<char>((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  std::string pq = g_dir + "/simple.parquet";
  write_file(pq, bin);
  std::vector<FileColumn> schema;
  int ret = infer_parquet_schema(pq, schema);
  CHECK(ret == FS_OK);
  CHECK(schema.size() == 1 && schema[0].name == "c1"
        && schema[0].type == FS_COL_BIGINT);
  CollectCtx ctx;
  ret = scan_parquet(pq, schema, collect, &ctx);
  CHECK(ret == FS_OK);
  CHECK(ctx.rows.size() == 7);
  if (ctx.rows.size() == 7) {
    CHECK(ctx.rows[0][0].str_val == "0");
    CHECK(ctx.rows[3][0].str_val == "3");
    CHECK(ctx.rows[6][0].str_val == "6");
  }
}

static void test_glob()
{
  printf("== test_glob\n");
  std::vector<std::string> files;
  int ret = glob_files(g_dir + "/*", files);
  CHECK(ret == FS_OK);
  bool found_csv = false, found_jsonl = false;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].find("basic.csv") != std::string::npos) found_csv = true;
    if (files[i].find("basic.jsonl") != std::string::npos) found_jsonl = true;
  }
  CHECK(found_csv && found_jsonl);
}

static void test_security()
{
  printf("== test_security\n");
  std::string canonical;
  // under secure dir: ok
  CHECK(check_secure_path(g_dir + "/basic.csv", g_dir, canonical) == FS_OK);
  // outside: violation (use /etc/passwd with secure dir = fixture dir)
  CHECK(check_secure_path("/etc/passwd", g_dir, canonical) == FS_ERR_PATH_VIOLATION);
  // parent escape attempt
  CHECK(check_secure_path(g_dir + "/../etc/passwd", g_dir, canonical)
        == FS_ERR_PATH_VIOLATION);
  // missing file
  CHECK(check_secure_path(g_dir + "/no_such_file.csv", g_dir, canonical)
        == FS_ERR_FILE_NOT_EXIST);
  // empty secure dir disables
  CHECK(check_secure_path("/etc/hostname", "", canonical) == FS_OK);
  // write path may not exist
  CHECK(check_secure_write_path(g_dir + "/export.csv", g_dir, canonical) == FS_OK);
}

static void test_rewriter()
{
  printf("== test_rewriter\n");
  FileSqlSessionCtx ctx;
  ctx.enable_file_sql = true;
  ctx.secure_file_priv = g_dir;
  RewriteResult r;

  // basic rewrite
  int ret = rewrite_file_sql(
      "select id, name from read_csv('" + g_dir + "/basic.csv') f order by id",
      ctx, r);
  CHECK(ret == FS_OK && r.handled && !r.sql.empty());
  CHECK(r.sql.find("UNION ALL") != std::string::npos);
  CHECK(r.sql.find(") f order by id") != std::string::npos);
  CHECK(r.sql.find("id__2") != std::string::npos);
  CHECK(r.sql.find("hello,\nworld") != std::string::npos);
  // multiline value must survive
  CHECK(r.sql.find("CAST('2026-08-03 12:13:14' AS DATETIME)") != std::string::npos);

  // alias-less call followed by ORDER keyword: order must not be eaten
  ret = rewrite_file_sql(
      "select file from glob('" + g_dir + "/*') order by file", ctx, r);
  CHECK(ret == FS_OK && r.handled);
  CHECK(r.sql.find(") glob order by file") != std::string::npos);
  CHECK(r.sql.substr(r.sql.find(") glob") + 6) == " order by file");

  // enabled off -> not supported
  FileSqlSessionCtx off;
  off.enable_file_sql = false;
  ret = rewrite_file_sql(
      "select count(*) from read_csv('" + g_dir + "/basic.csv')", off, r);
  CHECK(ret == FS_ERR_NOT_SUPPORTED && !r.handled);

  // sample_size=10 -> not supported
  ret = rewrite_file_sql(
      "select * from read_csv('" + g_dir + "/basic.csv', sample_size = 10)",
      ctx, r);
  CHECK(ret == FS_ERR_NOT_SUPPORTED);

  // sample_size=-1 -> ok
  ret = rewrite_file_sql(
      "select count(*) from read_csv('" + g_dir + "/basic.csv', header = true,"
      " sample_size = -1) f", ctx, r);
  CHECK(ret == FS_OK && r.handled);

  // format xml -> parse error
  ret = rewrite_file_sql(
      "select * from read_csv('" + g_dir + "/basic.csv', 'xml')", ctx, r);
  CHECK(ret == FS_OK && !r.handled);   // falls through to ob parser (1064)

  // jsonl with format option
  ret = rewrite_file_sql(
      "select id from read_json('" + g_dir + "/basic.jsonl',"
      " format = 'newline_delimited') f order by id", ctx, r);
  CHECK(ret == FS_OK && r.handled);
  CHECK(r.sql.find("2026-08-02") != std::string::npos);

  // describe
  ret = rewrite_file_sql(
      "describe select * from read_csv('" + g_dir + "/basic.csv')", ctx, r);
  CHECK(ret == FS_OK && r.handled);
  CHECK(r.sql.find("'BIGINT'") != std::string::npos);
  CHECK(r.sql.find("'BOOLEAN'") != std::string::npos);
  CHECK(r.sql.find("'DATE'") != std::string::npos);
  CHECK(r.sql.find("'DATETIME'") != std::string::npos);
  CHECK(r.sql.find("'id__2'") != std::string::npos);
  CHECK(r.sql.find("'YES'") != std::string::npos);
  CHECK(r.sql.find("'NO'") != std::string::npos);

  // parquet rewrite
  ret = rewrite_file_sql(
      "select count(*) from read_parquet('" + g_dir + "/simple.parquet') p",
      ctx, r);
  CHECK(ret == FS_OK && r.handled);
  CHECK(r.sql.find("SELECT 0 AS `c1`") != std::string::npos);

  // copy
  ret = rewrite_file_sql(
      "copy (select id, name, score from read_csv('" + g_dir + "/basic.csv')"
      " order by id) to '" + g_dir + "/export.csv' (format csv, header,"
      " nullstr 'NULL')", ctx, r);
  CHECK(ret == FS_OK && r.handled && r.copy.valid);
  CHECK(r.copy.dest_canonical == g_dir + "/export.csv");
  CHECK(r.copy.with_header);
  CHECK(r.copy.null_str == "NULL");
  CHECK(r.copy.inner_query.find("UNION ALL") != std::string::npos);

  // copy table form
  ret = rewrite_file_sql("copy t1 to '" + g_dir + "/out.csv' (header)", ctx, r);
  CHECK(ret == FS_OK && r.handled && r.copy.inner_query == "SELECT * FROM t1");

  // secure violation in path
  ret = rewrite_file_sql("select * from read_csv('/etc/passwd')", ctx, r);
  CHECK(ret == FS_ERR_PATH_VIOLATION);

  // copy outside secure dir
  ret = rewrite_file_sql("copy t1 to '/tmp/x.csv'", ctx, r);
  CHECK(ret == FS_ERR_PATH_VIOLATION);

  // plain sql untouched
  ret = rewrite_file_sql("select 1 from dual", ctx, r);
  CHECK(ret == FS_OK && !r.handled);

  // secure_file_priv empty disables
  FileSqlSessionCtx any;
  any.enable_file_sql = true;
  any.secure_file_priv = "";
  ret = rewrite_file_sql("select * from read_csv('/etc/hostname')", any, r);
  CHECK(ret == FS_OK && r.handled);

  // two files joined
  ret = rewrite_file_sql(
      "select a.id, b.note from read_csv('" + g_dir + "/basic.csv') a"
      " join read_json('" + g_dir + "/basic.jsonl') b on a.id = b.id", ctx, r);
  CHECK(ret == FS_OK && r.handled);
  CHECK(r.sql.find(") a join (") != std::string::npos);

  // csv escape helper
  CHECK(csv_escape_field("plain", ",", "\"") == "plain");
  CHECK(csv_escape_field("a,b", ",", "\"") == "\"a,b\"");
  CHECK(csv_escape_field("say \"hi\"", ",", "\"") == "\"say \"\"hi\"\"\"");
}

int main(int argc, char **argv)
{
  g_dir = argc > 1 ? argv[1] : ".";
  while (g_dir.size() > 1 && g_dir[g_dir.size()-1] == '/') g_dir.erase(g_dir.size()-1);

  test_type_lattice();
  test_csv_basic();
  test_csv_rows();
  test_jsonl();
  test_parquet();
  test_glob();
  test_security();
  test_rewriter();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
