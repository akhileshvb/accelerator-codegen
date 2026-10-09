// Randomized differential test for the SEC EDGAR Q1 aux module (mock engine).
//
// A reference (brute-force) evaluation of Q1 over a model of `pre` is compared
// with the aux module after every transaction. Transactions mix INSERT /
// DELETE / UPDATE mutations (including updates that move rows between groups,
// change adsh, null out stmt/rfile/adsh/line, and non-canonical adsh strings),
// multi-mutation transactions, aborts, subtransactions, and bulk batches that
// exceed the ApplyDelta row cap (rebuild path). Q1 has no template parameters,
// so there is no parameter axis.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "sec_edgar/q1/common/module.hpp"
#ifdef ACCEL_ENABLE_PARALLEL_AUX
#include "sec_edgar/q1/duckdb/aux/parallel/module.hpp"
#endif

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::core;
using query::SecEdgarQ1Config;
using query::SecEdgarQ1InitialScan;
using query::SecEdgarQ1Module;
using query::SecEdgarQ1PreInput;

namespace {

struct Row {
  bool stmt_null = false, rfile_null = false, adsh_null = false, line_null = false;
  std::string stmt, rfile, adsh;
  int64_t line = 0;
};

Row Normalize(Row r) {
  if (r.stmt_null) r.stmt.clear();
  if (r.rfile_null) r.rfile.clear();
  if (r.adsh_null) r.adsh.clear();
  if (r.line_null) r.line = 0;
  return r;
}

AuxRow ToAux(const Row &r) {
  AuxRow out;
  out.values.push_back(r.stmt_null ? AuxValue::Null(AuxType::String)
                                   : AuxValue::Of(AuxType::String, r.stmt));
  out.values.push_back(r.rfile_null ? AuxValue::Null(AuxType::String)
                                    : AuxValue::Of(AuxType::String, r.rfile));
  out.values.push_back(r.adsh_null ? AuxValue::Null(AuxType::String)
                                   : AuxValue::Of(AuxType::String, r.adsh));
  out.values.push_back(r.line_null ? AuxValue::Null(AuxType::Int64)
                                   : AuxValue::Of(AuxType::Int64, r.line));
  return out;
}

bool Same(const Row &a, const Row &b) {
  return a.stmt_null == b.stmt_null && a.rfile_null == b.rfile_null &&
         a.adsh_null == b.adsh_null && a.line_null == b.line_null &&
         a.stmt == b.stmt && a.rfile == b.rfile && a.adsh == b.adsh &&
         a.line == b.line;
}

struct Expected {
  std::string stmt, rfile;
  bool rfile_null;
  int64_t cnt, nf;
  bool avg_null;
  double avg;
};

std::vector<Expected> Reference(const std::vector<Row> &pre) {
  struct Acc {
    int64_t cnt = 0, line_sum = 0, line_cnt = 0;
    std::set<std::string> adsh;
  };
  std::map<std::tuple<std::string, bool, std::string>, Acc> groups;
  for (const auto &r : pre) {
    if (r.stmt_null) {
      continue;
    }
    auto &acc = groups[{r.stmt, r.rfile_null, r.rfile_null ? std::string() : r.rfile}];
    acc.cnt++;
    if (!r.line_null) {
      acc.line_sum += r.line;
      acc.line_cnt++;
    }
    if (!r.adsh_null) {
      acc.adsh.insert(r.adsh);
    }
  }
  std::vector<Expected> out;
  for (const auto &[key, acc] : groups) {
    out.push_back({std::get<0>(key), std::get<2>(key), std::get<1>(key), acc.cnt,
                   static_cast<int64_t>(acc.adsh.size()), acc.line_cnt == 0,
                   acc.line_cnt == 0 ? 0.0
                                     : static_cast<double>(
                                           static_cast<long double>(acc.line_sum) /
                                           static_cast<long double>(acc.line_cnt))});
  }
  return out;
}

std::string Key(const Expected &e) {
  return e.stmt + "|" + (e.rfile_null ? "<null>" : e.rfile) + "|" +
         std::to_string(e.cnt) + "|" + std::to_string(e.nf);
}

class Sink final : public AuxRowSink {
public:
  void Begin(const OutputSchema &schema) override { assert(schema.columns.size() == 5); }
  void WriteRow(const AuxRow &row) override { rows.push_back(row); }
  void End() override { ended = true; }
  std::vector<AuxRow> rows;
  bool ended = false;
};

class Builder final : public StateBuilder {
public:
  void Write(StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<StateImage> images;
  bool finished = false;
};

class Services final : public AuxEngineServices, public SecEdgarQ1InitialScan {
public:
  Services(const char *accelerator)
      : store(accelerator), accelerator(accelerator) {}

  void ScanPre(const std::function<void(const SecEdgarQ1PreInput &)> &consumer) const override {
    for (const auto &r : *pre) {
      SecEdgarQ1PreInput in;
      in.stmt_is_null = r.stmt_null;
      in.stmt = r.stmt;
      in.rfile_is_null = r.rfile_null;
      in.rfile = r.rfile;
      in.adsh_is_null = r.adsh_null;
      in.adsh = r.adsh;
      in.line_is_null = r.line_null;
      in.line = static_cast<int32_t>(r.line);
      consumer(in);
    }
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }
  const maintenance::CostModelParams &MaintenanceCostModel() const override { return model; }
  double ExpectedQueriesBeforeNextWrite() const override { return 10.0; }

  void Install(Builder &builder) {
    store.Install(builder.images, accelerator);
  }

  engine::duckdb::DuckDBStateStore store;
  maintenance::CostModelParams model;
  const std::vector<Row> *pre = nullptr;
  const char *accelerator;
};

std::map<std::string, int> g_action_counts;

class CountingModule final : public AuxQueryModule {
public:
  explicit CountingModule(std::shared_ptr<AuxQueryModule> inner) : inner(std::move(inner)) {}
  AcceleratorId Id() const override { return inner->Id(); }
  std::vector<TableDependency> Dependencies() const override { return inner->Dependencies(); }
  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override { return inner->CreateTxnBuffer(); }
  void Build(AuxEngineServices &s, StateBuilder &b) override { inner->Build(s, b); }
  InvalidationPlan InterpretMutation(const MutationBatch &m) const override { return inner->InterpretMutation(m); }
  void AccumulateMutation(const MutationBatch &m, AuxEngineServices &s, AuxTxnBuffer &b) override {
    inner->AccumulateMutation(m, s, b);
  }
  StagedMaintenanceOutcome StageMaintenance(AuxEngineServices &s, const InvalidationPlan &p,
                                            AuxTxnBuffer &b) override {
    auto outcome = inner->StageMaintenance(s, p, b);
    g_action_counts[std::to_string(static_cast<int>(outcome.action))]++;
    return outcome;
  }
  bool CheckValidity(AuxEngineServices &s, const QueryArguments &a) const override { return inner->CheckValidity(s, a); }
  void Execute(AuxEngineServices &s, const QueryArguments &a, AuxRowSink &k) override { inner->Execute(s, a, k); }
  void ExecuteFallback(AuxEngineServices &s, const QueryArguments &a, AuxRowSink &k) override {
    inner->ExecuteFallback(s, a, k);
  }
  std::shared_ptr<AuxQueryModule> inner;
};

struct Harness {
  explicit Harness(bool parallel)
      : services(parallel ? "sec_edgar_q1_aux_parallel" : "sec_edgar_q1") {
    services.model.apply_delta_max_rows = 256;
    services.model.apply_delta_max_part_fanout = 512;
    services.pre = &pre;
    std::shared_ptr<AuxQueryModule> shared;
    if (parallel) {
#ifdef ACCEL_ENABLE_PARALLEL_AUX
      shared = std::make_shared<query::SecEdgarQ1ParallelModule>(SecEdgarQ1Config{1}, &services);
#else
      std::cerr << "parallel module not compiled\n";
      std::exit(2);
#endif
    } else {
      shared = std::make_shared<SecEdgarQ1Module>(SecEdgarQ1Config{1}, &services);
    }
    module = shared;
    coordinator.Register(std::make_shared<CountingModule>(shared));
    coordinator.AttachServices(services);
  }

  void Build() {
    Builder builder;
    module->Build(services, builder);
    assert(builder.finished);
    services.Install(builder);
  }

  void Check(const std::string &label) {
    Sink sink;
    module->Execute(services, {}, sink);
    assert(sink.ended);
    auto expected = Reference(pre);
    expected.erase(std::remove_if(expected.begin(), expected.end(),
                                  [](const Expected &e) { return e.cnt <= 0; }),
                   expected.end());
    if (sink.rows.size() != expected.size()) {
      std::cerr << "FAIL " << label << ": rows " << sink.rows.size() << " vs "
                << expected.size() << "\n";
      std::abort();
    }
    std::multiset<std::string> want;
    for (const auto &e : expected) {
      want.insert(Key(e));
    }
    int64_t prev_cnt = std::numeric_limits<int64_t>::max();
    for (const auto &row : sink.rows) {
      Expected got;
      got.stmt = std::get<std::string>(row.values[0].payload);
      got.rfile_null = row.values[1].is_null;
      got.rfile = got.rfile_null ? std::string() : std::get<std::string>(row.values[1].payload);
      got.cnt = std::get<int64_t>(row.values[2].payload);
      got.nf = std::get<int64_t>(row.values[3].payload);
      got.avg_null = row.values[4].is_null;
      got.avg = got.avg_null ? 0.0 : std::get<double>(row.values[4].payload);
      assert(got.cnt <= prev_cnt);
      prev_cnt = got.cnt;
      auto it = want.find(Key(got));
      if (it == want.end()) {
        std::cerr << "FAIL " << label << ": unexpected row " << Key(got) << "\n";
        for (const auto &e : expected) {
          if (e.stmt == got.stmt && e.rfile_null == got.rfile_null && e.rfile == got.rfile) {
            std::cerr << "  expected for group: " << Key(e) << "\n";
          }
        }
        std::abort();
      }
      want.erase(it);
      for (const auto &e : expected) {
        if (Key(e) == Key(got)) {
          if (e.avg_null != got.avg_null ||
              e.avg != got.avg) {
            std::cerr << "FAIL " << label << ": avg " << got.avg << " vs " << e.avg << "\n";
            std::abort();
          }
          break;
        }
      }
    }
  }

  std::vector<Row> pre;
  Services services;
  AuxTransactionCoordinator coordinator;
  std::shared_ptr<AuxQueryModule> module;
};

struct Gen {
  std::mt19937_64 rng;
  std::vector<std::string> canonical;
  explicit Gen(uint64_t seed) : rng(seed) {
    for (int i = 0; i < 300; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%010llu-%02d-%06d",
                    static_cast<unsigned long long>(rng() % 10000000000ULL),
                    static_cast<int>(rng() % 100), static_cast<int>(rng() % 1000000));
      canonical.push_back(buf);
    }
    // boundary values
    canonical.push_back("0000000000-00-000000");
    canonical.push_back("9999999999-99-999999");
  }
  int R(int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); }
  Row Make() {
    static const char *stmts[] = {"BS", "IS", "CF", "EQ", "CI", "", "zz", "UN"};
    static const char *rfiles[] = {"H", "X", "", "R2", "H"};
    static const char *weird[] = {"", "short", "ZZZZZZZZZZ-ZZ-ZZZZZZ",
                                  "0001234567-22-00012",   // 19 chars
                                  "0001234567-22-0001234", // 21 chars
                                  "0001234567_22_000123",  // wrong separators
                                  "000123456a-22-000123"};
    Row r;
    r.stmt_null = R(12) == 0;
    r.stmt = stmts[R(8)];
    r.rfile_null = R(12) == 0;
    r.rfile = rfiles[R(5)];
    r.adsh_null = R(25) == 0;
    if (R(10) == 0) {
      r.adsh = weird[R(7)];
    } else if (R(8) == 0) { // fresh accession: grows the dictionary / page table
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%010llu-%02d-%06d",
                    static_cast<unsigned long long>(rng() % 10000000000ULL),
                    static_cast<int>(rng() % 100), static_cast<int>(rng() % 1000000));
      r.adsh = buf;
    } else {
      r.adsh = canonical[static_cast<std::size_t>(R(static_cast<int>(canonical.size())))];
    }
    r.line_null = R(15) == 0;
    const int kind = R(10);
    r.line = kind == 0 ? -R(1000) : kind == 1 ? 2147483000 + R(600) : R(500);
    return Normalize(r);
  }
  Row Mutate(const Row &old) {
    Row r = old;
    const int which = R(8);
    Row fresh = Make();
    switch (which) {
    case 0: r.line = fresh.line; r.line_null = fresh.line_null; break;
    case 1: r.stmt = fresh.stmt; r.stmt_null = fresh.stmt_null; break;
    case 2: r.rfile = fresh.rfile; r.rfile_null = fresh.rfile_null; break;
    case 3: r.adsh = fresh.adsh; r.adsh_null = fresh.adsh_null; break;
    case 4: r = fresh; break;
    case 5: r.line = old.line_null ? 5 : std::min<int64_t>(old.line + 1000, 2147483647); r.line_null = false; break;
    case 6: r.line_null = true; break;
    default: break; // no-op update
    }
    return Normalize(r);
  }
};

void RunScenario(bool parallel, uint64_t seed, int iterations) {
  Gen gen(seed);
  Harness h(parallel);
  const int initial = 200 + gen.R(2000);
  for (int i = 0; i < initial; ++i) {
    h.pre.push_back(gen.Make());
  }
  h.Build();
  h.Check("initial");

  for (int iter = 0; iter < iterations; ++iter) {
    const std::string label = std::string(parallel ? "parallel" : "single") + " seed=" +
                              std::to_string(seed) + " iter=" + std::to_string(iter);
    std::vector<Row> model = h.pre; // transaction-local view
    std::vector<MutationBatch> batches;
    const int nmut = 1 + gen.R(3);
    for (int m = 0; m < nmut; ++m) {
      MutationBatch batch;
      batch.table = 1;
      const int kind = gen.R(3);
      const bool bulk = gen.R(40) == 0;
      const int count = bulk ? 300 + gen.R(400) : 1 + gen.R(20);
      if (kind == 0 || model.empty()) {
        batch.kind = MutationKind::Insert;
        for (int k = 0; k < count; ++k) {
          Row r = gen.Make();
          batch.new_rows.push_back(ToAux(r));
          model.push_back(r);
        }
      } else if (kind == 1) {
        batch.kind = MutationKind::Delete;
        for (int k = 0; k < count && !model.empty(); ++k) {
          const std::size_t idx = static_cast<std::size_t>(gen.R(static_cast<int>(model.size())));
          batch.old_rows.push_back(ToAux(model[idx]));
          model[idx] = model.back();
          model.pop_back();
        }
      } else {
        batch.kind = MutationKind::Update;
        batch.updated_columns = {static_cast<ColumnId>(gen.R(4))};
        for (int k = 0; k < count; ++k) {
          const std::size_t idx = static_cast<std::size_t>(gen.R(static_cast<int>(model.size())));
          Row updated = gen.Mutate(model[idx]);
          batch.old_rows.push_back(ToAux(model[idx]));
          batch.new_rows.push_back(ToAux(updated));
          model[idx] = updated;
        }
      }
      batches.push_back(std::move(batch));
    }

    const int fate = gen.R(10);
    if (fate == 0) { // abort whole transaction
      for (const auto &b : batches) {
        h.coordinator.OnMutation(b);
      }
      h.coordinator.PreCommit();
      h.coordinator.Abort();
      h.Check(label + " (abort)");
      continue;
    }
    if (fate == 1 && batches.size() >= 2) { // abort a subtransaction holding the last batch
      std::vector<Row> committed_model = model;
      // recompute model without the last batch
      std::vector<Row> partial = h.pre;
      // apply all but last batch to `partial` by replaying batches
      auto replay = [&](std::vector<Row> &rows, const MutationBatch &b) {
        auto find = [&](const AuxRow &aux) {
          Row want;
          want.stmt_null = aux.values[0].is_null;
          want.stmt = want.stmt_null ? "" : std::get<std::string>(aux.values[0].payload);
          want.rfile_null = aux.values[1].is_null;
          want.rfile = want.rfile_null ? "" : std::get<std::string>(aux.values[1].payload);
          want.adsh_null = aux.values[2].is_null;
          want.adsh = want.adsh_null ? "" : std::get<std::string>(aux.values[2].payload);
          want.line_null = aux.values[3].is_null;
          want.line = want.line_null ? 0 : std::get<int64_t>(aux.values[3].payload);
          return want;
        };
        if (b.kind == MutationKind::Insert) {
          for (const auto &a : b.new_rows) rows.push_back(find(a));
        } else if (b.kind == MutationKind::Delete) {
          for (const auto &a : b.old_rows) {
            const Row w = find(a);
            for (std::size_t i = 0; i < rows.size(); ++i) {
              if (Same(rows[i], w)) { rows[i] = rows.back(); rows.pop_back(); break; }
            }
          }
        } else {
          for (std::size_t k = 0; k < b.old_rows.size(); ++k) {
            const Row w = find(b.old_rows[k]);
            for (std::size_t i = 0; i < rows.size(); ++i) {
              if (Same(rows[i], w)) { rows[i] = find(b.new_rows[k]); break; }
            }
          }
        }
      };
      for (std::size_t i = 0; i + 1 < batches.size(); ++i) {
        replay(partial, batches[i]);
      }
      for (std::size_t i = 0; i + 1 < batches.size(); ++i) {
        h.coordinator.OnMutation(batches[i]);
      }
      h.coordinator.BeginSubtransaction();
      h.coordinator.OnMutation(batches.back());
      h.coordinator.AbortSubtransaction();
      h.pre = partial; // the base table already holds the writes at PRE_COMMIT
      h.coordinator.PreCommit();
      h.coordinator.Commit();
      h.Check(label + " (subxact abort)");
      continue;
    }
    if (fate == 2) { // all inside a committed subtransaction
      h.coordinator.BeginSubtransaction();
      for (const auto &b : batches) {
        h.coordinator.OnMutation(b);
      }
      h.coordinator.CommitSubtransaction();
    } else {
      for (const auto &b : batches) {
        h.coordinator.OnMutation(b);
      }
    }
    h.pre = model; // the base table already holds the writes at PRE_COMMIT
    h.coordinator.PreCommit();
    h.coordinator.Commit();
    h.Check(label);
  }
}

} // namespace

int main(int argc, char **argv) {
  const int iterations = argc > 1 ? std::atoi(argv[1]) : 400;
  const uint64_t base_seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20260808;
  for (int s = 0; s < 6; ++s) {
    RunScenario(false, base_seed + static_cast<uint64_t>(s), iterations);
#ifdef ACCEL_ENABLE_PARALLEL_AUX
    RunScenario(true, base_seed + 1000 + static_cast<uint64_t>(s), iterations);
#endif
  }
  for (const auto &[action, count] : g_action_counts) {
    std::cout << "action " << action << ": " << count << "\n";
  }
  assert(g_action_counts.count(std::to_string(static_cast<int>(MaintenanceAction::ApplyDelta))) > 0);
  assert(g_action_counts.count(std::to_string(static_cast<int>(MaintenanceAction::RebuildAll))) > 0);
  std::cout << "test_shared_sec_edgar_q1_random OK\n";
  return 0;
}
