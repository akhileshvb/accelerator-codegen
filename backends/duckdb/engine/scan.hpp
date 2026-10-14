// GENERATED BACKEND
#pragma once

// The DuckDB engine's scan services: SQL on a runtime's connection (or a
// reader's private one), value decoding for the queries' InitialScan
// implementations (aux_query_registry.hpp re-exports these), and the
// per-thread table-scan count the runtime uses to tell a state hit from a
// SQL fallback.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// Scans normally run on the runtime's (writer's) connection. A reader thread
// (ReaderScope from another connection) must not touch it: a ScanRedirect
// sends its scans to a private connection, opened on first use. That
// connection reads at the reader's snapshot (`snapshot`, the reader's
// transaction start time; 0 = the newest data), not at the newest commit:
// the SQL fallback must answer from the same state as the rest of the
// reader's transaction (snapshot isolation).
struct ScanRedirect {
  ScanRedirect(::duckdb::DatabaseInstance &db_p, std::atomic<uint64_t> &opened_p,
               ::duckdb::transaction_t snapshot_p = 0);
  ~ScanRedirect();
  ::duckdb::Connection &Connection();
  static thread_local ScanRedirect *current;

private:
  ::duckdb::DatabaseInstance &db;
  std::atomic<uint64_t> &opened;
  ::duckdb::transaction_t snapshot;
  ScanRedirect *previous;
  std::unique_ptr<::duckdb::Connection> connection;
};

// SQL this thread ran against the tables (a read that runs any answered from
// SQL, not from the state).
extern thread_local uint64_t t_table_scans;

// ---- scan helpers for InitialScan implementations ----
namespace scan {

void Require(bool condition, const std::string &message);
std::string SqlDateLiteral(int32_t days);
std::string SqlDateInPredicate(const char *column, const std::set<int32_t> &days);
std::string SqlIntInPredicate(const char *column, const std::set<int32_t> &values);
std::string SqlInt64InPredicate(const char *column, const std::unordered_set<int64_t> &values);
// A DECIMAL's value at scale 2 (the aux kernels' fixed point).
int64_t DecimalRaw(const ::duckdb::Value &value);
int64_t SafeInt64(const ::duckdb::Value &value);
int32_t SafeInt32(const ::duckdb::Value &value);
int32_t DateDays(const ::duckdb::Value &value);
char FirstChar(const ::duckdb::Value &value);
// " LIMIT <n>" when the environment variable is set (bench scans).
std::string OptionalBenchScanLimitSuffix(const char *env_name);

// The connection a scan runs on: the runtime's, or a reader thread's private
// one (counted as a table scan either way).
::duckdb::Connection &ScanConnection(::duckdb::Connection &connection);

// Serializes use of one connection across threads (ApplyWriteAll stages the
// accelerators' maintenance in parallel, all on the writer's connection; a
// DuckDB connection runs one query at a time and a streaming result dies when
// another query runs on it). Hold it for a scan's whole duration, fetch loop
// included. Recursive; a no-op (unowned lock) for a reader's private connection.
using ScanLock = std::unique_lock<std::recursive_mutex>;
ScanLock LockScan(::duckdb::Connection &connection);

inline ::duckdb::Value ChunkValue(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t col,
                                  ::duckdb::idx_t row) {
  return chunk.GetValue(col, row);
}

template <typename Callback>
void ScanQuery(::duckdb::Connection &connection_p, const std::string &sql, Callback &&callback) {
  auto lock = LockScan(connection_p);
  auto &connection = ScanConnection(connection_p);
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(), result ? result->GetError() : "DuckDB scan returned null");
  for (::duckdb::idx_t row_index = 0; row_index < result->RowCount(); ++row_index) {
    callback(*result, row_index);
  }
}

// Streaming scan for large SF builds: avoids materializing the full result.
template <typename Callback>
void ScanQueryStreaming(::duckdb::Connection &connection_p, const std::string &sql, Callback &&callback) {
  auto lock = LockScan(connection_p);
  auto &connection = ScanConnection(connection_p);
  auto result = connection.SendQuery(sql);
  Require(result != nullptr && !result->HasError(), result ? result->GetError() : "DuckDB scan returned null");
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      break;
    }
    for (::duckdb::idx_t row_index = 0; row_index < chunk->size(); ++row_index) {
      callback(*chunk, row_index);
    }
  }
  Require(!result->HasError(), result->GetError());
}

} // namespace scan

} // namespace accelerator_codegen::aux_state::engine::duckdb
