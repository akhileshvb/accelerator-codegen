#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/sec_edgar_q1.hpp"
#include "accelerator_codegen/sec_edgar_q1_finalize.hpp"
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

inline constexpr const char *kSecEdgarQ1AuxAcceleratorId = "sec_edgar_q1_aux";

struct SecEdgarQ1AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	int64_t partitions_read = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetSecEdgarQ1AuxDiagnostics();
void UpdateSecEdgarQ1AuxDiagnostics(const SecEdgarQ1AuxDiagnostics &diagnostics);
const SecEdgarQ1AuxDiagnostics &GetSecEdgarQ1AuxDiagnostics();

bool SecEdgarQ1AuxEnabled();

aux_state::ExecutionGateResult
CheckSecEdgarQ1GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch);

bool IsSecEdgarQ1DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies);

void RegisterSecEdgarQ1AuxAccelerator(duckdb::Connection &connection);
void BuildSecEdgarQ1AuxState(duckdb::ClientContext &context);
void RebuildSecEdgarQ1AuxPartition(duckdb::ClientContext &context, SecEdgarQ1PartitionKey partition);
void InvalidateSecEdgarQ1AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const SecEdgarQ1GroupPartition> TryGetReadableSecEdgarQ1GroupPartition(duckdb::ClientContext &context,
                                                                                       SecEdgarQ1PartitionKey partition);

std::vector<SecEdgarQ1OutputRow> CollectSecEdgarQ1AuxResult(duckdb::ClientContext &context);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1AuxFastKernel(std::vector<SecEdgarQ1OutputRow> rows);

duckdb::TableFunctionSet CreateSecEdgarQ1AuxFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ1AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ1AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
