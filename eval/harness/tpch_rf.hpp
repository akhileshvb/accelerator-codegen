// SCAFFOLDING
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {

// Mini-chunk TPC-H refresh used by aux write-interval sweeps and the RF
// differential. Full official RF sets are 0.1% of SF (huge on SF20); these
// use small alternating RF1/RF2 chunks so incremental maintenance stays
// measurable.
enum class TpchRfKind : uint8_t { Rf1Insert = 1, Rf2Delete = 2 };

struct TpchRfChunkPlan {
  TpchRfKind kind = TpchRfKind::Rf2Delete;
  int order_count = 1;
  // RF2: keys to delete. RF1: unused (keys allocated when the SQL is built).
  std::vector<int64_t> orderkeys;
};

// Pick the next chunk. Alternates RF2 then RF1 so deletes free keys and
// inserts exercise new-order paths without unbounded DB growth when both run.
TpchRfChunkPlan NextTpchRfChunkPlan(int write_seq, int order_count,
                                    ::duckdb::Connection &connection);

// The chunk as one SQL text (statements separated by `;`): RF2 deletes the
// plan's orders with their lineitems; RF1 inserts `order_count` orders cloned
// with their lineitems from the largest existing orders, under fresh keys
// above the maximum.
std::string TpchRfChunkStatements(::duckdb::Connection &connection, const TpchRfChunkPlan &plan);

// Apply chunk `write_seq` (NextTpchRfChunkPlan) to `runtime` as one captured
// write (DuckdbRuntime::ApplyWrite): mutation capture is on for the chunk, so
// the accelerator receives the SQL's own mutations, projected as it declares.
// Capture is restored to its previous state afterwards (hand-built probe
// writes, DuckdbRuntime::Apply, deliver their own mutation). `connection` is
// the runtime's connection.
void ApplyTpchRfChunk(::duckdb::Connection &connection,
                      engine::duckdb::DuckdbRuntime &runtime, int write_seq,
                      int order_count = 1);

} // namespace accelerator_codegen::aux_state::harness
