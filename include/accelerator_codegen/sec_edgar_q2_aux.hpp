#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/sec_edgar_q2.hpp"
#include "accelerator_codegen/sec_edgar_q2_finalize.hpp"
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

inline constexpr const char *kSecEdgarQ2AuxAcceleratorId = "sec_edgar_q2_aux";

struct SecEdgarQ2AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	int64_t partitions_read = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetSecEdgarQ2AuxDiagnostics();
void UpdateSecEdgarQ2AuxDiagnostics(const SecEdgarQ2AuxDiagnostics &diagnostics);
const SecEdgarQ2AuxDiagnostics &GetSecEdgarQ2AuxDiagnostics();

bool SecEdgarQ2AuxEnabled();

aux_state::ExecutionGateResult
CheckSecEdgarQ2GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch);

bool IsSecEdgarQ2DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies);

void RegisterSecEdgarQ2AuxAccelerator(duckdb::Connection &connection);
void BuildSecEdgarQ2AuxState(duckdb::ClientContext &context);
void RebuildSecEdgarQ2AuxPartition(duckdb::ClientContext &context, SecEdgarQ2PartitionKey partition);
void InvalidateSecEdgarQ2AuxStateForTests(duckdb::ClientContext &context);

std::vector<SecEdgarQ2OutputRow> CollectSecEdgarQ2AuxResult(duckdb::ClientContext &context);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2AuxFastKernel(std::vector<SecEdgarQ2OutputRow> rows);

duckdb::TableFunctionSet CreateSecEdgarQ2AuxFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ2AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ2AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
