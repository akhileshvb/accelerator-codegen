// GENERATED BACKEND
#pragma once

// ---- Engine capture of writes (S1) ----
//
// The DuckDB fork's MutationRecorder reports each insert / update / delete,
// with the columns the database's watch provider asks for, to consumers
// registered on the database (DatabaseCaptureRegistry): every connection's
// writes and transactions reach the host.

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "substrate/aux/runtime/columnar_batch.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

class DuckdbHost;

// A DuckDB value as the modules read it (integers Int64, dates AuxDate,
// decimals ScaledDecimal, doubles Double, everything else its text).
core::AuxValue CapturedValue(const ::duckdb::Value &value);

// The watch provider: per table, the columns some accelerator projects
// (union, reference-counted). Registered in the database's object cache under
// the key MutationWatchProvider::TryGet looks up.
class AuxWatchedColumns final : public ::duckdb::MutationWatchProvider {
public:
  static constexpr const char *kKey = "accel_state_registry";
  static ::duckdb::string ObjectType() { return "MutationWatchProvider"; }

  void Add(const std::string &table, const std::vector<int32_t> &columns);
  void Remove(const std::string &table, const std::vector<int32_t> &columns);
  bool TryGetWatchedColumns(::duckdb::TableCatalogEntry &table,
                            ::duckdb::vector<::duckdb::PhysicalIndex> &columns) override;
  // Updates report the updated columns' new values plus the old values of
  // every watched column, so a capture can rebuild the old and new rows. Every
  // update of a watched table is reported, also one that changes no watched
  // column: a key-only delta is a "row with this key changed" signal.
  bool TryGetUpdateOldColumns(::duckdb::TableCatalogEntry &table,
                              const ::duckdb::vector<::duckdb::PhysicalIndex> &,
                              ::duckdb::vector<::duckdb::PhysicalIndex> &old_columns) override;

private:
  // False if `table` is not watched.
  bool Columns(const std::string &table, ::duckdb::vector<::duckdb::PhysicalIndex> &columns);

  std::mutex mutex;
  std::map<std::string, std::map<int32_t, int>> watched;
};

AuxWatchedColumns &WatchedColumns(::duckdb::DatabaseInstance &db);

// One mutation event as a columnar batch of `table`.
runtime::ColumnarBatch ToColumnarBatch(const ::duckdb::TableMutationEvent &event,
                                       core::TableId table);

// Forwards every connection's mutations and transaction lifecycle to a host.
// (Commit ids come through CommitIds: both interfaces derive from
// ClientContextState, so they are separate objects.) A connection keeps these
// objects past the host, so they hold it weakly and hold it for each callback.
class DatabaseCapture final : public ::duckdb::MutationConsumer {
public:
  // Before the object is registered.
  void Bind(std::weak_ptr<DuckdbHost> bound) { host = std::move(bound); }
  void OnTableMutation(::duckdb::ClientContext &context,
                       const ::duckdb::TableMutationEvent &event) override;
  void TransactionPreCommit(::duckdb::MetaTransaction &transaction,
                            ::duckdb::ClientContext &context) override;
  void TransactionCommit(::duckdb::MetaTransaction &transaction,
                         ::duckdb::ClientContext &context) override;
  void TransactionRollback(::duckdb::MetaTransaction &transaction,
                           ::duckdb::ClientContext &context) override;

private:
  std::weak_ptr<DuckdbHost> host;
};

class CommitIds final : public ::duckdb::CommitObserver {
public:
  void Bind(std::weak_ptr<DuckdbHost> bound) { host = std::move(bound); }
  void OnCommit(::duckdb::AttachedDatabase &, ::duckdb::transaction_t) override {}
  void OnCommitWithContext(::duckdb::ClientContext &context, ::duckdb::AttachedDatabase &db,
                           ::duckdb::transaction_t commit_id) override;

private:
  std::weak_ptr<DuckdbHost> host;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
