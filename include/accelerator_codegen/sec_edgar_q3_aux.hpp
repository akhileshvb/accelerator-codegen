#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/sec_edgar_q3.hpp"
#include "accelerator_codegen/sec_edgar_q3_finalize.hpp"
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

inline constexpr const char *kSecEdgarQ3AuxAcceleratorId = "sec_edgar_q3_aux";

struct SecEdgarQ3AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	int64_t partitions_read = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetSecEdgarQ3AuxDiagnostics();
void UpdateSecEdgarQ3AuxDiagnostics(const SecEdgarQ3AuxDiagnostics &diagnostics);
const SecEdgarQ3AuxDiagnostics &GetSecEdgarQ3AuxDiagnostics();

bool SecEdgarQ3AuxEnabled();

aux_state::ExecutionGateResult
CheckSecEdgarQ3GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch);

bool IsSecEdgarQ3DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies);

void RegisterSecEdgarQ3AuxAccelerator(duckdb::Connection &connection);
void BuildSecEdgarQ3AuxState(duckdb::ClientContext &context);
void RebuildSecEdgarQ3AuxPartition(duckdb::ClientContext &context, SecEdgarQ3PartitionKey partition);
void InvalidateSecEdgarQ3AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const SecEdgarQ3OutputPartition> GetSecEdgarQ3AuxOutputPartition(duckdb::ClientContext &context);

duckdb::unique_ptr<KernelInstance>
CreateSecEdgarQ3AuxFastKernel(std::shared_ptr<const SecEdgarQ3OutputPartition> output);

duckdb::TableFunctionSet CreateSecEdgarQ3AuxFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ3AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ3AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
