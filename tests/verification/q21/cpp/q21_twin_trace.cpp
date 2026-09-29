// Q21 C++ <-> Dafny-twin extract-diff, C++ half.
//
// Generates seeded random scenarios (typed initial tables + transactions of
// typed mutations, under TPC-H primary keys + lineitem->orders referential
// integrity), drives the generated TpchQ21Module through the
// PostgresStateStore-like harness, and writes one JSON line per scenario with
// the inputs and the observable outputs:
//   * per transaction: the StageMaintenance action / invalidate-all flag and
//     the sorted set of published partition ids;
//   * per query (every nation name, after build and after every transaction):
//     whether the accelerator store was valid, and the emitted rows.
// q21_twin_replay.py replays the same inputs on the compiled Dafny twin and
// compares.  Finite testing only (not a universal C++ <-> Twin proof).
#include <cstdlib>
#include <random>
#include <sstream>
#include "q21_aux_harness.hpp"

namespace {

struct TMut {  // typed mutation descriptor
  char table;  // 'L' lineitem, 'O' orders, 'S' supplier, 'N' nation
  MutationKind kind;
  std::vector<std::vector<long long>> olds, news;  // ints; strings in sold/snew
  std::vector<std::string> sold, snew;
};

std::string J(const std::string &s) {
  std::string o = "\"";
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
  return o + "\"";
}

std::string JRows(const Rows &r) {
  std::string o = "[";
  for (size_t i = 0; i < r.size(); ++i) {
    if (i) o += ",";
    o += "[" + J(r[i].first) + "," + std::to_string(r[i].second) + "]";
  }
  return o + "]";
}

AuxRow LiAux(const Li &l) { return {{I(l.ok), I(l.sk), D(l.commit), D(l.receipt)}}; }

const char *ActionName(maint::MaintenanceAction a) {
  switch (a) {
  case maint::MaintenanceAction::NoOp: return "NoOp";
  case maint::MaintenanceAction::ApplyDelta: return "ApplyDelta";
  case maint::MaintenanceAction::RebuildPartition: return "RebuildPartition";
  case maint::MaintenanceAction::RebuildAll: return "RebuildAll";
  default: return "InvalidateAndFallback";
  }
}

}  // namespace

int main(int argc, char **argv) {
  int seeds = argc > 1 ? std::atoi(argv[1]) : 200;
  int steps = argc > 2 ? std::atoi(argv[2]) : 25;
  const int32_t C = 100;
  const std::vector<std::string> names = {"N", "M", "P"};
  for (int seed = 0; seed < seeds; ++seed) {
    std::mt19937 rng(seed);
    auto R = [&](int n) { return (int)(rng() % n); };
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
    std::ostringstream js;
    js << "{\"seed\":" << seed << ",\"names\":[\"N\",\"M\",\"P\"],\"db\":{\"nat\":[";
    for (size_t i = 0; i < svc.nat.size(); ++i) js << (i ? "," : "") << "[" << svc.nat[i].k << "," << J(svc.nat[i].name) << "]";
    js << "],\"sup\":[";
    for (size_t i = 0; i < svc.sup.size(); ++i) js << (i ? "," : "") << "[" << svc.sup[i].k << "," << J(svc.sup[i].name) << "," << svc.sup[i].nk << "]";
    js << "],\"ord\":[";
    for (size_t i = 0; i < svc.ord.size(); ++i) js << (i ? "," : "") << "[" << svc.ord[i].k << "," << J(std::string(1, svc.ord[i].st)) << "]";
    js << "],\"li\":[";
    for (size_t i = 0; i < svc.li.size(); ++i) js << (i ? "," : "") << "[" << svc.li[i].ok << "," << svc.li[i].sk << "," << svc.li[i].commit << "," << svc.li[i].receipt << "]";
    js << "]},\"steps\":[";

    BuildInto(svc, m);
    auto queries = [&](std::ostringstream &o) {
      o << "\"queries\":[";
      bool first = true;
      bool rebuilt = false;
      for (auto &nm : names) {
        bool valid = svc.store.CheckValidity({TpchQ21Layout::kAccelerator, {}}, {}).IsValid();
        bool hit;
        auto rows = Exec(svc, m, nm, hit);
        o << (first ? "" : ",") << "{\"name\":" << J(nm) << ",\"valid\":" << (valid ? "true" : "false")
          << ",\"rows\":" << JRows(rows) << "}";
        first = false;
        if (!valid) rebuilt = true;
      }
      o << "]";
      if (rebuilt) BuildInto(svc, m);  // EnsureBuilt after the fallback queries
    };
    {
      std::ostringstream o; o << "{\"txn\":[],"; queries(o); o << "}";
      js << o.str();
    }
    for (int step = 0; step < steps; ++step) {
      auto buf = m.CreateTxnBuffer();
      InvalidationPlan plan;
      std::ostringstream tj; tj << "[";
      int nm = 1 + R(3);
      auto emit = [&](const std::string &table, const char *kind, const std::string &olds, const std::string &news) {
        tj << (tj.str().size() > 1 ? "," : "") << "{\"t\":" << J(table) << ",\"k\":" << J(kind)
           << ",\"old\":" << olds << ",\"new\":" << news << "}";
      };
      auto liJ = [&](const std::vector<Li> &v) {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + std::string("[") + std::to_string(v[i].ok) + "," + std::to_string(v[i].sk) + "," + std::to_string(v[i].commit) + "," + std::to_string(v[i].receipt) + "]";
        return o + "]";
      };
      auto ordJ = [&](const std::vector<Ord> &v) {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + std::string("[") + std::to_string(v[i].k) + "," + J(std::string(1, v[i].st)) + "]";
        return o + "]";
      };
      auto apply = [&](const MutationBatch &mb) {
        m.AccumulateMutation(mb, svc, *buf); MergeInvalidationPlan(plan, m.InterpretMutation(mb));
      };
      for (int k = 0; k < nm; ++k) {
        int kind = R(9);
        if (kind <= 2 && !svc.ord.empty()) {
          Li l{svc.ord[R(svc.ord.size())].k, 1 + R(5), C, R(2) ? C + 1 : C - 1};
          MutationBatch mb; mb.table = 3; mb.kind = MutationKind::Insert; mb.new_rows = {LiAux(l)};
          svc.li.push_back(l); apply(mb); emit("L", "ins", "[]", liJ({l}));
        } else if (kind == 3 && !svc.li.empty()) {
          size_t i = R(svc.li.size()); Li l = svc.li[i];
          MutationBatch mb; mb.table = 3; mb.kind = MutationKind::Delete; mb.old_rows = {LiAux(l)};
          svc.li.erase(svc.li.begin() + i); apply(mb); emit("L", "del", liJ({l}), "[]");
        } else if (kind == 4 && !svc.li.empty()) {
          size_t i = R(svc.li.size()); Li l = svc.li[i]; Li n = l; n.receipt = R(2) ? C + 1 : C - 1;
          MutationBatch mb; mb.table = 3; mb.kind = MutationKind::Update; mb.old_rows = {LiAux(l)}; mb.new_rows = {LiAux(n)};
          svc.li[i] = n; apply(mb); emit("L", "upd", liJ({l}), liJ({n}));
        } else if (kind == 5) {
          int64_t ok = next_order++; char st = "FOP"[R(3)];
          MutationBatch mo; mo.table = 2; mo.kind = MutationKind::Insert; mo.new_rows = {{{I(ok), S(std::string(1, st))}}};
          svc.ord.push_back({ok, st}); apply(mo); emit("O", "ins", "[]", ordJ({{ok, st}}));
          std::vector<Li> ls; int n = R(4);
          for (int i = 0; i < n; ++i) ls.push_back({ok, 1 + R(5), C, R(2) ? C + 1 : C - 1});
          MutationBatch ml; ml.table = 3; ml.kind = MutationKind::Insert;
          for (auto &l : ls) { ml.new_rows.push_back(LiAux(l)); svc.li.push_back(l); }
          apply(ml); emit("L", "ins", "[]", liJ(ls));
        } else if (kind == 6 && !svc.ord.empty()) {
          size_t oi = R(svc.ord.size()); Ord o = svc.ord[oi];
          std::vector<Li> ls;
          MutationBatch ml; ml.table = 3; ml.kind = MutationKind::Delete;
          for (size_t i = 0; i < svc.li.size();) {
            if (svc.li[i].ok == o.k) { ls.push_back(svc.li[i]); ml.old_rows.push_back(LiAux(svc.li[i])); svc.li.erase(svc.li.begin() + i); }
            else ++i;
          }
          apply(ml); emit("L", "del", liJ(ls), "[]");
          MutationBatch mo; mo.table = 2; mo.kind = MutationKind::Delete; mo.old_rows = {{{I(o.k), S(std::string(1, o.st))}}};
          svc.ord.erase(svc.ord.begin() + oi); apply(mo); emit("O", "del", ordJ({o}), "[]");
        } else if (kind == 7 && !svc.ord.empty()) {
          size_t oi = R(svc.ord.size()); Ord o = svc.ord[oi]; char st = "FOP"[R(3)];
          MutationBatch mo; mo.table = 2; mo.kind = MutationKind::Update;
          mo.old_rows = {{{I(o.k), S(std::string(1, o.st))}}}; mo.new_rows = {{{I(o.k), S(std::string(1, st))}}};
          svc.ord[oi].st = st; apply(mo); emit("O", "upd", ordJ({o}), ordJ({{o.k, st}}));
        } else if (R(2)) {
          size_t ni = R(svc.nat.size()); Nat n = svc.nat[ni]; std::string nn = names[R(3)];
          MutationBatch mb; mb.table = 4; mb.kind = MutationKind::Update;
          mb.old_rows = {{{I(n.k), S(n.name)}}}; mb.new_rows = {{{I(n.k), S(nn)}}};
          svc.nat[ni].name = nn; apply(mb);
          emit("N", "upd", "[[" + std::to_string(n.k) + "," + J(n.name) + "]]", "[[" + std::to_string(n.k) + "," + J(nn) + "]]");
        } else {
          size_t si = R(svc.sup.size()); Sup s = svc.sup[si]; Sup ns = s;
          ns.name = std::string(1, char('A' + R(3))); ns.nk = R(3);
          MutationBatch mb; mb.table = 1; mb.kind = MutationKind::Update;
          mb.old_rows = {{{I(s.k), S(s.name), I(s.nk)}}}; mb.new_rows = {{{I(ns.k), S(ns.name), I(ns.nk)}}};
          svc.sup[si] = ns; apply(mb);
          emit("S", "upd", "[[" + std::to_string(s.k) + "," + J(s.name) + "," + std::to_string(s.nk) + "]]",
               "[[" + std::to_string(ns.k) + "," + J(ns.name) + "," + std::to_string(ns.nk) + "]]");
        }
      }
      tj << "]";
      auto out = m.StageMaintenance(svc, plan, *buf);
      bool inval_all = out.invalidations.scope == InvalidationScope::AllPartitions ||
                       out.pending_rebuild.scope == InvalidationScope::AllPartitions;
      std::set<long long> pids;
      for (auto &u : out.updates) pids.insert((long long)u.partition);
      InvalidationPlan early = out.invalidations;
      if (early.IsEmpty() && !out.pending_rebuild.IsEmpty()) early = out.pending_rebuild;
      if (early.IsEmpty() && !out.updates.empty()) { early.scope = InvalidationScope::PartitionSet;
        for (auto &u : out.updates) early.partitions.insert(u.partition); }
      if (!early.IsEmpty()) svc.store.Invalidate(early);
      if (!out.updates.empty()) svc.store.TryPublish(out, 0);
      std::ostringstream o;
      o << ",{\"txn\":" << tj.str() << ",\"action\":" << J(ActionName(out.action))
        << ",\"invalidate_all\":" << (inval_all ? "true" : "false") << ",\"published\":[";
      bool f = true; for (auto p : pids) { o << (f ? "" : ",") << p; f = false; }
      o << "],";
      queries(o);
      o << "}";
      js << o.str();
    }
    js << "]}";
    std::printf("%s\n", js.str().c_str());
  }
  return 0;
}
