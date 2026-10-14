// GENERATED BACKEND
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "backends/duckdb/engine/chunk_source.hpp"
#include "backends/duckdb/engine/native_scan.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/storage/table/filtered_scan_consumer.hpp"

namespace duckdb {
class ClientContext;
class Connection;
class FunctionData;
class LogicalDependencyList;
class LogicalType;
class TableFilterSet;
struct RowGroupOrderOptions;
}

namespace accelerator_codegen {

// DuckDB backend adapter for the shared NativeScanSpec contract.
duckdb::unique_ptr<duckdb::FunctionData> BindNativeScan(duckdb::ClientContext &context, const NativeScanSpec &scan_spec);
void AddNativeTableDependencies(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data);
struct NativeScanConsumerOptions {
	duckdb::unique_ptr<duckdb::TableFilterSet> additional_filters;
	duckdb::unique_ptr<duckdb::RowGroupOrderOptions> row_group_order;
};

duckdb::idx_t GetNativeScanTableRowCount(duckdb::ClientContext &context, const NativeScanSpec &scan_spec);
duckdb::idx_t GetNativeScanTableRowCount(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
duckdb::unique_ptr<ChunkSource> CreateNativeChunkSource(duckdb::ClientContext &context,
                                                        const duckdb::FunctionData &bind_data,
                                                        const duckdb::TableFilterSet *additional_filters = nullptr);

//! True when the bound native scan pushed at least one table filter.
bool NativeScanHasFilters(const duckdb::FunctionData &bind_data);

//! Drain base + local storage via DataTable::ScanFilteredConsumer.
//! Returns false if the client was interrupted (consumer returned STOP).
bool RunNativeFilteredScanConsumer(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data,
                                   const duckdb::FilteredScanConsumer &consumer,
                                   const NativeScanConsumerOptions *options = nullptr);

struct NativeParallelScanHandle;

//! Fill a NativeParallelScanHandle from bound native-scan FunctionData (parallel path).
void InitializeNativeParallelScanHandle(NativeParallelScanHandle &handle, duckdb::ClientContext &context,
                                        const duckdb::FunctionData &bind_data);

//! Shared ScanNativeBaseTable contract — DuckDB implementation (native storage).
void ScanNativeBaseTable(duckdb::ClientContext &context, const NativeScanSpec &scan_spec,
                         const NativeScanChunkConsumer &consumer);

} // namespace accelerator_codegen
