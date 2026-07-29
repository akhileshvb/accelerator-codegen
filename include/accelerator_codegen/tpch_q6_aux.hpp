#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/tpch_q6_finalize.hpp"
#include "accelerator_codegen/tpch_q6.hpp"
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

inline constexpr const char *kQ6AuxAcceleratorId = "tpch_q6_aux";

using Q6PartitionKey = aux_state::PartitionKey;

struct Q6AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	Q6PartitionKey start_partition = 0;
	Q6PartitionKey end_partition = 0;
	int64_t partitions_merged = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetQ6AuxDiagnostics();
void UpdateQ6AuxDiagnostics(const Q6AuxDiagnostics &diagnostics);
const Q6AuxDiagnostics &GetQ6AuxDiagnostics();

bool Q6AuxEnabled();

inline Q6PartitionKey Q6PartitionKeyFromShipdate(duckdb::date_t shipdate) {
	return static_cast<Q6PartitionKey>(shipdate.days);
}

inline duckdb::date_t Q6ShipdateFromPartitionKey(Q6PartitionKey key) {
	return duckdb::date_t {static_cast<int32_t>(key)};
}

aux_state::ExecutionGateResult CheckQ6WindowExecutionGate(const aux_state::PartitionedStateStore &store,
                                                           const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                           Q6PartitionKey start_partition,
                                                           Q6PartitionKey end_partition,
                                                           uint64_t reader_visible_mutation_epoch);

bool IsQ6DependentTableMutation(const duckdb::TableMutationEvent &event,
                                 const std::vector<aux_state::TableDependency> &dependencies);

void RegisterQ6AuxAccelerator(duckdb::Connection &connection);
void BuildQ6AuxState(duckdb::ClientContext &context);
void RebuildQ6AuxPartition(duckdb::ClientContext &context, Q6PartitionKey partition);
void InvalidateQ6AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const Q6DayPartition> TryGetReadableQ6DayPartition(duckdb::ClientContext &context,
                                                                    Q6PartitionKey partition);

Q6AccT ComposeQ6AuxRevenue(duckdb::ClientContext &context, const Q6Parameters &parameters);

duckdb::TableFunctionSet CreateTpchQ6AuxFunctionSet();
duckdb::TableFunctionSet CreateQ6AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ6AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
