#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "accelerator_codegen/tpch_q1_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {
class ClientContext;
class Connection;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

inline constexpr const char *kQ1AuxAcceleratorId = "tpch_q1_aux";

using Q1PartitionKey = aux_state::PartitionKey;

struct Q1AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	Q1PartitionKey cutoff_partition = 0;
	int64_t partitions_merged = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetQ1AuxDiagnostics();
void UpdateQ1AuxDiagnostics(const Q1AuxDiagnostics &diagnostics);
const Q1AuxDiagnostics &GetQ1AuxDiagnostics();

bool Q1AuxEnabled();
Q1PartitionKey PartitionKeyFromShipdate(duckdb::date_t shipdate);
Q1PartitionKey PartitionKeyFromCutoff(const Q1Parameters &parameters);

aux_state::ExecutionGateResult CheckQ1PrefixExecutionGate(const aux_state::PartitionedStateStore &store,
                                                            const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                            Q1PartitionKey cutoff);

bool IsQ1DependentTableMutation(const duckdb::TableMutationEvent &event,
                                const std::vector<aux_state::TableDependency> &dependencies);

void RegisterQ1AuxAccelerator(duckdb::Connection &connection);
void BuildQ1AuxState(duckdb::ClientContext &context);
void RebuildQ1AuxPartition(duckdb::ClientContext &context, Q1PartitionKey partition);
void InvalidateQ1AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const Q1DayPartition> TryGetReadableDayPartition(duckdb::ClientContext &context,
                                                                 Q1PartitionKey partition);

std::vector<Q1OutputRow> MergeQ1PartitionsUpTo(duckdb::ClientContext &context, Q1PartitionKey cutoff);

duckdb::unique_ptr<KernelInstance> CreateQ1AuxFastKernel(std::vector<Q1OutputRow> rows);

duckdb::TableFunctionSet CreateTpchQ1AuxFunctionSet();
duckdb::TableFunctionSet CreateQ1AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ1AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
