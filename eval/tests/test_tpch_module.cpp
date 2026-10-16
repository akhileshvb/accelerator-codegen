// Unit test of one TPC-H aux module, run alone on ModuleRuntime (no host, no
// table function): build, answer vs the reference SQL, maintenance of point
// writes (single and several in one commit), invalidation, rebuild.
// Built once per query with ACCEL_TPCH_QUERY=<N> as test_tpch_q<N>_aux.
//
// usage: test_tpch_q<N>_aux [tpch_test.duckdb]   (default: ACCEL_TPCH_TEST_DB)
#ifndef ACCEL_TPCH_QUERY
#error "ACCEL_TPCH_QUERY must name the query"
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "eval/bench/tpch/single-threaded/tpch_arg_variants.hpp"
#include "eval/harness/cost_calibration.hpp"
#include "eval/harness/differential.hpp"
#include "eval/tests/test_support/db_paths.hpp"
#include "eval/tests/test_support/module_runtime.hpp"

namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace engine = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;
namespace support = accelerator_codegen::aux_state::test;
namespace sweep = accelerator_codegen::benchmarks::aux_sweep;

constexpr int kQuery = ACCEL_TPCH_QUERY;
constexpr std::size_t kPointWrites = 3;
constexpr int kCommittedProbes = 4;

void Require(bool ok, const std::string &message) {
  if (!ok) {
    throw std::runtime_error("Q" + std::to_string(kQuery) + ": " + message);
  }
}

struct CollectSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

void Sql(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  Require(result && !result->HasError(), sql + ": " + (result ? result->GetError() : "null"));
}

// Rows equal; doubles up to the order their sums were accumulated in. (The
// reference SQL's comparison is too coarse to see one cent move a revenue.)
bool SameRows(const std::vector<core::AuxRow> &a, const std::vector<core::AuxRow> &b) {
  const auto same = [](const core::AuxValue &x, const core::AuxValue &y) {
    if (x.type != y.type || x.is_null != y.is_null) {
      return false;
    }
    if (x.is_null || x.type != core::AuxType::Double) {
      return x.is_null || x.payload == y.payload;
    }
    const double u = std::get<double>(x.payload), v = std::get<double>(y.payload);
    return std::abs(u - v) <= 1e-9 * std::max({1.0, std::abs(u), std::abs(v)});
  };
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [&](const core::AuxRow &x, const core::AuxRow &y) {
    return std::equal(x.values.begin(), x.values.end(), y.values.begin(), y.values.end(), same);
  });
}

// The query's module on ModuleRuntime.
struct HostedModule {
  HostedModule(duckdb::Connection &connection, const engine::SharedQueryCatalogEntry &entry)
      : binding(Bind(connection, entry)),
        runtime(connection, std::shared_ptr<core::AuxQueryModule>(binding, &binding->Module()),
                core::LoadAuxCostModelJson(harness::SharedCostModelConfigPath(entry))) {}

  static std::shared_ptr<engine::duckdb::DuckdbRuntime::Binding> Bind(
      duckdb::Connection &connection, const engine::SharedQueryCatalogEntry &entry) {
    const auto *query = engine::duckdb::FindAuxQuery(entry.accelerator_id);
    Require(query != nullptr, "no aux query registered");
    return query->make_binding(connection);
  }

  std::shared_ptr<engine::duckdb::DuckdbRuntime::Binding> binding;
  support::ModuleRuntime runtime;
};

class ModuleCheck {
public:
  ModuleCheck(duckdb::Connection &connection_p, const engine::SharedQueryCatalogEntry &entry_p)
      : connection(connection_p), entry(entry_p), tested(connection_p, entry_p), fresh(connection_p, entry_p),
        runtime(tested.runtime), variants(sweep::BuildTpchArgPool(kQuery, entry_p)) {}

  // The module answers every argument variant like the reference SQL and like
  // a module built from scratch, from its state (`must_serve`) or else from
  // the fallback.
  void ExpectMatchesReference(const std::string &when, bool must_serve = true) {
    fresh.runtime.Build();
    for (const auto &variant : variants) {
      CollectSink sink;
      Require(runtime.Read(variant.arguments, sink) || !must_serve, when + ": state did not serve");
      try {
        harness::CompareAuxRowsToSql(connection, entry, variant.base_sql, sink.rows);
        CollectSink expected;
        fresh.runtime.Read(variant.arguments, expected);
        Require(SameRows(sink.rows, expected.rows), "differs from a module built from scratch");
      } catch (const std::exception &error) {
        throw std::runtime_error("Q" + std::to_string(kQuery) + " " + when + ": " + error.what());
      }
    }
  }

  void Run() {
    runtime.Build();
    ExpectMatchesReference("after build");

    auto probes = engine::CollectInvalidatingSharedProbes(connection, entry);
    PointWrites(probes.front());
    CommitTogether({probes.begin() + 1, probes.begin() + 1 + std::min<long>(kCommittedProbes, probes.size() - 1)});

    runtime.Runtime().Invalidate();
    ExpectMatchesReference("after invalidation", false);
    runtime.Build();
    ExpectMatchesReference("after rebuild");
  }

private:
  // The same probe applied repeatedly, each write maintained on its own.
  void PointWrites(engine::SharedProbe probe) {
    for (std::size_t i = 0; i < kPointWrites; ++i) {
      Sql(connection, engine::ProbeWriteSql(probe));
      runtime.Commit({engine::BuildSharedMutation(entry, probe)});
      engine::AdvanceProbe(probe);
      ExpectMatchesReference("after point write " + std::to_string(i + 1));
    }
  }

  // Several rows written in one transaction, delivered as one commit.
  void CommitTogether(std::vector<engine::SharedProbe> probes) {
    if (probes.empty()) {
      return;
    }
    std::vector<core::MutationBatch> batches;
    Sql(connection, "BEGIN TRANSACTION");
    for (auto &probe : probes) {
      Sql(connection, engine::ProbeWriteSql(probe));
      batches.push_back(engine::BuildSharedMutation(entry, probe));
    }
    Sql(connection, "COMMIT");
    runtime.Commit(batches);
    ExpectMatchesReference("after a commit of " + std::to_string(probes.size()) + " writes");
  }

  duckdb::Connection &connection;
  const engine::SharedQueryCatalogEntry &entry;
  HostedModule tested, fresh;
  support::ModuleRuntime &runtime;
  std::vector<sweep::TpchArgVariant> variants;
};

} // namespace

int main(int argc, char **argv) {
  try {
    namespace ts = accelerator_codegen::test_support;
    ts::ScratchDir scratch("tpch_module_q" + std::to_string(kQuery));
    const std::string db_path =
        ts::PrivateDbCopy(argc > 1 ? argv[1] : ts::TpchTestDbPath(), scratch);
    duckdb::DuckDB database(db_path.c_str());
    duckdb::Connection connection(database);
    ModuleCheck(connection, engine::LookupSharedQuery(engine::SharedWorkloadKind::Tpch, kQuery)).Run();
    std::cout << "PASS tpch Q" << kQuery << " aux module\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
