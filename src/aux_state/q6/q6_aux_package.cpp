#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q6.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/tpch_q6_aux_kernel.hpp"
#include "accelerator_codegen/tpch_q6_incremental.hpp"
#include "accelerator_codegen/tpch_q6_rebuild_policy.hpp"
#include "accelerator_codegen/tpch_q6_write_policy.hpp"

#include <chrono>

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/aux_state/execution_load.hpp"
#include "accelerator_codegen/aux_state/reader_mutation_epoch.hpp"
#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"
#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kLineitemDecimalWidth = 15;
constexpr uint8_t kLineitemDecimalScale = 2;

struct BoundQ6AuxData : public duckdb::FunctionData {
	explicit BoundQ6AuxData(std::unique_ptr<Q6Parameters> params_p) : params(std::move(params_p)) {
	}
	std::unique_ptr<Q6Parameters> params;
	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
		return duckdb::make_uniq<BoundQ6AuxData>(std::make_unique<Q6Parameters>(*params));
	}
	bool Equals(const duckdb::FunctionData &other_p) const override {
		return params->Equals(*other_p.Cast<BoundQ6AuxData>().params);
	}
};

struct BoundRebuildPartitionData : public duckdb::FunctionData {
	explicit BoundRebuildPartitionData(Q6PartitionKey partition_p) : partition(partition_p) {
	}
	Q6PartitionKey partition = 0;
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

struct Q6AuxLocalState : public duckdb::LocalTableFunctionState {
	duckdb::unique_ptr<KernelInstance> kernel;
	duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> bound_scans;
	duckdb::vector<duckdb::unique_ptr<ChunkSource>> inputs;
	std::unique_ptr<aux_state::ActiveExecutionScope> execution_scope;
	bool use_fallback = false;
	bool inputs_consumed = false;
	bool finalized = false;
	bool output_exhausted = false;
};

struct Q6AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override {
		return 1;
	}
};

int64_t ConvertToScaledDecimal(duckdb::ClientContext &context, const duckdb::Value &input,
                               const std::string &parameter_name) {
	duckdb::Value cast_value;
	std::string error;
	const auto target_type = duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale);
	if (!input.TryCastAs(context, target_type, cast_value, &error, false)) {
		throw duckdb::BinderException("Could not cast %s to %s: %s", parameter_name, target_type.ToString(), error);
	}
	return cast_value.GetValueUnsafe<int64_t>();
}

Q6Parameters BindQ6AuxParameters(duckdb::ClientContext &context, const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException("accel_tpch_q6_aux expects exactly 3 arguments: start_date, discount, quantity");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q6_aux arguments must be non-NULL");
	}
	Q6Parameters params;
	params.start_date = inputs[0].GetValue<duckdb::date_t>();
	params.end_date = Q6ComputeExclusiveEndDate(params.start_date);
	const auto discount_raw = ConvertToScaledDecimal(context, inputs[1], "discount");
	const auto quantity_raw = ConvertToScaledDecimal(context, inputs[2], "quantity");
	params.discount_lower_raw = discount_raw - 1;
	params.discount_upper_raw = discount_raw + 1;
	params.quantity_upper_raw = quantity_raw;
	return params;
}

NativeScanSpec CreateQ6FallbackScanSpec(const Q6Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	scan_spec.filters = {
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::DATE(parameters.start_date)},
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
	    {"l_discount", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DECIMAL(parameters.discount_lower_raw, kLineitemDecimalWidth, kLineitemDecimalScale)},
	    {"l_discount", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
	     duckdb::Value::DECIMAL(parameters.discount_upper_raw, kLineitemDecimalWidth, kLineitemDecimalScale)},
	    {"l_quantity", duckdb::ExpressionType::COMPARE_LESSTHAN,
	     duckdb::Value::DECIMAL(parameters.quantity_upper_raw, kLineitemDecimalWidth, kLineitemDecimalScale)},
	};
	return scan_spec;
}

void InitQ6AuxLocalState(duckdb::ClientContext &context, Q6AuxLocalState &local_state, const Q6Parameters &parameters) {
	RecordQ6WorkloadQuery();
	EnsurePendingQ6RebuildApplied(context);
	const auto start_partition = Q6PartitionKeyFromShipdate(parameters.start_date);
	const auto end_partition = Q6PartitionKeyFromShipdate(parameters.end_date);
	auto txn_buffer = aux_state::GetOrCreateTxnInvalidationBuffer(context);
	auto registry = aux_state::AcceleratorStateRegistry::GetOrCreate(*context.db);
	auto &store = registry->GetStore(kQ6AuxAcceleratorId);

	ResetQ6AuxDiagnostics();
	Q6AuxDiagnostics diagnostics;
	diagnostics.start_partition = start_partition;
	diagnostics.end_partition = end_partition;

	bool window_valid = false;
	{
		auto &maintenance = registry->GetMaintenanceState(kQ6AuxAcceleratorId);
		const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(context, kQ6AuxAcceleratorId,
		                                                                   maintenance.GetCommittedMutationEpoch());
		const auto gate = CheckQ6WindowExecutionGate(store, *txn_buffer, start_partition, end_partition, reader_epoch);
		window_valid = gate.use_aux;
		diagnostics.bypassed_by_txn = gate.bypassed_by_txn;
	}

	if (window_valid) {
		const auto answer_start = std::chrono::steady_clock::now();
		auto revenue = ComposeQ6AuxRevenue(context, parameters);
		diagnostics.answer_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - answer_start).count();
		diagnostics.used_aux_path = true;
		diagnostics.partitions_merged = GetQ6AuxDiagnostics().partitions_merged;
		local_state.kernel = CreateQ6AuxFastKernel(revenue);
		UpdateQ6AuxDiagnostics(diagnostics);
		RecordQ6AuxQueryOutcome(true);
		local_state.execution_scope =
		    std::make_unique<aux_state::ActiveExecutionScope>(aux_state::ExecutionKind::AuxQuery, kQ6AuxAcceleratorId);
		return;
	}

	local_state.use_fallback = true;
	diagnostics.used_fallback = true;
	UpdateQ6AuxDiagnostics(diagnostics);
	local_state.kernel = CreateQ6Kernel(parameters);
	auto lineitem_scan = BindNativeScan(context, CreateQ6FallbackScanSpec(parameters));
	local_state.bound_scans.push_back(std::move(lineitem_scan));
	local_state.inputs.push_back(CreateNativeChunkSource(context, *local_state.bound_scans[0]));
	RecordQ6AuxQueryOutcome(false);
	local_state.execution_scope =
	    std::make_unique<aux_state::ActiveExecutionScope>(aux_state::ExecutionKind::FallbackQuery, kQ6AuxAcceleratorId);
}

} // namespace

namespace {

duckdb::unique_ptr<duckdb::FunctionData> Q6AuxBind(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input,
                                                    duckdb::vector<duckdb::LogicalType> &return_types,
                                                    duckdb::vector<duckdb::string> &names) {
	auto parameters = BindQ6AuxParameters(context, input.inputs);
	return_types = {duckdb::LogicalType::DECIMAL(kQ6RevenueDecimalWidth, kQ6RevenueDecimalScale)};
	names = {"revenue"};
	return duckdb::make_uniq<BoundQ6AuxData>(std::make_unique<Q6Parameters>(parameters));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> Q6AuxInit(duckdb::ClientContext &,
                                                                duckdb::TableFunctionInitInput &) {
	return duckdb::make_uniq<Q6AuxGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> Q6AuxLocalInit(duckdb::ExecutionContext &context,
                                                                    duckdb::TableFunctionInitInput &input,
                                                                    duckdb::GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<BoundQ6AuxData>();
	auto local_state = duckdb::make_uniq<Q6AuxLocalState>();
	InitQ6AuxLocalState(context.client, *local_state, *bind_data.params);
	return std::move(local_state);
}

void Q6AuxExec(duckdb::ClientContext &, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &local_state = input.local_state->Cast<Q6AuxLocalState>();
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
	BuildQ6AuxState(context);
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
	RebuildQ6AuxPartition(context, bind_data.partition);
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
		throw duckdb::BinderException("accel_q6_aux_rebuild_partition expects DATE shipdate");
	}
	return_types = {duckdb::LogicalType::VARCHAR};
	names = {"status"};
	const auto shipdate = bind_input.inputs[0].GetValue<duckdb::date_t>();
	return duckdb::make_uniq<BoundRebuildPartitionData>(Q6PartitionKeyFromShipdate(shipdate));
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> RebuildPartitionLocalInit(duckdb::ExecutionContext &,
                                                                              duckdb::TableFunctionInitInput &,
                                                                              duckdb::GlobalTableFunctionState *) {
	return duckdb::make_uniq<RebuildPartitionLocalState>();
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ6AuxFunctionSet() {
	duckdb::TableFunction function("accel_tpch_q6_aux",
	                               {duckdb::LogicalType::DATE, duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
	                                duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale)},
	                               Q6AuxExec, Q6AuxBind, Q6AuxInit, Q6AuxLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ6AuxBuildFunctionSet() {
	duckdb::TableFunction function("accel_q6_aux_build", {}, BuildTableFunction, BuildBind, nullptr, BuildLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

duckdb::TableFunctionSet CreateQ6AuxRebuildPartitionFunctionSet() {
	duckdb::TableFunction function("accel_q6_aux_rebuild_partition", {duckdb::LogicalType::DATE}, RebuildPartitionExec,
	                               RebuildPartitionBind, nullptr, RebuildPartitionLocalInit);
	return duckdb::TableFunctionSet(std::move(function));
}

} // namespace accelerator_codegen
