// Concurrent snapshot test of the Verus Q21 accelerator on DuckDB: readers
// in other connections must see their own snapshot's state while a writer
// commits.
//
//   q21_verus_duckdb_snapshots <db-copy> <writes> <readers> <seed>
//
// One writer commits single-row updates that change Q21's answer (an order's
// o_orderstatus flips between 'F' and 'O', or a lineitem's receipt date
// moves across its commit date), captured by the engine
// (DuckdbRuntime::EnableMutationCapture).  Each reader repeatedly opens a
// transaction and runs, at that one snapshot, the accelerator, a pause, the
// catalog SQL, a pause, and the accelerator again; all three must agree,
// while commits land during the pauses.  A reader that saw a version older
// or newer than its snapshot (or one mixing two commits) fails the check.
//
// Run it with ACCEL_AUX_STORE=verus for the verified Verus store, unset for
// the C++ store.  The database file is modified; pass a copy.
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "catalog/query_catalog.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
namespace catalog = accelerator_codegen::aux_state::engine;
using catalog::duckdb::DuckdbRuntime;

void Require(bool ok, const std::string &msg) {
  if (!ok)
    throw std::runtime_error(msg);
}

std::unique_ptr<duckdb::MaterializedQueryResult> Q(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  Require(r && !r->HasError(), (r ? r->GetError() : "query failed") + ": " + sql);
  return r;
}

std::string Lit(const std::string &s) {
  std::string out = "'";
  for (char ch : s)
    out += ch == '\'' ? std::string("''") : std::string(1, ch);
  return out + "'";
}

std::string BaseSql(const std::string &p) {
  return "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation "
         "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
         "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
         "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
         "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE "
         "l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
         "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND n_name = " +
         Lit(p) + " GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100";
}

std::string AccelSql(const std::string &p) {
  return "SELECT * FROM accel_tpch_q21_verus(" + Lit(p) + ")";
}

using Rows = std::vector<std::pair<std::string, int64_t>>;
Rows Fetch(duckdb::Connection &c, const std::string &sql) {
  auto r = Q(c, sql);
  Rows rows;
  for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
    rows.emplace_back(r->GetValue(0, i).ToString(), r->GetValue(1, i).GetValue<int64_t>());
  return rows;
}

void PauseUpTo(std::mt19937_64 &rng, int micros) {
  std::this_thread::sleep_for(std::chrono::microseconds(rng() % static_cast<uint64_t>(micros)));
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 5, "usage: q21_verus_duckdb_snapshots <db-copy> <writes> <readers> <seed>");
    const std::string path = argv[1];
    const int writes = std::atoi(argv[2]);
    const int readers = std::atoi(argv[3]);
    const uint64_t seed = std::strtoull(argv[4], nullptr, 10);

    duckdb::DuckDB db(path);
    duckdb::Connection wc(db), setup(db);
    accelerator_codegen::RegisterAcceleratorFunctions(setup);

    // Nations with suppliers, and orders whose lineitems they supply.
    std::vector<std::string> nations;
    {
      auto r = Q(setup, "SELECT DISTINCT n_name FROM nation, supplier WHERE s_nationkey = n_nationkey "
                        "ORDER BY 1 LIMIT 6");
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
        nations.push_back(r->GetValue(0, i).ToString());
    }
    std::vector<int64_t> keys;
    {
      auto r = Q(setup, "SELECT DISTINCT l_orderkey FROM lineitem, supplier, nation "
                        "WHERE l_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name IN (" +
                            [&] {
                              std::string in;
                              for (const auto &n : nations)
                                in += (in.empty() ? "" : ", ") + Lit(n);
                              return in;
                            }() +
                            ") ORDER BY 1 LIMIT 2000");
      for (duckdb::idx_t i = 0; i < r->RowCount(); ++i)
        keys.push_back(r->GetValue(0, i).GetValue<int64_t>());
    }
    Require(!nations.empty() && !keys.empty(), "no Q21 data to write on");

    DuckdbRuntime rt(wc, *catalog::FindSharedQueryByAccelerator("tpch_q21_verus"), 10.0);
    rt.EnableMutationCapture();
    rt.Build();

    std::atomic<bool> done{false};
    std::atomic<int> checks{0}, mismatches{0}, failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < readers; ++t) {
      threads.emplace_back([&, t] {
        std::mt19937_64 rng(seed * 1000003 + t);
        try {
          duckdb::Connection rc(db);
          while (!done.load()) {
            const auto &p = nations[rng() % nations.size()];
            Q(rc, "BEGIN TRANSACTION");
            const auto a = Fetch(rc, AccelSql(p)); // the snapshot starts here
            PauseUpTo(rng, 3000);
            const auto b = Fetch(rc, BaseSql(p));
            PauseUpTo(rng, 3000);
            const auto c = Fetch(rc, AccelSql(p));
            Q(rc, "COMMIT");
            ++checks;
            if (a != b || c != b) {
              if (++mismatches <= 10)
                std::cout << "MISMATCH reader " << t << " nation '" << p << "': accel " << a.size()
                          << " / " << c.size() << " rows, sql " << b.size() << " rows\n";
            }
          }
        } catch (const std::exception &e) {
          if (++failures <= 5)
            std::cerr << "reader " << t << " failed: " << e.what() << "\n";
        }
      });
    }

    std::mt19937_64 rng(seed);
    for (int i = 0; i < writes; ++i) {
      const auto k = std::to_string(keys[rng() % keys.size()]);
      if (rng() % 2) {
        rt.ApplyWrite("UPDATE orders SET o_orderstatus = CASE WHEN o_orderstatus = 'F' THEN 'O' "
                      "ELSE 'F' END WHERE o_orderkey = " + k);
      } else {
        rt.ApplyWrite("UPDATE lineitem SET l_receiptdate = CASE WHEN l_receiptdate > l_commitdate "
                      "THEN l_commitdate ELSE l_commitdate + 1 END WHERE l_orderkey = " + k +
                      " AND l_linenumber = (SELECT min(l_linenumber) FROM lineitem WHERE "
                      "l_orderkey = " + k + ")");
      }
      PauseUpTo(rng, 2000);
    }
    done = true;
    for (auto &th : threads)
      th.join();

    // The newest state against the SQL, outside any reader's snapshot.
    for (const auto &p : nations) {
      ++checks;
      if (Fetch(setup, AccelSql(p)) != Fetch(setup, BaseSql(p))) {
        ++mismatches;
        std::cout << "MISMATCH final nation '" << p << "'\n";
      }
    }

    const bool ok = mismatches == 0 && failures == 0 && rt.StaleInstallCount() == 0;
    std::cout << "q21 verus duckdb snapshots: store " << (rt.UsesVerusStore() ? "verus" : "cpp")
              << " writes " << writes << " readers " << readers << " checks " << checks
              << " mismatches " << mismatches << " reader_failures " << failures
              << " reader_fallbacks " << rt.reader_fallbacks.load() << " versions "
              << rt.StateVersionCount() << " stale_installs " << rt.StaleInstallCount() << "\n";
    std::cout << "q21 verus duckdb snapshots: " << (ok ? "PASS" : "FAIL") << std::endl;
    return ok ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "q21_verus_duckdb_snapshots failed: " << e.what() << "\n";
    return 2;
  }
}
