// Q24 aux module: engine-free randomized differential against a brute-force
// oracle of the SQL semantics. Covers every maintenance action (INSERT / DELETE
// / UPDATE on num and pre, join-key and filter-column moves, irrelevant column
// updates, NULL keys, multi-mutation transactions, aborts, bulk rebuild) and
// checks the incremental path really stays incremental (no re-scan).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "sec_edgar/q24/duckdb/aux/module.hpp"
#include "test_support/q24_random_writes.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::engine::duckdb::DuckDBStateStore;
using namespace accelerator_codegen::aux_state::query;

namespace {

using namespace q24test;

struct Expected {
  std::optional<std::string> tag, version;
  int64_t cnt;
  double total;
};

// Brute-force evaluation of the query text (tie-break: tag, version asc).
std::vector<Expected> Oracle(const std::vector<NumRow> &num,
                             const std::vector<PreRow> &pre) {
  std::set<std::tuple<std::string, std::string, std::string>> p;
  for (const auto &r : pre)
    if (r.adsh && r.tag && r.version) p.insert({*r.adsh, *r.tag, *r.version});
  std::map<std::pair<OptStr, OptStr>, std::pair<int64_t, long double>> groups;
  for (const auto &r : num) {
    if (!(r.uom && *r.uom == "USD" && r.ddate && *r.ddate >= 20230101 &&
          *r.ddate <= 20231231 && r.value))
      continue;
    if (r.adsh && r.tag && r.version && p.count({*r.adsh, *r.tag, *r.version}))
      continue;
    auto &g = groups[{r.tag, r.version}];
    g.first++;
    g.second += *r.value;
  }
  std::vector<Expected> out;
  for (const auto &[k, g] : groups)
    if (g.first > 10)
      out.push_back({k.first, k.second, g.first, static_cast<double>(g.second)});
  std::stable_sort(out.begin(), out.end(), [](const Expected &a, const Expected &b) {
    return a.cnt > b.cnt; // map order already (tag, version) ascending
  });
  if (out.size() > 100) out.resize(100);
  return out;
}

class Services final : public AuxEngineServices, public SecEdgarQ24InitialScan {
public:
  Services() : store("sec_edgar_q24") {}

  void ScanNum(const std::function<void(const SecEdgarQ24NumInput &)> &c) override {
    scans++;
    for (const auto &r : num) {
      assert(r.adsh && r.tag && r.version && r.uom && r.ddate);
      c({*r.adsh, *r.tag, *r.version, *r.uom, r.value ? *r.value : 0.0,
         !r.value.has_value(), static_cast<int32_t>(*r.ddate)});
    }
  }
  void ScanPre(const std::function<void(const SecEdgarQ24PreInput &)> &c) override {
    scans++;
    for (const auto &r : pre)
      if (r.adsh && r.tag && r.version) c({*r.adsh, *r.tag, *r.version});
  }
  // Targeted lookup (not a rescan): post-transaction truth per triple.
  bool LoadTripleStates(const std::vector<SecEdgarQ24TripleKey> &keys,
                        std::vector<SecEdgarQ24TripleState> &out) override {
    if (!use_triple_lookup) return false;
    out.assign(keys.size(), {});
    for (std::size_t i = 0; i < keys.size(); ++i) {
      for (const auto &r : pre)
        if (r.adsh && r.tag && r.version && *r.adsh == keys[i].adsh &&
            *r.tag == keys[i].tag && *r.version == keys[i].version)
          out[i].pre_rows++;
      for (const auto &r : num)
        if (r.adsh && r.tag && r.version && *r.adsh == keys[i].adsh &&
            *r.tag == keys[i].tag && *r.version == keys[i].version && r.uom &&
            *r.uom == "USD" && r.ddate && *r.ddate >= 20230101 &&
            *r.ddate <= 20231231 && r.value) {
          out[i].num_rows++;
          out[i].num_sum += *r.value;
        }
    }
    return true;
  }
  bool ScanQualifyingNumBatches(
      const std::function<void(const SecEdgarQ24NumBatch &)> &c) override {
    if (!use_batches) return false;
    scans++;
    // Deliver in small batches to exercise batch boundaries.
    std::vector<Q24Str> a, t, v;
    std::vector<double> val;
    auto flush = [&] {
      if (a.empty()) return;
      c({a.size(), a.data(), t.data(), v.data(), val.data()});
      a.clear(); t.clear(); v.clear(); val.clear();
    };
    auto S = [](const OptStr &s) { return s ? Q24Str::Of(*s) : Q24Str::Null(); };
    for (const auto &r : num) {
      if (!(r.uom && *r.uom == "USD" && r.ddate && *r.ddate >= 20230101 &&
            *r.ddate <= 20231231 && r.value))
        continue;
      a.push_back(S(r.adsh)); t.push_back(S(r.tag)); v.push_back(S(r.version));
      val.push_back(*r.value);
      if (a.size() == 7) flush();
    }
    flush();
    return true;
  }
  bool ScanPreBatches(
      const std::function<void(const SecEdgarQ24PreBatch &)> &c) override {
    if (!use_batches) return false;
    scans++;
    std::vector<Q24Str> a, t, v;
    auto flush = [&] {
      if (a.empty()) return;
      c({a.size(), a.data(), t.data(), v.data()});
      a.clear(); t.clear(); v.clear();
    };
    auto S = [](const OptStr &s) { return s ? Q24Str::Of(*s) : Q24Str::Null(); };
    for (const auto &r : pre) {
      a.push_back(S(r.adsh)); t.push_back(S(r.tag)); v.push_back(S(r.version));
      if (a.size() == 5) flush();
    }
    flush();
    return true;
  }

  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }
  MaintenanceDecision ChooseMaintenance(const MaintenanceRequest &r) const override {
    if (r.incremental_supported && !r.whole_state_required && r.rows_touched <= 1024)
      return {MaintenanceAction::ApplyDelta, 0.0};
    return {MaintenanceAction::RebuildAll, 0.0};
  }

  DuckDBStateStore store;
  std::vector<NumRow> num;
  std::vector<PreRow> pre;
  bool use_batches = true;
  bool use_triple_lookup = true;
  int scans = 0;
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
  void Begin(const OutputSchema &schema) override {
    assert(schema.columns.size() == 4);
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

struct Fixture {
  Services services;
  std::shared_ptr<SecEdgarQ24Module> module;
  AuxTransactionCoordinator coordinator;

  Fixture() {
    module = std::make_shared<SecEdgarQ24Module>(SecEdgarQ24Config{101, 102},
                                                 &services);
    coordinator.Register(module);
    coordinator.AttachServices(services);
  }
  void Build() {
    Builder b;
    module->Build(services, b);
    assert(b.finished);
    services.store.Install(std::move(b.images), "sec_edgar_q24");
  }
  void Check(const char *what) {
    Sink sink;
    module->Execute(services, {}, sink);
    assert(sink.ended);
    const auto expected = Oracle(services.num, services.pre);
    if (sink.rows.size() != expected.size()) {
      std::cerr << "FAIL(" << what << "): rows " << sink.rows.size() << " vs "
                << expected.size() << "\n";
      std::abort();
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto &r = sink.rows[i];
      const auto &e = expected[i];
      const bool tag_ok = e.tag ? (!r.values[0].is_null &&
                                   std::get<std::string>(r.values[0].payload) == *e.tag)
                                : r.values[0].is_null;
      const bool ver_ok = e.version
                              ? (!r.values[1].is_null &&
                                 std::get<std::string>(r.values[1].payload) == *e.version)
                              : r.values[1].is_null;
      const auto cnt = std::get<int64_t>(r.values[2].payload);
      const auto total = std::get<double>(r.values[3].payload);
      const double scale = std::max({1.0, std::abs(total), std::abs(e.total)});
      if (!tag_ok || !ver_ok || cnt != e.cnt ||
          !(std::abs(total - e.total) <= scale * 1e-9 ||
            (std::isinf(total) && total == e.total))) {
        std::cerr << "FAIL(" << what << "): row " << i << " got cnt=" << cnt
                  << " total=" << total << " expected cnt=" << e.cnt
                  << " total=" << e.total << "\n";
        std::abort();
      }
    }
  }
  void Txn(const std::vector<MutationBatch> &batches, bool commit = true) {
    for (const auto &b : batches) coordinator.OnMutation(b);
    if (!commit) {
      coordinator.Abort();
      return;
    }
    coordinator.PreCommit();
    coordinator.Commit();
  }
};

void RunRandom(uint64_t seed, bool nulls, bool batches, int steps) {
  Fixture f;
  Gen g(seed, nulls);
  f.services.use_batches = batches;
  for (int i = 0; i < 700; ++i) f.services.num.push_back(g.Num());
  for (int i = 0; i < 120; ++i) f.services.pre.push_back(g.Pre());
  f.Build();
  assert(f.module->CheckValidity(f.services, {}));
  f.Check("initial build");

  int incremental = 0, rebuilt = 0;
  for (int step = 0; step < steps; ++step) {
    const int txn_size = g.R(6) == 0 ? 1 + g.R(4) : 1; // multi-mutation txns
    const bool abort_txn = g.R(15) == 0;
    std::vector<MutationBatch> batches;
    bool expect_incremental = true;
    // Snapshot the oracle tables so an aborted txn can be undone.
    const auto num_before = f.services.num;
    const auto pre_before = f.services.pre;
    int kind = 0;
    for (int i = 0; i < txn_size; ++i) {
      bool inc = true;
      Typed typed;
      batches.push_back(RandomWrite(g, f.services.num, f.services.pre, inc, kind, typed));
      expect_incremental = expect_incremental && inc;
    }
    const int scans_before = f.services.scans;
    f.Txn(batches, !abort_txn);
    if (abort_txn) {
      f.services.num = num_before;
      f.services.pre = pre_before;
    } else {
      if (f.services.scans == scans_before) incremental++; else rebuilt++;
      if (expect_incremental && f.services.scans != scans_before) {
        std::cerr << "FAIL: expected incremental maintenance but state was "
                     "rescanned (seed " << seed << " step " << step << " kind "
                  << kind << ")\n";
        std::abort();
      }
      if (!expect_incremental && f.services.scans == scans_before) {
        // pre delete/update of the *last* copy must rebuild; that is the only
        // way these kinds can avoid a scan if the batch was empty.
      }
    }
    assert(f.module->CheckValidity(f.services, {}));
    f.Check(("seed " + std::to_string(seed) + " step " + std::to_string(step)).c_str());
  }
  std::cout << "seed " << seed << " nulls=" << nulls << " batches=" << batches
            << ": " << incremental << " incremental, " << rebuilt
            << " rebuilt txns OK\n";
  assert(incremental > rebuilt);
}

void TestEdgeCases() {
  // Tie-heavy top-k, >100 groups: exact rows must still equal the oracle
  // (module and oracle share the same deterministic tie-break).
  {
    Fixture f;
    for (int t = 0; t < 130; ++t)
      for (int i = 0; i < 11 + (t % 3); ++i)
        f.services.num.push_back({"adsh" + std::to_string(i), "T" + std::to_string(t),
                                  "v", 20230601, 0, "USD", "", 1.5 * i});
    f.Build();
    f.Check("many groups");
    // Make one group lose rows below the HAVING threshold, another gain.
    std::vector<NumRow> victims;
    for (int i = 0; i < 3; ++i) {
      victims.push_back(f.services.num.back());
      f.services.num.pop_back();
    }
    f.Txn({NumBatch(MutationKind::Delete, victims, {})});
    f.Check("threshold delete");
  }
  // HAVING boundary: exactly 10 -> absent, 11 -> present, back to 10 -> absent.
  {
    Fixture f;
    for (int i = 0; i < 10; ++i)
      f.services.num.push_back({"a" + std::to_string(i), "T", "v", 20230601, 0, "USD", "", 1.0});
    f.Build();
    f.Check("10 rows");
    NumRow extra{"aX", "T", "v", 20230601, 0, "USD", "", 5.0};
    f.services.num.push_back(extra);
    f.Txn({NumBatch(MutationKind::Insert, {}, {extra})});
    f.Check("11 rows");
    f.services.num.pop_back();
    f.Txn({NumBatch(MutationKind::Delete, {extra}, {})});
    f.Check("back to 10");
  }
  // Pre insert retracts orphans; a later pre-insert duplicate is a no-op.
  {
    Fixture f;
    for (int i = 0; i < 15; ++i)
      f.services.num.push_back({"a0", "T", "v", 20230601, 0, "USD", "", 2.0});
    f.Build();
    f.Check("orphans");
    const int scans = f.services.scans;
    f.services.pre.push_back({"a0", "T", "v"});
    f.Txn({PreBatch(MutationKind::Insert, {}, {f.services.pre.back()})});
    f.Check("pre insert kills orphans");
    f.services.pre.push_back({"a0", "T", "v"});
    f.Txn({PreBatch(MutationKind::Insert, {}, {f.services.pre.back()})});
    f.Check("duplicate pre insert");
    assert(f.services.scans == scans); // all incremental
    // Deleting one of two duplicate pre rows keeps the triple matched.
    f.services.pre.pop_back();
    f.Txn({PreBatch(MutationKind::Delete, {{"a0", "T", "v"}}, {})});
    f.Check("duplicate pre delete");
    // Deleting the last copy turns the rows back into orphans.
    f.services.pre.pop_back();
    f.Txn({PreBatch(MutationKind::Delete, {{"a0", "T", "v"}}, {})});
    f.Check("last pre delete");
    assert(f.services.scans == scans); // pre deletes never rescan
    // Same txn: delete the (absent) triple's pre row and touch its num rows.
    f.services.pre.push_back({"a0", "T", "v"});
    f.Txn({PreBatch(MutationKind::Insert, {}, {f.services.pre.back()})});
    f.Check("re-insert pre");
    f.services.pre.pop_back();
    NumRow extra{"a0", "T", "v", 20230601, 0, "USD", "", 7.0};
    f.services.num.push_back(extra);
    f.Txn({PreBatch(MutationKind::Delete, {{"a0", "T", "v"}}, {}),
           NumBatch(MutationKind::Insert, {}, {extra})});
    f.Check("pre delete + num insert same txn");
    assert(f.services.scans == scans);
    // Without the lookup hook, pre deletes fall back to a rebuild (exact).
    f.services.use_triple_lookup = false;
    f.services.pre.push_back({"a0", "T", "v"});
    f.Txn({PreBatch(MutationKind::Insert, {}, {f.services.pre.back()})});
    f.services.pre.pop_back();
    f.Txn({PreBatch(MutationKind::Delete, {{"a0", "T", "v"}}, {})});
    f.Check("pre delete without lookup hook");
    assert(f.services.scans > scans);
  }
  // Non-finite values: maintained via rebuild, still exact.
  {
    Fixture f;
    for (int i = 0; i < 12; ++i)
      f.services.num.push_back({"a" + std::to_string(i), "T", "v", 20230601, 0, "USD", "", 1.0});
    f.Build();
    NumRow inf{"aI", "T", "v", 20230601, 0, "USD", "", INFINITY};
    f.services.num.push_back(inf);
    f.Txn({NumBatch(MutationKind::Insert, {}, {inf})});
    f.Check("inf insert");
    f.services.num.pop_back();
    f.Txn({NumBatch(MutationKind::Delete, {inf}, {})});
    f.Check("inf delete");
  }
  // Abort leaves state untouched; unrelated table ids are ignored.
  {
    Fixture f;
    for (int i = 0; i < 12; ++i)
      f.services.num.push_back({"a" + std::to_string(i), "T", "v", 20230601, 0, "USD", "", 1.0});
    f.Build();
    NumRow extra{"aX", "T", "v", 20230601, 0, "USD", "", 5.0};
    f.Txn({NumBatch(MutationKind::Insert, {}, {extra})}, /*commit=*/false);
    f.Check("aborted insert");
    MutationBatch other;
    other.table = 999;
    other.kind = MutationKind::Insert;
    other.new_rows = {ToAux(extra)};
    f.Txn({other});
    f.Check("foreign table");
  }
  // Pre overlay overflow folds into a rebuild without losing correctness.
  {
    Fixture f;
    for (int i = 0; i < 12; ++i)
      f.services.num.push_back({"a" + std::to_string(i), "T", "v", 20230601, 0, "USD", "", 1.0});
    f.Build();
    for (std::size_t i = 0; i < SecEdgarQ24Module::kMaxOverlayKeys + 50; ++i) {
      PreRow r{"p" + std::to_string(i), "T", "v"};
      f.services.pre.push_back(r);
      f.Txn({PreBatch(MutationKind::Insert, {}, {r})});
      if (i % 1500 == 0) f.Check("overlay growth");
    }
    f.Check("overlay overflow");
    PreRow kill{"a3", "T", "v"};
    f.services.pre.push_back(kill);
    f.Txn({PreBatch(MutationKind::Insert, {}, {kill})});
    f.Check("insert after overflow rebuild");
  }
}

// Exhaustive-ish check of the membership structure itself: many pairs (so all
// three width tiers are populated, including long slices), duplicates, absent
// keys of every kind, scalar and SIMD slice search, single + batched probes.
void TestPreBaseTiers() {
  std::mt19937_64 rng(42);
  std::set<std::tuple<std::string, std::string, std::string>> truth;
  Q24RawPre raw;
  const int adsh_n = 600, tag_n = 300000, ver_n = 40;
  std::vector<std::string> adshs, tags, vers;
  for (int i = 0; i < adsh_n; ++i) adshs.push_back("0001-" + std::to_string(i));
  for (int i = 0; i < tag_n; ++i) tags.push_back("Tag" + std::to_string(i));
  for (int i = 0; i < ver_n; ++i) vers.push_back("v" + std::to_string(i));
  for (int a = 0; a < adsh_n; ++a) {
    // slice length varies from 1 to ~3000 so scalar, SIMD-linear and
    // binary+SIMD windows are all hit; skewed tag choice => frequency tiers.
    const int len = 1 + static_cast<int>(rng() % (a % 7 == 0 ? 3000 : 120));
    for (int k = 0; k < len; ++k) {
      const double u = static_cast<double>(rng() % 1000000) / 1e6;
      // 55% skewed head (small frequency-ranked ids), 45% long uniform tail.
      const int t = (rng() % 100 < 55) ? static_cast<int>(std::pow(u, 6.0) * 3000)
                                       : static_cast<int>(rng() % tag_n);
      const int v = static_cast<int>(rng() % (t < 100 ? 3 : ver_n));
      raw.Add(Q24Str::Of(adshs[a]), Q24Str::Of(tags[t]), Q24Str::Of(vers[v]));
      truth.insert({adshs[a], tags[t], vers[v]});
      if (rng() % 9 == 0) // duplicate pre row
        raw.Add(Q24Str::Of(adshs[a]), Q24Str::Of(tags[t]), Q24Str::Of(vers[v]));
    }
  }
  raw.Add(Q24Str::Null(), Q24Str::Of(tags[0]), Q24Str::Of(vers[0])); // ignored
  std::vector<Q24RawPre> parts;
  parts.push_back(std::move(raw));
  const auto base = BuildQ24PreBase(parts);
  assert(base->triple_count == truth.size());
  assert(!base->ids16.empty() && !base->ids32.empty());

  const bool had_simd = Q24SimdActive();
  for (const bool simd : {false, true}) {
    Q24SetSimdEnabled(simd);
    std::vector<Q24Str> a, t, v;
    std::vector<std::tuple<std::string, std::string, std::string>> keys;
    for (int i = 0; i < 60000; ++i) {
      const bool absent_adsh = rng() % 20 == 0, absent_tag = rng() % 20 == 0;
      std::string aa = absent_adsh ? "zzz" + std::to_string(rng() % 5) : adshs[rng() % adsh_n];
      std::string tt = absent_tag ? "Nope" : tags[rng() % tag_n];
      std::string vv = vers[rng() % ver_n];
      if (i % 3 == 0 && !truth.empty()) { // known members, plus neighbours
        auto it = truth.begin();
        std::advance(it, static_cast<long>(rng() % truth.size()));
        std::tie(aa, tt, vv) = *it;
      }
      keys.push_back({aa, tt, vv});
    }
    std::vector<uint8_t> hits(keys.size());
    for (const auto &k : keys) {
      a.push_back(Q24Str::Of(std::get<0>(k)));
      t.push_back(Q24Str::Of(std::get<1>(k)));
      v.push_back(Q24Str::Of(std::get<2>(k)));
    }
    base->ContainsBatch(a.data(), t.data(), v.data(), keys.size(), hits.data());
    std::size_t members = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const bool expect = truth.count(keys[i]) != 0;
      members += expect;
      assert(base->Contains(a[i], t[i], v[i]) == expect);
      assert((hits[i] != 0) == expect);
    }
    assert(members > 15000 && members < keys.size());
    // NULL components never match.
    assert(!base->Contains(Q24Str::Null(), t[0], v[0]));
    assert(!base->Contains(a[0], Q24Str::Null(), v[0]));
  }
  Q24SetSimdEnabled(had_simd);
  std::cout << "pre base tiers OK (triples=" << truth.size() << ", simd="
            << (had_simd ? "avx2" : "scalar") << ")\n";
}

} // namespace

int main(int argc, char **argv) {
  {
    accelerator_codegen::aux_state::maintenance::CostModelParams gated;
    gated.apply_delta_max_rows = 1024;
    const auto small = SelectAuxMaintenanceAction(
        {1, 1, true, false, true, false, false, MaintenanceAction::NoOp, 10.0}, gated);
    assert(small.action == MaintenanceAction::ApplyDelta);
    const auto bulk = SelectAuxMaintenanceAction(
        {1025, 1, true, false, true, false, false, MaintenanceAction::NoOp, 10.0}, gated);
    assert(bulk.action == MaintenanceAction::RebuildAll);
  }
  TestPreBaseTiers();
  TestEdgeCases();
  const int seeds = argc > 1 ? std::atoi(argv[1]) : 6;
  const int steps = argc > 2 ? std::atoi(argv[2]) : 300;
  for (int s = 1; s <= seeds; ++s) {
    RunRandom(1000 + s, /*nulls=*/false, /*batches=*/false, steps);
    RunRandom(2000 + s, /*nulls=*/true, /*batches=*/true, steps);
    RunRandom(3000 + s, /*nulls=*/false, /*batches=*/true, steps);
  }
  // Bulk insert beyond the ApplyDelta row cap takes the rebuild path.
  {
    Fixture f;
    Gen g(7, false);
    for (int i = 0; i < 300; ++i) f.services.num.push_back(g.Num());
    f.Build();
    std::vector<NumRow> rows;
    for (int i = 0; i < 2600; ++i) rows.push_back(g.Num());
    for (auto &r : rows) f.services.num.push_back(r);
    const int scans = f.services.scans;
    f.Txn({NumBatch(MutationKind::Insert, {}, rows)});
    assert(f.services.scans > scans);
    f.Check("bulk insert");
  }
  std::cout << "test_shared_sec_edgar_q24 PASS\n";
  return 0;
}
