// GENERATED BACKEND - engine_backend `host` layer gate-test UDFs for Postgres.
// A toy host registry (RegisterHostAuxModule / Find / Execute / Build) over
// the test bridge (extension_test_bridge.hpp) and the pg_accel_host_* UDFs
// that drive it through SQL. Built only into the test MODULE
// accelerator_codegen_pg_test; no production accelerator registers here.

#include <algorithm>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <variant>
#include <vector>

#include "backends/postgres/tests/extension_test_bridge.hpp"
#include "backends/postgres/state/postgres_aux_backend.hpp"
#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_types.hpp"

namespace accel = accelerator_codegen::aux_state;
namespace core = accel::core;
namespace pg_rt = accel::runtime::postgres;

namespace {

struct HostModuleEntry {
  std::string key;
  std::shared_ptr<core::AuxQueryModule> module;
};

std::vector<HostModuleEntry> g_host_modules;
std::mutex g_host_module_mutex;

void EnsureBridgeInitialized() {
  static std::once_flag once;
  std::call_once(once, [] {
    pg_rt::InitTxnBridge();
  });
}

} // namespace

namespace accelerator_codegen::host::postgres {

void RegisterHostAuxModule(const std::string &workload, int query_id,
                           std::shared_ptr<core::AuxQueryModule> module) {
  std::lock_guard<std::mutex> lock(g_host_module_mutex);
  std::string key = workload + "_q" + std::to_string(query_id);
  g_host_modules.push_back(HostModuleEntry{key, std::move(module)});
}

core::AuxQueryModule *FindHostAuxModule(const std::string &workload, int query_id) {
  std::string key = workload + "_q" + std::to_string(query_id);
  for (auto &entry : g_host_modules) {
    if (entry.key == key) return entry.module.get();
  }
  return nullptr;
}

std::vector<std::string> ListHostAuxModules() {
  std::vector<std::string> keys;
  for (const auto &entry : g_host_modules) keys.push_back(entry.key);
  return keys;
}

bool ExecuteHostAuxModule(const std::string &workload, int query_id,
                          std::vector<core::AuxRow> &rows_out) {
  EnsureBridgeInitialized();
  auto *mod = FindHostAuxModule(workload, query_id);
  if (!mod) return false;

  auto &svc = pg_rt::GetAuxBackend();
  core::QueryArguments args;

  struct CollectSink : core::AuxRowSink {
    std::vector<core::AuxRow> &rows;
    CollectSink(std::vector<core::AuxRow> &r) : rows(r) {}
    void Begin(const core::OutputSchema &) override {}
    void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
    void End() override {}
  } sink(rows_out);

  mod->Execute(svc, args, sink);
  return true;
}

bool ExecuteHostAuxFallback(const std::string &workload, int query_id,
                            std::vector<core::AuxRow> &rows_out) {
  auto *mod = FindHostAuxModule(workload, query_id);
  if (!mod) return false;

  auto &svc = pg_rt::GetAuxBackend();
  core::QueryArguments args;

  struct CollectSink : core::AuxRowSink {
    std::vector<core::AuxRow> &rows;
    CollectSink(std::vector<core::AuxRow> &r) : rows(r) {}
    void Begin(const core::OutputSchema &) override {}
    void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
    void End() override {}
  } sink(rows_out);

  mod->ExecuteFallback(svc, args, sink);
  return true;
}

bool BuildHostAuxModule(const std::string &workload, int query_id) {
  EnsureBridgeInitialized();
  auto *mod = FindHostAuxModule(workload, query_id);
  if (!mod) return false;

  auto &svc = pg_rt::GetAuxBackend();
  auto &store = pg_rt::GetStateStore();

  class HostBuilder : public core::StateBuilder {
  public:
    core::AuxStateStore &store;
    std::vector<core::StateImage> images;
    HostBuilder(core::AuxStateStore &s) : store(s) {}
    void Write(core::StateImage image) override {
      images.push_back(std::move(image));
    }
    void Finish() override {
      core::StagedMaintenanceOutcome outcome;
      outcome.action = core::MaintenanceAction::ApplyDelta;
      outcome.updates = std::move(images);
      store.TryPublish(outcome, 0);
    }
  } builder(store);

  mod->Build(svc, builder);
  return true;
}

} // namespace accelerator_codegen::host::postgres


// ---- AuxValue serialization helper ----

namespace {

std::string AuxValueToText(const core::AuxValue &value) {
  if (value.is_null) return "NULL";
  return std::visit(
      [](const auto &payload) -> std::string {
        using T = std::decay_t<decltype(payload)>;
        std::ostringstream ss;
        if constexpr (std::is_same_v<T, bool>) {
          ss << (payload ? "true" : "false");
        } else if constexpr (std::is_same_v<T, int64_t>) {
          ss << payload;
        } else if constexpr (std::is_same_v<T, uint64_t>) {
          ss << payload;
        } else if constexpr (std::is_same_v<T, double>) {
          ss << std::fixed << std::setprecision(6) << payload;
        } else if constexpr (std::is_same_v<T, std::string>) {
          ss << payload;
        } else if constexpr (std::is_same_v<T, core::ScaledDecimal>) {
          ss << payload.value << "." << payload.scale;
        } else if constexpr (std::is_same_v<T, core::AuxDate>) {
          ss << "days=" << payload.days_since_epoch;
        }
        return ss.str();
      },
      value.payload);
}

} // namespace

// PG_MODULE_MAGIC comes from backends/postgres/engine/extension_engine.cpp,
// which the test MODULE also links.

extern "C" {

#include <postgres.h>
#include <fmgr.h>
#include <utils/builtins.h>
#include <catalog/pg_type_d.h>
#include <executor/spi.h>
#include <utils/lsyscache.h>
#include <utils/tuplestore.h>
#include <executor/tuptable.h>
#include <access/tupdesc.h>
#include <executor/executor.h>

PG_FUNCTION_INFO_V1(pg_accel_host_list);
PG_FUNCTION_INFO_V1(pg_accel_host_execute);
PG_FUNCTION_INFO_V1(pg_accel_host_fallback);
PG_FUNCTION_INFO_V1(pg_accel_host_build);
PG_FUNCTION_INFO_V1(pg_accel_host_register_toy);

Datum pg_accel_host_list(PG_FUNCTION_ARGS) {
  auto keys = accelerator_codegen::host::postgres::ListHostAuxModules();

  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  rsinfo->returnMode = SFRM_Materialize;
  MemoryContext per_query_ctx = rsinfo->econtext ? rsinfo->econtext->ecxt_per_query_memory : CurrentMemoryContext;
  MemoryContext oldctx = MemoryContextSwitchTo(per_query_ctx);
  Tuplestorestate *tupstore = tuplestore_begin_heap(true, false, 0);
  TupleDesc tupdesc = CreateTemplateTupleDesc(1);
  TupleDescInitEntry(tupdesc, (AttrNumber)1, "module", TEXTOID, -1, 0);

  for (const auto &key : keys) {
    Datum values[1];
    bool nulls[1] = {false};
    values[0] = CStringGetTextDatum(key.c_str());
    tuplestore_putvalues(tupstore, tupdesc, values, nulls);
  }

  rsinfo->setResult = tupstore;
  rsinfo->setDesc = tupdesc;
  MemoryContextSwitchTo(oldctx);
  PG_RETURN_DATUM((Datum)0);
}

Datum pg_accel_host_execute(PG_FUNCTION_ARGS) {
  const char *workload_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
  int32_t query_id = PG_GETARG_INT32(1);

  std::vector<core::AuxRow> rows;
  std::string workload(workload_str);
  bool ok = accelerator_codegen::host::postgres::ExecuteHostAuxModule(workload, query_id, rows);

  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  rsinfo->returnMode = SFRM_Materialize;
  MemoryContext per_query_ctx = rsinfo->econtext ? rsinfo->econtext->ecxt_per_query_memory : CurrentMemoryContext;
  MemoryContext oldctx = MemoryContextSwitchTo(per_query_ctx);
  Tuplestorestate *tupstore = tuplestore_begin_heap(true, false, 0);
  TupleDesc tupdesc = CreateTemplateTupleDesc(1);
  TupleDescInitEntry(tupdesc, (AttrNumber)1, "row", TEXTOID, -1, 0);

  if (ok) {
    for (const auto &row : rows) {
      std::ostringstream ss;
      for (size_t i = 0; i < row.values.size(); ++i) {
        if (i > 0) ss << "|";
        ss << AuxValueToText(row.values[i]);
      }
      std::string line = ss.str();
      Datum values[1];
      bool nulls[1] = {false};
      values[0] = CStringGetTextDatum(line.c_str());
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } else {
    Datum values[1];
    bool nulls[1] = {false};
    std::string err = "module not found: " + workload + "_q" + std::to_string(query_id);
    values[0] = CStringGetTextDatum(err.c_str());
    tuplestore_putvalues(tupstore, tupdesc, values, nulls);
  }

  rsinfo->setResult = tupstore;
  rsinfo->setDesc = tupdesc;
  MemoryContextSwitchTo(oldctx);
  PG_RETURN_DATUM((Datum)0);
}

Datum pg_accel_host_fallback(PG_FUNCTION_ARGS) {
  const char *workload_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
  int32_t query_id = PG_GETARG_INT32(1);

  std::vector<core::AuxRow> rows;
  std::string workload(workload_str);
  bool ok = accelerator_codegen::host::postgres::ExecuteHostAuxFallback(workload, query_id, rows);

  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  rsinfo->returnMode = SFRM_Materialize;
  MemoryContext per_query_ctx = rsinfo->econtext ? rsinfo->econtext->ecxt_per_query_memory : CurrentMemoryContext;
  MemoryContext oldctx = MemoryContextSwitchTo(per_query_ctx);
  Tuplestorestate *tupstore = tuplestore_begin_heap(true, false, 0);
  TupleDesc tupdesc = CreateTemplateTupleDesc(1);
  TupleDescInitEntry(tupdesc, (AttrNumber)1, "row", TEXTOID, -1, 0);

  if (ok) {
    for (const auto &row : rows) {
      std::ostringstream ss;
      for (size_t i = 0; i < row.values.size(); ++i) {
        if (i > 0) ss << "|";
        ss << AuxValueToText(row.values[i]);
      }
      std::string line = ss.str();
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

Datum pg_accel_host_build(PG_FUNCTION_ARGS) {
  const char *workload_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
  int32_t query_id = PG_GETARG_INT32(1);

  std::string workload(workload_str);
  bool ok = accelerator_codegen::host::postgres::BuildHostAuxModule(workload, query_id);
  PG_RETURN_TEXT_P(cstring_to_text(ok ? "built" : "not_found"));
}

Datum pg_accel_host_register_toy(PG_FUNCTION_ARGS) {
  accel::runtime::postgres::InitTxnBridge();
  PG_RETURN_TEXT_P(cstring_to_text("registered"));
}

} // extern "C"
