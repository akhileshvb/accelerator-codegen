// GENERATED BACKEND
//
// Per-backend Runtime (aux store + sync protocol + result sink) shared by
// every query's extension/scan.cpp.  Module only.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <chrono>
#include <unordered_set>

#include "backends/postgres/api/engine_helpers.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "backends/postgres/state/aux_server.hpp"

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

// Planner settings for the one-off state build (restored afterwards, also on
// error: the GUC nest level unwinds with the transaction).  The build queries
// join and aggregate whole tables (tens of millions of rows) whose join
// cardinalities PostgreSQL badly underestimates; with the session defaults it
// picks nested loops over millions of outer rows and spilling hash
// aggregates.  Hash joins/aggregates in memory are several times faster, and
// JIT compilation buys nothing for a single scan.  SET accel.build_tuning =
// off keeps the session settings.
class BuildGucScope {
public:
  explicit BuildGucScope(bool wanted) {
    if (!wanted || !aux_server::BuildTuning()) {
      return;
    }
    nest = NewGUCNestLevel();
    Set("work_mem", "1GB");
    Set("enable_nestloop", "off");
    Set("enable_mergejoin", "off");
    Set("jit", "off");
  }
  ~BuildGucScope() { Restore(); }
  void Restore() {
    if (nest > 0) {
      AtEOXact_GUC(true, nest);
      nest = 0;
    }
  }

private:
  static void Set(const char *name, const char *value) {
    (void)set_config_option(name, value, PGC_USERSET, PGC_S_SESSION,
                            GUC_ACTION_SAVE, true, 0, false);
  }
  int nest = 0;
};


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
                          aux_sync::Participant,
                      public accelerator_codegen::aux_state::engine::postgres::
                          aux_server::ServerRuntime {
public:
  // A table/column whose text ordering the verified result depends on.
  struct CollationKey {
    const char *relation;
    const char *column;
  };

  Runtime(const char *cost_model_env, core::AuxCostModelLocation cost_model,
          core::AcceleratorId accelerator, Config config,
          std::vector<CollationKey> c_collation_keys_p = {},
          bool tune_build_p = false)
      : c_collation_keys(std::move(c_collation_keys_p)), tune_build(tune_build_p),
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
            [cost_model_env, cost_model] {
              return core::LoadAuxCostModelJson(
                  ResolveCostModelPath(cost_model_env, cost_model));
            }),
        scan(), module(config, &scan), accelerator(std::move(accelerator)) {
    auto module_view = std::shared_ptr<Module>(&module, [](Module *) {});
    coordinator.Register(std::move(module_view));
    std::vector<core::TableId> tables;
    for (const auto &dependency : module.Dependencies()) {
      tables.push_back(dependency.table);
    }
    dep_slots = auxsync::SlotsOf(tables);
    namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
    if (srv::InServer()) {
      // The state server hosts this runtime: it is maintained from shipped
      // commits, not from this process's transactions.
      srv::RegisterServerRuntime(this);
      return;
    }
    if (!srv::SharedEnabled()) {
      accelerator_codegen::aux_state::engine::postgres::
          RegisterPostgresMutationBackend(&backend);
    }
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresXactCallbacks();
    auxsync::RegisterParticipant(this);
  }

  void Execute(const core::QueryArguments &arguments, FunctionCallInfo fcinfo) {
    namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
    // Every read in this statement: one aux state version.
    accelerator_codegen::aux_state::AuxReadScope one_version;
    // TokenForSnapshot: freeze (S, epoch) at the earliest UDF entry.
    accelerator_codegen::aux_state::engine::postgres::PinTokenForActiveSnapshot();

    RequireCCollation();
    RequireMaterialize(fcinfo);
    if (srv::InServer()) {
      ServerExecute(arguments, fcinfo);
      return;
    }
    if (srv::SharedEnabled()) {
      SharedExecute(arguments, fcinfo);
      return;
    }
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
    if (hit && !marker.own_commits.empty()) {
      // S covers base, sees every own commit, and (counters, read after S)
      // no other dependency writer committed or is in flight: the writers
      // visible in S are exactly base + own commits, so S is the new base.
      auxsync::SnapMark rebased = auxsync::MarkOf(statement);
      if (rebased.ok) {
        marker.base = std::move(rebased);
        marker.own_commits.clear();
      }
    }
    if (aux_server::LogDecisions()) {
      elog(LOG, "DBG %s hit=%d rebuilt=%d synced=%d built=%d S[xmin=%u xmax=%u xcnt=%u sub=%d] base[ok=%d xmax=%u run=%zu] own=%zu seq_ok=%d",
           accelerator.c_str(), (int)hit, (int)rebuilt, (int)marker.synced, (int)built,
           statement->xmin, statement->xmax, statement->xcnt, (int)statement->subxcnt,
           (int)marker.base.ok, marker.base.xmax, marker.base.running.size(),
           marker.own_commits.size(), (int)CountersMatch());
    }
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

  // Shared state: ask the per-database worker, which holds the one copy.  The
  // worker declines (or is unreachable) whenever it cannot prove the state
  // equals the database as this statement's snapshot sees it; then the
  // answer comes from SQL here, as in per-backend mode.
  void SharedExecute(const core::QueryArguments &arguments,
                     FunctionCallInfo fcinfo) {
    namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
    // A transaction's own writes to a dependency are not in anyone's state.
    if (!auxsync::OwnRegisteredAnyOf(dep_slots)) {
      core::AuxExecutePath path = core::AuxExecutePath::Fallback;
      if (srv::RemoteExecute(fcinfo, accelerator, path) == srv::Remote::Served) {
        core::RecordAuxExecute(path);
        return;
      }
    }
    auto *return_set_info =
        reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    PgSink sink(return_set_info->setResult, return_set_info->setDesc);
    t_table_scans = 0;
    module.ExecuteFallback(backend, arguments, sink);
    core::RecordAuxExecute(core::AuxExecutePath::Fallback);
  }

  // In the worker: the same hit / rebuild decision, but a statement that
  // cannot be served is declined (the caller answers from SQL) instead of
  // being answered by a fallback here.
  void ServerExecute(const core::QueryArguments &arguments,
                     FunctionCallInfo fcinfo) {
    namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
    srv::NoteCurrentRuntime(this);
    Snapshot statement = GetActiveSnapshot();
    bool hit = HitAllowed(statement);
    bool rebuilt = false;
    if (!hit && NeedsSync() && CanRebuildNow() && RebuildWarranted()) {
      Rebuild();
      hit = HitAllowed(statement);
      rebuilt = hit;
    }
    if (hit) {
      behind_since_ms = 0;
    }
    if (!hit) {
      srv::NoteDeclined();
      return;
    }
    auto *return_set_info =
        reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    PgSink sink(return_set_info->setResult, return_set_info->setDesc);
    t_table_scans = 0;
    module.Execute(backend, arguments, sink);
    srv::NoteServed(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                    : rebuilt         ? core::AuxExecutePath::RebuiltOnRead
                                      : core::AuxExecutePath::Hit);
  }

  // ---- ServerRuntime ----
  const core::AcceleratorId &ServerKey() const override { return accelerator; }
  std::vector<core::TableDependency> ServerDependencies() const override {
    return const_cast<Module &>(module).Dependencies();
  }
  void ServerInvalidate() override {
    marker.synced = false;
    marker.own_commits.clear();
  }
  // One committed transaction, shipped by its backend (see aux_server.hpp).
  // Folds it into the state only if the state was synced right before it, the
  // capture saw every write the commit made to this accelerator's tables, and
  // the counters show nothing else slipped in; otherwise the marker is
  // dropped and the state is rebuilt by the next query.
  void ServerApply(const aux_server::ApplyInfo &info,
                   const aux_server::ApplyEntry *entry) override {
    namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
    bool touched = false;
    for (auto slot : dep_slots)
      touched = touched || info.wrote[slot];
    if (!touched || !built || !marker.synced) {
      return;
    }
    auto fail = [&] {
      marker.synced = false;
      marker.own_commits.clear();
      srv::NoteApplyOutcome(false);
    };
    auto fail_because = [&](const char *why) {
      elog(LOG, "accel apply %s: not applied (%s): the next query rebuilds",
           accelerator.c_str(), why);
      fail();
    };
    if (!entry) {
      return fail_because("the commit shipped no entry for it");
    }
    for (auto slot : dep_slots) {
      if (info.wrote[slot] &&
          std::find(entry->examined_slots.begin(), entry->examined_slots.end(),
                    slot) == entry->examined_slots.end()) {
        return fail_because("a written table was not examined");
      }
    }
    std::vector<uint64_t> seq;
    auxsync::ReadSeq(dep_slots, seq);
    for (std::size_t i = 0; i < dep_slots.size(); ++i) {
      const auto slot = dep_slots[i];
      const uint64_t add = info.wrote[slot] ? 1 : 0;
      if (seq[i] != marker.seq[i] + add ||
          (add && info.old_seq[slot] != marker.seq[i])) {
        return fail_because("another commit slipped in");
      }
    }
    // One LOG line per apply: what the module chose and what it cost (the
    // Postgres cost models are calibrated from these).
    std::size_t rows = 0;
    for (const auto &batch : entry->batches)
      rows += batch.old_rows.size() + batch.new_rows.size();
    core::ClearMaintenanceDecisionLog();
    const auto started = std::chrono::steady_clock::now();
    auto report = [&](const char *outcome) {
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();
      elog(LOG, "accel apply %s: %s batches=%zu rows=%zu ms=%.1f %s",
           accelerator.c_str(), outcome, entry->batches.size(), rows, ms,
           core::FormatLastMaintenanceDecision().c_str());
    };
    try {
      if (!entry->batches.empty()) {
        for (const auto &batch : entry->batches) {
          backend.CaptureMutationAtDepth(batch, 0);
        }
        backend.OnPreCommit();
        backend.OnCommit();
      }
    } catch (const std::exception &e) {
      try {
        backend.OnAbort();
      } catch (...) {
      }
      store.ClearTransactionDirty();
      report(e.what());
      return fail();
    } catch (...) {
      try {
        backend.OnAbort();
      } catch (...) {
      }
      store.ClearTransactionDirty();
      report("failed");
      return fail();
    }
    std::vector<uint64_t> after;
    auxsync::ReadSeq(dep_slots, after);
    if (store.NeedsRebuild() || after != seq) {
      report(store.NeedsRebuild() ? "invalidated" : "raced");
      return fail();
    }
    report("applied");
    marker.seq = seq;
    marker.own_commits.push_back(info.xid);
    if (marker.own_commits.size() >= kRebaseOwnCommits) {
      // Nothing else committed on these tables (the counters above), so a
      // snapshot taken now sees exactly the applied commits: it becomes the
      // new base and the list of applied commits can be dropped.
      auxsync::SnapMark base = auxsync::MarkOf(GetLatestSnapshot());
      if (!base.ok) {
        return fail();
      }
      marker.base = std::move(base);
      marker.own_commits.clear();
    }
    srv::NoteApplyOutcome(true);
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
    if (accelerator_codegen::aux_state::engine::postgres::aux_server::
            SharedEnabled()) {
      // This backend's copy (if any) is not maintained in shared mode.
      marker.synced = false;
      return;
    }
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
  // Own commits are dropped (folded into `base`) by the first hit after them
  // (Execute) and by the server's rebase, so this bound is only reached by a
  // writer that commits this many times without any read in between.
  static constexpr std::size_t kMaxOwnCommits = std::size_t{1} << 20;
  static constexpr std::size_t kRebaseOwnCommits = 64;
  static constexpr int64_t kBehindRebuildMs = 30000;
  int64_t behind_since_ms = 0; // first declined read of a synced state behind the counters
  // Committed transactions whose writes the store maintained locally.  A
  // statement snapshot S sees all of them iff the newest precedes S.xmax and
  // none is listed running in S: O(|S.xip|) per check, independent of how
  // many commits are pending.
  struct OwnCommits {
    std::unordered_set<TransactionId> set;
    TransactionId latest = InvalidTransactionId;
    void push_back(TransactionId xid) {
      set.insert(xid);
      if (!TransactionIdIsValid(latest) || TransactionIdFollows(xid, latest)) {
        latest = xid;
      }
    }
    std::size_t size() const { return set.size(); }
    bool empty() const { return set.empty(); }
    void clear() {
      set.clear();
      latest = InvalidTransactionId;
    }
    bool AllSeenBy(Snapshot snapshot) const {
      if (set.empty()) {
        return true;
      }
      if (!auxsync::UsableSnapshot(snapshot) ||
          !TransactionIdPrecedes(latest, snapshot->xmax)) {
        return false;
      }
      for (uint32 i = 0; i < snapshot->xcnt; ++i) {
        if (set.count(snapshot->xip[i])) {
          return false;
        }
      }
      for (int32 i = 0; i < snapshot->subxcnt; ++i) {
        if (set.count(snapshot->subxip[i])) {
          return false;
        }
      }
      return true;
    }
  };
  struct SyncMarker {
    bool synced = false;
    auxsync::SnapMark base;
    OwnCommits own_commits;
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

  // NeedsSync also holds while the state is in sync and only a commit in
  // flight (a writer between its statement and the worker's apply of it) keeps
  // the counters from matching.  Rebuilding then would throw away a state the
  // apply is about to catch up, and a long rebuild keeps the worker busy, so
  // every commit meanwhile goes unshipped and leaves its accelerators stale
  // too.  Decline instead (the caller answers from SQL); rebuild only a state
  // known stale, or one still behind after kBehindRebuildMs (a commit that was
  // never shipped).
  bool RebuildWarranted() {
    namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
    if (!built || !marker.synced || store.NeedsRebuild()) {
      return true;
    }
    const int64_t now = static_cast<int64_t>(GetCurrentTimestamp() / 1000);
    if (srv::ApplyPending()) {
      behind_since_ms = 0;
      return false;
    }
    if (behind_since_ms == 0) {
      behind_since_ms = now;
    }
    return now - behind_since_ms >= kBehindRebuildMs;
  }

  bool HitAllowed(Snapshot statement) const {
    if (!built || !marker.synced || store.NeedsRebuild() ||
        store.IsTransactionDirty() || auxsync::OwnRegisteredAnyOf(dep_slots)) {
      return false;
    }
    if (!auxsync::Covers(statement, marker.base)) {
      return false;
    }
    if (!marker.own_commits.AllSeenBy(statement)) {
      return false;
    }
    // Read after the statement snapshot was taken.  Writers still in flight
    // are fine as long as S cannot see them (ReadCountersFor).
    std::vector<uint64_t> seq;
    return auxsync::ReadCountersFor(statement, dep_slots, seq) &&
           seq == marker.seq;
  }

  bool CanRebuildNow() const {
    return !TransactionIdIsValid(GetTopTransactionIdIfAny()) &&
           !auxsync::OwnRegisteredAny() && !store.IsTransactionDirty() &&
           !IsInParallelMode() && !RecoveryInProgress();
  }

  void Rebuild() {
    std::vector<uint64_t> before;
    if (!auxsync::ReadCounters(dep_slots, /*allow_own=*/false, before)) {
      // A writer is in flight: the result could not be certified.  The
      // marker is left alone: hits are re-validated against the counters.
      return;
    }
    aux_server::BusyScope busy;
    const auto started = std::chrono::steady_clock::now();
    marker = SyncMarker{};
    Snapshot fresh = GetLatestSnapshot();
    auxsync::SnapMark base = auxsync::MarkOf(fresh);
    PushActiveSnapshot(fresh);
    Builder builder;
    {
      BuildGucScope tuning(tune_build);
      try {
        module.Build(backend, builder);
      } catch (...) {
        tuning.Restore();
        PopActiveSnapshot();
        throw;
      }
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
    elog(aux_server::InServer() ? LOG : DEBUG1,
         "accel rebuild %s: %s ms=%.1f", accelerator.c_str(),
         marker.synced ? "synced" : "unsynced",
         std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - started)
             .count());
    if (!aux_server::InServer()) {
      accelerator_codegen::aux_state::engine::postgres::
          RegisterPostgresBackendForMutations();
    }
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
  // Build with BuildGucScope: for builds that join/aggregate the whole num
  // table, where the session's planner settings pick nested loops / spilling
  // aggregates (q4, q6, q24); others keep the session settings.
  bool tune_build = false;
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
};

} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
