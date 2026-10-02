// GENERATED BACKEND
//
// Per-backend Runtime (aux store + sync protocol + result sink) shared by
// every query's extension/scan.cpp.  Module only.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/engine_helpers.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

class Builder final : public core::StateBuilder {
public:
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

class PgSink final : public core::AuxRowSink {
public:
  PgSink(Tuplestorestate *tuplestore_p, TupleDesc tuple_desc_p)
      : tuplestore(tuplestore_p), tuple_desc(tuple_desc_p) {}

  void Begin(const core::OutputSchema &) override {}

  void WriteRow(const core::AuxRow &row) override {
    std::array<Datum, 12> values{};
    std::array<bool, 12> nulls{};
    if (row.values.size() > values.size()) {
      ereport(ERROR, (errmsg("auxiliary output row exceeds PostgreSQL schema")));
    }
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (row.values[column].is_null) {
        nulls[column] = true;
        continue;
      }
      values[column] = ToDatum(
          row.values[column], TupleDescAttr(tuple_desc, column)->atttypid);
    }
    tuplestore_putvalues(tuplestore, tuple_desc, values.data(), nulls.data());
  }

  void End() override {}

private:
  static std::string DecimalText(const core::ScaledDecimal &decimal) {
    const bool negative = decimal.value < 0;
    uint64_t magnitude = negative
                            ? static_cast<uint64_t>(-(decimal.value + 1)) + 1
                            : static_cast<uint64_t>(decimal.value);
    std::string digits = std::to_string(magnitude);
    if (decimal.scale > 0) {
      if (digits.size() <= static_cast<std::size_t>(decimal.scale)) {
        digits.insert(0, static_cast<std::size_t>(decimal.scale) + 1 -
                               digits.size(),
                     '0');
      }
      digits.insert(digits.size() - decimal.scale, 1, '.');
    }
    if (negative) {
      digits.insert(digits.begin(), '-');
    }
    return digits;
  }

  static Datum ToDatum(const core::AuxValue &value, Oid type_oid) {
    if (std::holds_alternative<int64_t>(value.payload)) {
      return Int64GetDatum(std::get<int64_t>(value.payload));
    }
    if (std::holds_alternative<double>(value.payload)) {
      const Datum as_float8 =
          Float8GetDatum(std::get<double>(value.payload));
      // Q7 (and similar) emit Double while the SQL function returns numeric.
      // A float8 Datum is not a valid Numeric pointer and SIGSEGVs on output.
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall1(float8_numeric, as_float8);
      }
      return as_float8;
    }
    if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
      const auto text = DecimalText(
          std::get<core::ScaledDecimal>(value.payload));
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall3(
            numeric_in, CStringGetDatum(text.c_str()),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
      }
      return Float8GetDatum(std::stod(text));
    }
    if (std::holds_alternative<core::AuxDate>(value.payload)) {
      return DateADTGetDatum(std::get<core::AuxDate>(value.payload)
                                 .days_since_epoch - kPostgresDateEpochOffset);
    }
    if (std::holds_alternative<std::string>(value.payload)) {
      const auto &text = std::get<std::string>(value.payload);
      // Exact numeric results are emitted as numeric text (pg_numeric).
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall3(
            numeric_in, CStringGetDatum(text.c_str()),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
      }
      return CStringGetTextDatum(text.c_str());
    }
    return (Datum)0;
  }

  Tuplestorestate *tuplestore;
  TupleDesc tuple_desc;
};

namespace auxsync = accelerator_codegen::aux_state::engine::postgres::aux_sync;

// Per-backend Runtime.  The Aux store is process-local; aux_sync decides when
// it may serve a hit for the statement snapshot (cross-backend protocol, see
// aux_sync.hpp):
//
//   Sync marker  (base, own_commits, seq): the store equals the Aux of the
//                database whose dependency-table writers are exactly those
//                committed-visible in `base` plus `own_commits`, and `seq[i]`
//                counts exactly those writers for dependency slot i.
//   Hit(S)       marker synced, store fully valid, this transaction has no
//                captured dependency writes, S covers `base`, S sees every
//                own commit, and (read after S) inflight == 0 and
//                seq == marker.seq on every dependency slot.
//   Rebuild      only in a transaction without an xid (so the build snapshot
//                contains no uncommitted writes of this backend), from a
//                fresh snapshot taken between two counter reads.
//   Own commit   the locally maintained store stays synced iff no other
//                writer was in flight or committed on a dependency slot
//                between PRE_COMMIT (after the maintenance snapshot) and this
//                commit's counter increment.
template <typename Module, typename Config, typename ScanT>
class Runtime final : public accelerator_codegen::aux_state::engine::postgres::
                          aux_sync::Participant {
public:
  // A table/column whose text ordering the verified result depends on.
  struct CollationKey {
    const char *relation;
    const char *column;
  };

  Runtime(const char *cost_model_env, const char *default_config_path,
          core::AcceleratorId accelerator, Config config,
          std::vector<CollationKey> c_collation_keys_p = {})
      : c_collation_keys(std::move(c_collation_keys_p)),
        store([]() {
          return accelerator_codegen::aux_state::engine::postgres::
              AuxMutationEpoch();
        }),
        coordinator(),
        backend(
            coordinator, store,
            [] {
              return accelerator_codegen::aux_state::engine::postgres::
                  PinTokenForActiveSnapshot();
            },
            [cost_model_env, default_config_path] {
              return core::LoadAuxCostModelJson(
                  ResolveCostModelPath(cost_model_env, default_config_path));
            }),
        scan(), module(config, &scan), accelerator(std::move(accelerator)) {
    auto module_view = std::shared_ptr<Module>(&module, [](Module *) {});
    coordinator.Register(std::move(module_view));
    std::vector<core::TableId> tables;
    for (const auto &dependency : module.Dependencies()) {
      tables.push_back(dependency.table);
    }
    dep_slots = auxsync::SlotsOf(tables);
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresMutationBackend(&backend);
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresXactCallbacks();
    auxsync::RegisterParticipant(this);
  }

  void Execute(const core::QueryArguments &arguments, FunctionCallInfo fcinfo) {
    // Every read in this statement: one aux state version.
    accelerator_codegen::aux_state::AuxReadScope one_version;
    // TokenForSnapshot: freeze (S, epoch) at the earliest UDF entry.
    accelerator_codegen::aux_state::engine::postgres::ForcePinActiveSnapshot();

    RequireCCollation();
    RequireMaterialize(fcinfo);
    // Prefer this Runtime for subsequent trigger captures in this session.
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresMutationBackend(&backend);

    Snapshot statement = GetActiveSnapshot();
    bool hit = HitAllowed(statement);
    bool rebuilt = false;
    if (!hit && NeedsSync() && CanRebuildNow()) {
      Rebuild();
      hit = HitAllowed(statement);
      rebuilt = hit;
    }
    observed_mutations = accel_pg_get_observer_mutation_count();
    auto *return_set_info =
        reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    PgSink sink(return_set_info->setResult, return_set_info->setDesc);
    elog(DEBUG1, "accelerator %s: %s", accelerator.c_str(),
         hit ? "aux hit" : "fallback");
    t_table_scans = 0;
    if (hit) {
      module.Execute(backend, arguments, sink);
    } else {
      module.ExecuteFallback(backend, arguments, sink);
    }
    core::RecordAuxExecute(!hit || t_table_scans > 0
                               ? core::AuxExecutePath::Fallback
                           : rebuilt ? core::AuxExecutePath::RebuiltOnRead
                                     : core::AuxExecutePath::Hit);
  }

  void BeforePreCommit() override {
    precommit_ok = false;
    if (!auxsync::OwnRegisteredAnyOf(dep_slots) || !marker.synced) {
      return;
    }
    std::vector<uint64_t> seq;
    precommit_ok = auxsync::ReadCounters(dep_slots, /*allow_own=*/true, seq) &&
                   seq == marker.seq;
  }

  void AfterCommit(const auxsync::CommitInfo &info) override {
    if (info.maintenance_failed) {
      marker.synced = false;
    }
    if (auxsync::OwnRegisteredAnyOf(dep_slots) &&
        TransactionIdIsValid(info.xid)) {
      bool ok = marker.synced && precommit_ok;
      for (std::size_t i = 0; ok && i < dep_slots.size(); ++i) {
        const auto slot = dep_slots[i];
        if (!info.wrote[slot]) {
          continue;
        }
        if (info.old_seq[slot] != marker.seq[i]) {
          ok = false;
        } else {
          marker.seq[i] += 1;
        }
      }
      if (ok) {
        if (marker.own_commits.size() >= kMaxOwnCommits) {
          ok = false;
        } else {
          marker.own_commits.push_back(info.xid);
        }
      }
      marker.synced = ok;
    }
    if (store.NeedsRebuild() || auxsync::OwnTruncatedAnyOf(dep_slots)) {
      marker.synced = false;
    }
    precommit_ok = false;
  }

private:
  static constexpr std::size_t kMaxOwnCommits = 1024;
  struct SyncMarker {
    bool synced = false;
    auxsync::SnapMark base;
    std::vector<TransactionId> own_commits;
    std::vector<uint64_t> seq;
  };

  bool CountersMatch() const {
    std::vector<uint64_t> seq;
    return auxsync::ReadCounters(dep_slots, /*allow_own=*/false, seq) &&
           seq == marker.seq;
  }

  bool NeedsSync() const {
    return !built || !marker.synced || store.NeedsRebuild() || !CountersMatch();
  }

  bool HitAllowed(Snapshot statement) const {
    if (!built || !marker.synced || store.NeedsRebuild() ||
        store.IsTransactionDirty() || auxsync::OwnRegisteredAnyOf(dep_slots)) {
      return false;
    }
    if (!auxsync::Covers(statement, marker.base)) {
      return false;
    }
    for (auto xid : marker.own_commits) {
      if (!auxsync::Sees(statement, xid)) {
        return false;
      }
    }
    // Read after the statement snapshot was taken.
    return CountersMatch();
  }

  bool CanRebuildNow() const {
    return !TransactionIdIsValid(GetTopTransactionIdIfAny()) &&
           !auxsync::OwnRegisteredAny() && !store.IsTransactionDirty() &&
           !IsInParallelMode() && !RecoveryInProgress();
  }

  void Rebuild() {
    marker = SyncMarker{};
    std::vector<uint64_t> before;
    if (!auxsync::ReadCounters(dep_slots, /*allow_own=*/false, before)) {
      return; // a writer is in flight: the result could not be certified
    }
    Snapshot fresh = GetLatestSnapshot();
    auxsync::SnapMark base = auxsync::MarkOf(fresh);
    PushActiveSnapshot(fresh);
    Builder builder;
    try {
      module.Build(backend, builder);
    } catch (...) {
      PopActiveSnapshot();
      throw;
    }
    PopActiveSnapshot();
    if (!builder.finished) {
      throw std::runtime_error("auxiliary build did not finish");
    }
    store.Install(std::move(builder.images), accelerator);
    built = true;
    std::vector<uint64_t> after;
    if (base.ok && auxsync::ReadCounters(dep_slots, /*allow_own=*/false, after) &&
        after == before) {
      marker.synced = true;
      marker.base = std::move(base);
      marker.seq = std::move(before);
    }
    elog(DEBUG1, "accelerator %s: rebuild (%s)", accelerator.c_str(),
         marker.synced ? "synced" : "unsynced");
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresBackendForMutations();
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
    auto *return_set_info =
        reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    if (!return_set_info ||
        !(return_set_info->allowedModes & SFRM_Materialize)) {
      ereport(ERROR,
              (errmsg("accelerator auxiliary functions require materialize mode")));
    }
    InitMaterializedSRF(fcinfo, 0);
  }

  std::vector<CollationKey> c_collation_keys;
  PostgresStateStore store;
  core::AuxTransactionCoordinator coordinator;
  accelerator_codegen::aux_state::engine::postgres::PostgresAuxBackend backend;
  ScanT scan;
  Module module;
  std::string accelerator;
  std::vector<std::size_t> dep_slots;
  SyncMarker marker;
  bool built = false;
  bool precommit_ok = false;
  int observed_mutations = 0;
};

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
