// Model-based test of the SEC-EDGAR Q3 aux module (no DuckDB database needed).
//
// A brute-force evaluation of the query over plain vectors is the oracle.
// Random INSERT / DELETE / UPDATE mutations on `num` and `sub` (including
// updates of join keys and filter columns that move rows in/out of the
// result) are fed through the real transaction coordinator + DuckDB state
// store; after every commit Execute() must equal the oracle and, for every
// supported mutation, the module must have maintained its state with
// ApplyDelta (no rebuild scan).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "backends/duckdb/state/duckdb_state_store.hpp"
#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "sec_edgar/q3/duckdb/aux/parallel/module.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::core;
using namespace accelerator_codegen::aux_state::query;

namespace {

constexpr TableId kNumTable = 1;
constexpr TableId kSubTable = 2;

void Check(bool ok, const std::string &what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << std::endl;
    std::abort();
  }
}

// ---- reference tables ------------------------------------------------------

uint64_t g_verifies = 0;
uint64_t g_nonempty = 0;

struct NumM {
  std::string adsh;
  bool adsh_null = false;
  std::string tag = "Revenues";
  std::string uom = "USD";
  bool uom_null = false;
  bool value_null = false;
  double value = 0.0;
};

struct SubM {
  std::string adsh;
  std::string name;
  bool name_null = false;
  int cik = 0;
  bool cik_null = false;
  int fy = 2022;
  bool fy_null = false;
};

AuxValue Str(const std::string &v) { return AuxValue::Of(AuxType::String, v); }
AuxValue Int(int64_t v) { return AuxValue::Of(AuxType::Int64, v); }

AuxRow NumRowOf(const NumM &m) {
  AuxRow row;
  row.values = {m.adsh_null ? AuxValue::Null(AuxType::String) : Str(m.adsh),
                Str(m.tag),
                Str("v1"),
                Int(20220331),
                Int(4),
                m.uom_null ? AuxValue::Null(AuxType::String) : Str(m.uom),
                Str(""),
                m.value_null ? AuxValue::Null(AuxType::Double)
                             : AuxValue::Of(AuxType::Double, m.value)};
  return row;
}

AuxRow SubRowOf(const SubM &m) {
  AuxRow row;
  row.values = {Str(m.adsh),
                m.name_null ? AuxValue::Null(AuxType::String) : Str(m.name),
                m.cik_null ? AuxValue::Null(AuxType::Int64) : Int(m.cik),
                m.fy_null ? AuxValue::Null(AuxType::Int64) : Int(m.fy)};
  return row;
}

struct Expected {
  std::string name;
  bool name_null;
  int cik;
  bool cik_null;
  double total;
};

struct Tables {
  std::vector<NumM> num;
  std::map<std::string, SubM> sub;

  // `ambiguous` receives groups whose total is numerically indistinguishable
  // from the HAVING threshold (floating point noise decides those in any
  // engine, e.g. a single group that is the whole cik).
  std::vector<Expected> Query(std::vector<Expected> *ambiguous = nullptr,
                              int64_t want_fy = 2022,
                              const std::string &want_uom = "USD",
                              bool null_arg = false) const {
    if (null_arg) {
      return {}; // a comparison with NULL is never true
    }
    struct GroupKey {
      std::string name;
      bool name_null;
      int cik;
      bool cik_null;
      bool operator<(const GroupKey &o) const {
        return std::tie(name_null, name, cik_null, cik) <
               std::tie(o.name_null, o.name, o.cik_null, o.cik);
      }
    };
    std::map<GroupKey, long double> groups;
    std::map<std::pair<bool, int>, long double> ciks;
    for (const auto &n : num) {
      if (n.adsh_null || n.uom_null || n.uom != want_uom || n.value_null) {
        continue;
      }
      const auto it = sub.find(n.adsh);
      if (it == sub.end() || it->second.fy_null || it->second.fy != want_fy) {
        continue;
      }
      const auto &s = it->second;
      groups[{s.name_null ? "" : s.name, s.name_null, s.cik_null ? 0 : s.cik,
              s.cik_null}] += n.value;
      ciks[{s.cik_null, s.cik_null ? 0 : s.cik}] += n.value;
    }
    std::vector<Expected> out;
    if (ciks.empty()) {
      return out;
    }
    long double total = 0;
    for (const auto &[k, v] : ciks) {
      total += v;
    }
    const long double avg = total / static_cast<long double>(ciks.size());
    for (const auto &[k, v] : groups) {
      if (std::fabs(static_cast<double>(v - avg)) <=
          1e-9 * std::max(1.0, std::fabs(static_cast<double>(avg)))) {
        if (ambiguous) {
          ambiguous->push_back({k.name, k.name_null, k.cik, k.cik_null,
                                static_cast<double>(v)});
        }
        continue;
      }
      if (v > avg) {
        out.push_back({k.name, k.name_null, k.cik, k.cik_null,
                       static_cast<double>(v)});
      }
    }
    std::sort(out.begin(), out.end(), [](const Expected &a, const Expected &b) {
      return a.total > b.total;
    });
    if (out.size() > 100) {
      out.resize(100);
    }
    return out;
  }
};

// ---- engine plumbing -------------------------------------------------------

class Services final : public AuxEngineServices, public SecEdgarQ3InitialScan {
public:
  explicit Services(const Tables &tables, const char *accelerator)
      : tables(tables), store(accelerator) {}

  void ScanNumAggregates(
      const std::function<void(const SecEdgarQ3NumAggInput &)> &consumer) override {
    ++num_scans;
    std::map<std::pair<std::string, std::string>, std::pair<double, int64_t>> agg;
    for (const auto &n : tables.num) {
      if (n.adsh_null || n.uom_null || n.value_null) {
        continue;
      }
      auto &a = agg[{n.adsh, n.uom}];
      a.first += n.value;
      a.second += 1;
    }
    for (const auto &[key, a] : agg) {
      consumer({key.first, key.second, a.first, a.second});
    }
  }

  void ScanSub(
      const std::function<void(const SecEdgarQ3SubInput &)> &consumer) override {
    for (const auto &[adsh, s] : tables.sub) {
      SecEdgarQ3SubInput in;
      in.adsh = adsh;
      in.name = s.name;
      in.name_is_null = s.name_null;
      in.cik = s.cik;
      in.cik_is_null = s.cik_null;
      in.fy = s.fy;
      in.fy_is_null = s.fy_null;
      consumer(in);
    }
  }

  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  const Tables &tables;
  engine::duckdb::DuckDBStateStore store;
  int num_scans = 0;
};

struct Builder final : StateBuilder {
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

struct Sink final : AuxRowSink {
  void Begin(const OutputSchema &schema) override {
    Check(schema.columns.size() == 3, "schema width");
  }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

struct Harness {
  explicit Harness(const char *accelerator, bool parallel)
      : services(tables, accelerator) {
    if (parallel) {
      module = std::make_shared<SecEdgarQ3ParallelModule>(
          SecEdgarQ3Config{kNumTable, kSubTable}, &services);
    } else {
      module = std::make_shared<SecEdgarQ3Module>(
          SecEdgarQ3Config{kNumTable, kSubTable}, &services);
    }
    coordinator.Register(module);
    coordinator.AttachServices(services);
  }

  void Build() {
    Builder builder;
    module->Build(services, builder);
    Check(builder.finished, "build finished");
    services.store.Install(std::move(builder.images), module->Id());
  }

  void Commit() {
    coordinator.PreCommit();
    coordinator.Commit();
  }

  void Mutate(TableId table, MutationKind kind, std::vector<AuxRow> old_rows,
              std::vector<AuxRow> new_rows, std::vector<ColumnId> cols = {}) {
    MutationBatch batch;
    batch.table = table;
    batch.kind = kind;
    batch.old_rows = std::move(old_rows);
    batch.new_rows = std::move(new_rows);
    batch.updated_columns = std::move(cols);
    coordinator.OnMutation(batch);
  }

  std::vector<AuxRow> Run(const QueryArguments &args = {}) {
    Sink sink;
    module->Execute(services, args, sink);
    Check(sink.ended, "sink ended");
    return std::move(sink.rows);
  }

  // Every verify checks the defaults and a few (fy, uom) drawn from the
  // parameter domain plus edge values (NULL arguments included).
  void Verify(const std::string &context) {
    VerifyParams(context, 2022, "USD", false, true);
    static std::mt19937 param_rng(12345);
    static const int64_t fys[] = {2021, 2022, 2023, 2024, 2019, 2025, 0, 2022, 2022};
    static const char *uoms[] = {"USD", "shares", "pure", "CAD", "CNY", "EUR",
                                 "nonexistent", "USD", "USD", "usd", ""};
    for (int i = 0; i < 3; ++i) {
      const auto fy = fys[param_rng() % (sizeof fys / sizeof *fys)];
      const std::string uom = uoms[param_rng() % (sizeof uoms / sizeof *uoms)];
      const bool null_arg = param_rng() % 25 == 0;
      VerifyParams(context, fy, uom, null_arg, false);
    }
  }

  void VerifyParams(const std::string &context_in, int64_t fy,
                    const std::string &uom, bool null_arg, bool defaults) {
    const std::string context = context_in + " [fy=" + std::to_string(fy) +
                                " uom=" + uom + (null_arg ? " NULL" : "") + "]";
    QueryArguments args;
    if (!defaults) {
      args.values.push_back(null_arg ? AuxValue::Null(AuxType::Int64)
                                     : Int(fy));
      args.values.push_back(AuxValue::Of(AuxType::String, uom));
    }
    auto got_rows = Run(args);
    std::vector<Expected> ambiguous;
    auto want = tables.Query(&ambiguous, fy, uom, null_arg);
    ++g_verifies;
    g_nonempty += want.empty() ? 0 : 1;
    auto is_ambiguous = [&](const std::string &name, bool name_null, int64_t cik,
                            bool cik_null) {
      for (const auto &a : ambiguous) {
        if (a.name_null == name_null && (name_null || a.name == name) &&
            a.cik_null == cik_null && (cik_null || a.cik == cik)) {
          return true;
        }
      }
      return false;
    };
    std::vector<AuxRow> got;
    for (auto &row : got_rows) {
      const auto &r = row.values;
      if (is_ambiguous(r[0].is_null ? "" : std::get<std::string>(r[0].payload),
                       r[0].is_null, r[1].is_null ? 0 : std::get<int64_t>(r[1].payload),
                       r[1].is_null)) {
        continue;
      }
      got.push_back(std::move(row));
    }
    Check(got.size() == want.size(),
          context + ": row count " + std::to_string(got.size()) + " vs " +
              std::to_string(want.size()));
    for (std::size_t i = 0; i < got.size(); ++i) {
      const auto &r = got[i].values;
      const double total = std::get<double>(r[2].payload);
      const double tol = 1e-9 * std::max(1.0, std::fabs(want[i].total));
      Check(std::fabs(total - want[i].total) <= tol,
            context + ": total mismatch at row " + std::to_string(i) + " got " +
                std::to_string(total) + " want " +
                std::to_string(want[i].total));
      Check(r[0].is_null == want[i].name_null, context + ": name null");
      if (!r[0].is_null) {
        Check(std::get<std::string>(r[0].payload) == want[i].name,
              context + ": name mismatch");
      }
      Check(r[1].is_null == want[i].cik_null, context + ": cik null");
      if (!r[1].is_null) {
        Check(std::get<int64_t>(r[1].payload) == want[i].cik,
              context + ": cik mismatch");
      }
    }
  }

  Tables tables;
  Services services;
  std::shared_ptr<SecEdgarQ3Module> module;
  AuxTransactionCoordinator coordinator;
};

// ---- random workload ---------------------------------------------------------

struct Gen {
  explicit Gen(uint32_t seed, bool nonfinite = false)
      : rng(seed), nonfinite(nonfinite) {
    for (int i = 0; i < 24; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%010d-22-%06d", 1000 + i % 9, i);
      adsh_pool.emplace_back(buf);
    }
    for (int i = 0; i < 6; ++i) {
      adsh_pool.push_back("odd-adsh-" + std::to_string(i)); // non-SEC format
    }
    for (int i = 0; i < 10; ++i) {
      names.push_back("Name " + std::to_string(i));
    }
    names.push_back("");
  }

  int Int(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(rng);
  }
  double Value() {
    // fractional, almost never tied; some negative, some huge
    double v = std::uniform_real_distribution<double>(-2000.0, 9000.0)(rng);
    if (nonfinite && Int(0, 25) == 0) {
      switch (Int(0, 2)) {
      case 0:
        return std::numeric_limits<double>::quiet_NaN();
      case 1:
        return std::numeric_limits<double>::infinity();
      default:
        return -std::numeric_limits<double>::infinity();
      }
    }
    switch (Int(0, 15)) {
    case 0:
      return v * 1e3;
    case 1:
      return -std::fabs(v);
    default:
      return v;
    }
  }
  NumM RandomNum() {
    NumM n;
    n.adsh = adsh_pool[Int(0, static_cast<int>(adsh_pool.size()) - 1)];
    n.adsh_null = Int(0, 40) == 0;
    const char *uoms[] = {"USD", "USD", "USD", "USD", "EUR", "pure", "shares", "CAD", "CNY"};
    n.uom = uoms[Int(0, 8)];
    n.uom_null = Int(0, 40) == 0;
    n.value_null = Int(0, 12) == 0;
    n.value = Value();
    n.tag = "Tag" + std::to_string(Int(0, 5));
    return n;
  }
  SubM RandomSub(const std::string &adsh) {
    SubM s;
    s.adsh = adsh;
    s.name = names[Int(0, static_cast<int>(names.size()) - 1)];
    s.name_null = Int(0, 30) == 0;
    s.cik = 100 + Int(0, 11);
    s.cik_null = Int(0, 40) == 0;
    const int fys[] = {2022, 2022, 2022, 2021, 2023, 2024};
    s.fy = fys[Int(0, 5)];
    s.fy_null = Int(0, 30) == 0;
    return s;
  }

  std::mt19937 rng;
  bool nonfinite = false;
  std::vector<std::string> adsh_pool;
  std::vector<std::string> names;
};

struct Stats {
  uint64_t ops = 0;
  uint64_t no_rebuild_violations = 0;
};

void RunRandom(bool parallel, uint32_t seed, int iterations,
               bool nonfinite = false) {
  Harness h(parallel ? "sec_edgar_q3_aux_parallel" : "sec_edgar_q3", parallel);
  Gen gen(seed, nonfinite);
  // initial data
  for (int i = 0; i < 60; ++i) {
    h.tables.num.push_back(gen.RandomNum());
  }
  for (const auto &adsh : gen.adsh_pool) {
    if (gen.Int(0, 3) != 0) {
      h.tables.sub[adsh] = gen.RandomSub(adsh);
    }
  }
  h.Build();
  h.Verify("initial");
  const int builds_before = h.services.num_scans;

  for (int it = 0; it < iterations; ++it) {
    const int txn_ops = gen.Int(1, 3);
    for (int t = 0; t < txn_ops; ++t) {
      switch (gen.Int(0, 11)) {
      case 0:
      case 1: { // num insert (1-3 rows)
        std::vector<AuxRow> rows;
        const int count = gen.Int(1, 3);
        for (int i = 0; i < count; ++i) {
          auto n = gen.RandomNum();
          h.tables.num.push_back(n);
          rows.push_back(NumRowOf(n));
        }
        h.Mutate(kNumTable, MutationKind::Insert, {}, std::move(rows));
        break;
      }
      case 2:
      case 3: { // num delete
        if (h.tables.num.empty()) {
          break;
        }
        std::vector<AuxRow> rows;
        const int count = gen.Int(1, 2);
        for (int i = 0; i < count && !h.tables.num.empty(); ++i) {
          const auto idx = static_cast<std::size_t>(
              gen.Int(0, static_cast<int>(h.tables.num.size()) - 1));
          rows.push_back(NumRowOf(h.tables.num[idx]));
          h.tables.num.erase(h.tables.num.begin() + idx);
        }
        h.Mutate(kNumTable, MutationKind::Delete, std::move(rows), {});
        break;
      }
      case 4:
      case 5: { // num value update
        if (h.tables.num.empty()) {
          break;
        }
        const auto idx = static_cast<std::size_t>(
            gen.Int(0, static_cast<int>(h.tables.num.size()) - 1));
        auto old_row = NumRowOf(h.tables.num[idx]);
        h.tables.num[idx].value = gen.Value();
        h.tables.num[idx].value_null = gen.Int(0, 10) == 0;
        h.Mutate(kNumTable, MutationKind::Update, {old_row},
                 {NumRowOf(h.tables.num[idx])}, {7});
        break;
      }
      case 6: { // num uom / adsh update (filter + join key)
        if (h.tables.num.empty()) {
          break;
        }
        const auto idx = static_cast<std::size_t>(
            gen.Int(0, static_cast<int>(h.tables.num.size()) - 1));
        auto old_row = NumRowOf(h.tables.num[idx]);
        std::vector<ColumnId> cols;
        if (gen.Int(0, 1)) {
          const char *to[] = {"USD", "EUR", "shares", "CAD"};
          h.tables.num[idx].uom = to[gen.Int(0, 3)];
          h.tables.num[idx].uom_null = gen.Int(0, 15) == 0;
          cols.push_back(5);
        }
        if (gen.Int(0, 1) || cols.empty()) {
          h.tables.num[idx].adsh = gen.adsh_pool[gen.Int(
              0, static_cast<int>(gen.adsh_pool.size()) - 1)];
          h.tables.num[idx].adsh_null = gen.Int(0, 20) == 0;
          cols.push_back(0);
        }
        if (gen.Int(0, 3) == 0) {
          cols.clear(); // engine did not report columns: treat as all
        }
        h.Mutate(kNumTable, MutationKind::Update, {old_row},
                 {NumRowOf(h.tables.num[idx])}, cols);
        break;
      }
      case 7: { // num update of an irrelevant column only
        if (h.tables.num.empty()) {
          break;
        }
        const auto idx = static_cast<std::size_t>(
            gen.Int(0, static_cast<int>(h.tables.num.size()) - 1));
        auto old_row = NumRowOf(h.tables.num[idx]);
        h.tables.num[idx].tag = "Other" + std::to_string(gen.Int(0, 9));
        h.Mutate(kNumTable, MutationKind::Update, {old_row},
                 {NumRowOf(h.tables.num[idx])}, {1});
        break;
      }
      case 8: { // sub insert
        std::vector<std::string> free;
        for (const auto &a : gen.adsh_pool) {
          if (!h.tables.sub.count(a)) {
            free.push_back(a);
          }
        }
        if (free.empty()) {
          break;
        }
        const auto adsh = free[gen.Int(0, static_cast<int>(free.size()) - 1)];
        auto s = gen.RandomSub(adsh);
        h.tables.sub[adsh] = s;
        h.Mutate(kSubTable, MutationKind::Insert, {}, {SubRowOf(s)});
        break;
      }
      case 9: { // sub delete
        if (h.tables.sub.empty()) {
          break;
        }
        auto itr = h.tables.sub.begin();
        std::advance(itr, gen.Int(0, static_cast<int>(h.tables.sub.size()) - 1));
        auto old_row = SubRowOf(itr->second);
        h.tables.sub.erase(itr);
        h.Mutate(kSubTable, MutationKind::Delete, {old_row}, {});
        break;
      }
      case 10: { // sub update fy / name / cik
        if (h.tables.sub.empty()) {
          break;
        }
        auto itr = h.tables.sub.begin();
        std::advance(itr, gen.Int(0, static_cast<int>(h.tables.sub.size()) - 1));
        auto old_row = SubRowOf(itr->second);
        auto s = gen.RandomSub(itr->first);
        std::vector<ColumnId> cols;
        if (gen.Int(0, 1)) {
          itr->second.fy = s.fy;
          itr->second.fy_null = s.fy_null;
          cols.push_back(3);
        }
        if (gen.Int(0, 1)) {
          itr->second.name = s.name;
          itr->second.name_null = s.name_null;
          cols.push_back(1);
        }
        if (gen.Int(0, 1) || cols.empty()) {
          itr->second.cik = s.cik;
          itr->second.cik_null = s.cik_null;
          cols.push_back(2);
        }
        h.Mutate(kSubTable, MutationKind::Update, {old_row},
                 {SubRowOf(itr->second)}, cols);
        break;
      }
      case 11: { // sub primary-key update (adsh change)
        if (h.tables.sub.empty()) {
          break;
        }
        std::vector<std::string> free;
        for (const auto &a : gen.adsh_pool) {
          if (!h.tables.sub.count(a)) {
            free.push_back(a);
          }
        }
        if (free.empty()) {
          break;
        }
        auto itr = h.tables.sub.begin();
        std::advance(itr, gen.Int(0, static_cast<int>(h.tables.sub.size()) - 1));
        auto old_row = SubRowOf(itr->second);
        auto moved = itr->second;
        moved.adsh = free[gen.Int(0, static_cast<int>(free.size()) - 1)];
        h.tables.sub.erase(itr);
        h.tables.sub[moved.adsh] = moved;
        h.Mutate(kSubTable, MutationKind::Update, {old_row}, {SubRowOf(moved)},
                 {0});
        break;
      }
      }
    }
    h.Commit();
    h.Verify("seed " + std::to_string(seed) + " iter " + std::to_string(it));
    if (it % 97 == 0) {
      // an aborted transaction must not leak into the state
      h.Mutate(kNumTable, MutationKind::Insert, {}, {NumRowOf(gen.RandomNum())});
      h.coordinator.Abort();
      h.Verify("after abort");
    }
  }
  // NaN / inf rows cannot be maintained incrementally (rebuild is expected).
  Check(nonfinite || h.services.num_scans == builds_before,
        "state was rebuilt although every mutation is maintainable with "
        "ApplyDelta (rebuild scans=" +
            std::to_string(h.services.num_scans - builds_before) + ")");
}

// ---- directed scenarios --------------------------------------------------------

void TestDirected(bool parallel) {
  Harness h(parallel ? "sec_edgar_q3_aux_parallel" : "sec_edgar_q3", parallel);
  auto sub = [&](const char *adsh, const char *name, int cik, int fy) {
    SubM s;
    s.adsh = adsh;
    s.name = name;
    s.cik = cik;
    s.fy = fy;
    h.tables.sub[adsh] = s;
    return s;
  };
  auto num = [&](const char *adsh, double value) {
    NumM n;
    n.adsh = adsh;
    n.value = value;
    h.tables.num.push_back(n);
    return n;
  };
  sub("0000000001-22-000001", "Alpha", 1, 2022);
  sub("0000000002-22-000002", "Beta", 2, 2022);
  sub("0000000003-22-000003", "Gamma", 3, 2022);
  sub("0000000004-21-000004", "Delta", 4, 2021);
  num("0000000001-22-000001", 10);
  num("0000000002-22-000002", 20);
  num("0000000003-22-000003", 100);
  num("0000000004-21-000004", 1e9);
  h.Build();
  h.Verify("baseline");
  {
    const auto rows = h.Run();
    Check(rows.size() == 1 &&
              std::get<std::string>(rows[0].values[0].payload) == "Gamma",
          "only Gamma exceeds the average");
  }
  const int scans = h.services.num_scans;

  // INSERT num for an existing filing
  auto added = num("0000000001-22-000001", 51);
  h.Mutate(kNumTable, MutationKind::Insert, {}, {NumRowOf(added)});
  h.Commit();
  h.Verify("insert num");

  // DELETE the only row of a group: the group (and its cik) must disappear
  // even though a negative average would otherwise let a 0-sum group in.
  auto neg = num("0000000002-22-000002", -500);
  h.Mutate(kNumTable, MutationKind::Insert, {}, {NumRowOf(neg)});
  h.Commit();
  h.Verify("negative sums");
  // delete the rows of Beta completely
  std::vector<AuxRow> gone;
  for (auto it = h.tables.num.begin(); it != h.tables.num.end();) {
    if (it->adsh == "0000000002-22-000002") {
      gone.push_back(NumRowOf(*it));
      it = h.tables.num.erase(it);
    } else {
      ++it;
    }
  }
  h.Mutate(kNumTable, MutationKind::Delete, std::move(gone), {});
  h.Commit();
  h.Verify("group vanished after deleting its rows");

  // UPDATE sub.fy: filing moves into / out of the fy=2022 result
  {
    auto old_row = SubRowOf(h.tables.sub["0000000004-21-000004"]);
    h.tables.sub["0000000004-21-000004"].fy = 2022;
    h.Mutate(kSubTable, MutationKind::Update, {old_row},
             {SubRowOf(h.tables.sub["0000000004-21-000004"])}, {3});
    h.Commit();
    h.Verify("fy 2021 -> 2022");
    auto old2 = SubRowOf(h.tables.sub["0000000004-21-000004"]);
    h.tables.sub["0000000004-21-000004"].fy = 2023;
    h.Mutate(kSubTable, MutationKind::Update, {old2},
             {SubRowOf(h.tables.sub["0000000004-21-000004"])}, {3});
    h.Commit();
    h.Verify("fy 2022 -> 2023");
  }
  // UPDATE sub.cik / name: rows move between groups
  {
    auto old_row = SubRowOf(h.tables.sub["0000000003-22-000003"]);
    h.tables.sub["0000000003-22-000003"].cik = 1; // joins Alpha's cik
    h.Mutate(kSubTable, MutationKind::Update, {old_row},
             {SubRowOf(h.tables.sub["0000000003-22-000003"])}, {2});
    h.Commit();
    h.Verify("cik change merges cik totals");
    auto old2 = SubRowOf(h.tables.sub["0000000003-22-000003"]);
    h.tables.sub["0000000003-22-000003"].name = "Alpha";
    h.Mutate(kSubTable, MutationKind::Update, {old2},
             {SubRowOf(h.tables.sub["0000000003-22-000003"])}, {1});
    h.Commit();
    h.Verify("name change merges groups");
  }
  // sub INSERT of a filing whose num rows already exist (orphan rows)
  {
    num("0000000009-22-000009", 4242);
    h.Mutate(kNumTable, MutationKind::Insert, {}, {NumRowOf(h.tables.num.back())});
    h.Commit();
    h.Verify("orphan num row ignored");
    auto s = sub("0000000009-22-000009", "Orphan Inc", 9, 2022);
    h.Mutate(kSubTable, MutationKind::Insert, {}, {SubRowOf(s)});
    h.Commit();
    h.Verify("sub insert adopts earlier num rows");
    auto old_row = SubRowOf(h.tables.sub["0000000009-22-000009"]);
    h.tables.sub.erase("0000000009-22-000009");
    h.Mutate(kSubTable, MutationKind::Delete, {old_row}, {});
    h.Commit();
    h.Verify("sub delete");
  }
  Check(h.services.num_scans == scans, "no rebuild in directed scenario");

  // A row that does not follow the documented layout falls back to a rebuild
  // and stays correct.
  {
    AuxRow bad;
    bad.values = {Str("0000000001-22-000001"), Str("USD"),
                  AuxValue::Of(AuxType::Double, 7.0)};
    NumM n;
    n.adsh = "0000000001-22-000001";
    n.value = 7.0;
    h.tables.num.push_back(n);
    h.Mutate(kNumTable, MutationKind::Insert, {}, {bad});
    h.Commit();
    h.Verify("rebuild fallback on unrecognised row layout");
    Check(h.services.num_scans > scans, "fallback rebuilt");
  }
  // NaN / inf cannot be subtracted back out: rebuild (and still correct).
  {
    NumM n;
    n.adsh = "0000000001-22-000001";
    n.value = std::numeric_limits<double>::infinity();
    n.uom = "EUR"; // non-finite rows cannot be maintained: rebuild, still exact
    h.tables.num.push_back(n);
    h.Mutate(kNumTable, MutationKind::Insert, {}, {NumRowOf(n)});
    h.Commit();
    h.Verify("inf row of another unit");
  }
}

// Kernel equivalence: scalar vs AVX2 on random arrays (including zeros,
// negative values and cnt == 0 slots).
void TestKernels() {
  std::mt19937 rng(7);
  for (int trial = 0; trial < 200; ++trial) {
    const uint32_t n = 1 + rng() % 2000;
    SecEdgarQ3AggArray array;
    array.size = n;
    const auto chunks = (n + kSecEdgarQ3AggChunk - 1) / kSecEdgarQ3AggChunk;
    for (uint32_t c = 0; c < chunks; ++c) {
      auto chunk = std::make_shared<SecEdgarQ3AggChunkData>();
      std::memset(chunk.get(), 0, sizeof(*chunk));
      for (std::size_t i = 0; i < kSecEdgarQ3AggChunk; ++i) {
        if (c * kSecEdgarQ3AggChunk + i >= n) {
          break;
        }
        if (rng() % 5 == 0) {
          continue; // empty slot
        }
        chunk->cnt[i] = 1 + rng() % 7;
        chunk->sum[i] = std::uniform_real_distribution<double>(-1e4, 1e5)(rng);
      }
      array.chunks.push_back(chunk);
    }
    double s0, s1;
    uint64_t c0, c1;
    SecEdgarQ3Kernels::AvgInputsScalar(array, s0, c0);
    SecEdgarQ3Kernels::AvgInputsAvx2(array, s1, c1);
    Check(c0 == c1, "kernel count");
    Check(std::fabs(s0 - s1) <= 1e-9 * std::max(1.0, std::fabs(s0)), "kernel sum");
    const double thr = c0 ? s0 / static_cast<double>(c0) : 0.0;
    std::vector<SecEdgarQ3Result::Row> a, b;
    SecEdgarQ3Kernels::CollectScalar(array, thr, a);
    SecEdgarQ3Kernels::CollectAvx2(array, thr, b);
    Check(a.size() == b.size(), "collect size");
    for (std::size_t i = 0; i < a.size(); ++i) {
      Check(a[i].group == b[i].group && a[i].total == b[i].total, "collect row");
    }
  }
}

// Add/remove cycles must not drift: a huge value inserted and deleted again
// would wipe out the low bits of a plain double running sum.
void TestNoDrift(bool parallel) {
  Harness h(parallel ? "sec_edgar_q3_aux_parallel" : "sec_edgar_q3", parallel);
  SubM a;
  a.adsh = "0000000001-22-000001";
  a.name = "Alpha";
  a.cik = 1;
  a.fy = 2022;
  SubM b = a;
  b.adsh = "0000000002-22-000002";
  b.name = "Beta";
  b.cik = 2;
  h.tables.sub[a.adsh] = a;
  h.tables.sub[b.adsh] = b;
  NumM na;
  na.adsh = a.adsh;
  na.value = 9000.5;
  NumM nb;
  nb.adsh = b.adsh;
  nb.value = 1000.25;
  h.tables.num.push_back(na);
  h.tables.num.push_back(nb);
  h.Build();
  std::mt19937 rng(5);
  for (int i = 0; i < 400; ++i) {
    NumM extra;
    extra.adsh = (i % 2) ? a.adsh : b.adsh;
    extra.value = (1e16 + static_cast<double>(rng() % 100000)) * ((i % 3) ? 1.0 : -1.0) +
                  static_cast<double>(rng() % 1000) / 7.0;
    h.Mutate(kNumTable, MutationKind::Insert, {}, {NumRowOf(extra)});
    h.Commit();
    h.Mutate(kNumTable, MutationKind::Delete, {NumRowOf(extra)}, {});
    h.Commit();
  }
  h.Verify("after add/remove cycles");
  const auto rows = h.Run();
  Check(rows.size() == 1 && std::get<double>(rows[0].values[2].payload) == 9000.5,
        "no drift after add/remove cycles");
}

} // namespace

int main() {
  TestKernels();
  TestNoDrift(false);
  TestNoDrift(true);
  TestDirected(false);
  TestDirected(true);
  const int iterations = std::getenv("ACCEL_Q3_TEST_ITERS")
                             ? std::atoi(std::getenv("ACCEL_Q3_TEST_ITERS"))
                             : 600;
  for (uint32_t seed = 1; seed <= 8; ++seed) {
    RunRandom(false, seed, iterations);
  }
  RunRandom(true, 99, iterations);
  for (uint32_t seed = 101; seed <= 104; ++seed) {
    RunRandom(false, seed, iterations, /*nonfinite=*/true);
  }
  std::cout << "test_shared_sec_edgar_q3 OK (" << g_verifies
            << " verifications, " << g_nonempty << " with non-empty results)"
            << std::endl;
  return 0;
}
