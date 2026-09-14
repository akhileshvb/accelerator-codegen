// GENERATED BACKEND — txn + mutations + scan + runtime: Postgres MODULE bridge
// Wires RegisterXactCallback to PostgresAuxBackend::OnPreCommit/OnCommit/OnAbort.
// Wires AFTER trigger to PostgresAuxBackend::CaptureMutation for INSERT/UPDATE/DELETE.
// scan-layer UDFs: pg_accel_scan_init / pg_accel_scan_rows
// runtime-layer UDFs: pg_accel_runtime_init / pg_accel_runtime_build / pg_accel_runtime_execute

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <utils/builtins.h>
#include <access/xact.h>
#include <access/htup_details.h>
#include <access/table.h>
#include <catalog/pg_type_d.h>
#include <commands/trigger.h>
#include <executor/spi.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <utils/tuplestore.h>
#include <executor/tuptable.h>
#include <access/tupdesc.h>
#include <executor/executor.h>
PG_MODULE_MAGIC;
}
#include <algorithm>

#include <mutex>
#include <optional>
#include <string>
#include <sstream>
#include <vector>

#include "accelerator_codegen/runtime/postgres/postgres_extension_bridge.hpp"
#include "accelerator_codegen/runtime/postgres/scan_helper.hpp"

namespace accel = accelerator_codegen::aux_state;
namespace core = accel::core;

namespace accelerator_codegen::aux_state::runtime::postgres {

namespace {

struct ToyPayload {
  int count;
  std::string label;
};

class TxnToyModule final : public core::AuxQueryModule {
public:
  core::AcceleratorId Id() const override { return "txn_toy"; }

  std::vector<core::TableDependency> Dependencies() const override {
    core::TableDependency dep;
    dep.table = 1;
    return {dep};
  }

  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<core::BasicAuxTxnBuffer>();
  }

  void Build(core::AuxEngineServices &svc, core::StateBuilder &builder) override {
    (void)svc;
    auto payload = std::make_shared<const ToyPayload>(ToyPayload{1, "built"});
    builder.Write(core::StateImage::Make(Id(), 1, 1, payload));
    builder.Finish();
  }

  core::InvalidationPlan InterpretMutation(const core::MutationBatch &) const override {
    core::InvalidationPlan plan;
    plan.scope = accel::InvalidationScope::PartitionSet;
    plan.partitions.insert(1);
    return plan;
  }

  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override {
  }

  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &,
                   const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override {
    core::StagedMaintenanceOutcome outcome;
    outcome.action = core::MaintenanceAction::ApplyDelta;
    auto payload = std::make_shared<const ToyPayload>(ToyPayload{42, "staged"});
    outcome.updates.push_back(core::StateImage::Make(Id(), 1, 1, payload));
    return outcome;
  }

  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override {
    return true;
  }

  void Execute(core::AuxEngineServices &,
               const core::QueryArguments &,
               core::AuxRowSink &sink) override {
    core::OutputSchema schema;
    schema.columns.push_back({"val", core::AuxType::Int64, false});
    sink.Begin(schema);
    core::AuxRow row;
    row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{42}));
    sink.WriteRow(row);
    sink.End();
  }

  void ExecuteFallback(core::AuxEngineServices &,
                       const core::QueryArguments &,
                       core::AuxRowSink &sink) override {
    core::OutputSchema schema;
    schema.columns.push_back({"val", core::AuxType::Int64, false});
    sink.Begin(schema);
    core::AuxRow row;
    row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{99}));
    sink.WriteRow(row);
    sink.End();
  }
};

std::unique_ptr<PgStore> g_state_store;
std::unique_ptr<core::AuxTransactionCoordinator> g_coordinator;
std::unique_ptr<PgBackend> g_backend;
std::shared_ptr<core::AuxQueryModule> g_toy_module;
core::Version g_current_epoch = 0;
std::mutex g_mutex;

std::vector<core::MutationBatch> g_captured_mutations;
std::mutex g_capture_mutex;

void XactCallback(XactEvent event, void * /*arg*/) {
  if (!g_backend) { return; }
  switch (event) {
  case XACT_EVENT_PRE_COMMIT:
    g_backend->OnPreCommit();
    break;
  case XACT_EVENT_COMMIT: {
    bool had_work = g_coordinator && g_coordinator->HasPendingWork();
    g_backend->OnCommit();
    if (had_work) {
      std::lock_guard<std::mutex> lock(g_mutex);
      ++g_current_epoch;
    }
    break;
  }
  case XACT_EVENT_ABORT:
    g_backend->OnAbort();
    break;
  default:
    break;
  }
}

void DoShutdown() {
  if (g_backend) { UnregisterXactCallback(XactCallback, nullptr); }
  g_toy_module.reset();
  g_backend.reset();
  g_coordinator.reset();
  g_state_store.reset();
  g_current_epoch = 0;
  std::lock_guard<std::mutex> cap_lock(g_capture_mutex);
  g_captured_mutations.clear();
}

void FillMutationRowFromTrigger(core::AuxRow &row, HeapTuple htup,
                                TupleDesc tupdesc, int natts,
                                const std::optional<std::vector<int32_t>> &capture_attnums) {
  bool use_full_row = !capture_attnums.has_value() || capture_attnums->empty();
  for (int i = 0; i < natts; ++i) {
    if (!use_full_row) {
      int32_t phys_attnum = i + 1;
      if (std::find(capture_attnums->begin(), capture_attnums->end(), phys_attnum) == capture_attnums->end()) {
        continue;
      }
    }
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
    if (attr->attisdropped) { continue; }
    bool isnull = false;
    Datum d = heap_getattr(htup, i + 1, tupdesc, &isnull);
    Oid typid = attr->atttypid;
    if (isnull) {
      core::AuxType at = core::AuxType::String;
      if (typid == INT4OID || typid == INT8OID || typid == INT2OID) at = core::AuxType::Int64;
      else if (typid == FLOAT4OID || typid == FLOAT8OID) at = core::AuxType::Double;
      row.values.push_back(core::AuxValue::Null(at));
    } else if (typid == INT4OID) {
      row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{DatumGetInt32(d)}));
    } else if (typid == INT8OID) {
      row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{DatumGetInt64(d)}));
    } else if (typid == INT2OID) {
      row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{DatumGetInt16(d)}));
    } else if (typid == FLOAT4OID) {
      row.values.push_back(core::AuxValue::Of(core::AuxType::Double, double{DatumGetFloat4(d)}));
    } else if (typid == FLOAT8OID) {
      row.values.push_back(core::AuxValue::Of(core::AuxType::Double, double{DatumGetFloat8(d)}));
    } else if (typid == BOOLOID) {
      row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{DatumGetBool(d) ? 1 : 0}));
    } else {
      char *str = TextDatumGetCString(d);
      row.values.push_back(core::AuxValue::Of(core::AuxType::String, std::string(str)));
    }
  }
}

} // anonymous namespace

void InitTxnBridge() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state_store) { return; }
  g_state_store = std::make_unique<PgStore>();
  auto visibility_cb = []() -> core::VisibilityToken {
    std::lock_guard<std::mutex> lock(g_mutex);
    core::VisibilityToken tok;
    tok.mutation_epoch = g_current_epoch;
    return tok;
  };
  g_coordinator = std::make_unique<core::AuxTransactionCoordinator>();
  g_toy_module = std::make_shared<TxnToyModule>();
  g_coordinator->Register(g_toy_module);
  g_backend = std::make_unique<PgBackend>(*g_coordinator, *g_state_store, visibility_cb);
  RegisterXactCallback(XactCallback, nullptr);
}

void ShutdownTxnBridge() {
  std::lock_guard<std::mutex> lock(g_mutex);
  DoShutdown();
}

core::AuxTransactionCoordinator &GetCoordinator() { return *g_coordinator; }
PgStore &GetStateStore() { return *g_state_store; }
PgBackend &GetAuxBackend() { return *g_backend; }

std::string TxnTestCaptureMutation(int64_t table_id, int kind_int,
                                    int64_t count_value, const std::string &label) {
  core::MutationBatch batch;
  batch.table = static_cast<core::TableId>(table_id);
  batch.kind = static_cast<core::MutationKind>(kind_int);
  core::AuxRow row;
  row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, count_value));
  row.values.push_back(core::AuxValue::Of(core::AuxType::String, label));
  batch.new_rows.push_back(row);
  if (g_backend) { g_backend->CaptureMutation(batch); return "ok"; }
  return "not_initialized";
}

std::string TxnTestReadState() {
  if (!g_backend) return "not_initialized";
  auto tok = g_backend->CurrentVisibility();
  auto &store = g_backend->StateStore();
  auto snapshot = store.Read(tok);
  std::ostringstream oss;
  oss << "epoch=" << tok.mutation_epoch << " available=" << (snapshot.available ? 1 : 0)
      << " partitions=" << snapshot.partitions.size();
  for (const auto &img : snapshot.partitions) {
    auto *tp = static_cast<const ToyPayload *>(img.payload.get());
    if (tp) {
      oss << " [" << img.accelerator << " p" << img.partition
          << " v" << img.version << " count=" << tp->count << " label=" << tp->label << "]";
    }
  }
  return oss.str();
}

std::string TxnTestCheckEpoch() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::ostringstream oss;
  oss << "epoch=" << g_current_epoch;
  return oss.str();
}

std::string TxnTestResetAll() {
  std::lock_guard<std::mutex> lock(g_mutex);
  DoShutdown();
  return "ok";
}

// ---- visibility-layer test bridge ----
// Reuses the same global state store, coordinator, and backend as the txn layer.
// Visibility tests use distinct UDF names but go through the same full txn lifecycle.

void InitVisibilityBridge() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_state_store) { return; }
  g_state_store = std::make_unique<PgStore>();
  auto visibility_cb = []() -> core::VisibilityToken {
    std::lock_guard<std::mutex> lock(g_mutex);
    core::VisibilityToken tok;
    tok.mutation_epoch = g_current_epoch;
    return tok;
  };
  g_coordinator = std::make_unique<core::AuxTransactionCoordinator>();
  g_toy_module = std::make_shared<TxnToyModule>();
  g_coordinator->Register(g_toy_module);
  g_backend = std::make_unique<PgBackend>(*g_coordinator, *g_state_store, visibility_cb);
  RegisterXactCallback(XactCallback, nullptr);
}

std::string VisTestCaptureMutation(int64_t table_id, int kind_int,
                                    int64_t count_value, const std::string &label) {
  core::MutationBatch batch;
  batch.table = static_cast<core::TableId>(table_id);
  batch.kind = static_cast<core::MutationKind>(kind_int);
  core::AuxRow row;
  row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, count_value));
  row.values.push_back(core::AuxValue::Of(core::AuxType::String, label));
  batch.new_rows.push_back(row);
  if (g_backend) { g_backend->CaptureMutation(batch); return "ok"; }
  return "not_initialized";
}

std::string VisTestReadState() {
  if (!g_backend) return "not_initialized";
  auto tok = g_backend->CurrentVisibility();
  auto &store = g_backend->StateStore();
  auto snapshot = store.Read(tok);
  std::ostringstream oss;
  oss << "epoch=" << tok.mutation_epoch << " available=" << (snapshot.available ? 1 : 0)
      << " partitions=" << snapshot.partitions.size();
  for (const auto &img : snapshot.partitions) {
    auto *tp = static_cast<const ToyPayload *>(img.payload.get());
    if (tp) {
      oss << " [" << img.accelerator << " p" << img.partition
          << " v" << img.version << " count=" << tp->count << " label=" << tp->label << "]";
    }
  }
  return oss.str();
}

std::string VisTestCheckEpoch() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::ostringstream oss;
  oss << "epoch=" << g_current_epoch;
  return oss.str();
}

std::string VisTestResetAll() {
  std::lock_guard<std::mutex> lock(g_mutex);
  DoShutdown();
  return "ok";
}


void RecordCapturedMutation(const core::MutationBatch &mutation) {
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  g_captured_mutations.push_back(mutation);
}

std::string MutationsTestLastCapture() {
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  if (g_captured_mutations.empty()) return "none";
  const auto &m = g_captured_mutations.back();
  std::ostringstream oss;
  oss << "table=" << m.table << " kind=" << static_cast<int>(m.kind)
      << " new_rows=" << m.new_rows.size() << " old_rows=" << m.old_rows.size();
  for (const auto &row : m.new_rows) {
    for (const auto &v : row.values) {
      oss << " ";
      if (v.is_null) oss << "NULL";
      else if (v.type == core::AuxType::Int64) oss << std::get<int64_t>(v.payload);
      else if (v.type == core::AuxType::String) oss << "\"" << std::get<std::string>(v.payload) << "\"";
      else oss << "?";
    }
  }
  for (const auto &row : m.old_rows) {
    for (const auto &v : row.values) {
      oss << " ";
      if (v.is_null) oss << "NULL";
      else if (v.type == core::AuxType::Int64) oss << std::get<int64_t>(v.payload);
      else if (v.type == core::AuxType::String) oss << "\"" << std::get<std::string>(v.payload) << "\"";
      else oss << "?";
    }
  }
  return oss.str();
}

std::string MutationsTestDrainCaptures() {
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  std::ostringstream oss;
  oss << g_captured_mutations.size();
  g_captured_mutations.clear();
  return oss.str();
}

std::string MutationsTestCaptureCount() {
  std::lock_guard<std::mutex> lock(g_capture_mutex);
  return std::to_string(g_captured_mutations.size());
}

void CaptureTriggerMutation(FunctionCallInfo fcinfo) {
  TriggerData *trigdata = (TriggerData *)fcinfo->context;
  if (!trigdata || !trigdata->tg_relation || !g_backend) return;
  Oid relid = RelationGetRelid(trigdata->tg_relation);
  core::MutationBatch batch;
  batch.table = static_cast<core::TableId>(relid);
  TupleDesc tupdesc = RelationGetDescr(trigdata->tg_relation);
  int natts = tupdesc->natts;
  auto capture_attnums = g_coordinator->MutationCaptureAttnums(batch.table);
  if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event)) {
    batch.kind = core::MutationKind::Insert;
    HeapTuple in_tup = trigdata->tg_newtuple ? trigdata->tg_newtuple : trigdata->tg_trigtuple;
    if (in_tup) {
      core::AuxRow row;
      FillMutationRowFromTrigger(row, in_tup, tupdesc, natts, capture_attnums);
      batch.new_rows.push_back(std::move(row));
    }
  } else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) {
    batch.kind = core::MutationKind::Delete;
    if (trigdata->tg_trigtuple) {
      core::AuxRow old_row;
      FillMutationRowFromTrigger(old_row, trigdata->tg_trigtuple, tupdesc, natts, capture_attnums);
      batch.old_rows.push_back(std::move(old_row));
    }
  } else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event)) {
    batch.kind = core::MutationKind::Update;
    if (trigdata->tg_trigtuple) {
      core::AuxRow old_row;
      FillMutationRowFromTrigger(old_row, trigdata->tg_trigtuple, tupdesc, natts, capture_attnums);
      batch.old_rows.push_back(std::move(old_row));
    }
    if (trigdata->tg_newtuple) {
      core::AuxRow new_row;
      FillMutationRowFromTrigger(new_row, trigdata->tg_newtuple, tupdesc, natts, capture_attnums);
      batch.new_rows.push_back(std::move(new_row));
    }
  }
  g_backend->CaptureMutation(batch);
  RecordCapturedMutation(batch);
}

// ---- scan-layer bridge implementation ----
void InitScanBridge() {}

std::string ScanRows(const std::string &table_name,
                     const std::string &columns_csv,
                     const std::string &filter_col,
                     const std::string &filter_val) {
  PostgresScanSpec spec;
  spec.table_name = table_name;
  spec.filter_column = filter_col;
  spec.filter_op = filter_col.empty() ? "" : "eq";
  spec.filter_value = filter_val;
  if (!columns_csv.empty()) {
    std::istringstream iss(columns_csv);
    std::string col;
    while (std::getline(iss, col, ',')) {
      spec.projected_columns.push_back(col);
    }
  }
  std::ostringstream result;
  bool first_row = true;
  ScanNativeTable(spec, [&](const std::vector<const char *> &col_names,
                            const std::vector<const char *> &col_values,
                            int col_count) {
    if (!first_row) result << '\n';
    first_row = false;
    for (int i = 0; i < col_count; ++i) {
      if (i > 0) result << ',';
      result << col_names[i] << '=';
      if (col_values[i] == nullptr) result << "NULL";
      else result << col_values[i];
    }
  });
  return result.str();
}

// ---- runtime-layer bridge implementation ----

namespace {

struct RuntimePayload {
  int id;
  std::string name;
  bool name_is_null = false;
  int val = 0;
  bool val_is_null = false;
};

struct RuntimeCollectingBuilder : core::StateBuilder {
  std::vector<core::StateImage> images;
  void Write(core::StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  bool finished = false;
};

class RuntimeToyModule final : public core::AuxQueryModule {
public:
  core::AcceleratorId Id() const override { return "runtime_toy"; }

  std::vector<core::TableDependency> Dependencies() const override {
    core::TableDependency dep;
    dep.table = 1;
    return {dep};
  }

  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<core::BasicAuxTxnBuffer>();
  }

  void SetTableName(const std::string &name) { table_name_ = name; }

  void Build(core::AuxEngineServices &svc, core::StateBuilder &builder) override {
    (void)svc;
    pending_build_images_.clear();
    PostgresScanSpec spec;
    spec.table_name = table_name_;
    spec.projected_columns = {"id", "name", "val"};
    ScanNativeTable(spec, [&](const std::vector<const char *> &col_names,
                              const std::vector<const char *> &col_values,
                              int col_count) {
      if (col_count < 3) return;
      RuntimePayload rp;
      for (int i = 0; i < col_count; ++i) {
        std::string cn(col_names[i]);
        if (cn == "id") {
          if (col_values[i]) rp.id = std::stoi(col_values[i]);
        } else if (cn == "name") {
          if (col_values[i]) rp.name = col_values[i];
          else rp.name_is_null = true;
        } else if (cn == "val") {
          if (col_values[i]) rp.val = std::stoi(col_values[i]);
          else rp.val_is_null = true;
        }
      }
      auto payload = std::make_shared<const RuntimePayload>(rp);
      auto image = core::StateImage::Make(Id(), static_cast<core::PartitionId>(rp.id), 1, payload);
      builder.Write(image);
      pending_build_images_.push_back(std::move(image));
    });
    builder.Finish();
  }

  core::InvalidationPlan InterpretMutation(const core::MutationBatch &) const override {
    core::InvalidationPlan plan;
    plan.scope = accel::InvalidationScope::PartitionSet;
    plan.partitions.insert(1);
    return plan;
  }

  void AccumulateMutation(const core::MutationBatch &, core::AuxEngineServices &, core::AuxTxnBuffer &) override {}

  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &, core::AuxTxnBuffer &) override {
    core::StagedMaintenanceOutcome outcome;
    if (!pending_build_images_.empty()) {
      // Return build images as ApplyDelta updates to avoid the sync-rebuild
      // clearing path in PreCommit (Postgres AllowsSynchronousRebuildOnCommit==false).
      // Include AllPartitions invalidation to clear stale slots before publishing.
      outcome.action = core::MaintenanceAction::ApplyDelta;
      outcome.updates = std::move(pending_build_images_);
      pending_build_images_.clear();
      outcome.invalidations = core::AllPartitionsPlan();
    } else {
      outcome.action = core::MaintenanceAction::ApplyDelta;
    }
    return outcome;
  }

  bool CheckValidity(core::AuxEngineServices &, const core::QueryArguments &) const override { return true; }

  void Execute(core::AuxEngineServices &svc, const core::QueryArguments &, core::AuxRowSink &sink) override {
    auto &store = svc.StateStore();
    auto tok = svc.CurrentVisibility();
    auto snapshot = store.Read(tok);
    core::OutputSchema schema;
    schema.columns.push_back({"id", core::AuxType::Int64, false});
    schema.columns.push_back({"name", core::AuxType::String, true});
    schema.columns.push_back({"val", core::AuxType::Int64, false});
    sink.Begin(schema);

    // Sort partitions by id for deterministic output order
    std::vector<const core::StateImage *> sorted;
    for (const auto &img : snapshot.partitions) {
      sorted.push_back(&img);
    }
    std::sort(sorted.begin(), sorted.end(), [](const core::StateImage *a, const core::StateImage *b) {
      auto *ap = static_cast<const RuntimePayload *>(a->payload.get());
      auto *bp = static_cast<const RuntimePayload *>(b->payload.get());
      if (!ap || !bp) return ap != nullptr;
      return ap->id < bp->id;
    });

    for (const auto *img : sorted) {
      auto *rp = static_cast<const RuntimePayload *>(img->payload.get());
      if (!rp) continue;
      core::AuxRow row;
      row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{rp->id}));
      if (rp->name_is_null)
        row.values.push_back(core::AuxValue::Null(core::AuxType::String));
      else
        row.values.push_back(core::AuxValue::Of(core::AuxType::String, rp->name));
      if (rp->val_is_null)
        row.values.push_back(core::AuxValue::Null(core::AuxType::Int64));
      else
        row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, int64_t{rp->val}));
      sink.WriteRow(row);
    }
    sink.End();
  }

  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &, core::AuxRowSink &sink) override {
    core::OutputSchema schema;
    schema.columns.push_back({"id", core::AuxType::Int64, false});
    schema.columns.push_back({"name", core::AuxType::String, true});
    schema.columns.push_back({"val", core::AuxType::Int64, false});
    sink.Begin(schema);
    sink.End();
  }

private:
  std::string table_name_;
  std::vector<core::StateImage> pending_build_images_;
};

std::unique_ptr<PgStore> g_rt_store;
std::unique_ptr<core::AuxTransactionCoordinator> g_rt_coordinator;
std::unique_ptr<PgBackend> g_rt_backend;
std::shared_ptr<RuntimeToyModule> g_rt_module;
core::Version g_rt_epoch = 0;
std::mutex g_rt_mutex;
bool g_rt_initialized = false;

} // anonymous namespace

void InitRuntimeBridge(const std::string &table_name) {
  std::lock_guard<std::mutex> lock(g_rt_mutex);
  if (g_rt_initialized) return;
  g_rt_store = std::make_unique<PgStore>([]() -> core::Version {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    return g_rt_epoch;
  });
  auto visibility_cb = []() -> core::VisibilityToken {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    core::VisibilityToken tok;
    tok.mutation_epoch = g_rt_epoch;
    return tok;
  };
  g_rt_coordinator = std::make_unique<core::AuxTransactionCoordinator>();
  g_rt_module = std::make_shared<RuntimeToyModule>();
  g_rt_module->SetTableName(table_name);
  g_rt_coordinator->Register(g_rt_module);
  g_rt_backend = std::make_unique<PgBackend>(*g_rt_coordinator, *g_rt_store, visibility_cb);
  g_rt_initialized = true;
}

void ShutdownRuntimeBridge() {
  std::lock_guard<std::mutex> lock(g_rt_mutex);
  g_rt_module.reset();
  g_rt_backend.reset();
  g_rt_coordinator.reset();
  g_rt_store.reset();
  g_rt_epoch = 0;
  g_rt_initialized = false;
}

std::string RuntimeBuild() {
  if (!g_rt_backend || !g_rt_module) return "not_initialized";
  // Build to populate pending_build_images_.
  RuntimeCollectingBuilder builder;
  g_rt_module->Build(*g_rt_backend, builder);
  // Inject a synthetic mutation so PreCommit triggers StageMaintenance
  // (which returns the build images via the coordinator publish path).
  core::MutationBatch batch;
  batch.table = 1;
  batch.kind = core::MutationKind::Insert;
  g_rt_backend->CaptureMutation(batch);
  // Bump epoch before Commit so EpochProvider stamps images with
  // the correct epoch during TryPublish.
  {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    ++g_rt_epoch;
  }
  g_rt_backend->OnPreCommit();
  g_rt_backend->OnCommit();
  return "ok";
}
std::string RuntimeExecute() {
  if (!g_rt_backend || !g_rt_module) return "not_initialized";
  struct CollectingSink : core::AuxRowSink {
    std::ostringstream result;
    bool first = true;
    void Begin(const core::OutputSchema &) override {}
    void WriteRow(const core::AuxRow &row) override {
      if (!first) result << '\n';
      first = false;
      for (size_t i = 0; i < row.values.size(); ++i) {
        if (i > 0) result << ',';
        const auto &v = row.values[i];
        if (v.is_null) {
          result << "NULL";
        } else {
          if (v.type == core::AuxType::Int64) result << std::get<int64_t>(v.payload);
          else if (v.type == core::AuxType::String) result << std::get<std::string>(v.payload);
        }
      }
    }
    void End() override {}
  };
  CollectingSink sink;
  core::QueryArguments args;
  g_rt_module->Execute(*g_rt_backend, args, sink);
  if (sink.first) return "empty";
  return sink.result.str();
}

std::string RuntimeReadState() {
  if (!g_rt_backend) return "not_initialized";
  auto tok = g_rt_backend->CurrentVisibility();
  auto &store = g_rt_backend->StateStore();
  auto snapshot = store.Read(tok);
  std::ostringstream oss;
  oss << "available=" << (snapshot.available ? 1 : 0) << " partitions=" << snapshot.partitions.size();
  for (const auto &img : snapshot.partitions) {
    auto *rp = static_cast<const RuntimePayload *>(img.payload.get());
    if (rp) {
      oss << " [" << img.accelerator << " p" << img.partition
          << " v" << img.version << " id=" << rp->id << " name=" << rp->name << " val=" << rp->val << "]";
    }
  }
  return oss.str();
}

} // namespace accelerator_codegen::aux_state::runtime::postgres

extern "C" {

/* ---- txn-layer UDFs ---- */
PG_FUNCTION_INFO_V1(pg_accel_txn_init);
PG_FUNCTION_INFO_V1(pg_accel_txn_capture);
PG_FUNCTION_INFO_V1(pg_accel_txn_read);
PG_FUNCTION_INFO_V1(pg_accel_txn_epoch);
PG_FUNCTION_INFO_V1(pg_accel_txn_reset);

Datum pg_accel_txn_init(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::runtime::postgres::InitTxnBridge();
  PG_RETURN_INT32(1);
}

Datum pg_accel_txn_capture(PG_FUNCTION_ARGS) {
  int64_t table_id = PG_GETARG_INT64(0);
  int32_t kind = PG_GETARG_INT32(1);
  int64_t count_value = PG_GETARG_INT64(2);
  const char *label = text_to_cstring(PG_GETARG_TEXT_PP(3));
  std::string result = accelerator_codegen::aux_state::runtime::postgres::TxnTestCaptureMutation(table_id, kind, count_value, std::string(label));
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_txn_read(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::TxnTestReadState();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_txn_epoch(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::TxnTestCheckEpoch();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_txn_reset(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::TxnTestResetAll();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

/* ---- mutations-layer UDFs ---- */
PG_FUNCTION_INFO_V1(pg_accel_mutations_trigger);
PG_FUNCTION_INFO_V1(pg_accel_mutations_last);
PG_FUNCTION_INFO_V1(pg_accel_mutations_drain);
PG_FUNCTION_INFO_V1(pg_accel_mutations_count);

Datum pg_accel_mutations_trigger(PG_FUNCTION_ARGS) {
  if (!CALLED_AS_TRIGGER(fcinfo))
    ereport(ERROR, (errmsg("pg_accel_mutations_trigger must be called as a trigger")));
  accelerator_codegen::aux_state::runtime::postgres::CaptureTriggerMutation(fcinfo);
  TriggerData *trigdata = (TriggerData *)fcinfo->context;
  if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
    return PointerGetDatum(trigdata->tg_trigtuple);
  return PointerGetDatum(trigdata->tg_newtuple ? trigdata->tg_newtuple : trigdata->tg_trigtuple);
}

Datum pg_accel_mutations_last(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::MutationsTestLastCapture();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_mutations_drain(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::MutationsTestDrainCaptures();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_mutations_count(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::MutationsTestCaptureCount();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

/* ---- scan-layer UDFs ---- */
PG_FUNCTION_INFO_V1(pg_accel_scan_init);
PG_FUNCTION_INFO_V1(pg_accel_scan_rows);

Datum pg_accel_scan_init(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::runtime::postgres::InitScanBridge();
  PG_RETURN_INT32(1);
}

Datum pg_accel_scan_rows(PG_FUNCTION_ARGS) {
  const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
  const char *columns_csv = text_to_cstring(PG_GETARG_TEXT_PP(1));
  const char *filter_col = text_to_cstring(PG_GETARG_TEXT_PP(2));
  const char *filter_val = text_to_cstring(PG_GETARG_TEXT_PP(3));
  std::string result = accelerator_codegen::aux_state::runtime::postgres::ScanRows(std::string(table_name), std::string(columns_csv), std::string(filter_col), std::string(filter_val));
  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  rsinfo->returnMode = SFRM_Materialize;
  MemoryContext per_query_ctx = rsinfo->econtext ? rsinfo->econtext->ecxt_per_query_memory : CurrentMemoryContext;
  MemoryContext oldctx = MemoryContextSwitchTo(per_query_ctx);
  Tuplestorestate *tupstore = tuplestore_begin_heap(true, false, 0);
  TupleDesc tupdesc = CreateTemplateTupleDesc(1);
  TupleDescInitEntry(tupdesc, (AttrNumber)1, "row", TEXTOID, -1, 0);
  if (!result.empty()) {
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
      Datum values[1];
      bool nulls[1] = {false};
      values[0] = CStringGetTextDatum(line.c_str());
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  }
  rsinfo->setResult = tupstore;
  rsinfo->setDesc = tupdesc;
  MemoryContextSwitchTo(oldctx);
  PG_RETURN_DATUM((Datum)0);
}

/* ---- runtime-layer UDFs ---- */
PG_FUNCTION_INFO_V1(pg_accel_runtime_init);
PG_FUNCTION_INFO_V1(pg_accel_runtime_build);
PG_FUNCTION_INFO_V1(pg_accel_runtime_execute);
PG_FUNCTION_INFO_V1(pg_accel_runtime_read);
PG_FUNCTION_INFO_V1(pg_accel_runtime_reset);

Datum pg_accel_runtime_init(PG_FUNCTION_ARGS) {
  const char *table_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
  accelerator_codegen::aux_state::runtime::postgres::InitRuntimeBridge(std::string(table_name));
  PG_RETURN_INT32(1);
}

Datum pg_accel_runtime_build(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::RuntimeBuild();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_runtime_execute(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::RuntimeExecute();
  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  rsinfo->returnMode = SFRM_Materialize;
  MemoryContext per_query_ctx = rsinfo->econtext ? rsinfo->econtext->ecxt_per_query_memory : CurrentMemoryContext;
  MemoryContext oldctx = MemoryContextSwitchTo(per_query_ctx);
  Tuplestorestate *tupstore = tuplestore_begin_heap(true, false, 0);
  TupleDesc tupdesc = CreateTemplateTupleDesc(1);
  TupleDescInitEntry(tupdesc, (AttrNumber)1, "row", TEXTOID, -1, 0);
  if (!result.empty() && result != "empty") {
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
      Datum values[1];
      bool nulls[1] = {false};
      values[0] = CStringGetTextDatum(line.c_str());
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  }
  rsinfo->setResult = tupstore;
  rsinfo->setDesc = tupdesc;
  MemoryContextSwitchTo(oldctx);
  PG_RETURN_DATUM((Datum)0);
}

Datum pg_accel_runtime_read(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::RuntimeReadState();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_runtime_reset(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::runtime::postgres::ShutdownRuntimeBridge();
  PG_RETURN_TEXT_P(cstring_to_text("ok"));
}


/* ---- visibility-layer UDFs ---- */
PG_FUNCTION_INFO_V1(pg_accel_vis_init);
PG_FUNCTION_INFO_V1(pg_accel_vis_capture);
PG_FUNCTION_INFO_V1(pg_accel_vis_read);
PG_FUNCTION_INFO_V1(pg_accel_vis_epoch);
PG_FUNCTION_INFO_V1(pg_accel_vis_reset);

Datum pg_accel_vis_init(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::runtime::postgres::InitVisibilityBridge();
  PG_RETURN_INT32(1);
}

Datum pg_accel_vis_capture(PG_FUNCTION_ARGS) {
  int64_t table_id = PG_GETARG_INT64(0);
  int32_t kind = PG_GETARG_INT32(1);
  int64_t count_value = PG_GETARG_INT64(2);
  const char *label = text_to_cstring(PG_GETARG_TEXT_PP(3));
  std::string result = accelerator_codegen::aux_state::runtime::postgres::VisTestCaptureMutation(table_id, kind, count_value, std::string(label));
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_vis_read(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::VisTestReadState();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_vis_epoch(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::VisTestCheckEpoch();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

Datum pg_accel_vis_reset(PG_FUNCTION_ARGS) {
  std::string result = accelerator_codegen::aux_state::runtime::postgres::VisTestResetAll();
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

extern "C" {
void accel_pg_init_backend(const char *backing_dir);
}

void _PG_init(void) { accel_pg_init_backend(nullptr); }

} // extern "C"
