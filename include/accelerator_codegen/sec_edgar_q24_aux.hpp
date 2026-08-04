#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/execution_gate.hpp"
#include "accelerator_codegen/aux_state/invalidation_plan.hpp"
#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/sec_edgar_q24.hpp"
#include "accelerator_codegen/sec_edgar_q24_finalize.hpp"
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

inline constexpr const char *kSecEdgarQ24AuxAcceleratorId = "sec_edgar_q24_aux";

struct SecEdgarQ24AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	int64_t partitions_read = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetSecEdgarQ24AuxDiagnostics();
void UpdateSecEdgarQ24AuxDiagnostics(const SecEdgarQ24AuxDiagnostics &diagnostics);
const SecEdgarQ24AuxDiagnostics &GetSecEdgarQ24AuxDiagnostics();

bool SecEdgarQ24AuxEnabled();

aux_state::ExecutionGateResult
CheckSecEdgarQ24GroupExecutionGate(const aux_state::PartitionedStateStore &store,
                                  const aux_state::TxnInvalidationBuffer &txn_buffer,
                                  uint64_t reader_visible_mutation_epoch);

bool IsSecEdgarQ24DependentTableMutation(const duckdb::TableMutationEvent &event,
                                        const std::vector<aux_state::TableDependency> &dependencies);

aux_state::InvalidationPlan InterpretSecEdgarQ24AuxTableMutation(const duckdb::TableMutationEvent &event,
                                                                 const std::vector<aux_state::TableDependency> &dependencies);

void RegisterSecEdgarQ24AuxAccelerator(duckdb::Connection &connection);
void BuildSecEdgarQ24AuxState(duckdb::ClientContext &context);
void RebuildSecEdgarQ24AuxPartition(duckdb::ClientContext &context, SecEdgarQ24PartitionKey partition);
void InvalidateSecEdgarQ24AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const SecEdgarQ24OutputPartition> GetSecEdgarQ24AuxOutputPartition(duckdb::ClientContext &context);

duckdb::unique_ptr<KernelInstance>
CreateSecEdgarQ24AuxFastKernel(std::shared_ptr<const SecEdgarQ24OutputPartition> output);

duckdb::TableFunctionSet CreateSecEdgarQ24AuxFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ24AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ24AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
