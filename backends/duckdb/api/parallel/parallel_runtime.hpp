// SCAFFOLDING
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <vector>

#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {

//! Cache-line pad to reduce false sharing on per-worker counters / aggregates.
template <class T>
struct alignas(64) CacheLinePadded {
	T value {};
};

inline duckdb::idx_t ParallelWorkerCount(duckdb::ClientContext &context) {
	auto &scheduler = duckdb::TaskScheduler::GetScheduler(context);
	const auto n = static_cast<duckdb::idx_t>(scheduler.NumberOfThreads());
	return n == 0 ? 1 : n;
}

//! Bound native table + parallel scan state for one input.
struct NativeParallelScanHandle {
	duckdb::ClientContext *context = nullptr;
	duckdb::DuckTransaction *transaction = nullptr;
	duckdb::DataTable *storage = nullptr;
	duckdb::vector<duckdb::StorageIndex> storage_indexes;
	duckdb::vector<duckdb::LogicalType> projected_types;
	duckdb::optional_ptr<duckdb::TableFilterSet> filters;
	duckdb::ParallelTableScanState parallel_state;
	std::atomic<duckdb::idx_t> tasks_claimed {0};
	std::atomic<bool> failed {false};

	void Initialize(duckdb::ClientContext &context_p, const duckdb::FunctionData &bind_data);
};

//! Prepare a worker-local TableScanState (column ids + filters) before claiming.
void PrepareParallelLocalScanState(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state);

//! Claim next row-group assignment. Returns false when the scan is exhausted.
bool ClaimNextParallelAssignment(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state);

//! Drain one claimed assignment via DataTable::Scan into chunks.
//! Returns false if interrupted.
bool DrainParallelAssignmentChunks(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state,
                                   duckdb::DataChunk &chunk,
                                   const std::function<void(duckdb::DataChunk &)> &consume);

//! Drain one claimed assignment via ScanFilteredConsumer.
bool DrainParallelAssignmentFiltered(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state,
                                     const duckdb::FilteredScanConsumer &consumer);

//! TPC-H o_orderkey spans ~4× orders cardinality (SF1: 1.5M rows → keys ≤ 6M).
inline int64_t EstimateTpchDenseOrderCapacity(duckdb::ClientContext &context,
                                              const duckdb::FunctionData &bind_data) {
	NativeParallelScanHandle handle;
	handle.Initialize(context, bind_data);
	const auto rows = handle.storage->GetTotalRows();
	const auto estimated = static_cast<int64_t>(rows) * 4 + 1;
	return estimated < 1 ? 1 : estimated;
}

//! Run N OS-thread workers that each loop claim → drain chunks → consume via native
//! DataTable::NextParallelScan. Workers are ephemeral per query (no persistent aux state).
void RunParallelChunkScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data,
                          duckdb::idx_t worker_count,
                          const std::function<void(duckdb::idx_t worker_id, duckdb::DataChunk &chunk)> &consume);

//! Same as RunParallelChunkScan but uses ScanFilteredConsumer per assignment.
void RunParallelFilteredScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data,
                             duckdb::idx_t worker_count,
                             const std::function<void(duckdb::idx_t worker_id, duckdb::TransactionData,
                                                     duckdb::CollectionScanState &, duckdb::RowGroup &,
                                                     duckdb::SelectionVector &, duckdb::idx_t)> &consume);

//! Debug helpers (no-ops when NDEBUG).
#ifdef NDEBUG
inline void ParallelAssert(bool, const char *) {
}
#else
inline void ParallelAssert(bool condition, const char *message) {
	if (!condition) {
		throw duckdb::InternalException("parallel assert failed: %s", message);
	}
}
#endif

} // namespace accelerator_codegen
