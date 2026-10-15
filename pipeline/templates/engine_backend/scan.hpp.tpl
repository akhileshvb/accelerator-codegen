// GENERATED BACKEND — S5/S8 engine-local scans for {{engine_name}}.
// Partitioned, typed, chunked scans of a base table inside a build snapshot,
// in the engine's own native types (NOT a portable DataChunk IR, NOT a client
// SQL round-trip). Partitions run in parallel on the thread budget
// (ThreadBudget::ParallelFor); chunk callbacks may be invoked concurrently,
// one partition per thread.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

struct {{EngineName}}ScanSpec {
  std::string table;                     // the engine's name for the base table
  std::vector<std::string> columns;      // projection, order preserved
  // Optional residual filter — shape is engine-specific.
};

// One chunk of projected rows in the engine's native layout (rename/widen per
// SDK hints + analysis); valid for the duration of the callback.
struct {{EngineName}}Chunk;
using {{EngineName}}ChunkConsumer = std::function<void(const {{EngineName}}Chunk &chunk, std::size_t partition)>;

// Scans `spec` inside the open build snapshot, `partitions` ways.
void Scan{{EngineName}}Table(const {{EngineName}}ScanSpec &spec, std::size_t partitions,
                             const {{EngineName}}ChunkConsumer &consumer);

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
