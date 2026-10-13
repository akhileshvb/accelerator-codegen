// Module-level randomized differential test for the SEC-EDGAR Q4 aux module.
//
// A tiny in-memory "database" (num/sub/tag/pre in physical column layout) is
// the source of truth.  A naive nested-loop evaluator of q4.sql is the oracle.
// Every table x {INSERT, DELETE, UPDATE (key/filter/payload columns)} is
// exercised through the real AuxTransactionCoordinator + DuckDBStateStore,
// including multi-mutation transactions and aborts, and the module output is
// compared with the oracle after every transaction.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "sec_edgar/q4/common/aux/module.hpp"
#include "test_support/sec_edgar_q4_txn_gen.hpp"

using namespace accelerator_codegen::aux_state::core;
namespace aas = accelerator_codegen::aux_state;
using accelerator_codegen::aux_state::query::SecEdgarQ4Config;
using accelerator_codegen::aux_state::query::SecEdgarQ4InitialScan;
using accelerator_codegen::aux_state::query::SecEdgarQ4Module;
using accelerator_codegen::aux_state::query::SecEdgarQ4NumInput;
using accelerator_codegen::aux_state::query::SecEdgarQ4PreInput;
using accelerator_codegen::aux_state::query::SecEdgarQ4SubInput;
using accelerator_codegen::aux_state::query::SecEdgarQ4TagInput;

namespace {

using q4gen::Db;
using q4gen::NumRow;
using q4gen::OutRow;
using q4gen::Oracle;
using q4gen::PreRow;
using q4gen::SubRow;
using q4gen::TagRow;
using q4gen::ToRow;
constexpr TableId kNum = q4gen::kNum, kSub = q4gen::kSub, kTag = q4gen::kTag, kPre = q4gen::kPre;

// ------------------------------------------------------------ mock engine
class Services final : public AuxEngineServices, public SecEdgarQ4InitialScan {
public:
  Services() : store("sec_edgar_q4") {}

  void ScanSub(const std::function<void(const SecEdgarQ4SubInput &)> &consumer) const override {
    for (const auto &s : db->sub) {
      if (!s.sic) continue;
      SecEdgarQ4SubInput in;
      in.adsh = s.adsh;
      in.cik = s.cik.value_or(0);
      in.cik_null = !s.cik;
      in.sic = *s.sic;
      consumer(in);
    }
  }
  void ScanTag(const std::function<void(const SecEdgarQ4TagInput &)> &consumer) const override {
    for (const auto &t : db->tag) {
      if (!t.abstract) continue;
      SecEdgarQ4TagInput in;
      in.tag = t.tag;
      in.version = t.version;
      in.abstract_flag = *t.abstract;
      in.tlabel = t.tlabel.value_or("");
      in.tlabel_null = !t.tlabel;
      consumer(in);
    }
  }
  void ScanPre(const std::function<void(const SecEdgarQ4PreInput &)> &consumer) const override {
    for (const auto &p : db->pre) {
      if (p.adsh && p.tag && p.version && p.stmt) {
        consumer({*p.adsh, *p.tag, *p.version, *p.stmt});
      }
    }
  }
  void ScanNum(const std::function<void(const SecEdgarQ4NumInput &)> &consumer) const override {
    ++num_scans;
    for (const auto &n : db->num) {
      if (n.adsh && n.tag && n.version && n.uom) {
        consumer({*n.adsh, *n.tag, *n.version, *n.uom, n.value.value_or(0.0), !n.value});
      }
    }
  }

  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  const Db *db = nullptr;
  mutable int num_scans = 0;
  accelerator_codegen::aux_state::engine::duckdb::DuckDBStateStore store;
};

class CountingModule final : public SecEdgarQ4Module {
public:
  using SecEdgarQ4Module::SecEdgarQ4Module;
  void Build(AuxEngineServices &s, StateBuilder &b) override {
    ++builds;
    SecEdgarQ4Module::Build(s, b);
  }
  int builds = 0;
};

class Builder final : public StateBuilder {
public:
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

class Sink final : public AuxRowSink {
public:
  void Begin(const OutputSchema &schema) override { assert(schema.columns.size() == 6); }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

bool Close(long double a, long double b) {
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
  if (std::isinf(a) || std::isinf(b)) return a == b;
  const long double scale = std::max<long double>({1.0L, std::fabsl(a), std::fabsl(b)});
  return std::fabsl(a - b) <= scale * 1e-9L;
}

void Publish(Services &services, std::vector<StateImage> images) {
  services.store.Install(std::move(images), "sec_edgar_q4");
}

std::string Describe(const std::vector<OutRow> &rows, size_t n = 3) {
  std::string out;
  for (size_t i = 0; i < std::min(n, rows.size()); ++i) {
    out += "[" + std::to_string(rows[i].sic) + "," + rows[i].tlabel.value_or("NULL") + "," +
           std::to_string(rows[i].companies) + "," + std::to_string((double)rows[i].total) + "] ";
  }
  return out;
}

QueryArguments ToArgs(const q4gen::Params &p) {
  QueryArguments a;
  a.values.push_back(p.lo ? AuxValue::Of(AuxType::Int64, *p.lo) : AuxValue::Null(AuxType::Int64));
  a.values.push_back(p.hi ? AuxValue::Of(AuxType::Int64, *p.hi) : AuxValue::Null(AuxType::Int64));
  a.values.push_back(p.stmt ? AuxValue::Of(AuxType::String, *p.stmt) : AuxValue::Null(AuxType::String));
  a.values.push_back(p.uom ? AuxValue::Of(AuxType::String, *p.uom) : AuxValue::Null(AuxType::String));
  return a;
}

void Check(CountingModule &module, Services &services, const Db &db, const char *what,
           int step, const q4gen::Params &params = q4gen::Params{}, bool explicit_args = false) {
  Sink sink;
  if (explicit_args) {
    module.Execute(services, ToArgs(params), sink);
  } else {
    module.Execute(services, {}, sink);
  }
  assert(sink.ended);
  const auto expected = Oracle(db, params);
  bool ok = sink.rows.size() == expected.size();
  std::string detail = "rows expected=" + std::to_string(expected.size()) +
                       " actual=" + std::to_string(sink.rows.size());
  for (size_t i = 0; ok && i < expected.size(); ++i) {
    const auto &row = sink.rows[i];
    const auto sic = std::get<int64_t>(row.values[0].payload);
    const bool tl_null = row.values[1].is_null;
    const std::optional<std::string> tl =
        tl_null ? std::nullopt : std::optional<std::string>(std::get<std::string>(row.values[1].payload));
    const auto companies = std::get<int64_t>(row.values[3].payload);
    const auto total = std::get<double>(row.values[4].payload);
    const auto avg = std::get<double>(row.values[5].payload);
    if (sic != expected[i].sic || tl != expected[i].tlabel || companies != expected[i].companies ||
        !Close(total, expected[i].total) || !Close(avg, expected[i].avg) ||
        std::get<std::string>(row.values[2].payload) != *params.stmt) {
      ok = false;
      detail = "mismatch at row " + std::to_string(i) + " expected " + Describe({expected[i]}, 1) +
               " got sic=" + std::to_string(sic) + " tl=" + (tl_null ? "NULL" : *tl) +
               " n=" + std::to_string(companies) + " total=" + std::to_string(total) +
               " avg=" + std::to_string(avg) + " expected_total=" + std::to_string((double)expected[i].total) +
               " expected_avg=" + std::to_string((double)expected[i].avg) + " expected_n=" + std::to_string(expected[i].companies);
    }
  }
  if (!ok) {
    std::cerr << "FAIL step " << step << " (" << what << "): " << detail << '\n';
    std::abort();
  }
}

} // namespace

int main(int argc, char **argv) {
  {
    accelerator_codegen::aux_state::maintenance::CostModelParams gated;
    gated.apply_delta_max_rows = 1024;
    const auto small = SelectAuxMaintenanceAction(
        {1, 1, true, false, true, false, false, MaintenanceAction::NoOp, 10.0}, gated);
    assert(small.action == MaintenanceAction::ApplyDelta);
  }
  const int seeds = argc > 1 ? std::atoi(argv[1]) : 3;
  const int steps = argc > 2 ? std::atoi(argv[2]) : 300;

  for (int seed = 1; seed <= seeds + 1; ++seed) {
    const bool limit_case = seed == seeds + 1; // > 500 qualifying groups
    auto config = q4gen::MakeConfig(seed, limit_case);
    config.wide_keys = seed % 2 == 1;                       // negative / huge cik values
    config.nonfinite = !limit_case && seed % 3 != 0;        // inf, NaN, 1e300, subnormals
    q4gen::Model model(static_cast<uint64_t>(seed) * 7919, config);
    model.Init();
    Db &db = model.db;

    Services services;
    services.db = &db;
    auto module = std::make_shared<CountingModule>(SecEdgarQ4Config{kNum, kSub, kTag, kPre}, &services);
    AuxTransactionCoordinator coordinator;
    coordinator.Register(module);
    coordinator.AttachServices(services);
    {
      Builder builder;
      module->Build(services, builder);
      assert(builder.finished);
      Publish(services, std::move(builder.images));
    }
    const int builds_after_initial = module->builds;
    assert(module->CheckValidity(services, {}));
    Check(*module, services, db, "initial", 0);
    for (int k = 0; k < 12; ++k) {
      Check(*module, services, db, "initial-params", 0, q4gen::RandomParams(model), true);
    }
    if (limit_case) {
      Sink big;
      module->Execute(services, {}, big);
      assert(big.rows.size() == 500 && "LIMIT 500 must truncate a >500 group result");
    }

    int incremental = 0, rebuilt = 0, param_checks = 0;
    size_t result_rows_total = 0, param_rows_total = 0;
    for (int step = 1; step <= steps; ++step) {
      const Db snapshot = db;
      const int64_t rid_snapshot = model.next_rid;
      const bool abort_txn = model.Coin(0.08);
      const int builds_before = module->builds;
      auto txn = model.NextTxn();
      for (const auto &mutation : txn) {
        coordinator.OnMutation(mutation.batch);
      }
      coordinator.PreCommit();
      if (abort_txn) {
        coordinator.Abort();
        db = snapshot; // rolled back
        model.next_rid = rid_snapshot;
      } else {
        coordinator.Commit();
      }
      if (module->builds > builds_before) {
        ++rebuilt;
      } else {
        ++incremental;
      }
      assert(module->CheckValidity(services, {}));
      Check(*module, services, db, model.last_what, step);
      result_rows_total += Oracle(db).size();
      // Random parameters (sic ranges, stmt, uom, NULLs) against the same state.
      for (int k = 0; k < 3; ++k) {
        const auto params = q4gen::RandomParams(model);
        Check(*module, services, db, model.last_what, step, params, true);
        param_rows_total += Oracle(db, params).size();
        ++param_checks;
      }
    }
    std::cerr << "seed " << seed << ": " << steps << " txns ok (incremental=" << incremental
              << " rebuilds=" << rebuilt << " initial_builds=" << builds_after_initial
              << " avg_result_rows=" << result_rows_total / steps << " param_checks=" << param_checks
              << " avg_param_rows=" << param_rows_total / std::max(1, param_checks) << ")\n";
    // Everything except parse failures / oversized batches stays incremental.
    // Non-finite data (inf/NaN cannot be subtracted back out) legitimately rebuilds.
    assert(rebuilt <= (config.nonfinite ? steps / 2 : steps / 20));
  }

// ---- targeted: pre/sub/tag/num edge cases against the oracle
  {
    Db db;
    db.sub = {{"a", 100, 4100, "x"}, {"b", 200, 4100, "x"}, {"c", 300, 4000, "x"}};
    db.tag = {{"T", "1", 0, "Rev"}, {"U", "1", 0, std::nullopt}};
    db.pre = {{"a", "T", "1", "EQ", 1}, {"b", "T", "1", "EQ", 1}, {"a", "U", "1", "EQ", 1},
              {"b", "U", "1", "EQ", 1}};
    db.num = {{"a", "T", "1", "USD", 100.0, 1}, {"b", "T", "1", "USD", 200.0, 1},
              {"a", "U", "1", "USD", 5.0, 1}, {"b", "U", "1", "USD", 6.0, 1}};
    Services services;
    services.db = &db;
    auto module = std::make_shared<CountingModule>(SecEdgarQ4Config{kNum, kSub, kTag, kPre}, &services);
    AuxTransactionCoordinator coordinator;
    coordinator.Register(module);
    coordinator.AttachServices(services);
    Builder builder;
    module->Build(services, builder);
    Publish(services, std::move(builder.images));
    Check(*module, services, db, "edge-initial", 0); // NULL tlabel group present
    Sink sink;
    module->Execute(services, {}, sink);
    assert(sink.rows.size() == 2);

    // duplicate pre row doubles the contribution (multiplicity), insert is incremental
    PreRow dup{std::string("a"), std::string("T"), std::string("1"), std::string("EQ"), 2};
    db.pre.push_back(dup);
    MutationBatch m;
    m.table = kPre;
    m.kind = MutationKind::Insert;
    m.new_rows = {ToRow(dup)};
    coordinator.OnMutation(m);
    coordinator.PreCommit();
    coordinator.Commit();
    Check(*module, services, db, "pre-dup", 1);

    // the second cik drops out of a group when its only num row is deleted
    const NumRow gone = db.num[1];
    db.num.erase(db.num.begin() + 1);
    MutationBatch d;
    d.table = kNum;
    d.kind = MutationKind::Delete;
    d.old_rows = {ToRow(gone)};
    coordinator.OnMutation(d);
    coordinator.PreCommit();
    coordinator.Commit();
    Check(*module, services, db, "num-delete-distinct", 2);
    Sink after;
    module->Execute(services, {}, after);
    assert(after.rows.size() == 1); // only the NULL-tlabel group still has 2 companies
    assert(module->builds == 1);
  }
  std::cout << "test_shared_sec_edgar_q4 passed\n";
  return 0;
}
