// Q21 aux module differential: Build / ApplyDelta / rebuild vs brute-force SQL.
//
// Drives the generated TpchQ21Module (engine chosen by -DQ21_HDR) through a
// store with PostgresStateStore publish/validity semantics, forcing ApplyDelta
// whenever the module offers it, and compares every hit/fallback answer with a
// nested-loop evaluation of the Q21 SQL (GROUP BY s_name).  Scenarios A-F are
// the regressions for the bugs fixed on 2026-09-29; Fuzz() is a randomized
// differential under TPC-H primary keys + lineitem->orders referential
// integrity.  Finite testing only -- not a proof.
#include <cassert>
#include <cstdlib>
#include <random>
#include "q21_aux_harness.hpp"

static int failures = 0;
static void Check(const char *name, Svc &svc, TpchQ21Module &m, const std::string &nation) {
  bool hit = false;
  auto got = Exec(svc, m, nation, hit);
  auto want = Reference(svc, nation);
  bool ok = got == want;
  if (!ok) failures++;
  std::printf("%-44s nation=%s hit=%d accel=%s sql=%s %s\n", name, nation.c_str(), hit,
              Show(got).c_str(), Show(want).c_str(), ok ? "OK" : "MISMATCH");
}


// Randomized differential: transactions of mixed mutations under PK + RI.
static void Fuzz(int seeds, int steps) {
  const int32_t C = 100;
  int checks = 0, fallback_rebuilds = 0, applied = 0;
  for (int seed = 0; seed < seeds; ++seed) {
    std::mt19937 rng(seed);
    auto R = [&](int n) { return (int)(rng() % n); };
    const std::vector<std::string> names = {"N", "M", "P"};
    Svc svc; TpchQ21Module m(Cfg(), &svc);
    for (int k = 0; k < 3; ++k) svc.nat.push_back({k, names[R(3)]});
    for (int s = 1; s <= 5; ++s) svc.sup.push_back({s, std::string(1, char('A' + R(3))), R(3)});
    int64_t next_order = 1;
    for (int o = 0; o < 6; ++o) {
      int64_t ok = next_order++;
      svc.ord.push_back({ok, "FOP"[R(3)]});
      int n = R(4);
      for (int i = 0; i < n; ++i) svc.li.push_back({ok, 1 + R(5), C, R(2) ? C + 1 : C - 1});
    }
    BuildInto(svc, m);
    for (int step = 0; step < steps; ++step) {
      auto buf = m.CreateTxnBuffer();
      InvalidationPlan plan;
      int nm = 1 + R(3);
      for (int k = 0; k < nm; ++k) {
        MutationBatch mb;
        int kind = R(9);
        if (kind <= 2 && !svc.ord.empty()) {            // lineitem insert
          int64_t ok = svc.ord[R(svc.ord.size())].k;
          Li l{ok, 1 + R(5), C, R(2) ? C + 1 : C - 1};
          mb.table = 3; mb.kind = MutationKind::Insert;
          mb.new_rows = {{{I(l.ok), I(l.sk), D(l.commit), D(l.receipt)}}};
          svc.li.push_back(l);
        } else if (kind == 3 && !svc.li.empty()) {       // lineitem delete
          size_t i = R(svc.li.size()); Li l = svc.li[i];
          mb.table = 3; mb.kind = MutationKind::Delete;
          mb.old_rows = {{{I(l.ok), I(l.sk), D(l.commit), D(l.receipt)}}};
          svc.li.erase(svc.li.begin() + i);
        } else if (kind == 4 && !svc.li.empty()) {       // lineitem update (receiptdate)
          size_t i = R(svc.li.size()); Li l = svc.li[i]; Li n = l; n.receipt = R(2) ? C + 1 : C - 1;
          mb.table = 3; mb.kind = MutationKind::Update;
          mb.old_rows = {{{I(l.ok), I(l.sk), D(l.commit), D(l.receipt)}}};
          mb.new_rows = {{{I(n.ok), I(n.sk), D(n.commit), D(n.receipt)}}};
          svc.li[i] = n;
        } else if (kind == 5) {                          // RF1: new order (+ lineitems below)
          int64_t ok = next_order++; char st = "FOP"[R(3)];
          mb.table = 2; mb.kind = MutationKind::Insert; mb.new_rows = {{{I(ok), S(std::string(1, st))}}};
          svc.ord.push_back({ok, st});
          m.AccumulateMutation(mb, svc, *buf); MergeInvalidationPlan(plan, m.InterpretMutation(mb));
          MutationBatch ml; ml.table = 3; ml.kind = MutationKind::Insert;
          int n = R(4);
          for (int i = 0; i < n; ++i) { Li l{ok, 1 + R(5), C, R(2) ? C + 1 : C - 1}; svc.li.push_back(l);
            ml.new_rows.push_back({{I(l.ok), I(l.sk), D(l.commit), D(l.receipt)}}); }
          mb = ml;
        } else if (kind == 6 && !svc.ord.empty()) {       // RF2: delete lineitems then order
          size_t oi = R(svc.ord.size()); Ord o = svc.ord[oi];
          MutationBatch ml; ml.table = 3; ml.kind = MutationKind::Delete;
          for (size_t i = 0; i < svc.li.size();) {
            if (svc.li[i].ok == o.k) { auto &l = svc.li[i];
              ml.old_rows.push_back({{I(l.ok), I(l.sk), D(l.commit), D(l.receipt)}}); svc.li.erase(svc.li.begin() + i); }
            else ++i; }
          m.AccumulateMutation(ml, svc, *buf); MergeInvalidationPlan(plan, m.InterpretMutation(ml));
          mb.table = 2; mb.kind = MutationKind::Delete; mb.old_rows = {{{I(o.k), S(std::string(1, o.st))}}};
          svc.ord.erase(svc.ord.begin() + oi);
        } else if (kind == 7 && !svc.ord.empty()) {       // order status update
          size_t oi = R(svc.ord.size()); Ord o = svc.ord[oi]; char st = "FOP"[R(3)];
          mb.table = 2; mb.kind = MutationKind::Update;
          mb.old_rows = {{{I(o.k), S(std::string(1, o.st))}}}; mb.new_rows = {{{I(o.k), S(std::string(1, st))}}};
          svc.ord[oi].st = st;
        } else {                                          // nation rename / supplier update
          if (R(2)) { size_t ni = R(svc.nat.size()); Nat n = svc.nat[ni]; std::string nn = names[R(3)];
            mb.table = 4; mb.kind = MutationKind::Update;
            mb.old_rows = {{{I(n.k), S(n.name)}}}; mb.new_rows = {{{I(n.k), S(nn)}}}; svc.nat[ni].name = nn;
          } else { size_t si = R(svc.sup.size()); Sup s = svc.sup[si]; Sup ns = s;
            ns.name = std::string(1, char('A' + R(3))); ns.nk = R(3);
            mb.table = 1; mb.kind = MutationKind::Update;
            mb.old_rows = {{{I(s.k), S(s.name), I(s.nk)}}}; mb.new_rows = {{{I(ns.k), S(ns.name), I(ns.nk)}}};
            svc.sup[si] = ns; }
        }
        m.AccumulateMutation(mb, svc, *buf); MergeInvalidationPlan(plan, m.InterpretMutation(mb));
      }
      if (getenv("Q21_TRACE") && atoi(getenv("Q21_TRACE")) == seed && step == 0) {
        std::printf("-- seed %d txn %d: DB after txn\n", seed, step);
        for (auto &n : svc.nat) std::printf(" nation %d %s\n", n.k, n.name.c_str());
        for (auto &s : svc.sup) std::printf(" supp %ld %s nk=%d\n", (long)s.k, s.name.c_str(), s.nk);
        for (auto &o : svc.ord) std::printf(" order %ld %c\n", (long)o.k, o.st);
        for (auto &l : svc.li) std::printf(" li o=%ld s=%ld %s\n", (long)l.ok, (long)l.sk, l.receipt > l.commit ? "LATE" : "ok");
        auto &b = dynamic_cast<TpchQ21TxnBuffer &>(*buf);
        for (auto &d : b.LineitemDeltas()) std::printf(" dLI o=%ld s=%ld %s sign=%d\n", (long)d.orderkey, (long)d.suppkey, d.receiptdate > d.commitdate ? "LATE" : "ok", d.sign);
        for (auto &d : b.OrderDeltas()) std::printf(" dORD o=%ld %d->%d\n", (long)d.orderkey, d.old_orderstatus, d.new_orderstatus);
        std::printf(" rebuild=%d\n", b.RequiresRebuild());
      }
      auto out = m.StageMaintenance(svc, plan, *buf);
      InvalidationPlan early = out.invalidations;
      if (early.IsEmpty() && !out.pending_rebuild.IsEmpty()) early = out.pending_rebuild;
      if (early.IsEmpty() && !out.updates.empty()) { early.scope = InvalidationScope::PartitionSet;
        for (auto &u : out.updates) early.partitions.insert(u.partition); }
      if (!early.IsEmpty()) svc.store.Invalidate(early);
      if (!out.updates.empty()) svc.store.TryPublish(out, 0);
      if (out.action == maint::MaintenanceAction::ApplyDelta) applied++;
      // Query each name; half the time before the lazy rebuild (fallback path).
      if (!svc.store.CheckValidity({TpchQ21Layout::kAccelerator, {}}, {}).IsValid()) {
        fallback_rebuilds++;
        if (R(2)) for (auto &nm2 : names) { checks++; Check("fuzz fallback", svc, m, nm2); }
        BuildInto(svc, m);  // EnsureBuilt
      }
      for (auto &nm2 : names) { checks++; bool hit; auto got = Exec(svc, m, nm2, hit); auto want = Reference(svc, nm2);
        if (got != want) { failures++; std::printf("seed %d step %d nation %s hit=%d accel=%s sql=%s MISMATCH\n",
            seed, step, nm2.c_str(), hit, Show(got).c_str(), Show(want).c_str()); } }
    }
  }
  std::printf("fuzz: %d seeds x %d txns, %d checks, %d ApplyDelta commits, %d rebuild/invalidate commits\n",
              seeds, steps, checks, applied, fallback_rebuilds);
}

int main() {
  const int32_t C = 100, EARLY = 50, LATE = 200;
  // A: lineitem insert into an F order pruned at Build (no contribution yet).
  {
    Svc svc; TpchQ21Module m(Cfg(), &svc);
    svc.nat = {{0, "N"}}; svc.sup = {{1, "A", 0}, {2, "B", 0}, {3, "C", 0}};
    svc.ord = {{10, 'F'}}; svc.li = {{10, 1, C, EARLY}, {10, 2, C, EARLY}};
    BuildInto(svc, m); Check("A0 after build", svc, m, "N");
    MutationBatch mb; mb.table = 3; mb.kind = MutationKind::Insert;
    mb.new_rows = {{{I(10), I(3), D(C), D(LATE)}}};
    svc.li.push_back({10, 3, C, LATE});
    std::printf("  maintenance: %s\n", Maintain(svc, m, mb).c_str());
    Check("A1 after late lineitem insert", svc, m, "N");
  }
  // B: order status O->F on an order pruned at Build.
  {
    Svc svc; TpchQ21Module m(Cfg(), &svc);
    svc.nat = {{0, "N"}}; svc.sup = {{1, "A", 0}, {2, "B", 0}};
    svc.ord = {{10, 'O'}}; svc.li = {{10, 1, C, LATE}, {10, 2, C, EARLY}};
    BuildInto(svc, m); Check("B0 after build", svc, m, "N");
    MutationBatch mb; mb.table = 2; mb.kind = MutationKind::Update;
    mb.old_rows = {{{I(10), S("O")}}}; mb.new_rows = {{{I(10), S("F")}}};
    svc.ord[0].st = 'F';
    std::printf("  maintenance: %s\n", Maintain(svc, m, mb).c_str());
    Check("B1 after order status O->F", svc, m, "N");
  }
  // C: nation rename leaves the old name's fast-path partition valid.
  {
    Svc svc; TpchQ21Module m(Cfg(), &svc);
    svc.nat = {{0, "N"}}; svc.sup = {{1, "A", 0}, {2, "B", 0}};
    svc.ord = {{10, 'F'}}; svc.li = {{10, 1, C, LATE}, {10, 2, C, EARLY}};
    BuildInto(svc, m); Check("C0 after build", svc, m, "N");
    MutationBatch mb; mb.table = 4; mb.kind = MutationKind::Update;
    mb.old_rows = {{{I(0), S("N")}}}; mb.new_rows = {{{I(0), S("M")}}};
    svc.nat[0].name = "M";
    std::printf("  maintenance: %s\n", Maintain(svc, m, mb).c_str());
    Check("C1 old name after rename", svc, m, "N");
    Check("C2 new name after rename", svc, m, "M");
  }
  // D: two suppliers sharing s_name (GROUP BY s_name merges them in SQL).
  {
    Svc svc; TpchQ21Module m(Cfg(), &svc);
    svc.nat = {{0, "N"}}; svc.sup = {{1, "X", 0}, {2, "X", 0}, {3, "Y", 0}};
    svc.ord = {{10, 'F'}, {11, 'F'}};
    svc.li = {{10, 1, C, LATE}, {10, 3, C, EARLY}, {11, 2, C, LATE}, {11, 3, C, EARLY}};
    BuildInto(svc, m); Check("D0 duplicate s_name after build", svc, m, "N");
  }
  // E: TPC-H-style RF1 (new order + its lineitems in one txn) as a regression.
  {
    Svc svc; TpchQ21Module m(Cfg(), &svc);
    svc.nat = {{0, "N"}}; svc.sup = {{1, "A", 0}, {2, "B", 0}};
    svc.ord = {}; svc.li = {};
    BuildInto(svc, m);
    // order + lineitems accumulate into one buffer
    auto buf = m.CreateTxnBuffer();
    MutationBatch mo; mo.table = 2; mo.kind = MutationKind::Insert; mo.new_rows = {{{I(20), S("F")}}};
    MutationBatch ml; ml.table = 3; ml.kind = MutationKind::Insert;
    ml.new_rows = {{{I(20), I(1), D(C), D(LATE)}}, {{I(20), I(2), D(C), D(EARLY)}}};
    m.AccumulateMutation(mo, svc, *buf); m.AccumulateMutation(ml, svc, *buf);
    svc.ord.push_back({20, 'F'}); svc.li.push_back({20, 1, C, LATE}); svc.li.push_back({20, 2, C, EARLY});
    auto out = m.StageMaintenance(svc, m.InterpretMutation(ml), *buf);
    InvalidationPlan early = out.invalidations;
    if (early.IsEmpty() && !out.pending_rebuild.IsEmpty()) early = out.pending_rebuild;
    if (early.IsEmpty() && !out.updates.empty()) { early.scope = InvalidationScope::PartitionSet;
      for (auto &u : out.updates) early.partitions.insert(u.partition); }
    if (!early.IsEmpty()) svc.store.Invalidate(early);
    if (!out.updates.empty()) svc.store.TryPublish(out, 0);
    Check("E1 RF1-style insert (control)", svc, m, "N");
  }
  // F: RF1 new order contributes to a supplier whose nation already has waiters.
  {
    Svc svc; TpchQ21Module m(Cfg(), &svc);
    svc.nat = {{0, "N"}}; svc.sup = {{1, "A", 0}, {2, "B", 0}};
    svc.ord = {{10, 'F'}}; svc.li = {{10, 1, C, LATE}, {10, 2, C, EARLY}};
    BuildInto(svc, m); Check("F0 after build", svc, m, "N");
    auto buf = m.CreateTxnBuffer();
    MutationBatch mo; mo.table = 2; mo.kind = MutationKind::Insert; mo.new_rows = {{{I(20), S("F")}}};
    MutationBatch ml; ml.table = 3; ml.kind = MutationKind::Insert;
    ml.new_rows = {{{I(20), I(2), D(C), D(LATE)}}, {{I(20), I(1), D(C), D(EARLY)}}};
    m.AccumulateMutation(mo, svc, *buf); m.AccumulateMutation(ml, svc, *buf);
    svc.ord.push_back({20, 'F'}); svc.li.push_back({20, 2, C, LATE}); svc.li.push_back({20, 1, C, EARLY});
    auto out = m.StageMaintenance(svc, m.InterpretMutation(ml), *buf);
    InvalidationPlan early; early.scope = InvalidationScope::PartitionSet;
    for (auto &u : out.updates) early.partitions.insert(u.partition);
    if (out.updates.empty()) { early = out.pending_rebuild; }
    svc.store.Invalidate(early); svc.store.TryPublish(out, 0);
    std::printf("  maintenance: %s\n", out.action == maint::MaintenanceAction::ApplyDelta ? "ApplyDelta" : "other");
    Check("F1 RF1 new order, nation has waiters", svc, m, "N");
  }
  Fuzz(3000, 40);
  std::printf("mismatches: %d\n", failures);
  return failures == 0 ? 0 : 1;
}
