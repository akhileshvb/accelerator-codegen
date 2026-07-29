#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/tpch_q3_finalize.hpp"
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

inline constexpr const char *kQ3AuxAcceleratorId = "tpch_q3_aux";

using Q3PartitionKey = aux_state::PartitionKey;

inline constexpr Q3PartitionKey kQ3SegmentBuilding = 1;
inline constexpr Q3PartitionKey kQ3SegmentFurniture = 2;
inline constexpr Q3PartitionKey kQ3SegmentHousehold = 3;
inline constexpr Q3PartitionKey kQ3SegmentMachinery = 4;
inline constexpr Q3PartitionKey kQ3SegmentAutomobile = 5;

struct Q3AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	Q3PartitionKey requested_partition = 0;
	bool partition_valid = false;
	int64_t orders_scanned = 0;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetQ3AuxDiagnostics();
void UpdateQ3AuxDiagnostics(const Q3AuxDiagnostics &diagnostics);
const Q3AuxDiagnostics &GetQ3AuxDiagnostics();

bool Q3AuxEnabled();

Q3PartitionKey PartitionKeyFromMktsegment(const std::string &segment);
std::string MktsegmentFromPartitionKey(Q3PartitionKey key);

bool IsQ3DependentTableMutation(const duckdb::TableMutationEvent &event,
                                const std::vector<aux_state::TableDependency> &dependencies);

void RegisterQ3AuxAccelerator(duckdb::Connection &connection);
void BuildQ3AuxState(duckdb::ClientContext &context);
void RebuildQ3AuxPartition(duckdb::ClientContext &context, Q3PartitionKey partition);
void InvalidateQ3AuxStateForTests(duckdb::ClientContext &context);

std::shared_ptr<const Q3SegmentPartition> TryGetReadableSegmentPartition(duckdb::ClientContext &context,
                                                                         Q3PartitionKey partition);

std::vector<Q3OutputRow> ComposeQ3AuxResult(duckdb::ClientContext &context, const std::string &market_segment,
                                            duckdb::date_t cutoff_date);

duckdb::TableFunctionSet CreateTpchQ3AuxFunctionSet();
duckdb::TableFunctionSet CreateQ3AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ3AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
