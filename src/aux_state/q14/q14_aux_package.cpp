#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q14.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q14_aux_kernel.hpp"
#include "accelerator_codegen/tpch_q14_maintenance.hpp"
#include "accelerator_codegen/tpch_q14_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q14_write_policy.hpp"

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

struct BoundQ14AuxData : public duckdb::FunctionData {
	explicit BoundQ14AuxData(std::unique_ptr<Q14Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<Q14Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundQ14AuxData>(std::make_unique<Q14Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundQ14AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(std::unique_ptr<Q14Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<Q14Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundRebuildPartitionData>(std::make_unique<Q14Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundRebuildPartitionData>().params);
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

struct Q14AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_scans;
	duckdb::vector<duckdb::unique_ptr<ChunkSource>> inputs;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
	bool output_exhausted = false;
};

struct Q14AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

Q14Parameters BindQ14AuxParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q14_aux expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q14_aux start_date must be non-NULL");
	}
	return MakeQ14Parameters(inputs[0].GetValue<duckdb::date_t>());
}

void InitQ14AuxLocalState(duckdb::ClientContext &context, Q14AuxLocalState &local_state, const Q14Parameters &parameters) {
	RecordQ14WorkloadQuery();
	EnsurePendingQ14RebuildApplied(context);
	const auto partition = PartitionKeyFromStartDate(parameters.start_date);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore("tpch_q14_aux");
	const auto gate = aux_state::CheckExecutionGate(store, *txn_buffer, partition);

	ResetQ14AuxDiagnostics();
	Q14AuxDiagnostics diagnostics;
	diagnostics.requested_partition = partition;
	diagnostics.partition_valid = gate.use_aux;

	if (gate.use_aux) {
		auto totals = TryGetReadableMonthTotals(context, partition);
		if (!totals) {
			local_state.use_fallback = true;
			diagnostics.used_fallback = true;
		} else {
			diagnostics.used_aux_path = true;
			local_state.kernel = CreateQ14AuxFastKernel(*totals);
			UpdateQ14AuxDiagnostics(diagnostics);
			RecordQ14AuxQueryOutcome(true);
			return;
		}
	} else {
		local_state.use_fallback = true;
		diagnostics.used_fallback = true;
		diagnostics.bypassed_by_txn = gate.bypassed_by_txn;
	}

	UpdateQ14AuxDiagnostics(diagnostics);

	local_state.kernel = CreateQ14Kernel(parameters);
	auto part_scan = BindNativeScan(context, CreateQ14PartScanSpec(parameters));
	auto lineitem_scan = BindNativeScan(context, CreateQ14LineitemScanSpec(parameters));
	local_state.bound_scans.push_back(std::move(part_scan));
	local_state.bound_scans.push_back(std::move(lineitem_scan));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[0]));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[1]));
	RecordQ14AuxQueryOutcome(false);
}

duckdb::unique_ptr<duckdb::FunctionData> Q14AuxBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
                                                    duckdb::vector<duckdb::LogicalType> &return_types,
                                                    duckdb::vector<duckdb::string> &names) {
	auto parameters = BindQ14AuxParameters(input.inputs);
	return_types = {duckdb::LogicalType::DOUBLE};
	names = {"promo_revenue"};
	return duckdb::make_uniq<BoundQ14AuxData>(std::make_unique<Q14Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Q14AuxInit(duckdb::ClientContext &,
                                                                duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<Q14AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> Q14AuxLocalInit(duckdb::ExecutionContext &context,
                                                                    duckdb::TableFunctionInitInput &input,
                                                                    duckdb::GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<BoundQ14AuxData>();
	auto local_state = duckdb::make_uniq<Q14AuxLocalState>();
	InitQ14AuxLocalState(context.client, *local_state, *bind_data.params);
	return std::move(local_state);
}

void Q14AuxExec(duckdb::ClientContext &, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<Q14AuxLocalState>();
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
	BuildQ14AuxState(context);
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
	const auto partition = PartitionKeyFromStartDate(bind_data.params->start_date);
	RebuildQ14AuxPartition(context, partition);
	output.SetCardinality(1);
	output.SetValue(0, 0, duckdb::Value("ok"));
	local_state.done = true;
}

duckdb::unique_ptr<duckdb::FunctionData> RebuildPartitionBind(duckdb::ClientContext &,
                                                              duckdb::TableFunctionBindInput &bind_input,
                                                              duckdb::vector<duckdb::LogicalType> &return_types,
                                                              duckdb::vector<duckdb::string> &names) {
	if (bind_input.inputs.size() != 1 || bind_input.inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_q14_aux_rebuild_partition expects DATE start_date");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	return duckdb::make_uniq<BoundRebuildPartitionData>(
	    std::make_unique<Q14Parameters>(MakeQ14Parameters(bind_input.inputs[0].GetValue<duckdb::date_t>())));
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ14AuxFunctionSet() {
	duckdb::TableFunction function("accel_tpch_q14_aux", {duckdb::LogicalType::DATE}, Q14AuxExec, Q14AuxBind, Q14AuxInit,
	                               Q14AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ14AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_q14_aux_build", {}, BuildTableFunction, BuildBind, nullptr, BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ14AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_q14_aux_rebuild_partition", {duckdb::LogicalType::DATE}, RebuildPartitionExec,
	                               RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
