#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/sec_edgar_q4.hpp"
#include "accelerator_codegen/sec_edgar_q4_finalize.hpp"
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

inline constexpr const char *kSecEdgarQ4AuxAcceleratorId = "sec_edgar_q4_aux";

struct SecEdgarQ4AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	int64_t partitions_read = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetSecEdgarQ4AuxDiagnostics();
void UpdateSecEdgarQ4AuxDiagnostics(const SecEdgarQ4AuxDiagnostics &diagnostics);
const SecEdgarQ4AuxDiagnostics &GetSecEdgarQ4AuxDiagnostics();

bool SecEdgarQ4AuxEnabled();

aux_state::ExecutionGateResult
CheckSecEdgarQ4GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch);

bool IsSecEdgarQ4DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies);

void RegisterSecEdgarQ4AuxAccelerator(duckdb::Connection &connection);
void BuildSecEdgarQ4AuxState(duckdb::ClientContext &context);
void RebuildSecEdgarQ4AuxPartition(duckdb::ClientContext &context, SecEdgarQ4PartitionKey partition);
void InvalidateSecEdgarQ4AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const SecEdgarQ4OutputPartition> GetSecEdgarQ4AuxOutputPartition(duckdb::ClientContext &context);

duckdb::unique_ptr<KernelInstance>
CreateSecEdgarQ4AuxFastKernel(std::shared_ptr<const SecEdgarQ4OutputPartition> output);

duckdb::TableFunctionSet CreateSecEdgarQ4AuxFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ4AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ4AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
