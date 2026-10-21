// GENERATED BACKEND
#pragma once

// Partitioned typed scans for parallel builds (S8, with core::PartitionedBuild).
//
// ScanPartitioned splits a table into row-group ranges (fixed by the table's
// row groups, so the same data gives the same partitions) and streams each
// range on its own connection, at the build's snapshot, on a pool thread. A
// range is read straight from the row groups, so its cost is its own rows.
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
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

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

  ::duckdb::LogicalType Type() const {
    switch (kind) {
    case Kind::Int:
      return ::duckdb::LogicalType::BIGINT;
    case Kind::Date:
      return ::duckdb::LogicalType::DATE;
    case Kind::Decimal:
      return ::duckdb::LogicalType::DECIMAL(18, static_cast<uint8_t>(scale));
    case Kind::Double:
      return ::duckdb::LogicalType::DOUBLE;
    case Kind::Text:
      return ::duckdb::LogicalType::VARCHAR;
    }
    return ::duckdb::LogicalType::SQLNULL;
  }

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

// One partition's reader: a private connection in a transaction at `snapshot`
// (0: the newest data), scanning row groups of a table directly with the same
// visibility rules and casts as `SELECT CAST(column AS type) ...`.
class RowGroupReader {
public:
  RowGroupReader(::duckdb::DatabaseInstance &db, ::duckdb::transaction_t snapshot, const std::string &table,
                 const std::vector<BuildColumn> &columns)
      : connection(db, snapshot), specs(columns) {
    (*connection).BeginTransaction();
    auto &context = *(*connection).context;
    auto &entry = ::duckdb::Catalog::GetEntry<::duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, table);
    storage = &entry.GetStorage();
    transaction = &::duckdb::DuckTransaction::Get(context, entry.catalog);
    ::duckdb::vector<::duckdb::LogicalType> source_types;
    ::duckdb::vector<::duckdb::LogicalType> target_types;
    for (const auto &column : columns) {
      Require(entry.ColumnExists(column.name), "build scan column missing: " + column.name);
      const auto &definition = entry.GetColumn(column.name);
      source_types.push_back(definition.Type());
      target_types.push_back(column.Type());
      storage_columns.push_back(entry.GetStorageIndex(::duckdb::ColumnIndex(definition.Oid())));
    }
    source.Initialize(context, source_types);
    typed.Initialize(context, target_types);
  }
  ~RowGroupReader() { (*connection).Rollback(); }

  std::size_t RowGroupCount() { return storage->GetRowGroupCollection()->GetRowGroups()->GetSegmentCount(); }

  // sink(const ChunkBatch &) for the rows of row groups [first, last), in table order.
  template <class Sink> void Scan(std::size_t first, std::size_t last, Sink &&sink) {
    auto &collection = *storage->GetRowGroupCollection();
    auto groups = collection.GetRowGroups();
    auto *begin = groups->GetSegmentByIndex(static_cast<int64_t>(first)).get();
    auto *end = groups->GetSegmentByIndex(static_cast<int64_t>(last) - 1).get();
    Require(begin && end, "build scan row group range out of bounds");
    ::duckdb::TableScanState state;
    state.Initialize(storage_columns);
    ::duckdb::RowGroupCollection::InitializeScanInRowGroup(*(*connection).context, state.table_state, collection,
                                                           *begin, 0, end->GetRowStart() + end->GetNode().count);
    while (true) {
      source.Reset();
      state.table_state.Scan(*transaction, source);
      if (source.size() == 0) {
        break;
      }
      CastToTargets();
      sink(ChunkBatch(typed, specs));
    }
  }

private:
  void CastToTargets() {
    typed.Reset();
    for (::duckdb::idx_t c = 0; c < specs.size(); ++c) {
      if (source.data[c].GetType() == typed.data[c].GetType()) {
        typed.data[c].Reference(source.data[c]);
      } else {
        ::duckdb::VectorOperations::Cast(*(*connection).context, source.data[c], typed.data[c], source.size());
      }
    }
    typed.SetCardinality(source.size());
  }

  SnapshotConnection connection;
  const std::vector<BuildColumn> &specs;
  ::duckdb::DataTable *storage = nullptr;
  ::duckdb::DuckTransaction *transaction = nullptr;
  ::duckdb::vector<::duckdb::StorageIndex> storage_columns;
  ::duckdb::DataChunk source;
  ::duckdb::DataChunk typed;
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
  // Every partition reads where a serial scan through `connection` would: the
  // reader's snapshot under a fallback redirect, else the build's.
  const auto snapshot = ScanSnapshot(connection_p);
  auto &db = *connection.context->db;
  const std::size_t groups = RowGroupReader(db, snapshot, table, columns).RowGroupCount();
  // Partition p holds row groups [groups * p / partitions, groups * (p + 1) / partitions).
  services.ParallelFor(partitions, [&](std::size_t p) {
    const std::size_t first = groups * p / partitions;
    const std::size_t last = groups * (p + 1) / partitions;
    if (first == last) {
      return;
    }
    RowGroupReader(db, snapshot, table, columns).Scan(first, last, [&](const ChunkBatch &batch) { sink(p, batch); });
  });
}

} // namespace accelerator_codegen::aux_state::engine::duckdb::scan
