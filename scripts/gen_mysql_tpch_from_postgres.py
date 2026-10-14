#!/usr/bin/env python3
"""Generate MySQL TPC-H aux trees, catalog, and runtime from the Postgres copies."""

from __future__ import annotations

import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


MYSQL_SCAN_HELPERS = r'''
// AuxDate uses Unix-epoch days. MySQL DATE text is YYYY-MM-DD.
constexpr int kUnixEpochDays = 719468;

int DaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - kUnixEpochDays;
}

int32_t ParseUnixDays(const std::string &text) {
  if (text.size() < 10) {
    return 0;
  }
  int y = std::atoi(text.substr(0, 4).c_str());
  unsigned m = static_cast<unsigned>(std::atoi(text.substr(5, 2).c_str()));
  unsigned d = static_cast<unsigned>(std::atoi(text.substr(8, 2).c_str()));
  if (y == 0 || m == 0 || d == 0) {
    return 0;
  }
  return DaysFromCivil(y, m, d);
}

std::string MysqlCliPrefix() {
  const char *host = std::getenv("MYSQL_HOST");
  const char *port = std::getenv("MYSQL_PORT");
  const char *user = std::getenv("MYSQL_USER");
  const char *password = std::getenv("MYSQL_PWD");
  const char *db = std::getenv("TPCH_MYSQL_DB");
  if (!host || !*host) host = "127.0.0.1";
  if (!port || !*port) port = "3306";
  if (!user || !*user) user = "root";
  if (!password) password = "mysql";
  if (!db || !*db) db = "accel_mysql_test";
  std::ostringstream cmd;
  cmd << "MYSQL_PWD=" << password << " mysql -h " << host << " -P " << port
      << " -u" << user << " -D " << db
      << " --batch --raw --skip-column-names -e ";
  return cmd.str();
}

std::string ShellEscape(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

std::vector<std::string> SplitTsv(const std::string &line) {
  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      cols.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  cols.push_back(cur);
  return cols;
}

template <typename Callback>
void ScanTypedTuples(const std::string &sql, Callback &&callback) {
  const std::string cmd = MysqlCliPrefix() + ShellEscape(sql) + " 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    throw std::runtime_error("mysql scan popen failed");
  }
  char buf[1 << 16];
  while (fgets(buf, sizeof(buf), pipe)) {
    std::string line(buf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    callback(SplitTsv(line));
  }
  const int rc = pclose(pipe);
  if (rc != 0) {
    throw std::runtime_error("mysql scan failed: " + sql);
  }
}

const std::string &Col(const std::vector<std::string> &cols, int attribute) {
  static const std::string kEmpty;
  if (attribute < 0 || static_cast<std::size_t>(attribute) >= cols.size()) {
    return kEmpty;
  }
  return cols[static_cast<std::size_t>(attribute)];
}

int64_t TypedInt(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text.empty() || text == "NULL") {
    return 0;
  }
  return std::strtoll(text.c_str(), nullptr, 10);
}

int32_t TypedDate(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text.empty() || text == "NULL") {
    return 0;
  }
  return ParseUnixDays(text);
}

std::string TypedText(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text == "NULL") {
    return "";
  }
  return text;
}

int64_t TypedDecimalRaw(const std::vector<std::string> &cols, int attribute) {
  const auto &text = Col(cols, attribute);
  if (text.empty() || text == "NULL") {
    return 0;
  }
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) {
    return 0;
  }
  return static_cast<int64_t>(std::llround(value * 100.0));
}

'''


def copy_aux_trees() -> None:
    src_root = ROOT / "accelerators/tpch"
    for qdir in sorted(src_root.glob("q*/postgres/aux")):
        dest = qdir.parent.parent / "mysql" / "aux"
        dest.mkdir(parents=True, exist_ok=True)
        for src in qdir.iterdir():
            if src.suffix == ".bak":
                continue
            text = src.read_text()
            text = text.replace("tpch/q", "tpch/q")  # keep
            text = text.replace("/postgres/aux/", "/mysql/aux/")
            (dest / src.name).write_text(text)
        print(f"copied {qdir} -> {dest}")


def postgres_to_mysql_sql(sql: str) -> str:
    def sub_date_sub(m: re.Match[str]) -> str:
        return f"DATE_SUB(DATE '{m.group(1)}', INTERVAL {m.group(2)} DAY)"

    def sub_date_add(m: re.Match[str]) -> str:
        unit = m.group(3).upper()
        if unit.startswith("MONTH"):
            unit = "MONTH"
        elif unit.startswith("YEAR"):
            unit = "YEAR"
        elif unit.startswith("DAY"):
            unit = "DAY"
        return f"DATE_ADD(DATE '{m.group(1)}', INTERVAL {m.group(2)} {unit})"

    sql = re.sub(
        r"DATE '([^']+)' - INTERVAL '(\d+)' DAY",
        sub_date_sub,
        sql,
    )
    sql = re.sub(
        r"DATE '([^']+)' \+ INTERVAL '(\d+)' ([A-Za-z]+)",
        sub_date_add,
        sql,
    )
    sql = sql.replace("SUBSTRING(c_phone FROM 1 FOR 2)", "SUBSTRING(c_phone, 1, 2)")
    sql = re.sub(
        r"SELECT \* FROM accel_tpch_q(\d+)(_aux)?\(",
        r"SELECT mysql_accel_tpch_q\1_aux(",
        sql,
    )
    sql = re.sub(
        r"SELECT revenue FROM accel_tpch_q(\d+)(_aux)?\(",
        r"SELECT mysql_accel_tpch_q\1_aux(",
        sql,
    )
    return sql


def write_catalog() -> None:
    src = (ROOT / "backends/postgres/api/catalog/query_catalog.cpp").read_text()
    src = src.replace("PostgreSQL query catalog", "MySQL query catalog")
    src = src.replace("catalog/postgres/query_catalog.hpp", "catalog/mysql/query_catalog.hpp")
    src = src.replace("engine::postgres", "engine::mysql")
    src = src.replace("PostgresSharedQueryCatalogEntry", "MysqlSharedQueryCatalogEntry")
    src = src.replace("kPostgresQueries", "kMysqlQueries")
    src = src.replace("LookupPostgresQuery", "LookupMysqlQuery")
    src = src.replace("PostgresQueriesForWorkload", "MysqlQueriesForWorkload")
    src = src.replace("postgres query not found", "mysql query not found")
    src = postgres_to_mysql_sql(src)
    dest_dir = ROOT / "backends/mysql/api/catalog"
    dest_dir.mkdir(parents=True, exist_ok=True)
    (dest_dir / "query_catalog.cpp").write_text(src)

    hdr = (ROOT / "backends/postgres/api/catalog/query_catalog.hpp").read_text()
    hdr = hdr.replace("PostgreSQL query catalog", "MySQL query catalog")
    hdr = hdr.replace("engine::postgres", "engine::mysql")
    hdr = hdr.replace("PostgresSharedQueryArguments", "MysqlSharedQueryArguments")
    hdr = hdr.replace("PostgresSharedQueryCatalogEntry", "MysqlSharedQueryCatalogEntry")
    hdr = hdr.replace("LookupPostgresQuery", "LookupMysqlQuery")
    hdr = hdr.replace("PostgresQueriesForWorkload", "MysqlQueriesForWorkload")
    (dest_dir / "query_catalog.hpp").write_text(hdr)
    print("wrote mysql query catalog")


def transform_runtime() -> None:
    src = (ROOT / "backends/postgres/api/runtime.cpp").read_text()
    start = src.find("// AuxDate uses Unix-epoch days")
    scans = src.find("void ScanTpchQ1Lineitems(")
    if start < 0 or scans < 0:
        raise RuntimeError("could not locate postgres scan helper splice points")
    prefix = src[:start]
    rest = src[scans:]

    prefix = prefix.replace("ACCELERATOR_CODEGEN_POSTGRES_EXTENSION", "ACCELERATOR_CODEGEN_MYSQL_PLUGIN")
    prefix = re.sub(r"tpch/q(\d+)/postgres/aux/", r"tpch/q\1/mysql/aux/", prefix)
    prefix = prefix.replace("postgres_xact_callbacks.hpp", "mysql_xact_callbacks.hpp")
    prefix = prefix.replace("postgres_aux_backend.hpp", "mysql_aux_backend.hpp")
    prefix = prefix.replace("postgres_state_store.hpp", "mysql_state_store.hpp")
    prefix = prefix.replace("postgres_tpch_table_ids.hpp", "mysql_tpch_table_ids.hpp")
    prefix = prefix.replace("accel_pg_get_observer_mutation_count", "mysql_accel_get_observer_mutation_count")
    prefix = prefix.replace("postgres.h", "cstdio")
    prefix = prefix.replace('#include "postgres/runtime.hpp"', '#include "mysql/runtime.hpp"')
    prefix = prefix.replace("engine::postgres", "engine::mysql")
    # Drop leftover postgres C includes that no longer parse.
    prefix = re.sub(r"extern \"C\" \{\n#include \"cstdio\".*?}\n\n", "", prefix, flags=re.S)
    prefix = prefix.replace("PostgresCostModelBasename", "MysqlCostModelBasename")
    prefix = prefix.replace("_aux_cost_model_postgres.json", "_aux_cost_model_mysql.json")
    prefix = prefix.replace("ACCELERATOR_CODEGEN_POSTGRES_", "ACCELERATOR_CODEGEN_MYSQL_")
    prefix = prefix.replace("engine-specific calibration when present.", "MySQL-specific calibration when present.")

    rest = rest.replace("(HeapTuple tuple, TupleDesc tuple_desc)", "(const std::vector<std::string> &cols)")
    rest = re.sub(r"TypedDate\(tuple, tuple_desc, (\d+)\)", r"TypedDate(cols, \1)", rest)
    rest = re.sub(r"TypedInt\(tuple, tuple_desc, (\d+)\)", r"TypedInt(cols, \1)", rest)
    rest = re.sub(r"TypedText\(tuple, tuple_desc, (\d+)\)", r"TypedText(cols, \1)", rest)
    rest = re.sub(r"TypedDecimalRaw\(tuple, tuple_desc, (\d+)\)", r"TypedDecimalRaw(cols, \1)", rest)
    rest = rest.replace("COUNT(*)::BIGINT", "CAST(COUNT(*) AS SIGNED)")
    rest = rest.replace("CAST(round(l_quantity * 100) AS BIGINT)", "CAST(ROUND(l_quantity * 100) AS SIGNED)")
    rest = rest.replace(
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT))",
        "SUM(CAST(ROUND(l_extendedprice * 100) AS SIGNED))",
    )
    rest = rest.replace(
        '"WHERE l_shipdate >= (DATE \'1970-01-01\' + " +\n      std::to_string(start_days) +\n      ") AND l_shipdate < (DATE \'1970-01-01\' + " + std::to_string(end_days) +\n      ")"',
        '"WHERE l_shipdate >= DATE_ADD(\'1970-01-01\', INTERVAL " +\n      std::to_string(start_days) +\n      " DAY) AND l_shipdate < DATE_ADD(\'1970-01-01\', INTERVAL " + std::to_string(end_days) +\n      " DAY)"',
    )
    rest = rest.replace("RegisterPostgresMutationBackend", "RegisterMysqlMutationBackend")
    rest = rest.replace("RegisterPostgresXactCallbacks", "RegisterMysqlXactCallbacks")
    rest = rest.replace("RegisterPostgresBackendForMutations", "RegisterMysqlBackendForMutations")
    rest = rest.replace("PostgresStateStore", "MysqlStateStore")
    rest = rest.replace("PostgresAuxBackend", "MysqlAuxBackend")
    rest = rest.replace("engine::postgres", "engine::mysql")
    rest = rest.replace("ACCELERATOR_CODEGEN_POSTGRES_", "ACCELERATOR_CODEGEN_MYSQL_")
    rest = rest.replace("pg_tbl", "mysql_tbl")
    rest = rest.replace("postgres::tpch_tables", "mysql::tpch_tables")
    rest = rest.replace("accel_pg_get_observer_mutation_count", "mysql_accel_get_observer_mutation_count")

    # Replace Lookup SPI helpers with ScanTypedTuples.
    rest = re.sub(
        r"bool LookupTpchQ18Order\(int64_t orderkey, query::TpchQ18OrderInput &out\) \{.*?\n\}",
        '''bool LookupTpchQ18Order(int64_t orderkey, query::TpchQ18OrderInput &out) {
  bool found = false;
  const std::string sql =
      "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice "
      "FROM orders WHERE o_orderkey = " +
      std::to_string(orderkey);
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    out = {TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2),
           TypedDecimalRaw(cols, 3)};
    found = out.key == orderkey;
  });
  return found;
}''',
        rest,
        count=1,
        flags=re.S,
    )
    rest = re.sub(
        r"bool LookupTpchQ19Part\(int64_t partkey, query::TpchQ19PartInput &out\) \{.*?\n\}",
        '''bool LookupTpchQ19Part(int64_t partkey, query::TpchQ19PartInput &out) {
  bool found = false;
  const std::string sql =
      "SELECT p_brand, p_container, p_size FROM part WHERE p_partkey = " +
      std::to_string(partkey);
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    out = {partkey, TypedText(cols, 0), TypedText(cols, 1),
           static_cast<int32_t>(TypedInt(cols, 2))};
    found = true;
  });
  return found;
}''',
        rest,
        count=1,
        flags=re.S,
    )

    # Replace PgSink + Execute + postgres execute wrappers.
    sink_start = rest.find("class PgSink final")
    if sink_start < 0:
        raise RuntimeError("PgSink not found")
    exec_start = rest.find("void ExecutePostgresTpchQ1")
    if exec_start < 0:
        raise RuntimeError("ExecutePostgresTpchQ1 not found")
    ns_end = rest.rfind("} // namespace accelerator_codegen::aux_state::engine::postgres")
    if ns_end < 0:
        ns_end = rest.rfind("} // namespace accelerator_codegen::aux_state::engine::mysql")
    runtime_block = rest[sink_start:exec_start]
    runtime_block = runtime_block.replace("class PgSink final : public core::AuxRowSink {", "class TsvSink final : public core::AuxRowSink {")
    runtime_block = re.sub(
        r"  PgSink\(Tuplestorestate \*tuplestore_p, TupleDesc tuple_desc_p\)\n      : tuplestore\(tuplestore_p\), tuple_desc\(tuple_desc_p\) \{\}",
        "  explicit TsvSink(std::string &out_p) : out(out_p) {}",
        runtime_block,
    )
    runtime_block = re.sub(
        r"  void WriteRow\(const core::AuxRow &row\) override \{.*?  void End\(\) override \{\}",
        '''  void WriteRow(const core::AuxRow &row) override {
    if (!out.empty() && out.back() != '\\n') {
      out.push_back('\\n');
    }
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (column > 0) {
        out.push_back('|');
      }
      if (row.values[column].is_null) {
        continue;
      }
      out += CellText(row.values[column]);
    }
    out.push_back('\\n');
  }

  void End() override {}''',
        runtime_block,
        count=1,
        flags=re.S,
    )
    runtime_block = re.sub(
        r"  static Datum ToDatum\(const core::AuxValue &value, Oid type_oid\) \{.*?    return \(Datum\)0;\n  \}",
        '''  static std::string CellText(const core::AuxValue &value) {
    if (std::holds_alternative<int64_t>(value.payload)) {
      return std::to_string(std::get<int64_t>(value.payload));
    }
    if (std::holds_alternative<double>(value.payload)) {
      std::ostringstream ss;
      ss.setf(std::ios::fixed);
      ss.precision(6);
      ss << std::get<double>(value.payload);
      return ss.str();
    }
    if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
      return DecimalText(std::get<core::ScaledDecimal>(value.payload));
    }
    if (std::holds_alternative<core::AuxDate>(value.payload)) {
      return std::to_string(std::get<core::AuxDate>(value.payload).days_since_epoch);
    }
    if (std::holds_alternative<std::string>(value.payload)) {
      return std::get<std::string>(value.payload);
    }
    if (std::holds_alternative<bool>(value.payload)) {
      return std::get<bool>(value.payload) ? "t" : "f";
    }
    return "";
  }''',
        runtime_block,
        count=1,
        flags=re.S,
    )
    runtime_block = runtime_block.replace("  Tuplestorestate *tuplestore;\n  TupleDesc tuple_desc;", "  std::string &out;")
    runtime_block = runtime_block.replace(
        "  void Execute(const core::QueryArguments &arguments, FunctionCallInfo fcinfo) {",
        "  void Execute(const core::QueryArguments &arguments, std::string &out) {",
    )
    runtime_block = re.sub(
        r"    RequireMaterialize\(fcinfo\);\n",
        "",
        runtime_block,
    )
    runtime_block = re.sub(
        r"    auto \*return_set_info =.*?    module.Execute\(backend, arguments, sink\);",
        "    TsvSink sink(out);\n    module.Execute(backend, arguments, sink);",
        runtime_block,
        count=1,
        flags=re.S,
    )
    runtime_block = re.sub(
        r"  void RequireMaterialize\(FunctionCallInfo fcinfo\) \{.*?  \}\n\n",
        "",
        runtime_block,
        count=1,
        flags=re.S,
    )

    wrappers = []
    for q in range(1, 23):
        wrappers.append(
            f"""std::string ExecuteMysqlTpchQ{q}(const core::QueryArguments &arguments) {{
  std::string out;
  detail::Q{q}Runtime().Execute(arguments, out);
  return out;
}}
"""
        )
    suffix = (
        rest[exec_start:ns_end]
        if False
        else ""
    )
    # Keep QNRuntime() functions that sit between PgSink/Runtime and ExecutePostgres.
    runtime_fns_start = runtime_block.find("Runtime<query::TpchQ1Module")
    # Actually QNRuntime is AFTER the Runtime class, still in runtime_block if we sliced to ExecutePostgres.
    body = (
        prefix
        + MYSQL_SCAN_HELPERS
        + rest[:sink_start]
        + runtime_block
        + "\n"
        + "\n".join(wrappers)
        + "\n} // namespace accelerator_codegen::aux_state::engine::mysql\n\n#endif\n"
    )
    # Fix leftover postgres namespace close if present in rest[:sink_start]
    body = body.replace(
        "} // namespace accelerator_codegen::aux_state::engine::postgres",
        "} // namespace accelerator_codegen::aux_state::engine::mysql",
    )
    dest_dir = ROOT / "backends/mysql/api"
    dest_dir.mkdir(parents=True, exist_ok=True)
    (dest_dir / "runtime.cpp").write_text(body)

    hdr = """// GENERATED BACKEND
#pragma once

#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include <string>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

std::string ExecuteMysqlTpchQ1(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ2(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ3(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ4(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ5(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ6(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ7(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ8(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ9(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ10(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ11(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ12(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ13(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ14(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ15(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ16(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ17(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ18(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ19(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ20(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ21(const core::QueryArguments &arguments);
std::string ExecuteMysqlTpchQ22(const core::QueryArguments &arguments);

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
"""
    (dest_dir / "runtime.hpp").write_text(hdr)
    print("wrote mysql runtime")


def copy_cost_models() -> None:
    for q in range(1, 23):
        cfg = ROOT / f"accelerators/tpch/q{q}/config"
        src = cfg / f"q{q}_aux_cost_model.json"
        dest = cfg / f"q{q}_aux_cost_model_mysql.json"
        data = json.loads(src.read_text())
        data["engine"] = "mysql"
        dest.write_text(json.dumps(data, indent=2) + "\n")
    print("copied mysql cost models")


def main() -> None:
    copy_aux_trees()
    write_catalog()
    transform_runtime()
    copy_cost_models()


if __name__ == "__main__":
    main()
