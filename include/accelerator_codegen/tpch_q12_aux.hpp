#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/tpch_q12_finalize.hpp"
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

inline constexpr const char *kQ12AuxAcceleratorId = "tpch_q12_aux";

using Q12PartitionKey = aux_state::PartitionKey;

struct Q12AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	Q12PartitionKey start_partition = 0;
	Q12PartitionKey end_partition = 0;
	int64_t partitions_merged = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetQ12AuxDiagnostics();
void UpdateQ12AuxDiagnostics(const Q12AuxDiagnostics &diagnostics);
const Q12AuxDiagnostics &GetQ12AuxDiagnostics();

bool Q12AuxEnabled();

Q12PartitionKey PartitionKeyFromReceiptdate(duckdb::date_t receiptdate);
duckdb::date_t ReceiptdateFromPartitionKey(Q12PartitionKey key);

aux_state::ExecutionGateResult CheckQ12WindowExecutionGate(const aux_state::PartitionedStateStore &store,
                                                           const aux_state::TxnInvalidationBuffer &txn_buffer,
                                                           Q12PartitionKey start_partition,
                                                           Q12PartitionKey end_partition,
                                                           uint64_t reader_visible_mutation_epoch);

bool IsQ12DependentTableMutation(const duckdb::TableMutationEvent &event,
                                 const std::vector<aux_state::TableDependency> &dependencies);

void RegisterQ12AuxAccelerator(duckdb::Connection &connection);
void BuildQ12AuxState(duckdb::ClientContext &context);
void RebuildQ12AuxPartition(duckdb::ClientContext &context, Q12PartitionKey partition);
void InvalidateQ12AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const Q12DayPartition> TryGetReadableQ12DayPartition(duckdb::ClientContext &context,
                                                                    Q12PartitionKey partition);

std::vector<Q12OutputRow> ComposeQ12AuxResult(duckdb::ClientContext &context, const std::string &shipmode1,
                                              const std::string &shipmode2, duckdb::date_t start_date);

duckdb::TableFunctionSet CreateTpchQ12AuxFunctionSet();
duckdb::TableFunctionSet CreateQ12AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ12AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
