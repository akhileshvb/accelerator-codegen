// GENERATED BACKEND - test MODULE for the writer side (S1-S4)
//
// Built only into accelerator_codegen_pg_writer_test, next to the writer
// sources. It stands in for what they depend on: a recording shard worker
// (EnsureWorker counts, SendCommit keeps the decoded commit in this
// backend's memory), the accelerator registry, and two fake accelerators.
// The wt_* SQL functions expose the shared registry and epochs and let
// test_postgres_writer hold a gate or stall a commit at a known point.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include <postgres.h>
#include <access/xact.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <storage/lock.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/snapmgr.h>
PG_MODULE_MAGIC;
}

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/state/messages.hpp"
#include "backends/postgres/state/shard_worker.hpp"
#include "backends/postgres/state/shared.hpp"

namespace pg = accelerator_codegen::aux_state::engine::postgres;
namespace core = accelerator_codegen::aux_state::core;
namespace runtime = accelerator_codegen::aux_state::runtime;

// ---- the accelerator registry (api/accelerators.cpp in the real module) -----------
namespace accelerator_codegen::aux_state::engine::postgres {
namespace {
std::map<std::string, PostgresAcceleratorGetter> &Registry() {
  static std::map<std::string, PostgresAcceleratorGetter> registry;
  return registry;
}
} // namespace

void RegisterPostgresAccelerator(const char *id, PostgresAcceleratorGetter get) { Registry()[id] = get; }

PostgresAccelerator *FindPostgresAccelerator(const core::AcceleratorId &id) {
  const auto it = Registry().find(id);
  return it == Registry().end() ? nullptr : &it->second();
}
} // namespace accelerator_codegen::aux_state::engine::postgres

namespace {

// fake_a: t1 columns 0-3. fake_b: t1 columns 2-6 and all of t2. (t1 has an
// eighth column, 7, that neither watches.)
class FakeAccelerator final : public pg::PostgresAccelerator {
public:
  FakeAccelerator(std::string id, std::vector<core::TableDependency> dependencies)
      : id_(std::move(id)), dependencies_(std::move(dependencies)) {}
  const core::AcceleratorId &Id() const override { return id_; }
  std::vector<core::TableDependency> Dependencies() const override { return dependencies_; }
  std::shared_ptr<core::AuxQueryModule> Module() override { return nullptr; }
  bool TuneBuild() const override { return false; }
  const accelerator_codegen::aux_state::maintenance::CostModelParams &CostModel() override { return cost_model_; }

private:
  std::string id_;
  std::vector<core::TableDependency> dependencies_;
  accelerator_codegen::aux_state::maintenance::CostModelParams cost_model_;
};

core::TableDependency Dependency(core::TableId table, std::vector<int32_t> attnums) {
  core::TableDependency dependency;
  dependency.table = table;
  dependency.mutation_attnums = std::move(attnums);
  return dependency;
}

pg::PostgresAccelerator &FakeA() {
  static FakeAccelerator accelerator("fake_a", {Dependency(1, {0, 1, 2, 3})});
  return accelerator;
}
pg::PostgresAccelerator &FakeB() {
  static FakeAccelerator accelerator("fake_b", {Dependency(1, {2, 3, 4, 5, 6}), Dependency(2, {})});
  return accelerator;
}
const pg::PostgresAcceleratorRegistrar register_a("fake_a", FakeA);
const pg::PostgresAcceleratorRegistrar register_b("fake_b", FakeB);

// ---- the recording shard worker ------------------------------------------------------
int g_ensure_calls = 0;
std::vector<std::string> g_sent; // rendered commits, in the order they were sent

std::string Render(const runtime::ColumnSet &columns, std::size_t rows) {
  std::ostringstream out;
  for (std::size_t c = 0; c < columns.columns.size(); c++) {
    out << " " << columns.attnums[c] << "=[";
    for (std::size_t r = 0; r < rows; r++) {
      const core::AuxValue value = columns.columns[c].Value(r);
      out << (r ? "," : "");
      if (value.is_null) {
        out << "NULL";
      } else if (const auto *v = std::get_if<int64_t>(&value.payload)) {
        out << "i" << *v;
      } else if (const auto *v = std::get_if<core::ScaledDecimal>(&value.payload)) {
        out << "d" << core::ScaledDecimal128ToString(v->value, v->scale);
      } else if (const auto *v = std::get_if<core::ScaledDecimal128>(&value.payload)) {
        out << "w" << core::ScaledDecimal128ToString(v->value, v->scale);
      } else if (const auto *v = std::get_if<double>(&value.payload)) {
        out << "f" << *v;
      } else if (const auto *v = std::get_if<core::AuxDate>(&value.payload)) {
        out << "D" << v->days_since_epoch;
      } else if (const auto *v = std::get_if<bool>(&value.payload)) {
        out << "b" << *v;
      } else if (const auto *v = std::get_if<std::string>(&value.payload)) {
        out << "s" << *v;
      }
    }
    out << "]";
  }
  return out.str();
}

std::string Render(const pg::CommitMessage &commit) {
  static const char *const kinds[] = {"insert", "delete", "update"};
  std::ostringstream out;
  out << "commit acc=" << commit.accelerator << " epoch=" << commit.epoch << " empty=" << commit.empty
      << " invalidates=" << commit.invalidates << " batches=" << commit.batches.size() << "\n";
  for (const auto &batch : commit.batches) {
    out << "  batch table=" << batch->table << " kind=" << kinds[static_cast<int>(batch->kind)]
        << " rows=" << batch->row_count << " updated=[";
    for (std::size_t i = 0; i < batch->updated_attnums.size(); i++) {
      out << (i ? "," : "") << batch->updated_attnums[i];
    }
    out << "]\n";
    if (!batch->old_rows.columns.empty()) {
      out << "    old" << Render(batch->old_rows, batch->row_count) << "\n";
    }
    if (!batch->new_rows.columns.empty()) {
      out << "    new" << Render(batch->new_rows, batch->row_count) << "\n";
    }
  }
  return out.str();
}

// ---- test hooks at the end of PRE_COMMIT ---------------------------------------------
// Registered before the writer's own callback (which registers at the first
// capture), so it runs after it: the epochs are already taken.
bool g_block_precommit = false;
bool g_fail_precommit = false;

constexpr uint32 kGateClass = 0x41434745; // shared.cpp's gates
constexpr uint32 kBlockClass = 0x5754424B;

LOCKTAG TestTag(int kind, int index) {
  LOCKTAG tag;
  SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, kind == 0 ? kGateClass : kBlockClass, static_cast<uint32>(index), 3);
  return tag;
}

void TestXactCallback(XactEvent event, void *) {
  if (event != XACT_EVENT_PRE_COMMIT) {
    return;
  }
  if (g_block_precommit) {
    const LOCKTAG tag = TestTag(1, 0);
    (void)LockAcquire(&tag, ExclusiveLock, true, false);
    LockRelease(&tag, ExclusiveLock, true);
  }
  if (g_fail_precommit) {
    ereport(ERROR, (errmsg("wt.fail_precommit")));
  }
}

} // namespace

namespace accelerator_codegen::aux_state::engine::postgres::shard {

void EnsureWorker(int) { g_ensure_calls++; }

bool SendCommit(int, std::string_view message, std::chrono::steady_clock::time_point) {
  g_sent.push_back(Render(DecodeCommit(message)));
  return true;
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

extern "C" {

void _PG_init(void) {
  pg::shared::DefineGucs();
  DefineCustomBoolVariable("wt.block_precommit", "Wait for the wt_lock(1, 0) lock at the end of PRE_COMMIT.", nullptr,
                           &g_block_precommit, false, PGC_USERSET, 0, nullptr, nullptr, nullptr);
  DefineCustomBoolVariable("wt.fail_precommit", "Fail at the end of PRE_COMMIT.", nullptr, &g_fail_precommit, false,
                           PGC_USERSET, 0, nullptr, nullptr, nullptr);
  RegisterXactCallback(TestXactCallback, nullptr);
}

PG_FUNCTION_INFO_V1(wt_activate);
Datum wt_activate(PG_FUNCTION_ARGS) {
  const std::string id = text_to_cstring(PG_GETARG_TEXT_PP(0));
  PG_RETURN_INT32(pg::shared::Activate(id));
}

PG_FUNCTION_INFO_V1(wt_find);
Datum wt_find(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(pg::shared::Find(text_to_cstring(PG_GETARG_TEXT_PP(0))));
}

PG_FUNCTION_INFO_V1(wt_id_of);
Datum wt_id_of(PG_FUNCTION_ARGS) {
  PG_RETURN_TEXT_P(cstring_to_text(pg::shared::IdOf(PG_GETARG_INT32(0)).c_str()));
}

PG_FUNCTION_INFO_V1(wt_generation);
Datum wt_generation(PG_FUNCTION_ARGS) { PG_RETURN_INT64(static_cast<int64>(pg::shared::Generation())); }

PG_FUNCTION_INFO_V1(wt_ensure_calls);
Datum wt_ensure_calls(PG_FUNCTION_ARGS) { PG_RETURN_INT32(g_ensure_calls); }

PG_FUNCTION_INFO_V1(wt_assigned);
Datum wt_assigned(PG_FUNCTION_ARGS) {
  PG_RETURN_INT64(static_cast<int64>(pg::shared::AssignedEpoch(PG_GETARG_INT32(0))));
}

// NULL: the snapshot is older than the ring.
PG_FUNCTION_INFO_V1(wt_reader_epoch);
Datum wt_reader_epoch(PG_FUNCTION_ARGS) {
  const auto epoch = pg::shared::ReaderEpoch(PG_GETARG_INT32(0), GetActiveSnapshot());
  if (!epoch) {
    PG_RETURN_NULL();
  }
  PG_RETURN_INT64(static_cast<int64>(*epoch));
}

PG_FUNCTION_INFO_V1(wt_horizon);
Datum wt_horizon(PG_FUNCTION_ARGS) {
  PG_RETURN_INT64(static_cast<int64>(pg::shared::HorizonEpoch(PG_GETARG_INT32(0))));
}

PG_FUNCTION_INFO_V1(wt_wrote);
Datum wt_wrote(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(pg::shared::TransactionWrote({static_cast<core::TableId>(PG_GETARG_INT64(0))}));
}

// wt_lock(0, i): hold accelerator i's gate; wt_lock(1, 0): the PRE_COMMIT stall.
PG_FUNCTION_INFO_V1(wt_lock);
Datum wt_lock(PG_FUNCTION_ARGS) {
  const LOCKTAG tag = TestTag(PG_GETARG_INT32(0), PG_GETARG_INT32(1));
  (void)LockAcquire(&tag, ExclusiveLock, true, false);
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(wt_unlock);
Datum wt_unlock(PG_FUNCTION_ARGS) {
  const LOCKTAG tag = TestTag(PG_GETARG_INT32(0), PG_GETARG_INT32(1));
  LockRelease(&tag, ExclusiveLock, true);
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(wt_sent);
Datum wt_sent(PG_FUNCTION_ARGS) {
  std::string all;
  for (const auto &commit : g_sent) {
    all += commit;
  }
  PG_RETURN_TEXT_P(cstring_to_text(all.c_str()));
}

PG_FUNCTION_INFO_V1(wt_sent_reset);
Datum wt_sent_reset(PG_FUNCTION_ARGS) {
  g_sent.clear();
  PG_RETURN_VOID();
}

// Thread tokens held by this backend.
std::size_t g_tokens_held = 0;

PG_FUNCTION_INFO_V1(wt_tokens_take);
Datum wt_tokens_take(PG_FUNCTION_ARGS) {
  const auto taken = pg::shared::ThreadTokens()->TryAcquire(static_cast<std::size_t>(PG_GETARG_INT32(0)));
  g_tokens_held += taken;
  PG_RETURN_INT32(static_cast<int32>(taken));
}

PG_FUNCTION_INFO_V1(wt_tokens_release);
Datum wt_tokens_release(PG_FUNCTION_ARGS) {
  pg::shared::ThreadTokens()->Release(g_tokens_held);
  g_tokens_held = 0;
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(wt_threads);
Datum wt_threads(PG_FUNCTION_ARGS) { PG_RETURN_INT32(static_cast<int32>(pg::shared::ThreadBudget())); }

} // extern "C"

#endif
