// GENERATED BACKEND
//
// The per-query accelerator every query's extension/scan.cpp instantiates: its
// module with the SPI scans bound (PostgresAccelerator, accelerators.hpp), and
// the reader side of a query.
//
//   Execute (a backend)  answers from the accelerator's shard worker at the
//                        statement snapshot's epoch (shard_worker.hpp), or from
//                        SQL (the module's ExecuteFallback, here in the
//                        backend) when the worker cannot serve it: this
//                        transaction wrote the accelerator's tables, the
//                        snapshot is older than the epoch ring remembers, or
//                        the worker has no state for the epoch in time.
//   Module (the worker)  the module whose scans run on the worker's SPI
//                        connection: builds and maintenance.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <memory>
#include <optional>
#include <vector>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/api/engine_helpers.hpp"
#include "backends/postgres/api/pg_sink.hpp"
#include "backends/postgres/state/shard_worker.hpp"
#include "backends/postgres/state/shared.hpp"
#include "substrate/aux/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

// What a module's ExecuteFallback sees: it reads SQL, not state.
class SqlServices final : public core::AuxEngineServices {
public:
  SqlServices() : store(VersionedAuxStore::Options{}) {}
  core::VisibilityToken CurrentVisibility() const override { return {}; }
  core::AuxStateStore &StateStore() override { return store; }

private:
  VersionedAuxStore store;
};

template <typename ModuleT, typename Config, typename ScanT>
class Runtime final : public PostgresAccelerator {
public:
  // A table/column whose text ordering the verified result depends on.
  struct CollationKey {
    const char *relation;
    const char *column;
  };

  Runtime(const char *cost_model_env, core::AuxCostModelLocation cost_model_location,
          core::AcceleratorId accelerator_p, Config config,
          std::vector<CollationKey> c_collation_keys_p = {}, bool tune_build = false)
      : cost_model_env(cost_model_env), cost_model_location(std::move(cost_model_location)),
        accelerator(std::move(accelerator_p)), c_collation_keys(std::move(c_collation_keys_p)),
        tune_build(tune_build), scan(), module(config, &scan) {
    for (const auto &dependency : module.Dependencies()) {
      tables.push_back(dependency.table);
    }
  }

  void Execute(const core::QueryArguments &arguments, FunctionCallInfo fcinfo) {
    RequireCCollation();
    RequireMaterialize(fcinfo);
    if (ServeFromWorker(arguments, fcinfo)) {
      core::RecordAuxExecute(core::AuxExecutePath::Hit);
      return;
    }
    auto *return_set_info = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    PgSink sink(return_set_info->setResult, return_set_info->setDesc);
    RunOrReport([&] { module.ExecuteFallback(sql, arguments, sink); });
    core::RecordAuxExecute(core::AuxExecutePath::Fallback);
  }

  // ---- PostgresAccelerator ----
  const core::AcceleratorId &Id() const override { return accelerator; }
  std::vector<core::TableDependency> Dependencies() const override {
    return module.Dependencies();
  }
  std::shared_ptr<core::AuxQueryModule> Module() override {
    return std::shared_ptr<core::AuxQueryModule>(&module, [](core::AuxQueryModule *) {});
  }
  bool TuneBuild() const override { return tune_build; }
  const maintenance::CostModelParams &CostModel() override {
    if (!cost_model) {
      cost_model = core::LoadAuxCostModelJson(ResolveCostModelPath(cost_model_env, cost_model_location));
    }
    return *cost_model;
  }

private:
  // The worker's answer at the statement snapshot's epoch; false: answer from
  // SQL.
  bool ServeFromWorker(const core::QueryArguments &arguments, FunctionCallInfo fcinfo) {
    // A transaction's own writes to the tables are in no worker's state.
    if (shared::TransactionWrote(tables)) {
      return false;
    }
    const int index = shared::Activate(accelerator);
    if (index < 0) {
      return false;
    }
    const auto epoch = shared::ReaderEpoch(index, GetActiveSnapshot());
    return epoch && shard::RemoteRead(index, *epoch, arguments, fcinfo) == shard::Remote::Served;
  }

  // Verified domain: the reference semantics orders text byte-wise
  // (TextCLeb), and so do Execute and ExecuteFallback.  PostgreSQL compares
  // text with memcmp exactly when lc_collate_is_c(collation) holds, so every
  // configured ORDER BY key column must have such a collation; otherwise fail
  // closed rather than return an ordering that may differ from PostgreSQL.
  void RequireCCollation() const {
    for (const auto &key : c_collation_keys) {
      const Oid relid = RelnameGetRelid(key.relation);
      if (!OidIsValid(relid)) {
        ereport(ERROR, (errmsg("accelerator %s: relation \"%s\" not found",
                               accelerator.c_str(), key.relation)));
      }
      const AttrNumber attnum = get_attnum(relid, key.column);
      if (attnum == InvalidAttrNumber) {
        ereport(ERROR, (errmsg("accelerator %s: column %s.%s not found",
                               accelerator.c_str(), key.relation, key.column)));
      }
      Oid type_oid = InvalidOid;
      int32 typmod = -1;
      Oid collation = InvalidOid;
      get_atttypetypmodcoll(relid, attnum, &type_oid, &typmod, &collation);
      if (!OidIsValid(collation) || !lc_collate_is_c(collation)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("accelerator %s requires C collation on %s.%s",
                        accelerator.c_str(), key.relation, key.column),
                 errdetail("Its verified result orders text byte-wise; under "
                           "this column's collation PostgreSQL may order "
                           "differently."),
                 errhint("ALTER TABLE %s ALTER COLUMN %s TYPE character "
                         "varying COLLATE \"C\"",
                         key.relation, key.column)));
      }
    }
  }

  void RequireMaterialize(FunctionCallInfo fcinfo) {
    auto *return_set_info = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    if (!return_set_info || !(return_set_info->allowedModes & SFRM_Materialize)) {
      ereport(ERROR, (errmsg("accelerator auxiliary functions require materialize mode")));
    }
    InitMaterializedSRF(fcinfo, 0);
  }

  const char *cost_model_env;
  core::AuxCostModelLocation cost_model_location;
  std::optional<maintenance::CostModelParams> cost_model;
  core::AcceleratorId accelerator;
  std::vector<CollationKey> c_collation_keys;
  bool tune_build;
  std::vector<core::TableId> tables;
  SqlServices sql;
  ScanT scan;
  ModuleT module;
};

// Registers a query's accelerator by id (see accelerators.hpp), from its
// scan.cpp:  const RuntimeRegistrar<&Q1Runtime> kRegistrar(<accelerator id>);
template <auto Get>
struct RuntimeRegistrar {
  explicit RuntimeRegistrar(const char *id) {
    RegisterPostgresAccelerator(id, [] () -> PostgresAccelerator & { return Get(); });
  }
};

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
