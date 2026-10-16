// GENERATED BACKEND
#pragma once

// Partitioned typed scans for parallel builds (S8, with core::PartitionedBuild).
//
// ScanPartitioned splits a table into row-group ranges (fixed by the table's
// row count, so the same data gives the same partitions) and streams each
// range on its own connection, at the build's snapshot, on a pool thread.
// Rows reach the sink as typed column batches; there are no boxed values.
// With one partition it is the build connection's serial scan.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "backends/duckdb/engine/duckdb_aux_backend.hpp"
#include "backends/duckdb/engine/scan.hpp"
#include "duckdb.hpp"
#include "duckdb/accelerator/snapshot_override.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb::scan {

// A projected column and how its values are read: Int as int64, Date as days
// since 1970-01-01, Decimal as value * 10^scale in int64, Double as double,
// Text as bytes. A column that can hold NULL must be declared Nullable and
// tested with BuildRow::IsNull before it is read.
struct BuildColumn {
  enum class Kind { Int, Date, Decimal, Double, Text };
  std::string name;
  Kind kind;
  int scale = 0;
  bool nullable = false;

  std::string Sql() const {
    switch (kind) {
    case Kind::Int:
      return "CAST(" + name + " AS BIGINT)";
    case Kind::Date:
      return "CAST(" + name + " AS DATE)";
    case Kind::Decimal:
      return "CAST(" + name + " AS DECIMAL(18," + std::to_string(scale) + "))";
    case Kind::Double:
      return "CAST(" + name + " AS DOUBLE)";
    case Kind::Text:
      return "CAST(" + name + " AS VARCHAR)";
    }
    return name;
  }
};

inline BuildColumn IntColumn(const char *name) { return {name, BuildColumn::Kind::Int}; }
inline BuildColumn DateColumn(const char *name) { return {name, BuildColumn::Kind::Date}; }
inline BuildColumn DecimalColumn(const char *name, int scale) {
  return {name, BuildColumn::Kind::Decimal, scale};
}
inline BuildColumn DoubleColumn(const char *name) { return {name, BuildColumn::Kind::Double}; }
inline BuildColumn TextColumn(const char *name) { return {name, BuildColumn::Kind::Text}; }
inline BuildColumn Nullable(BuildColumn column) {
  column.nullable = true;
  return column;
}

class ChunkBatch;

// One row of a batch; indexes are positions in the column list.
struct BuildRow {
  const ChunkBatch &batch;
  std::size_t row;

  int64_t Int(int column) const { return At<int64_t>(column); }
  int32_t Date(int column) const { return At<::duckdb::date_t>(column).days; }
  int64_t Decimal(int column) const { return At<int64_t>(column); }
  double Double(int column) const { return At<double>(column); }
  bool IsNull(int column) const;
  std::string_view TextView(int column) const;
  std::string Text(int column) const { return std::string(TextView(column)); }
  char Char(int column) const {
    const auto text = TextView(column);
    return text.empty() ? '\0' : text[0];
  }

private:
  template <class T> const T &At(int column) const;
};

// A DataChunk's columns in unified form (flat, constant and dictionary
// vectors alike); valid while the chunk is.
class ChunkBatch {
public:
  ChunkBatch(::duckdb::DataChunk &chunk, const std::vector<BuildColumn> &specs)
      : count(chunk.size()), columns(chunk.ColumnCount()) {
    for (::duckdb::idx_t c = 0; c < columns.size(); ++c) {
      chunk.data[c].ToUnifiedFormat(count, columns[c]);
      Require(specs[c].nullable || columns[c].validity.AllValid(), "unexpected NULL in a build scan column");
    }
  }
  std::size_t size() const { return count; }
  BuildRow Row(std::size_t row) const { return {*this, row}; }

private:
  friend struct BuildRow;
  std::size_t count;
  std::vector<::duckdb::UnifiedVectorFormat> columns;
};

template <class T> const T &BuildRow::At(int column) const {
  const auto &format = batch.columns[column];
  return reinterpret_cast<const T *>(format.data)[format.sel->get_index(row)];
}

inline bool BuildRow::IsNull(int column) const {
  const auto &format = batch.columns[column];
  return !format.validity.RowIsValid(format.sel->get_index(row));
}

inline std::string_view BuildRow::TextView(int column) const {
  const auto &text = At<::duckdb::string_t>(column);
  return {text.GetData(), text.GetSize()};
}

// Partition boundaries as row ids: partition p holds the row groups
// [groups * p / partitions, groups * (p + 1) / partitions); the first and the
// last partition are open-ended below and above.
inline std::vector<uint64_t> RowGroupBounds(uint64_t rows, std::size_t partitions) {
  constexpr uint64_t kRowGroupRows = DEFAULT_ROW_GROUP_SIZE;
  const uint64_t groups = (rows + kRowGroupRows - 1) / kRowGroupRows;
  std::vector<uint64_t> bounds(partitions + 1);
  for (std::size_t p = 0; p <= partitions; ++p) {
    bounds[p] = groups * p / partitions * kRowGroupRows;
  }
  return bounds;
}

// A private connection reading at `snapshot` (0: the newest data).
class SnapshotConnection {
public:
  SnapshotConnection(::duckdb::DatabaseInstance &db, ::duckdb::transaction_t snapshot_p)
      : snapshot(snapshot_p), connection(db) {
    if (snapshot) {
      ::duckdb::SetTransactionStartOverride(*connection.context, snapshot);
    }
  }
  ~SnapshotConnection() {
    if (snapshot) {
      ::duckdb::ClearTransactionStartOverride(*connection.context);
    }
  }
  ::duckdb::Connection &operator*() { return connection; }

private:
  ::duckdb::transaction_t snapshot;
  ::duckdb::Connection connection;
};

inline uint64_t TableRowCount(::duckdb::Connection &connection, const std::string &table) {
  uint64_t rows = 0;
  StreamChunks(connection, "SELECT count(*) FROM " + table, [&](::duckdb::DataChunk &chunk) {
    rows = static_cast<uint64_t>(chunk.GetValue(0, 0).GetValue<int64_t>());
  });
  return rows;
}

// Streams `table` as typed batches: sink(partition, const ChunkBatch &).
// Different partitions run concurrently on services' pool threads; one
// partition's batches arrive in table order on one thread.
template <class Sink>
void ScanPartitioned(const core::AuxEngineServices &services, ::duckdb::Connection &connection_p,
                     const std::string &table, const std::vector<BuildColumn> &columns,
                     std::size_t partitions, Sink &&sink) {
  std::string select = "SELECT ";
  for (std::size_t c = 0; c < columns.size(); ++c) {
    select += (c ? ", " : "") + columns[c].Sql();
  }
  select += " FROM " + table;
  const auto stream = [&](::duckdb::Connection &connection, const std::string &sql, std::size_t partition) {
    StreamChunks(connection, sql, [&](::duckdb::DataChunk &chunk) { sink(partition, ChunkBatch(chunk, columns)); });
  };

  auto lock = LockScan(connection_p);
  auto &connection = ScanConnection(connection_p);
  if (partitions <= 1) {
    stream(connection, select, 0);
    return;
  }
  const auto bounds = RowGroupBounds(TableRowCount(connection, table), partitions);
  // Every partition reads where a serial scan through `connection` would: the
  // reader's snapshot under a fallback redirect, else the build's.
  const auto snapshot = ScanSnapshot(connection_p);
  services.ParallelFor(partitions, [&](std::size_t p) {
    std::vector<std::string> conditions;
    if (p > 0) {
      conditions.push_back("rowid >= " + std::to_string(bounds[p]));
    }
    if (p + 1 < partitions) {
      if (bounds[p] == bounds[p + 1]) {
        return;
      }
      conditions.push_back("rowid < " + std::to_string(bounds[p + 1]));
    }
    std::string sql = select;
    for (std::size_t i = 0; i < conditions.size(); ++i) {
      sql += (i ? " AND " : " WHERE ") + conditions[i];
    }
    SnapshotConnection reader(*connection.context->db, snapshot);
    stream(*reader, sql, p);
  });
}

} // namespace accelerator_codegen::aux_state::engine::duckdb::scan
