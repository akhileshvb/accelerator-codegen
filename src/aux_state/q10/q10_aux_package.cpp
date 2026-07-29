#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q10.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q10_aux_kernel.hpp"
#include "accelerator_codegen/tpch_q10_incremental.hpp"
#include "accelerator_codegen/tpch_q10_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q10_write_policy.hpp"

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/reader_mutation_epoch.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

struct BoundQ10AuxData : public duckdb::FunctionData {
	explicit BoundQ10AuxData(std::unique_ptr<Q10Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<Q10Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundQ10AuxData>(std::make_unique<Q10Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundQ10AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(Q10PartitionKey partition_p) : partition(partition_p) {
	}
	Q10PartitionKey partition = 0;
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

struct Q10AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_scans;
	duckdb::vector<duckdb::unique_ptr<ChunkSource>> inputs;
	std::unique_ptr<aux_state::ActiveExecutionScope> execution_scope;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
	bool output_exhausted = false;
};

struct Q10AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

Q10Parameters BindQ10AuxParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q10_aux expects exactly 1 argument: start_date");
	}
	if (inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q10_aux start_date must be non-NULL");
	}
	return MakeQ10Parameters(inputs[0].GetValue<duckdb::date_t>());
}

void InitQ10AuxLocalState(duckdb::ClientContext &context, Q10AuxLocalState &local_state, const Q10Parameters &parameters) {
	RecordQ10WorkloadQuery();
	EnsurePendingQ10RebuildApplied(context);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ10AuxAcceleratorId);

	ResetQ10AuxDiagnostics();
	Q10AuxDiagnostics diagnostics;

	auto &maintenance = registry->GetMaintenanceState(kQ10AuxAcceleratorId);
	const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(context, kQ10AuxAcceleratorId,
	                                                                   maintenance.GetCommittedMutationEpoch());
	const auto gate = CheckQ10WindowExecutionGate(store, *txn_buffer, parameters.start_date, parameters.end_date,
	                                              reader_epoch);
	diagnostics.bypassed_by_txn = gate.bypassed_by_txn;

	if (gate.use_aux) {
		auto &side_stores = GetQ10SideStores(context);
		auto sort_keys = ComposeQ10AuxResult(context, parameters.start_date);
		diagnostics.used_aux_path = true;
		local_state.kernel = CreateQ10AuxFastKernel(side_stores, std::move(sort_keys));
		UpdateQ10AuxDiagnostics(diagnostics);
		RecordQ10AuxQueryOutcome(true);
		local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
		    aux_state::ExecutionKind::AuxQuery, kQ10AuxAcceleratorId);
		return;
	}

	local_state.use_fallback = true;
	diagnostics.used_fallback = true;
	UpdateQ10AuxDiagnostics(diagnostics);
	local_state.kernel = CreateQ10Kernel(parameters);
	auto nation_scan = BindNativeScan(context, CreateQ10NationScanSpec(parameters));
	auto orders_scan = BindNativeScan(context, CreateQ10OrdersScanSpec(parameters));
	auto lineitem_scan = BindNativeScan(context, CreateQ10LineitemScanSpec(parameters));
	auto customer_scan = BindNativeScan(context, CreateQ10CustomerScanSpec(parameters));
	local_state.bound_scans.push_back(std::move(nation_scan));
	local_state.bound_scans.push_back(std::move(orders_scan));
	local_state.bound_scans.push_back(std::move(lineitem_scan));
	local_state.bound_scans.push_back(std::move(customer_scan));
	for (auto &scan : local_state.bound_scans) {
		local_state.inputs.push_back(CreateNativeChunkSource(context, *scan));
	}
	RecordQ10AuxQueryOutcome(false);
	local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
	    aux_state::ExecutionKind::FallbackQuery, kQ10AuxAcceleratorId);
}

duckdb::unique_ptr<duckdb::FunctionData> Q10AuxBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
                                                    duckdb::vector<duckdb::LogicalType> &return_types,
                                                    duckdb::vector<duckdb::string> &names) {
	auto parameters = BindQ10AuxParameters(input.inputs);
	return_types = {duckdb::LogicalType::BIGINT,
	                  duckdb::LogicalType::VARCHAR,
	                  duckdb::LogicalType::DECIMAL(kQ10RevenueDecimalWidth, kQ10RevenueDecimalScale),
	                  duckdb::LogicalType::DECIMAL(15, 2),
	                  duckdb::LogicalType::VARCHAR,
	                  duckdb::LogicalType::VARCHAR,
	                  duckdb::LogicalType::VARCHAR,
	                  duckdb::LogicalType::VARCHAR};
	names = {"c_custkey", "c_name", "revenue", "c_acctbal", "n_name", "c_address", "c_phone", "c_comment"};
	return duckdb::make_uniq<BoundQ10AuxData>(std::make_unique<Q10Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Q10AuxInit(duckdb::ClientContext &,
                                                                duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<Q10AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> Q10AuxLocalInit(duckdb::ExecutionContext &context,
                                                                    duckdb::TableFunctionInitInput &input,
                                                                    duckdb::GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<BoundQ10AuxData>();
	auto local_state = duckdb::make_uniq<Q10AuxLocalState>();
	InitQ10AuxLocalState(context.client, *local_state, *bind_data.params);
	return std::move(local_state);
}

void Q10AuxExec(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<Q10AuxLocalState>();
	if (local_state.output_exhausted || !local_state.kernel) {
		output.SetCardinality(0);
		return;
	}

	if (local_state.use_fallback && !local_state.inputs_consumed) {
		for (duckdb::idx_t input_id = 0; input_id < 3 && input_id < local_state.inputs.size(); input_id++) {
			auto &source = local_state.inputs[input_id];
			while (source->Next()) {
				local_state.kernel->ConsumeInput(input_id, source->Chunk());
			}
		}
		if (Q10FusedScansEnabled() && local_state.bound_scans.size() >= 4) {
			Q10RunFusedCustomerScan(context, *local_state.kernel, *local_state.bound_scans[3]);
		} else if (local_state.inputs.size() >= 4) {
			auto &source = local_state.inputs[3];
			while (source->Next()) {
				local_state.kernel->ConsumeInput(3, source->Chunk());
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
	BuildQ10AuxState(context);
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
	RebuildQ10AuxPartition(context, bind_data.partition);
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
		throw duckdb::BinderException("accel_q10_aux_rebuild_partition expects DATE orderdate");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	const auto orderdate = bind_input.inputs[0].GetValue<duckdb::date_t>();
	return duckdb::make_uniq<BoundRebuildPartitionData>(PartitionKeyFromOrderdate(orderdate));
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> RebuildPartitionLocalInit(duckdb::ExecutionContext &,
                                                                              duckdb::TableFunctionInitInput &,
                                                                              duckdb::GlobalTableFunctionState *) {
	return duckdb::make_uniq<RebuildPartitionLocalState>();
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ10AuxFunctionSet() {
	duckdb::TableFunction function("accel_tpch_q10_aux", {duckdb::LogicalType::DATE}, Q10AuxExec, Q10AuxBind, Q10AuxInit,
	                               Q10AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ10AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_q10_aux_build", {}, BuildTableFunction, BuildBind, nullptr, BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ10AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_q10_aux_rebuild_partition", {duckdb::LogicalType::DATE}, RebuildPartitionExec,
	                               RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
