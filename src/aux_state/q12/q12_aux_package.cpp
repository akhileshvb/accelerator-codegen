#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q12.hpp"
#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/tpch_q12_aux_kernel.hpp"
#include "accelerator_codegen/tpch_q12_incremental.hpp"
#include "accelerator_codegen/tpch_q12_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q12_write_policy.hpp"

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/reader_mutation_epoch.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

struct BoundQ12AuxData : public duckdb::FunctionData {
	explicit BoundQ12AuxData(std::unique_ptr<Q12Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<Q12Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundQ12AuxData>(std::make_unique<Q12Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundQ12AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(Q12PartitionKey partition_p) : partition(partition_p) {
	}
	Q12PartitionKey partition = 0;
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

struct Q12AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_scans;
	duckdb::vector<duckdb::unique_ptr<ChunkSource>> inputs;
	std::unique_ptr<aux_state::ActiveExecutionScope> execution_scope;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
	bool output_exhausted = false;
};

struct Q12AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

Q12Parameters BindQ12AuxParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException("accel_tpch_q12_aux expects exactly 3 arguments: shipmode1, shipmode2, start_date");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q12_aux arguments must be non-NULL");
	}
	Q12Parameters params;
	params.shipmode1 = inputs[0].GetValue<std::string>();
	params.shipmode2 = inputs[1].GetValue<std::string>();
	params.start_date = inputs[2].GetValue<duckdb::date_t>();
	params.end_date = MakeQ12Parameters(params.shipmode1, params.shipmode2, params.start_date).end_date;
	return params;
}

void InitQ12AuxLocalState(duckdb::ClientContext &context, Q12AuxLocalState &local_state, const Q12Parameters &parameters) {
	RecordQ12WorkloadQuery();
	EnsurePendingQ12RebuildApplied(context);
	const auto start_partition = PartitionKeyFromReceiptdate(parameters.start_date);
	const auto end_partition = PartitionKeyFromReceiptdate(parameters.end_date);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ12AuxAcceleratorId);

	ResetQ12AuxDiagnostics();
	Q12AuxDiagnostics diagnostics;
	diagnostics.start_partition = start_partition;
	diagnostics.end_partition = end_partition;

	bool window_valid = false;
	{
		auto &maintenance = registry->GetMaintenanceState(kQ12AuxAcceleratorId);
		const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(context, kQ12AuxAcceleratorId,
		                                                                   maintenance.GetCommittedMutationEpoch());
		const auto gate = CheckQ12WindowExecutionGate(store, *txn_buffer, start_partition, end_partition, reader_epoch);
		window_valid = gate.use_aux;
		diagnostics.bypassed_by_txn = gate.bypassed_by_txn;
	}

	if (window_valid) {
		auto rows = ComposeQ12AuxResult(context, parameters.shipmode1, parameters.shipmode2, parameters.start_date);
		diagnostics.used_aux_path = true;
		local_state.kernel = CreateQ12AuxFastKernel(std::move(rows));
		UpdateQ12AuxDiagnostics(diagnostics);
		RecordQ12AuxQueryOutcome(true);
		local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
		    aux_state::ExecutionKind::AuxQuery, kQ12AuxAcceleratorId);
		return;
	}

	local_state.use_fallback = true;
	diagnostics.used_fallback = true;
	UpdateQ12AuxDiagnostics(diagnostics);
	local_state.kernel = CreateQ12Kernel(parameters);
	auto orders_scan = BindNativeScan(context, CreateQ12OrdersScanSpec(parameters));
	auto lineitem_scan = BindNativeScan(context, CreateQ12LineitemScanSpec(parameters));
	local_state.bound_scans.push_back(std::move(orders_scan));
	local_state.bound_scans.push_back(std::move(lineitem_scan));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[0]));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[1]));
	RecordQ12AuxQueryOutcome(false);
	local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
	    aux_state::ExecutionKind::FallbackQuery, kQ12AuxAcceleratorId);
}

duckdb::unique_ptr<duckdb::FunctionData> Q12AuxBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
                                                    duckdb::vector<duckdb::LogicalType> &return_types,
                                                    duckdb::vector<duckdb::string> &names) {
	auto parameters = BindQ12AuxParameters(input.inputs);
	return_types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::HUGEINT, duckdb::LogicalType::HUGEINT};
	names = {"l_shipmode", "high_line_count", "low_line_count"};
	return duckdb::make_uniq<BoundQ12AuxData>(std::make_unique<Q12Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Q12AuxInit(duckdb::ClientContext &,
                                                                duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<Q12AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> Q12AuxLocalInit(duckdb::ExecutionContext &context,
                                                                    duckdb::TableFunctionInitInput &input,
                                                                    duckdb::GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<BoundQ12AuxData>();
	auto local_state = duckdb::make_uniq<Q12AuxLocalState>();
	InitQ12AuxLocalState(context.client, *local_state, *bind_data.params);
	return std::move(local_state);
}

void Q12AuxExec(duckdb::ClientContext &, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<Q12AuxLocalState>();
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
	BuildQ12AuxState(context);
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
	RebuildQ12AuxPartition(context, bind_data.partition);
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
		throw duckdb::BinderException("accel_q12_aux_rebuild_partition expects DATE receiptdate");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	const auto receiptdate = bind_input.inputs[0].GetValue<duckdb::date_t>();
	return duckdb::make_uniq<BoundRebuildPartitionData>(PartitionKeyFromReceiptdate(receiptdate));
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> RebuildPartitionLocalInit(duckdb::ExecutionContext &,
                                                                              duckdb::TableFunctionInitInput &,
                                                                              duckdb::GlobalTableFunctionState *) {
	return duckdb::make_uniq<RebuildPartitionLocalState>();
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ12AuxFunctionSet() {
	duckdb::TableFunction function("accel_tpch_q12_aux",
	                               {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE},
	                               Q12AuxExec, Q12AuxBind, Q12AuxInit, Q12AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ12AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_q12_aux_build", {}, BuildTableFunction, BuildBind, nullptr, BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ12AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_q12_aux_rebuild_partition", {duckdb::LogicalType::DATE}, RebuildPartitionExec,
	                               RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
