#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine {

// Mini-chunk TPC-H refresh used by aux write-interval sweeps.
// Full official RF sets are 0.1% of SF (huge on SF20); sweeps use small
// alternating RF1/RF2 chunks so incremental maintenance stays measurable.
enum class TpchRfKind : uint8_t { Rf1Insert = 1, Rf2Delete = 2 };

struct TpchRfTableIds {
  core::TableId orders = 0;
  core::TableId lineitem = 0;
  bool watches_orders = false;
  bool watches_lineitem = false;
};

TpchRfTableIds TpchRfTableIdsForQuery(int query_id);

struct TpchRfChunkPlan {
  TpchRfKind kind = TpchRfKind::Rf2Delete;
  int order_count = 1;
  // RF2: keys to delete. RF1: unused (keys allocated at apply time).
  std::vector<int64_t> orderkeys;
};

// Pick the next chunk. Alternates RF2 then RF1 so deletes free keys and
// inserts exercise new-order paths without unbounded DB growth when both run.
TpchRfChunkPlan NextTpchRfChunkPlan(int write_seq, int order_count,
                                    ::duckdb::Connection &connection);

// Apply one RF chunk: mutate DuckDB, then return MutationBatches in the order
// modules expect (orders before lineitems on insert; lineitems before orders
// on delete). Caller runs CaptureMutation / PreCommit / Commit.
struct TpchRfApplyResult {
  std::vector<core::MutationBatch> mutations;
  std::string summary;
};

TpchRfApplyResult ApplyTpchRfChunkSql(::duckdb::Connection &connection,
                                      int query_id, const TpchRfChunkPlan &plan);

// The same chunk as one SQL text (statements separated by `;`), for engines
// whose capture sees the SQL's own mutations (Verus kits map them to their
// tables by name): RF2 deletes the plan's orders with their lineitems; RF1
// inserts `order_count` orders cloned with their lineitems from the largest
// existing orders, under fresh keys above the maximum.
std::string TpchRfChunkStatements(::duckdb::Connection &connection, const TpchRfChunkPlan &plan);

} // namespace accelerator_codegen::aux_state::engine
