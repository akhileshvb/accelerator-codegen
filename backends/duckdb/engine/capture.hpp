// GENERATED BACKEND
#pragma once

// ---- Engine capture of writes ----
//
// The DuckDB fork's MutationRecorder reports each insert / update / delete a
// connection makes, with the columns the database's watch provider asks for,
// to every MutationConsumer registered on that connection.

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "duckdb.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

class DuckdbRuntime;

// A captured DuckDB value as the modules read it (the same mapping as the
// PostgreSQL capture: integers Int64, dates AuxDate, decimals ScaledDecimal,
// doubles Double, everything else its text).
core::AuxValue CapturedValue(const ::duckdb::Value &value);

// The watch provider: per table, the columns some runtime's module projects
// (union over runtimes, reference-counted). Registered in the database's
// object cache under the key MutationWatchProvider::TryGet looks up.
class AuxWatchedColumns final : public ::duckdb::MutationWatchProvider {
public:
  static constexpr const char *kKey = "accel_state_registry";
  static ::duckdb::string ObjectType() { return "MutationWatchProvider"; }

  void Add(const std::string &table, const std::vector<int32_t> &columns) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto c : columns) {
      watched[table][c]++;
    }
  }
  void Remove(const std::string &table, const std::vector<int32_t> &columns) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table);
    if (it == watched.end()) {
      return;
    }
    for (auto c : columns) {
      auto col = it->second.find(c);
      if (col != it->second.end() && --col->second == 0) {
        it->second.erase(col);
      }
    }
    if (it->second.empty()) {
      watched.erase(it);
    }
  }
  bool TryGetWatchedColumns(::duckdb::TableCatalogEntry &table,
                            ::duckdb::vector<::duckdb::PhysicalIndex> &columns) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table.name);
    if (it == watched.end()) {
      return false;
    }
    columns.clear();
    for (const auto &c : it->second) {
      columns.emplace_back(static_cast<::duckdb::idx_t>(c.first));
    }
    return !columns.empty();
  }
  // Updates report the updated columns' new values plus the old values of
  // every watched column, so a capture can rebuild the old and new rows.
  // Every update of a watched table is reported, also one that changes no
  // watched column: a key-only (lossy) delta is a "row with this key changed"
  // signal (its kernel rescans the key), so dropping it loses the change. For a
  // full-row delta the old and new projections are equal and cancel.
  bool TryGetUpdateOldColumns(::duckdb::TableCatalogEntry &table,
                              const ::duckdb::vector<::duckdb::PhysicalIndex> &,
                              ::duckdb::vector<::duckdb::PhysicalIndex> &old_columns) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table.name);
    if (it == watched.end()) {
      return false;
    }
    old_columns.clear();
    for (const auto &c : it->second) {
      old_columns.emplace_back(static_cast<::duckdb::idx_t>(c.first));
    }
    return true;
  }

private:
  std::mutex mutex;
  std::unordered_map<std::string, std::map<int32_t, int>> watched;
};

AuxWatchedColumns &WatchedColumns(::duckdb::ClientContext &context);

// One consumer per connection: converts each mutation event's values once and
// hands the converted event to every runtime capturing on the connection, in
// registration order.  Events can arrive from parallel sink threads, so the
// runtime list is guarded and the conversion keeps no shared state.
class AuxCaptureHub final : public ::duckdb::MutationConsumer {
public:
  static constexpr const char *kKey = "accel_aux_capture_hub";

  void Add(DuckdbRuntime *runtime) {
    std::lock_guard<std::mutex> lock(mutex);
    runtimes.push_back(runtime);
  }
  // Returns true if no runtime is left.
  bool Remove(DuckdbRuntime *runtime) {
    std::lock_guard<std::mutex> lock(mutex);
    runtimes.erase(std::remove(runtimes.begin(), runtimes.end(), runtime), runtimes.end());
    return runtimes.empty();
  }

  void OnTableMutation(::duckdb::ClientContext &, const ::duckdb::TableMutationEvent &event) override;

private:
  std::mutex mutex;
  std::vector<DuckdbRuntime *> runtimes;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
