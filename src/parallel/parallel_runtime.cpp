#include "accelerator_codegen/parallel/parallel_runtime.hpp"

#include <stdexcept>
#include <utility>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace accelerator_codegen {
namespace {

// Parallel scan bind helpers live in duckdb_scan_adapter.cpp (same TU visibility via extern C++ API).

} // namespace

// Implemented in duckdb_scan_adapter.cpp
void InitializeNativeParallelScanHandle(NativeParallelScanHandle &handle, duckdb::ClientContext &context,
                                        const duckdb::FunctionData &bind_data);

void NativeParallelScanHandle::Initialize(duckdb::ClientContext &context_p, const duckdb::FunctionData &bind_data) {
	InitializeNativeParallelScanHandle(*this, context_p, bind_data);
}

void PrepareParallelLocalScanState(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state) {
	ParallelAssert(handle.storage != nullptr, "parallel scan handle not initialized");
	scan_state.Initialize(handle.storage_indexes, *handle.context, handle.filters);
}

bool ClaimNextParallelAssignment(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state) {
	ParallelAssert(handle.storage != nullptr, "parallel scan handle not initialized");
	if (handle.failed.load(std::memory_order_relaxed) || handle.context->interrupted) {
		return false;
	}
	const auto rows = handle.storage->NextParallelScan(*handle.context, handle.parallel_state, scan_state);
	if (rows == 0) {
		return false;
	}
	handle.tasks_claimed.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool DrainParallelAssignmentChunks(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state,
                                   duckdb::DataChunk &chunk,
                                   const std::function<void(duckdb::DataChunk &)> &consume) {
	ParallelAssert(handle.transaction != nullptr, "parallel scan missing transaction");
	while (true) {
		if (handle.context->interrupted) {
			return false;
		}
		chunk.Reset();
		handle.storage->Scan(*handle.transaction, chunk, scan_state);
		if (chunk.size() == 0) {
			return true;
		}
		consume(chunk);
	}
}

bool DrainParallelAssignmentFiltered(NativeParallelScanHandle &handle, duckdb::TableScanState &scan_state,
                                     const duckdb::FilteredScanConsumer &consumer) {
	ParallelAssert(handle.transaction != nullptr, "parallel scan missing transaction");
	duckdb::FilteredScanConsumer wrapped =
	    [ctx = handle.context, &consumer](duckdb::TransactionData transaction_data, duckdb::CollectionScanState &state,
	                                      duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
	                                      duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
		if (ctx->interrupted) {
			return duckdb::FilteredScanConsumerResult::STOP;
		}
		return consumer(transaction_data, state, row_group, sel, count);
	};
	const auto result = handle.storage->ScanFilteredConsumer(*handle.transaction, scan_state, wrapped);
	return result != duckdb::FilteredScanConsumerResult::STOP;
}

namespace {

class ParallelChunkWorkerTask : public duckdb::BaseExecutorTask {
public:
	ParallelChunkWorkerTask(duckdb::TaskExecutor &executor, NativeParallelScanHandle &handle_p, duckdb::idx_t worker_id_p,
	                        std::function<void(duckdb::idx_t, duckdb::DataChunk &)> consume_p)
	    : BaseExecutorTask(executor), handle(handle_p), worker_id(worker_id_p), consume(std::move(consume_p)) {
	}

	void ExecuteTask() override {
		try {
			duckdb::TableScanState scan_state;
			PrepareParallelLocalScanState(handle, scan_state);
			duckdb::DataChunk chunk;
			chunk.Initialize(*handle.context, handle.projected_types);
			while (ClaimNextParallelAssignment(handle, scan_state)) {
				if (!DrainParallelAssignmentChunks(handle, scan_state, chunk,
				                                   [&](duckdb::DataChunk &c) { consume(worker_id, c); })) {
					handle.failed.store(true, std::memory_order_relaxed);
					return;
				}
			}
		} catch (...) {
			handle.failed.store(true, std::memory_order_relaxed);
			throw;
		}
	}

private:
	NativeParallelScanHandle &handle;
	duckdb::idx_t worker_id;
	std::function<void(duckdb::idx_t, duckdb::DataChunk &)> consume;
};

class ParallelFilteredWorkerTask : public duckdb::BaseExecutorTask {
public:
	using ConsumeFn = std::function<void(duckdb::idx_t, duckdb::TransactionData, duckdb::CollectionScanState &,
	                                     duckdb::RowGroup &, duckdb::SelectionVector &, duckdb::idx_t)>;

	ParallelFilteredWorkerTask(duckdb::TaskExecutor &executor, NativeParallelScanHandle &handle_p,
	                           duckdb::idx_t worker_id_p, ConsumeFn consume_p)
	    : BaseExecutorTask(executor), handle(handle_p), worker_id(worker_id_p), consume(std::move(consume_p)) {
	}

	void ExecuteTask() override {
		try {
			duckdb::TableScanState scan_state;
			PrepareParallelLocalScanState(handle, scan_state);
			while (ClaimNextParallelAssignment(handle, scan_state)) {
				const bool ok = DrainParallelAssignmentFiltered(
				    handle, scan_state,
				    [&](duckdb::TransactionData td, duckdb::CollectionScanState &st, duckdb::RowGroup &rg,
				        duckdb::SelectionVector &sel, duckdb::idx_t count) {
					    consume(worker_id, td, st, rg, sel, count);
					    return duckdb::FilteredScanConsumerResult::CONTINUE;
				    });
				if (!ok) {
					handle.failed.store(true, std::memory_order_relaxed);
					return;
				}
			}
		} catch (...) {
			handle.failed.store(true, std::memory_order_relaxed);
			throw;
		}
	}

private:
	NativeParallelScanHandle &handle;
	duckdb::idx_t worker_id;
	ConsumeFn consume;
};

} // namespace

void RunParallelChunkScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data,
                          duckdb::idx_t worker_count,
                          const std::function<void(duckdb::idx_t worker_id, duckdb::DataChunk &chunk)> &consume) {
	if (worker_count == 0) {
		worker_count = 1;
	}
	NativeParallelScanHandle handle;
	handle.Initialize(context, bind_data);

	duckdb::TaskExecutor executor(context);
	for (duckdb::idx_t w = 0; w < worker_count; w++) {
		executor.ScheduleTask(duckdb::make_uniq<ParallelChunkWorkerTask>(executor, handle, w, consume));
	}
	executor.WorkOnTasks();
	if (handle.failed.load(std::memory_order_relaxed) || context.interrupted) {
		throw duckdb::InterruptException();
	}
}

void RunParallelFilteredScan(
    duckdb::ClientContext &context, const duckdb::FunctionData &bind_data, duckdb::idx_t worker_count,
    const std::function<void(duckdb::idx_t worker_id, duckdb::TransactionData, duckdb::CollectionScanState &,
                             duckdb::RowGroup &, duckdb::SelectionVector &, duckdb::idx_t)> &consume) {
	if (worker_count == 0) {
		worker_count = 1;
	}
	NativeParallelScanHandle handle;
	handle.Initialize(context, bind_data);

	duckdb::TaskExecutor executor(context);
	for (duckdb::idx_t w = 0; w < worker_count; w++) {
		executor.ScheduleTask(duckdb::make_uniq<ParallelFilteredWorkerTask>(executor, handle, w, consume));
	}
	executor.WorkOnTasks();
	if (handle.failed.load(std::memory_order_relaxed) || context.interrupted) {
		throw duckdb::InterruptException();
	}
}

} // namespace accelerator_codegen
