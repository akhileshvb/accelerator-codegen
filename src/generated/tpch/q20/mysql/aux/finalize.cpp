// ── Postgres finalize payload ────────────────────────────────────────────────
// Engine adapters (PostgreSQL) call these free functions to convert between
// the typed state representation and a portable byte-stream representation.
// The byte layout is identical to the one used by the DuckDB-centric
// Encode/Decode functions in module.cpp, but exposed at a separate
// compilation boundary that Postgres can link without pulling in DuckDB.

#include "tpch/q20/mysql/aux/module.hpp"

#include <cstdint>
#include <vector>

namespace accelerator_codegen::aux_state::query {

// Re-export so the Postgres adapter can find the symbols via a single TU.
// The actual implementations live in module.cpp.
extern std::vector<uint8_t> EncodeTpchQ20State(const TpchQ20State &state);
extern bool DecodeTpchQ20State(const std::vector<uint8_t> &bytes,
                               TpchQ20State &state);

} // namespace accelerator_codegen::aux_state::query
