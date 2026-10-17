// Read-your-own-writes through the DuckDB table function: a transaction that has
// written a table the accelerator depends on must get the answer of its own SQL
// (its uncommitted rows included), not the state published for other readers.
//
// Per query and per write (insert / delete / update of the dependency table):
//   BEGIN; <write>; accelerator vs reference SQL (must agree, and the write must
//   have changed the reference); another connection still sees the old answer;
//   ROLLBACK -> the accelerator answers the old state again;
//   then the same write committed -> the accelerator answers the new state.
//
// Usage: test_aux_read_own_writes <tpch.duckdb> <queries, e.g. 1,6>
// The database is copied into a scratch directory ($TMPDIR) per query.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/tests/test_support/db_paths.hpp"

namespace {

using accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime;
namespace engine = accelerator_codegen::aux_state::engine;
namespace ts = accelerator_codegen::test_support;
using Rows = std::vector<std::string>;

void Exec(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql.substr(0, 200) + ": " + (r ? r->GetError() : std::string("null")));
  }
}

// Sorted canonical rows; numbers at two decimals (the aux kernels' fixed point).
Rows Collect(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql.substr(0, 200) + ": " + (r ? r->GetError() : std::string("null")));
  }
  Rows rows;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i) {
    std::string row;
    for (duckdb::idx_t j = 0; j < r->ColumnCount(); ++j) {
      const auto v = r->GetValue(j, i);
      row += j ? "|" : "";
      if (v.IsNull()) {
        row += "NULL";
      } else if (v.type().IsIntegral()) {
        row += std::to_string(v.GetValue<int64_t>());
      } else if (v.type().IsNumeric()) {
        row += std::to_string(std::llround(v.GetValue<double>() * 100.0));
      } else {
        row += v.ToString();
      }
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::string Show(const Rows &rows) {
  return rows.empty() ? "(no rows)" : rows.front() + (rows.size() > 1 ? " ..." : "");
}

struct Write {
  const char *label;
  std::string sql;
};

// Writes on lineitem, scoped to the rows every tested query reads.
std::vector<Write> Writes() {
  const std::string scope = "l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 24";
  return {
      {"insert", "INSERT INTO lineitem SELECT * FROM lineitem WHERE " + scope + " LIMIT 50"},
      {"delete", "DELETE FROM lineitem WHERE " + scope + " AND l_orderkey < 200000"},
      {"update", "UPDATE lineitem SET l_extendedprice = l_extendedprice * 2, l_discount = 0.03 WHERE " + scope +
                     " AND l_orderkey < 200000"},
  };
}

int failures = 0;

void Check(bool ok, const std::string &what, const Rows &got, const Rows &want) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL " << what << ": got " << Show(got) << " want " << Show(want) << "\n";
  }
}

void RunQuery(const std::string &src, int q) {
  const auto &entry = engine::LookupSharedQuery(engine::SharedWorkloadKind::Tpch, q);
  const std::string accel_sql = entry.duckdb_aux_sql, ref_sql = entry.base_sql;
  for (const auto &write : Writes()) {
    ts::ScratchDir scratch("aux_read_own_writes_q" + std::to_string(q) + "_" + write.label);
    duckdb::DuckDB db(ts::PrivateDbCopy(src, scratch));
    duckdb::Connection wc(db), rc(db), other(db);
    for (auto *c : {&wc, &rc, &other}) {
      accelerator_codegen::RegisterAcceleratorFunctions(*c);
    }
    DuckdbRuntime runtime(wc, entry, 10.0);
    runtime.Build();
    const std::string tag = "Q" + std::to_string(q) + " " + write.label + ": ";
    const auto before = Collect(rc, ref_sql);
    Check(Collect(rc, accel_sql) == before, tag + "warm accelerator matches reference", Collect(rc, accel_sql), before);

    Exec(rc, "BEGIN TRANSACTION");
    Exec(rc, write.sql);
    const auto own = Collect(rc, ref_sql);
    if (own == before) {
      ++failures;
      std::cerr << "FAIL " << tag << "the write did not change the reference answer (invalid test)\n";
    }
    const auto accel_own = Collect(rc, accel_sql);
    Check(accel_own == own, tag + "own transaction sees its uncommitted write", accel_own, own);
    const auto accel_other = Collect(other, accel_sql);
    Check(accel_other == before, tag + "another connection does not see it", accel_other, before);
    Exec(rc, "ROLLBACK");
    const auto accel_rolled = Collect(rc, accel_sql);
    Check(accel_rolled == before, tag + "after ROLLBACK the old state", accel_rolled, before);

    Exec(rc, "BEGIN TRANSACTION");
    Exec(rc, write.sql);
    Exec(rc, "COMMIT");
    const auto after = Collect(rc, ref_sql);
    const auto accel_after = Collect(other, accel_sql);
    Check(accel_after == after, tag + "after COMMIT the new state", accel_after, after);
    std::cout << tag << "done\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: test_aux_read_own_writes <tpch.duckdb> <queries>\n";
    return 2;
  }
  try {
    std::stringstream ss(argv[2]);
    for (std::string item; std::getline(ss, item, ',');) {
      RunQuery(argv[1], std::stoi(item));
    }
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  std::cout << (failures ? "FAILED" : "OK") << " (" << failures << " failures)\n";
  return failures ? 1 : 0;
}
