#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q1_fused.hpp"
#include "accelerator_codegen/tpch_q1_maintenance.hpp"
#include "accelerator_codegen/tpch_q1_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q1_write_policy.hpp"

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

struct BoundQ1AuxData : public duckdb::FunctionData {
	explicit BoundQ1AuxData(std::unique_ptr<Q1Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<Q1Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundQ1AuxData>(std::make_unique<Q1Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundQ1AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(Q1PartitionKey partition_p) : partition(partition_p) {
	}
	Q1PartitionKey partition = 0;
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

struct Q1AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	Q1FusedKernel *fused_kernel = nullptr;
	duckdb::unique_ptr<duckdb::FunctionData> bound_scan;
	duckdb::unique_ptr<ChunkSource> input;
	std::unique_ptr<aux_state::ActiveExecutionScope> execution_scope;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
};

struct Q1AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

void InitQ1AuxLocalState(duckdb::ClientContext &context, Q1AuxLocalState &local_state, const Q1Parameters &parameters) {
	RecordQ1WorkloadQuery();
	EnsurePendingQ1RebuildApplied(context);
	const auto cutoff = PartitionKeyFromCutoff(parameters);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ1AuxAcceleratorId);
	auto &maintenance = registry->GetMaintenanceState(kQ1AuxAcceleratorId);
	const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(context, kQ1AuxAcceleratorId,
	                                                                   maintenance.GetCommittedMutationEpoch());
	const auto gate = CheckQ1PrefixExecutionGate(store, *txn_buffer, cutoff, reader_epoch);

	ResetQ1AuxDiagnostics();
	Q1AuxDiagnostics diagnostics;
	diagnostics.cutoff_partition = cutoff;

	if (Q1AuxEnabled() && gate.use_aux && store.ValidPartitionCount() > 0) {
		const auto start = std::chrono::steady_clock::now();
		auto rows = MergeQ1PartitionsUpTo(context, cutoff);
		diagnostics.used_aux_path = true;
		diagnostics.answer_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		local_state.kernel = CreateQ1AuxFastKernel(std::move(rows));
		UpdateQ1AuxDiagnostics(diagnostics);
		RecordQ1AuxQueryOutcome(true);
		local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
		    aux_state::ExecutionKind::AuxQuery, kQ1AuxAcceleratorId);
		return;
	}

	local_state.use_fallback = true;
	diagnostics.used_fallback = true;
	diagnostics.bypassed_by_txn = gate.bypassed_by_txn;
	UpdateQ1AuxDiagnostics(diagnostics);

	local_state.kernel = CreateQ1FusedKernel(parameters);
	local_state.fused_kernel = dynamic_cast<Q1FusedKernel *>(local_state.kernel.get());
	auto scan = BindNativeScan(context, CreateQ1ScanSpec(parameters));
	local_state.bound_scan = std::move(scan);
	local_state.input = CreateNativeChunkSource(context, *local_state.bound_scan);
	RecordQ1AuxQueryOutcome(false);
	local_state.execution_scope = std::make_unique<aux_state::ActiveExecutionScope>(
	    aux_state::ExecutionKind::FallbackQuery, kQ1AuxAcceleratorId);
}

duckdb::unique_ptr<duckdb::FunctionData> Q1AuxBind(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
                                                   duckdb::vector<duckdb::LogicalType> &return_types,
                                                   duckdb::vector<duckdb::string> &names) {
	if (input.inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q1_aux expects exactly 1 argument: day_offset");
	}
	if (input.inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q1_aux day_offset must be non-NULL");
	}
	auto day_offset = input.inputs[0].GetValue<int32_t>();
	auto parameters = MakeQ1Parameters(day_offset);
	return_types = {
	    duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::DECIMAL(38, 2), duckdb::LogicalType::DECIMAL(38, 2), duckdb::LogicalType::DECIMAL(38, 4),
	    duckdb::LogicalType::DECIMAL(38, 6), duckdb::LogicalType::DOUBLE, duckdb::LogicalType::DOUBLE,
	    duckdb::LogicalType::DOUBLE, duckdb::LogicalType::BIGINT};
	names = {"l_returnflag", "l_linestatus", "sum_qty", "sum_base_price", "sum_disc_price", "sum_charge", "avg_qty",
	         "avg_price", "avg_disc", "count_order"};
	return duckdb::make_uniq<BoundQ1AuxData>(std::make_unique<Q1Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Q1AuxInit(duckdb::ClientContext &,
                                                               duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<Q1AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> Q1AuxLocalInit(duckdb::ExecutionContext &context,
                                                                   duckdb::TableFunctionInitInput &input,
                                                                   duckdb::GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<BoundQ1AuxData>();
	auto local_state = duckdb::make_uniq<Q1AuxLocalState>();
	InitQ1AuxLocalState(context.client, *local_state, *bind_data.params);
	return std::move(local_state);
}

void Q1AuxExec(duckdb::ClientContext &context, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<Q1AuxLocalState>();
	if (!local_state.kernel) {
		output.SetCardinality(0);
		return;
	}

	if (local_state.use_fallback && !local_state.inputs_consumed) {
		if (!local_state.fused_kernel || !local_state.bound_scan) {
			throw duckdb::InternalException("Q1 aux fallback missing fused kernel or bound scan");
		}
		local_state.fused_kernel->RunFusedScan(context, *local_state.bound_scan);
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
	BuildQ1AuxState(context);
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
	RebuildQ1AuxPartition(context, bind_data.partition);
	output.SetCardinality(1);
	output.SetValue(0, 0, duckdb::Value("ok"));
	local_state.done = true;
}

duckdb::unique_ptr<duckdb::FunctionData> RebuildPartitionBind(duckdb::ClientContext &,
                                                              duckdb::TableFunctionBindInput &bind_input,
                                                              duckdb::vector<duckdb::LogicalType> &return_types,
                                                              duckdb::vector<duckdb::string> &names) {
	if (bind_input.inputs.size() != 1 || bind_input.inputs[0].IsNull()) {
		throw duckdb::BinderException("accel_q1_aux_rebuild_partition expects DATE shipdate");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	const auto shipdate = bind_input.inputs[0].GetValue<duckdb::date_t>();
	return duckdb::make_uniq<BoundRebuildPartitionData>(PartitionKeyFromShipdate(shipdate));
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ1AuxFunctionSet() {
	duckdb::TableFunction function("accel_tpch_q1_aux", {duckdb::LogicalType::INTEGER}, Q1AuxExec, Q1AuxBind, Q1AuxInit,
	                               Q1AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ1AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_q1_aux_build", {}, BuildTableFunction, BuildBind, nullptr, BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ1AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_q1_aux_rebuild_partition", {duckdb::LogicalType::DATE}, RebuildPartitionExec,
	                               RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
