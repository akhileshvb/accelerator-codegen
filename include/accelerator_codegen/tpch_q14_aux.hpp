#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/tpch_q14_finalize.hpp"
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

using Q14PartitionKey = aux_state::PartitionKey;

struct Q14MonthTotals {
	Q14AccT total_revenue_raw = 0;
	Q14AccT promo_revenue_raw = 0;
	int64_t row_count = 0;
	bool saw_contribution = false;
};

struct Q14AuxDiagnostics {
	bool used_aux_path = false;
	bool used_fallback = false;
	bool bypassed_by_txn = false;
	Q14PartitionKey requested_partition = 0;
	bool partition_valid = false;
	int64_t build_ns = 0;
	int64_t answer_ns = 0;
};

void ResetQ14AuxDiagnostics();
void UpdateQ14AuxDiagnostics(const Q14AuxDiagnostics &diagnostics);

bool Q14AuxEnabled();
Q14PartitionKey PartitionKeyFromDate(duckdb::date_t shipdate);
Q14PartitionKey PartitionKeyFromStartDate(duckdb::date_t start_date);

bool IsQ14DependentTableMutation(const duckdb::TableMutationEvent &event,
                                 const std::vector<aux_state::TableDependency> &dependencies);

void RegisterQ14AuxAccelerator(duckdb::Connection &connection);
void BuildQ14AuxState(duckdb::ClientContext &context);
void RebuildQ14AuxPartition(duckdb::ClientContext &context, Q14PartitionKey partition);
void InvalidateQ14AuxStateForTests(duckdb::ClientContext &context);

const Q14AuxDiagnostics &GetQ14AuxDiagnostics();
std::shared_ptr<const Q14MonthTotals> TryGetReadableMonthTotals(duckdb::ClientContext &context,
                                                                Q14PartitionKey partition);

duckdb::TableFunctionSet CreateTpchQ14AuxFunctionSet();
duckdb::TableFunctionSet CreateQ14AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ14AuxRebuildPartitionFunctionSet();

} // namespace accelerator_codegen
