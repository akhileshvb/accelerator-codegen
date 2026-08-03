#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/sec_edgar_q6.hpp"
#include "accelerator_codegen/sec_edgar_q6_aux.hpp"
#include "accelerator_codegen/sec_edgar_q6_fused.hpp"
#include "accelerator_codegen/sec_edgar_q6_maintenance.hpp"
#include "accelerator_codegen/sec_edgar_q6_rebuild_policy.hpp"
#include "accelerator_codegen/sec_edgar_q6_write_policy.hpp"

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

struct BoundSecEdgarQ6AuxData : public duckdb::FunctionData {
	explicit BoundSecEdgarQ6AuxData(std::unique_ptr<SecEdgarQ6Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<SecEdgarQ6Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundSecEdgarQ6AuxData>(std::make_unique<SecEdgarQ6Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundSecEdgarQ6AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(SecEdgarQ6PartitionKey partition_p) : partition(partition_p) {
	}
	SecEdgarQ6PartitionKey partition = 0;
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

struct SecEdgarQ6AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	SecEdgarQ6FusedKernel *fused_kernel = nullptr;
	duckdb::unique_ptr<duckdb::FunctionData> bound_scan;
	duckdb::unique_ptr<ChunkSource> input;
	std::unique_ptr<aux_state::ActiveExecutionScope> execution_scope;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
};

struct SecEdgarQ6AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

void InitSecEdgarQ6AuxLocalState(duckdb::ClientContext &context, SecEdgarQ6AuxLocalState &local_state) {
	RecordSecEdgarQ6WorkloadQuery();
	EnsurePendingSecEdgarQ6RebuildApplied(context);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kSecEdgarQ6AuxAcceleratorId);
	auto &maintenance = registry->GetMaintenanceState(kSecEdgarQ6AuxAcceleratorId);
	const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(context, kSecEdgarQ6AuxAcceleratorId,
	                                                                   maintenance.GetCommittedMutationEpoch());
	const auto gate = CheckSecEdgarQ6GroupExecutionGate(store, *txn_buffer, reader_epoch);

	ResetSecEdgarQ6AuxDiagnostics();
	SecEdgarQ6AuxDiagnostics diagnostics;

	if (SecEdgarQ6AuxEnabled() && gate.use_aux && store.ValidPartitionCount() > 0) {
		const auto start = std::chrono::steady_clock::now();
		auto output = GetSecEdgarQ6AuxOutputPartition(context);
		if (!output) {
			throw duckdb::InternalException("SEC EDGAR Q6 aux gate passed but output partition is missing");
		}
		diagnostics.used_aux_path = true;
		diagnostics.answer_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		local_state.kernel = CreateSecEdgarQ6AuxFastKernel(std::move(output));
		UpdateSecEdgarQ6AuxDiagnostics(diagnostics);
		RecordSecEdgarQ6AuxQueryOutcome(true);
		local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
		    aux_state::ExecutionKind::AuxQuery, kSecEdgarQ6AuxAcceleratorId);
		return;
	}

	local_state.use_fallback = true;
	diagnostics.used_fallback = true;
	diagnostics.bypassed_by_txn = gate.bypassed_by_txn;
	UpdateSecEdgarQ6AuxDiagnostics(diagnostics);

	SecEdgarQ6Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ6NumScanSpec());
	local_state.kernel = CreateSecEdgarQ6FusedKernel(parameters);
	local_state.fused_kernel = dynamic_cast<SecEdgarQ6FusedKernel *>(local_state.kernel.get());
	auto scan = BindNativeScan(context, CreateSecEdgarQ6NumScanSpec());
	local_state.bound_scan = std::move(scan);
	local_state.input = CreateNativeChunkSource(context, *local_state.bound_scan);
	RecordSecEdgarQ6AuxQueryOutcome(false);
	local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
	    aux_state::ExecutionKind::FallbackQuery, kSecEdgarQ6AuxAcceleratorId);
}

duckdb::unique_ptr<duckdb::FunctionData> SecEdgarQ6AuxBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
                                                         duckdb::vector<duckdb::LogicalType> &return_types,
                                                         duckdb::vector<duckdb::string> &names) {
	if (!input.inputs.empty()) {
		throw duckdb::BinderException("accel_sec_edgar_q6_aux expects no arguments");
	}
	SecEdgarQ6Parameters parameters;
	return_types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	                duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DOUBLE, duckdb::LogicalType::BIGINT};
	names = {"name", "stmt", "tag", "plabel", "total_value", "cnt"};
	return duckdb::make_uniq<BoundSecEdgarQ6AuxData>(std::make_unique<SecEdgarQ6Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> SecEdgarQ6AuxInit(duckdb::ClientContext &,
                                                                       duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<SecEdgarQ6AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> SecEdgarQ6AuxLocalInit(duckdb::ExecutionContext &context,
                                                                           duckdb::TableFunctionInitInput &,
                                                                           duckdb::GlobalTableFunctionState *) {
	auto local_state = duckdb::make_uniq<SecEdgarQ6AuxLocalState>();
	InitSecEdgarQ6AuxLocalState(context.client, *local_state);
	return std::move(local_state);
}

void SecEdgarQ6AuxExec(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<SecEdgarQ6AuxLocalState>();
	if (!local_state.kernel) {
		output.SetCardinality(0);
		return;
	}

	if (local_state.use_fallback && !local_state.inputs_consumed) {
		if (!local_state.fused_kernel || !local_state.bound_scan) {
			throw duckdb::InternalException("SEC EDGAR SecEdgarQ6 aux fallback missing fused kernel or bound scan");
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
	BuildSecEdgarQ6AuxState(context);
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
	RebuildSecEdgarQ6AuxPartition(context, bind_data.partition);
	output.SetCardinality(1);
	output.SetValue(0, 0, duckdb::Value("ok"));
	local_state.done = true;
}

duckdb::unique_ptr<duckdb::FunctionData> RebuildPartitionBind(duckdb::ClientContext &,
                                                              duckdb::TableFunctionBindInput &bind_input,
                                                              duckdb::vector<duckdb::LogicalType> &return_types,
                                                              duckdb::vector<duckdb::string> &names) {
	if (bind_input.inputs.size() != 1 || bind_input.inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_sec_edgar_q6_aux_rebuild_partition expects BIGINT partition key");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	const auto partition = static_cast<SecEdgarQ6PartitionKey>(bind_input.inputs[0].GetValue<int64_t>());
	return duckdb::make_uniq<BoundRebuildPartitionData>(partition);
}

} // namespace

duckdb::TableFunctionSet CreateSecEdgarQ6AuxFunctionSet() {
	duckdb::TableFunction function("accel_sec_edgar_q6_aux", {}, SecEdgarQ6AuxExec, SecEdgarQ6AuxBind, SecEdgarQ6AuxInit,
	                               SecEdgarQ6AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateSecEdgarQ6AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_sec_edgar_q6_aux_build", {}, BuildTableFunction, BuildBind, nullptr,
	                               BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateSecEdgarQ6AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_sec_edgar_q6_aux_rebuild_partition", {duckdb::LogicalType::BIGINT},
	                               RebuildPartitionExec, RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
