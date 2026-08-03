#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/sec_edgar_q6.hpp"
#include "accelerator_codegen/sec_edgar_q6_finalize.hpp"
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

inline constexpr const char *kSecEdgarQ6AuxAcceleratorId = "sec_edgar_q6_aux";

struct SecEdgarQ6AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	int64_t partitions_read = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetSecEdgarQ6AuxDiagnostics();
void UpdateSecEdgarQ6AuxDiagnostics(const SecEdgarQ6AuxDiagnostics &diagnostics);
const SecEdgarQ6AuxDiagnostics &GetSecEdgarQ6AuxDiagnostics();

bool SecEdgarQ6AuxEnabled();

aux_state::ExecutionGateResult
CheckSecEdgarQ6GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch);

bool IsSecEdgarQ6DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies);

void RegisterSecEdgarQ6AuxAccelerator(duckdb::Connection &connection);
void BuildSecEdgarQ6AuxState(duckdb::ClientContext &context);
void RebuildSecEdgarQ6AuxPartition(duckdb::ClientContext &context, SecEdgarQ6PartitionKey partition);
void InvalidateSecEdgarQ6AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const SecEdgarQ6OutputPartition> GetSecEdgarQ6AuxOutputPartition(duckdb::ClientContext &context);

duckdb::unique_ptr<KernelInstance>
CreateSecEdgarQ6AuxFastKernel(std::shared_ptr<const SecEdgarQ6OutputPartition> output);

duckdb::TableFunctionSet CreateSecEdgarQ6AuxFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ6AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ6AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
