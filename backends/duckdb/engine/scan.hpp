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
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "substrate/aux/core/aux_types.hpp"

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
  ::duckdb::transaction_t Snapshot() const { return snapshot; }
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
// The snapshot a scan through ScanConnection(connection) reads: the redirect's
// (the reader's) when one is active, else the connection's open transaction.
::duckdb::transaction_t ScanSnapshot(::duckdb::Connection &connection);

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

// Streams the result of `sql` on `connection` chunk by chunk, without
// materializing it. The caller owns the connection's scan lock.
template <typename ChunkFn>
void StreamChunks(::duckdb::Connection &connection, const std::string &sql, ChunkFn &&on_chunk) {
  auto result = connection.SendQuery(sql);
  Require(result != nullptr && !result->HasError(), result ? result->GetError() : "DuckDB scan returned null");
  while (auto chunk = result->Fetch()) {
    if (chunk->size() == 0) {
      break;
    }
    on_chunk(*chunk);
  }
  Require(!result->HasError(), result->GetError());
}

// Streaming scan for large SF builds: avoids materializing the full result.
template <typename Callback>
void ScanQueryStreaming(::duckdb::Connection &connection_p, const std::string &sql, Callback &&callback) {
  auto lock = LockScan(connection_p);
  StreamChunks(ScanConnection(connection_p), sql, [&](::duckdb::DataChunk &chunk) {
    for (::duckdb::idx_t row_index = 0; row_index < chunk.size(); ++row_index) {
      callback(chunk, row_index);
    }
  });
}

// The services of the module call running on this thread (ModuleBinding sets
// them), whose ParallelFor limit applies to the scans it issues.
extern thread_local const core::AuxEngineServices *t_scan_services;

class ScanServicesScope {
public:
  explicit ScanServicesScope(const core::AuxEngineServices &services) : saved(t_scan_services) {
    t_scan_services = &services;
  }
  ~ScanServicesScope() { t_scan_services = saved; }
  ScanServicesScope(const ScanServicesScope &) = delete;
  ScanServicesScope &operator=(const ScanServicesScope &) = delete;

private:
  const core::AuxEngineServices *saved;
};

// Streaming scan that decodes rows in parallel: decode(chunk, row) -> Input
// runs on pool threads, consume(Input &&) on this thread in row order, so the
// module's state sees exactly what ScanQueryStreaming would give it. With one
// worker it is ScanQueryStreaming.
template <typename Decode, typename Consume>
void ScanQueryDecoded(::duckdb::Connection &connection_p, const std::string &sql, Decode &&decode,
                      Consume &&consume) {
  using Input = decltype(decode(std::declval<const ::duckdb::DataChunk &>(), ::duckdb::idx_t{0}));
  const auto *services = t_scan_services;
  if (!services || services->WorkerCount() <= 1) {
    ScanQueryStreaming(connection_p, sql,
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) { consume(decode(chunk, row)); });
    return;
  }
  constexpr std::size_t kChunksPerBatch = 64;
  std::vector<::duckdb::unique_ptr<::duckdb::DataChunk>> chunks;
  std::vector<std::vector<Input>> decoded;
  const auto flush = [&] {
    decoded.assign(chunks.size(), {});
    services->ParallelFor(chunks.size(), [&](std::size_t i) {
      decoded[i].reserve(chunks[i]->size());
      for (::duckdb::idx_t row = 0; row < chunks[i]->size(); ++row) {
        decoded[i].push_back(decode(*chunks[i], row));
      }
    });
    for (auto &rows : decoded) {
      for (auto &input : rows) {
        consume(std::move(input));
      }
    }
    chunks.clear();
  };
  auto lock = LockScan(connection_p);
  auto &connection = ScanConnection(connection_p);
  auto result = connection.SendQuery(sql);
  Require(result != nullptr && !result->HasError(), result ? result->GetError() : "DuckDB scan returned null");
  while (auto chunk = result->Fetch()) {
    if (chunk->size() == 0) {
      break;
    }
    chunks.push_back(std::move(chunk));
    if (chunks.size() == kChunksPerBatch) {
      flush();
    }
  }
  flush();
  Require(!result->HasError(), result->GetError());
}

} // namespace scan

} // namespace accelerator_codegen::aux_state::engine::duckdb
