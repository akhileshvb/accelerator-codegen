#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q3.hpp"
#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q3_aux_kernel.hpp"
#include "accelerator_codegen/tpch_q3_incremental.hpp"
#include "accelerator_codegen/tpch_q3_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q3_write_policy.hpp"

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/reader_mutation_epoch.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

struct BoundQ3AuxData : public duckdb::FunctionData {
	explicit BoundQ3AuxData(std::unique_ptr<Q3Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<Q3Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundQ3AuxData>(std::make_unique<Q3Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundQ3AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(Q3PartitionKey partition_p) : partition(partition_p) {
	}
	Q3PartitionKey partition = 0;
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

struct Q3AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_scans;
	duckdb::vector<duckdb::unique_ptr<ChunkSource>> inputs;
	std::unique_ptr<aux_state::ActiveExecutionScope> execution_scope;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
	bool output_exhausted = false;
};

struct Q3AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

Q3Parameters BindQ3AuxParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q3_aux expects exactly 2 arguments: market_segment, cutoff_date");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q3_aux arguments must be non-NULL");
	}
	Q3Parameters params;
	params.market_segment = inputs[0].GetValue<std::string>();
	params.cutoff_date = inputs[1].GetValue<duckdb::date_t>();
	return params;
}

void InitQ3AuxLocalState(duckdb::ClientContext &context, Q3AuxLocalState &local_state, const Q3Parameters &parameters) {
	RecordQ3WorkloadQuery();
	EnsurePendingQ3RebuildApplied(context);
	const auto partition = PartitionKeyFromMktsegment(parameters.market_segment);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ3AuxAcceleratorId);
	auto &maintenance = registry->GetMaintenanceState(kQ3AuxAcceleratorId);
	const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(context, kQ3AuxAcceleratorId,
	                                                                   maintenance.GetCommittedMutationEpoch());
	const auto gate = aux_state::CheckExecutionGate(store, *txn_buffer, partition, reader_epoch);

	ResetQ3AuxDiagnostics();
	Q3AuxDiagnostics diagnostics;
	diagnostics.requested_partition = partition;
	diagnostics.partition_valid = gate.use_aux;

	if (gate.use_aux && partition != 0) {
		auto segment = TryGetReadableSegmentPartition(context, partition);
		if (!segment) {
			local_state.use_fallback = true;
			diagnostics.used_fallback = true;
		} else {
			diagnostics.used_aux_path = true;
			local_state.kernel = CreateQ3AuxFastKernel(ComposeQ3TopK(*segment, parameters.cutoff_date));
			UpdateQ3AuxDiagnostics(diagnostics);
			RecordQ3AuxQueryOutcome(true);
			local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
			    aux_state::ExecutionKind::AuxQuery, kQ3AuxAcceleratorId);
			return;
		}
	} else {
		local_state.use_fallback = true;
		diagnostics.used_fallback = true;
		diagnostics.bypassed_by_txn = gate.bypassed_by_txn;
	}

	UpdateQ3AuxDiagnostics(diagnostics);
	local_state.kernel = CreateQ3Kernel(parameters);
	auto customer_scan = BindNativeScan(context, CreateQ3CustomerScanSpec(parameters));
	auto orders_scan = BindNativeScan(context, CreateQ3OrdersScanSpec(parameters));
	auto lineitem_scan = BindNativeScan(context, CreateQ3LineitemScanSpec(parameters));
	local_state.bound_scans.push_back(std::move(customer_scan));
	local_state.bound_scans.push_back(std::move(orders_scan));
	local_state.bound_scans.push_back(std::move(lineitem_scan));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[0]));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[1]));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[2]));
	RecordQ3AuxQueryOutcome(false);
	local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
	    aux_state::ExecutionKind::FallbackQuery, kQ3AuxAcceleratorId);
}

duckdb::unique_ptr<duckdb::FunctionData> Q3AuxBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
                                                   duckdb::vector<duckdb::LogicalType> &return_types,
                                                   duckdb::vector<duckdb::string> &names) {
	auto parameters = BindQ3AuxParameters(input.inputs);
	return_types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::DECIMAL(38, 4), duckdb::LogicalType::DATE,
	                duckdb::LogicalType::INTEGER};
	names = {"l_orderkey", "revenue", "o_orderdate", "o_shippriority"};
	return duckdb::make_uniq<BoundQ3AuxData>(std::make_unique<Q3Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Q3AuxInit(duckdb::ClientContext &,
                                                               duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<Q3AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> Q3AuxLocalInit(duckdb::ExecutionContext &context,
                                                                   duckdb::TableFunctionInitInput &input,
                                                                   duckdb::GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<BoundQ3AuxData>();
	auto local_state = duckdb::make_uniq<Q3AuxLocalState>();
	InitQ3AuxLocalState(context.client, *local_state, *bind_data.params);
	return std::move(local_state);
}

void Q3AuxExec(duckdb::ClientContext &, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<Q3AuxLocalState>();
	if (local_state.output_exhausted || !local_state.kernel) {
		output.SetCardinality(0);
		return;
	}

	if (local_state.use_fallback && !local_state.inputs_consumed) {
		for (duckdb::idx_t input_id = 0; input_id < local_state.inputs.size(); input_id++) {
			auto &source = local_state.inputs[input_id];
			while (source->Next()) {
				local_state.kernel->ConsumeInput(input_id, source->Chunk());
			}
		}
		local_state.inputs_consumed = true;
		local_state.kernel->Finalize();
		local_state.finalized = true;
	}

	if (!local_state.finalized && !local_state.use_fallback) {
		local_state.kernel->Finalize();
		local_state.finalized = true;
	}

	if (!local_state.kernel->ProduceOutput(output)) {
		local_state.output_exhausted = true;
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

void BuildTableFunction(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<BuildLocalState>();
	if (local_state.done) {
		output.SetCardinality(0);
		return;
	}
	BuildQ3AuxState(context);
	output.SetCardinality(1);
	output.SetValue(0, 0, duckdb::Value("ok"));
	local_state.done = true;
}

void RebuildPartitionExec(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<RebuildPartitionLocalState>();
	if (local_state.done) {
		output.SetCardinality(0);
		return;
	}
	const auto &bind_data = input.bind_data->Cast<BoundRebuildPartitionData>();
	RebuildQ3AuxPartition(context, bind_data.partition);
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

duckdb::unique_ptr<duckdb::LocalTableFunctionState> BuildLocalInit(duckdb::ExecutionContext &,
                                                                   duckdb::TableFunctionInitInput &,
                                                                   duckdb::GlobalTableFunctionState *) {
	return duckdb::make_uniq<BuildLocalState>();
}

duckdb::unique_ptr<duckdb::FunctionData> RebuildPartitionBind(duckdb::ClientContext &,
                                                              duckdb::TableFunctionBindInput &bind_input,
                                                              duckdb::vector<duckdb::LogicalType> &return_types,
                                                              duckdb::vector<duckdb::string> &names) {
	if (bind_input.inputs.size() != 1 || bind_input.inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_q3_aux_rebuild_partition expects VARCHAR market_segment");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	const auto segment = bind_input.inputs[0].GetValue<std::string>();
	return duckdb::make_uniq<BoundRebuildPartitionData>(PartitionKeyFromMktsegment(segment));
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> RebuildPartitionLocalInit(duckdb::ExecutionContext &,
                                                                              duckdb::TableFunctionInitInput &,
                                                                              duckdb::GlobalTableFunctionState *) {
	return duckdb::make_uniq<RebuildPartitionLocalState>();
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ3AuxFunctionSet() {
	duckdb::TableFunction function("accel_tpch_q3_aux",
	                               {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE}, Q3AuxExec, Q3AuxBind,
	                               Q3AuxInit, Q3AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ3AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_q3_aux_build", {}, BuildTableFunction, BuildBind, nullptr, BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ3AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_q3_aux_rebuild_partition", {duckdb::LogicalType::VARCHAR}, RebuildPartitionExec,
	                               RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
