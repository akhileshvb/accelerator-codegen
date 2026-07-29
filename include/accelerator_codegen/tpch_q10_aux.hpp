#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/tpch_q10_finalize.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {
class ClientContext;
class Connection;
class DatabaseInstance;
}

namespace accelerator_codegen::aux_state {
struct TableDependency;
}

namespace accelerator_codegen {

inline constexpr const char *kQ10AuxAcceleratorId = "tpch_q10_aux";

using Q10PartitionKey = aux_state::PartitionKey;

struct Q10AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	int64_t partitions_merged = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetQ10AuxDiagnostics();
void UpdateQ10AuxDiagnostics(const Q10AuxDiagnostics &diagnostics);
const Q10AuxDiagnostics &GetQ10AuxDiagnostics();

bool Q10AuxEnabled();

Q10PartitionKey PartitionKeyFromOrderdate(duckdb::date_t orderdate);
duckdb::date_t OrderdateFromPartitionKey(Q10PartitionKey key);

Q10SideStores &GetQ10SideStores(duckdb::ClientContext &context);
void BuildQ10SideStores(duckdb::ClientContext &context);

aux_state::ExecutionGateResult CheckQ10WindowExecutionGate(const aux_state::PartitionedStateStore &store,
                                                           const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                           duckdb::date_t start_date, duckdb::date_t end_date,
                                                           uint64_t reader_visible_mutation_epoch);

bool IsQ10DependentTableMutation(const duckdb::TableMutationEvent &event,
                                 const std::vector<aux_state::TableDependency> &dependencies);

void RegisterQ10AuxAccelerator(duckdb::Connection &connection);
void BuildQ10AuxState(duckdb::ClientContext &context);
void RebuildQ10AuxPartition(duckdb::ClientContext &context, Q10PartitionKey partition);
void InvalidateQ10AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const Q10MonthPartition> TryGetReadableQ10MonthPartition(duckdb::ClientContext &context,
                                                                         Q10PartitionKey partition);

std::vector<Q10RawSortKey> ComposeQ10AuxResult(duckdb::ClientContext &context, duckdb::date_t start_date);

duckdb::TableFunctionSet CreateTpchQ10AuxFunctionSet();
duckdb::TableFunctionSet CreateQ10AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ10AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
