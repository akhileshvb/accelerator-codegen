#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/sec_edgar_q1.hpp"
#include "accelerator_codegen/sec_edgar_q1_aux.hpp"
#include "accelerator_codegen/sec_edgar_q1_fused.hpp"
#include "accelerator_codegen/sec_edgar_q1_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q1_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q1_write_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/reader_mutation_epoch.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

struct BoundSecEdgarQ1AuxData : public duckdb::FunctionData {
	explicit BoundSecEdgarQ1AuxData(std::unique_ptr<SecEdgarQ1Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<SecEdgarQ1Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundSecEdgarQ1AuxData>(std::make_unique<SecEdgarQ1Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundSecEdgarQ1AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(SecEdgarQ1PartitionKey partition_p) : partition(partition_p) {
	}
	SecEdgarQ1PartitionKey partition = 0;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundRebuildPartitionData>(partition);
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return partition == other_p.Cast<BoundRebuildPartitionData>().partition;
	}
};

struct BoundNoArgData : public duckdb::FunctionData {
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundNoArgData>();
	}
	bool Equals(const duckdb::FunctionData &) const override {
		return true;
	}
};

struct SecEdgarQ1AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	SecEdgarQ1FusedKernel *fused_kernel = nullptr;
	duckdb::unique_ptr<duckdb::FunctionData> bound_scan;
	duckdb::unique_ptr<ChunkSource> input;
	std::unique_ptr<aux_state::ActiveExecutionScope> execution_scope;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
};

struct SecEdgarQ1AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

void InitSecEdgarQ1AuxLocalState(duckdb::ClientContext &context, SecEdgarQ1AuxLocalState &local_state) {
	RecordSecEdgarQ1WorkloadQuery();
	EnsurePendingSecEdgarQ1RebuildApplied(context);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kSecEdgarQ1AuxAcceleratorId);
	auto &maintenance = registry->GetMaintenanceState(kSecEdgarQ1AuxAcceleratorId);
	const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(context, kSecEdgarQ1AuxAcceleratorId,
	                                                                   maintenance.GetCommittedMutationEpoch());
	const auto gate = CheckSecEdgarQ1GroupExecutionGate(store, *txn_buffer, reader_epoch);

	ResetSecEdgarQ1AuxDiagnostics();
	SecEdgarQ1AuxDiagnostics diagnostics;

	if (SecEdgarQ1AuxEnabled() && gate.use_aux && store.ValidPartitionCount() > 0) {
		const auto start = std::chrono::steady_clock::now();
		auto rows = CollectSecEdgarQ1AuxResult(context);
		diagnostics.used_aux_path = true;
		diagnostics.answer_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		local_state.kernel = CreateSecEdgarQ1AuxFastKernel(std::move(rows));
		UpdateSecEdgarQ1AuxDiagnostics(diagnostics);
		RecordSecEdgarQ1AuxQueryOutcome(true);
		local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
		    aux_state::ExecutionKind::AuxQuery, kSecEdgarQ1AuxAcceleratorId);
		return;
	}

	local_state.use_fallback = true;
	diagnostics.used_fallback = true;
	diagnostics.bypassed_by_txn = gate.bypassed_by_txn;
	UpdateSecEdgarQ1AuxDiagnostics(diagnostics);

	SecEdgarQ1Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ1ScanSpec());
	local_state.kernel = CreateSecEdgarQ1FusedKernel(parameters);
	local_state.fused_kernel = dynamic_cast<SecEdgarQ1FusedKernel *>(local_state.kernel.get());
	auto scan = BindNativeScan(context, CreateSecEdgarQ1ScanSpec());
	local_state.bound_scan = std::move(scan);
	local_state.input = CreateNativeChunkSource(context, *local_state.bound_scan);
	RecordSecEdgarQ1AuxQueryOutcome(false);
	local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
	    aux_state::ExecutionKind::FallbackQuery, kSecEdgarQ1AuxAcceleratorId);
}

duckdb::unique_ptr<duckdb::FunctionData> SecEdgarQ1AuxBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
                                                         duckdb::vector<duckdb::LogicalType> &return_types,
                                                         duckdb::vector<duckdb::string> &names) {
	if (!input.inputs.empty()) {
		throw duckdb::BinderException("accel_sec_edgar_q1_aux expects no arguments");
	}
	SecEdgarQ1Parameters parameters;
	return_types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
	                duckdb::LogicalType::BIGINT, duckdb::LogicalType::DOUBLE};
	names = {"stmt", "rfile", "cnt", "num_filings", "avg_line_num"};
	return duckdb::make_uniq<BoundSecEdgarQ1AuxData>(std::make_unique<SecEdgarQ1Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> SecEdgarQ1AuxInit(duckdb::ClientContext &,
                                                                       duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<SecEdgarQ1AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> SecEdgarQ1AuxLocalInit(duckdb::ExecutionContext &context,
                                                                           duckdb::TableFunctionInitInput &,
                                                                           duckdb::GlobalTableFunctionState *) {
	auto local_state = duckdb::make_uniq<SecEdgarQ1AuxLocalState>();
	InitSecEdgarQ1AuxLocalState(context.client, *local_state);
	return std::move(local_state);
}

void SecEdgarQ1AuxExec(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<SecEdgarQ1AuxLocalState>();
	if (!local_state.kernel) {
		output.SetCardinality(0);
		return;
	}

	if (local_state.use_fallback && !local_state.inputs_consumed) {
		if (!local_state.fused_kernel || !local_state.bound_scan) {
			throw duckdb::InternalException("SEC EDGAR SecEdgarQ1 aux fallback missing fused kernel or bound scan");
		}
		local_state.fused_kernel->RunFusedPreScan(context, *local_state.bound_scan);
		local_state.inputs_consumed = true;
		local_state.kernel->Finalize();
		local_state.finalized = true;
	}

	if (!local_state.finalized && !local_state.use_fallback) {
		local_state.kernel->Finalize();
		local_state.finalized = true;
	}

	if (!local_state.kernel->ProduceOutput(output)) {
		local_state.execution_scope.reset();
		output.SetCardinality(0);
	}
}

struct BuildLocalState : public duckdb::LocalTableFunctionState {
	bool done = false;
};

struct RebuildPartitionLocalState : public duckdb::LocalTableFunctionState {
	bool done = false;
};

duckdb::unique_ptr<duckdb::LocalTableFunctionState> BuildLocalInit(duckdb::ExecutionContext &,
                                                                   duckdb::TableFunctionInitInput &,
                                                                   duckdb::GlobalTableFunctionState *) {
	return duckdb::make_uniq<BuildLocalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> RebuildPartitionLocalInit(duckdb::ExecutionContext &,
                                                                              duckdb::TableFunctionInitInput &,
                                                                              duckdb::GlobalTableFunctionState *) {
	return duckdb::make_uniq<RebuildPartitionLocalState>();
}

void BuildTableFunction(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<BuildLocalState>();
	if (local_state.done) {
		output.SetCardinality(0);
		return;
	}
	BuildSecEdgarQ1AuxState(context);
	output.SetCardinality(1);
	output.SetValue(0, 0, duckdb::Value("ok"));
	local_state.done = true;
}

duckdb::unique_ptr<duckdb::FunctionData> BuildBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &,
                                                   duckdb::vector<duckdb::LogicalType> &return_types,
                                                   duckdb::vector<duckdb::string> &names) {
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	return duckdb::make_uniq<BoundNoArgData>();
}

void RebuildPartitionExec(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<RebuildPartitionLocalState>();
	if (local_state.done) {
		output.SetCardinality(0);
		return;
	}
	const auto &bind_data = input.bind_data->Cast<BoundRebuildPartitionData>();
	RebuildSecEdgarQ1AuxPartition(context, bind_data.partition);
	output.SetCardinality(1);
	output.SetValue(0, 0, duckdb::Value("ok"));
	local_state.done = true;
}

duckdb::unique_ptr<duckdb::FunctionData> RebuildPartitionBind(duckdb::ClientContext &,
                                                              duckdb::TableFunctionBindInput &bind_input,
                                                              duckdb::vector<duckdb::LogicalType> &return_types,
                                                              duckdb::vector<duckdb::string> &names) {
	if (bind_input.inputs.size() != 1 || bind_input.inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_sec_edgar_q1_aux_rebuild_partition expects BIGINT partition key");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	const auto partition = static_cast<SecEdgarQ1PartitionKey>(bind_input.inputs[0].GetValue<int64_t>());
	return duckdb::make_uniq<BoundRebuildPartitionData>(partition);
}

} // namespace

duckdb::TableFunctionSet CreateSecEdgarQ1AuxFunctionSet() {
	duckdb::TableFunction function("accel_sec_edgar_q1_aux", {}, SecEdgarQ1AuxExec, SecEdgarQ1AuxBind, SecEdgarQ1AuxInit,
	                               SecEdgarQ1AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateSecEdgarQ1AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_sec_edgar_q1_aux_build", {}, BuildTableFunction, BuildBind, nullptr,
	                               BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateSecEdgarQ1AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_sec_edgar_q1_aux_rebuild_partition", {duckdb::LogicalType::BIGINT},
	                               RebuildPartitionExec, RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
