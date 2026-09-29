// Shared harness pieces for the Q21 aux differential / twin extract-diff:
// a PostgresStateStore-like store, typed-scan services, builder, sink,
// brute-force SQL reference, and commit emulation.
#pragma once
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include Q21_HDR

using namespace accelerator_codegen::aux_state::core;
using namespace accelerator_codegen::aux_state::query;
using accelerator_codegen::aux_state::InvalidationScope;
namespace maint = accelerator_codegen::aux_state::maintenance;

static AuxValue S(std::string v) { return AuxValue::Of(AuxType::String, std::move(v)); }
static AuxValue I(int64_t v) { return AuxValue::Of(AuxType::Int64, v); }
static AuxValue D(int32_t d) { return AuxValue::Of(AuxType::Date, AuxDate{d}); }

struct Sup { int64_t k; std::string name; int32_t nk; };
struct Nat { int32_t k; std::string name; };
struct Ord { int64_t k; char st; };
struct Li  { int64_t ok, sk; int32_t commit, receipt; };

// PostgresStateStore-like: per-partition validity; accelerator-wide validity
// fails iff any partition is invalid; publish upserts; ReadPartitions skips invalid.
class PgLikeStore final : public AuxStateStore {
public:
  struct Slot { StateImage img; bool valid; };
  std::map<PartitionId, Slot> slots;
  StateSnapshot Read(const VisibilityToken &v) const override {
    StateSnapshot s; s.visibility = v;
    for (auto &[p, sl] : slots) if (sl.valid) s.partitions.push_back(sl.img);
    s.available = !s.partitions.empty(); return s;
  }
  StateSnapshot ReadPartitions(const VisibilityToken &v,
                               const std::vector<PartitionId> &ps) const override {
    StateSnapshot s; s.visibility = v;
    for (auto p : ps) { auto it = slots.find(p); if (it != slots.end() && it->second.valid) s.partitions.push_back(it->second.img); }
    s.available = !s.partitions.empty(); return s;
  }
  ValidityResult CheckValidity(const QueryKey &, const VisibilityToken &) const override {
    if (slots.empty()) return {ValidityCode::Missing, 0};
    for (auto &[p, sl] : slots) if (!sl.valid) return {ValidityCode::Invalidated, 0};
    return {ValidityCode::Valid, 1};
  }
  void Invalidate(const InvalidationPlan &plan) override {
    if (plan.scope == InvalidationScope::AllPartitions) { for (auto &[p, sl] : slots) sl.valid = false; return; }
    for (auto p : plan.partitions) { auto it = slots.find(p); if (it != slots.end()) it->second.valid = false; }
  }
  bool TryPublish(const StagedMaintenanceOutcome &o, Version) override {
    for (auto &img : o.updates) slots[img.partition] = {img, true};
    return true;
  }
  void Install(std::vector<StateImage> imgs) { slots.clear(); for (auto &i : imgs) slots[i.partition] = {i, true}; }
};

class Svc final : public AuxEngineServices, public TpchQ21InitialScan {
public:
  PgLikeStore store;
  std::vector<Sup> sup; std::vector<Nat> nat; std::vector<Ord> ord; std::vector<Li> li;
  void ScanSuppliers(const std::function<void(const TpchQ21SupplierInput &)> &f) override { for (auto &s : sup) f({s.k, s.name, s.nk}); }
  void ScanNations(const std::function<void(const TpchQ21NationInput &)> &f) override { for (auto &n : nat) f({n.k, n.name}); }
  void ScanOrders(const std::function<void(const TpchQ21OrderInput &)> &f) override { for (auto &o : ord) f({o.k, o.st}); }
  void ScanLineitems(const std::function<void(const TpchQ21LineitemInput &)> &f) override { for (auto &l : li) f({l.ok, l.sk, l.commit, l.receipt}); }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }
  MaintenanceDecision ChooseMaintenance(const MaintenanceRequest &r) const override {
    MaintenanceDecision d;
    d.action = r.incremental_supported ? maint::MaintenanceAction::ApplyDelta
                                       : maint::MaintenanceAction::RebuildAll;
    return d;
  }
};

class Builder final : public StateBuilder {
public:
  std::vector<StateImage> images;
  void Write(StateImage i) override { images.push_back(std::move(i)); }
  void Finish() override {}
};
class Sink final : public AuxRowSink {
public:
  std::vector<std::pair<std::string, int64_t>> rows;
  void Begin(const OutputSchema &) override {}
  void WriteRow(const AuxRow &r) override { rows.push_back({std::get<std::string>(r.values[0].payload), std::get<int64_t>(r.values[1].payload)}); }
  void End() override {}
};

using Rows = std::vector<std::pair<std::string, int64_t>>;

// Brute-force Q21 over the base tables (SQL semantics, GROUP BY s_name).
static Rows Reference(const Svc &db, const std::string &nation) {
  std::map<std::string, int64_t> cnt;
  for (auto &n : db.nat) if (n.name == nation)
  for (auto &s : db.sup) if (s.nk == n.k)
  for (auto &l1 : db.li) if (l1.sk == s.k && l1.receipt > l1.commit)
  for (auto &o : db.ord) if (o.k == l1.ok && o.st == 'F') {
    bool ex = false, nex = true;
    for (auto &l2 : db.li) if (l2.ok == l1.ok && l2.sk != l1.sk) ex = true;
    for (auto &l3 : db.li) if (l3.ok == l1.ok && l3.sk != l1.sk && l3.receipt > l3.commit) nex = false;
    if (ex && nex) cnt[s.name]++;
  }
  Rows r(cnt.begin(), cnt.end());
  std::sort(r.begin(), r.end(), [](auto &a, auto &b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  return r;
}

static TpchQ21Config Cfg() { return {1, 2, 3, 4}; }

static void BuildInto(Svc &svc, TpchQ21Module &m) {
  Builder b; m.Build(svc, b); svc.store.Install(b.images);
}

// Emulates AuxTransactionCoordinator PreCommit+Commit for one batch.
static std::string Maintain(Svc &svc, TpchQ21Module &m, const MutationBatch &mb) {
  auto buf = m.CreateTxnBuffer();
  auto plan = m.InterpretMutation(mb);
  m.AccumulateMutation(mb, svc, *buf);
  auto out = m.StageMaintenance(svc, plan, *buf);
  InvalidationPlan early = out.invalidations;
  if (early.IsEmpty() && !out.pending_rebuild.IsEmpty()) early = out.pending_rebuild;
  if (early.IsEmpty() && !out.updates.empty()) {
    early.scope = InvalidationScope::PartitionSet;
    for (auto &u : out.updates) early.partitions.insert(u.partition);
  }
  if (!early.IsEmpty()) svc.store.Invalidate(early);
  if (!out.updates.empty()) svc.store.TryPublish(out, 0);
  return out.action == maint::MaintenanceAction::ApplyDelta ? "ApplyDelta" : "other";
}

static Rows Exec(Svc &svc, TpchQ21Module &m, const std::string &nation, bool &hit) {
  hit = svc.store.CheckValidity({TpchQ21Layout::kAccelerator, {}}, {}).IsValid();
  Sink s; m.Execute(svc, QueryArguments{{S(nation)}}, s); return s.rows;
}

static std::string Show(const Rows &r) {
  std::string o = "[";
  for (auto &[n, c] : r) o += "(" + n + "," + std::to_string(c) + ")";
  return o + "]";
}

