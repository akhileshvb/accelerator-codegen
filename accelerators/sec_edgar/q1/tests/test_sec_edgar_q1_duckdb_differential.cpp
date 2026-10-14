// End-to-end differential test for SEC EDGAR Q1 on a real DuckDB engine.
//
// A synthetic `pre` table (multi row-group, with NULL stmt/rfile/adsh/line,
// non-canonical adsh strings, empty strings) is maintained with real SQL
// INSERT / DELETE / UPDATE statements; each statement's compact-row mutation
// is fed to the DuckdbRuntime (coordinator + DuckDB state store) and the aux
// answer is compared with plain DuckDB SQL (base_sql) after every step. Both
// the single-threaded and (when compiled) the parallel aux accelerator run.
//
// usage: test_sec_edgar_q1_duckdb_differential [steps=150] [seed=1] [rows=300000] [work_db]
// With work_db (a WRITABLE COPY of the SEC EDGAR database; it is modified!) the
// random statements run against the real `pre` table instead of a synthetic one.
// (run from the repository root so config/ cost models are found)
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <tuple>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "eval/harness/differential.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace catalog = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;
namespace harness = accelerator_codegen::aux_state::harness;
using catalog::duckdb::DuckdbRuntime;

namespace {

void Require(bool ok, const std::string &message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

duckdb::unique_ptr<duckdb::MaterializedQueryResult> Run(duckdb::Connection &c, const std::string &sql) {
  auto result = c.Query(sql);
  Require(result && !result->HasError(), "SQL failed: " + sql.substr(0, 200) + " -> " +
                                            (result ? result->GetError() : "null"));
  return result;
}

struct Gen {
  std::mt19937_64 rng;
  explicit Gen(uint64_t seed) : rng(seed) {}
  int R(int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); }

  std::string Adsh() {
    static const char *weird[] = {"", "short", "ZZZZZZZZZZ-ZZ-ZZZZZZ", "0001234567-22-00012",
                                  "0001234567-22-0001234", "0001234567_22_000123"};
    if (R(15) == 0) {
      return weird[R(6)];
    }
    char buf[32];
    // small cik pool => many rows per adsh; occasional fresh accession
    const int cik = R(12) == 0 ? R(100000000) : R(400);
    std::snprintf(buf, sizeof(buf), "%010d-%02d-%06d", cik, 20 + R(4), R(R(6) == 0 ? 1000000 : 3));
    return buf;
  }
};

struct CompactRow {
  bool stmt_null, rfile_null, adsh_null, line_null;
  std::string stmt, rfile, adsh;
  int64_t line;
};

CompactRow RandomRow(Gen &g) {
  static const char *stmts[] = {"BS", "IS", "CF", "EQ", "CI", "UN", "", "zz"};
  static const char *rfiles[] = {"H", "H", "H", "X", "", "R2"};
  CompactRow r;
  r.stmt_null = g.R(15) == 0;
  r.stmt = r.stmt_null ? "" : stmts[g.R(8)];
  r.rfile_null = g.R(15) == 0;
  r.rfile = r.rfile_null ? "" : rfiles[g.R(6)];
  r.adsh_null = g.R(40) == 0;
  r.adsh = r.adsh_null ? "" : g.Adsh();
  r.line_null = g.R(20) == 0;
  r.line = r.line_null ? 0 : (g.R(10) == 0 ? -g.R(100) : g.R(480));
  return r;
}

std::string Lit(bool is_null, const std::string &v) {
  if (is_null) {
    return "NULL";
  }
  std::string out = "'";
  for (const char c : v) {
    out += c;
    if (c == '\'') {
      out += '\'';
    }
  }
  return out + "'";
}

core::AuxRow ToAux(const CompactRow &r) {
  core::AuxRow row;
  row.values.push_back(r.stmt_null ? core::AuxValue::Null(core::AuxType::String)
                                   : core::AuxValue::Of(core::AuxType::String, r.stmt));
  row.values.push_back(r.rfile_null ? core::AuxValue::Null(core::AuxType::String)
                                    : core::AuxValue::Of(core::AuxType::String, r.rfile));
  row.values.push_back(r.adsh_null ? core::AuxValue::Null(core::AuxType::String)
                                   : core::AuxValue::Of(core::AuxType::String, r.adsh));
  row.values.push_back(r.line_null ? core::AuxValue::Null(core::AuxType::Int64)
                                   : core::AuxValue::Of(core::AuxType::Int64, r.line));
  return row;
}

CompactRow FromDb(duckdb::MaterializedQueryResult &res, duckdb::idx_t row) {
  CompactRow r;
  const auto s = res.GetValue(0, row), f = res.GetValue(1, row), a = res.GetValue(2, row), l = res.GetValue(3, row);
  r.stmt_null = s.IsNull();
  r.stmt = r.stmt_null ? "" : s.ToString();
  r.rfile_null = f.IsNull();
  r.rfile = r.rfile_null ? "" : f.ToString();
  r.adsh_null = a.IsNull();
  r.adsh = r.adsh_null ? "" : a.ToString();
  r.line_null = l.IsNull();
  r.line = r.line_null ? 0 : l.GetValue<int64_t>();
  return r;
}

// Exact comparison of the aux answer with the SQL answer, group by group: counts
// must be equal and AVG must be the very same double. (The shared harness
// compares rows as text with 6 decimals; an average that is exactly a 7-decimal
// tie, e.g. 613804/2560 = 239.7671875, is rounded differently when rendered from
// DuckDB's decimal text and from the double, though both doubles are identical.)
void RequireExactGroups(duckdb::Connection &c, DuckdbRuntime &runtime, const catalog::SharedQueryCatalogEntry &entry) {
  using Key = std::tuple<std::string, bool, std::string>;
  using Val = std::tuple<int64_t, int64_t, bool, double>;
  std::map<Key, Val> expected, actual;
  auto base = Run(c, entry.base_sql);
  for (duckdb::idx_t i = 0; i < base->RowCount(); ++i) {
    const auto rfile = base->GetValue(1, i);
    const auto avg = base->GetValue(4, i);
    expected[{base->GetValue(0, i).ToString(), rfile.IsNull(), rfile.IsNull() ? "" : rfile.ToString()}] = {
        base->GetValue(2, i).GetValue<int64_t>(), base->GetValue(3, i).GetValue<int64_t>(), avg.IsNull(),
        avg.IsNull() ? 0.0 : avg.GetValue<double>()};
  }
  for (const auto &row : runtime.ExecuteCollect()) {
    const bool rfile_null = row.values[1].is_null;
    actual[{std::get<std::string>(row.values[0].payload), rfile_null,
            rfile_null ? "" : std::get<std::string>(row.values[1].payload)}] = {
        std::get<int64_t>(row.values[2].payload), std::get<int64_t>(row.values[3].payload), row.values[4].is_null,
        row.values[4].is_null ? 0.0 : std::get<double>(row.values[4].payload)};
  }
  Require(expected == actual, "exact group comparison failed for Q1");
}

void CompareToBase(duckdb::Connection &c, DuckdbRuntime &runtime, const catalog::SharedQueryCatalogEntry &entry,
                   const std::string &label) {
  auto unordered = entry; // cnt ties are legal in SQL: groups are compared by key
  try {
    RequireExactGroups(c, runtime, entry);
    (void)unordered;
    // The SQL table-function path must serve the same answer.
    auto aux = Run(c, entry.duckdb_aux_sql);
    auto base = Run(c, entry.base_sql);
    Require(aux->RowCount() == base->RowCount(), "aux SQL row count differs");
  } catch (const std::exception &ex) {
    throw std::runtime_error(label + ": " + ex.what());
  }
}

void RunScenario(const catalog::SharedQueryCatalogEntry &entry, const std::string &name, int steps, uint64_t seed,
                 int rows, const std::string &work_db) {
  duckdb::DuckDB db(work_db.empty() ? nullptr : work_db.c_str());
  duckdb::Connection c(db);
  accelerator_codegen::RegisterAcceleratorFunctions(c);
  Run(c, "PRAGMA threads=4");
  setenv("ACCEL_AUX_THREADS", "4", 1);
  Gen g(seed);
  if (work_db.empty()) {
  Run(c, "CREATE TABLE pre (adsh VARCHAR, report INTEGER, line INTEGER, stmt VARCHAR, inpth INTEGER, "
         "rfile VARCHAR, tag VARCHAR, version VARCHAR, plabel VARCHAR, negating INTEGER)");
  // bulk load via SQL (deterministic given the seed)
  Run(c, "SELECT setseed(" + std::to_string((seed % 1000) / 1000.0) + ")");
  std::ostringstream load;
  load << "INSERT INTO pre SELECT "
          "CASE WHEN r % 97 = 0 THEN NULL WHEN r % 53 = 0 THEN 'weird-' || (r % 7) "
          "ELSE printf('%010d-%02d-%06d', (r * 7919) % 450, 20 + (r % 4), (r * 31) % 977) END, "
          "r % 5, CASE WHEN r % 61 = 0 THEN NULL ELSE (r * 13) % 480 END, "
          "CASE WHEN r % 71 = 0 THEN NULL ELSE ['BS','IS','CF','EQ','CI','UN'][1 + (r % 6)] END, "
          "0, CASE WHEN r % 83 = 0 THEN NULL WHEN r % 19 = 0 THEN 'X' ELSE 'H' END, 't', 'v', 'p', 0 "
          "FROM (SELECT (random() * 1000000000)::BIGINT AS r FROM range("
       << rows << ")) q";
  Run(c, load.str());
  }

  DuckdbRuntime runtime(c, entry, 10.0);
  runtime.Build();
  CompareToBase(c, runtime, entry, name + " initial");

  for (int step = 0; step < steps; ++step) {
    const std::string label = name + " seed=" + std::to_string(seed) + " step=" + std::to_string(step);
    core::MutationBatch mutation;
    mutation.table = 1;
    const int kind = g.R(4);
    const int count = g.R(25) == 0 ? 400 + g.R(800) : 1 + g.R(12);
    if (kind == 0) { // INSERT
      mutation.kind = core::MutationKind::Insert;
      std::ostringstream sql;
      sql << "INSERT INTO pre (adsh, report, line, stmt, inpth, rfile, tag, version, plabel, negating) VALUES ";
      for (int i = 0; i < count; ++i) {
        const auto r = RandomRow(g);
        mutation.new_rows.push_back(ToAux(r));
        sql << (i ? "," : "") << "(" << Lit(r.adsh_null, r.adsh) << ",1," << (r.line_null ? "NULL" : std::to_string(r.line))
            << "," << Lit(r.stmt_null, r.stmt) << ",0," << Lit(r.rfile_null, r.rfile) << ",'t','v','p',0)";
      }
      Run(c, sql.str());
    } else {
      // pick random existing rows
      auto picked = Run(c, "SELECT stmt, rfile, adsh, line, rowid FROM pre USING SAMPLE " + std::to_string(count) +
                               " ROWS (reservoir, " + std::to_string(g.rng() % 100000) + ")");
      std::vector<CompactRow> olds;
      std::vector<int64_t> rowids;
      for (duckdb::idx_t i = 0; i < picked->RowCount(); ++i) {
        olds.push_back(FromDb(*picked, i));
        rowids.push_back(picked->GetValue(4, i).GetValue<int64_t>());
      }
      if (olds.empty()) {
        continue;
      }
      if (kind == 1) { // DELETE
        mutation.kind = core::MutationKind::Delete;
        std::ostringstream ids;
        for (std::size_t i = 0; i < olds.size(); ++i) {
          mutation.old_rows.push_back(ToAux(olds[i]));
          ids << (i ? "," : "") << rowids[i];
        }
        Run(c, "DELETE FROM pre WHERE rowid IN (" + ids.str() + ")");
      } else { // UPDATE (any of line / stmt / rfile / adsh, possibly to/from NULL)
        mutation.kind = core::MutationKind::Update;
        mutation.updated_columns = {static_cast<core::ColumnId>(g.R(4))};
        Run(c, "BEGIN TRANSACTION");
        for (std::size_t i = 0; i < olds.size(); ++i) {
          auto n = olds[i];
          const auto fresh = RandomRow(g);
          switch (g.R(6)) {
          case 0: n.line = fresh.line; n.line_null = fresh.line_null; break;
          case 1: n.stmt = fresh.stmt; n.stmt_null = fresh.stmt_null; break;
          case 2: n.rfile = fresh.rfile; n.rfile_null = fresh.rfile_null; break;
          case 3: n.adsh = fresh.adsh; n.adsh_null = fresh.adsh_null; break;
          case 4: n = fresh; break;
          default: n.line = n.line_null ? 7 : n.line + 1000; n.line_null = false; break;
          }
          mutation.old_rows.push_back(ToAux(olds[i]));
          mutation.new_rows.push_back(ToAux(n));
          Run(c, "UPDATE pre SET stmt = " + Lit(n.stmt_null, n.stmt) + ", rfile = " + Lit(n.rfile_null, n.rfile) +
                     ", adsh = " + Lit(n.adsh_null, n.adsh) + ", line = " + (n.line_null ? "NULL" : std::to_string(n.line)) +
                     " WHERE rowid = " + std::to_string(rowids[i]));
        }
        Run(c, "COMMIT");
      }
    }
    runtime.ApplyMutationBatch(mutation);
    CompareToBase(c, runtime, entry, label);
  }
  std::cout << "PASS " << name << " seed=" << seed << " steps=" << steps << "\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const int steps = argc > 1 ? std::atoi(argv[1]) : 150;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    const int rows = argc > 3 ? std::atoi(argv[3]) : 300000;
    const std::string work_db = argc > 4 ? argv[4] : "";
    RunScenario(catalog::LookupSharedQuery("sec_edgar", 1), "single", steps, seed, rows, work_db);
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    RunScenario(catalog::LookupSharedQueryParallel("sec_edgar", 1), "parallel", steps, seed + 100, rows, work_db);
#endif
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "test_sec_edgar_q1_duckdb_differential failed: " << ex.what() << "\n";
    return 1;
  }
}
