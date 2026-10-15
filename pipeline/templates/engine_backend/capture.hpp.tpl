// GENERATED BACKEND — S1 capture for {{engine_name}}.
// Every change to a tracked table, from any connection, becomes one
// runtime::ColumnarBatch per statement and table, delivered with
// RuntimeHost::OnCapture(txn, depth, batch). Typed columns for
// RuntimeHost::CaptureColumns(table); old values on update and delete;
// no per-row heap allocation.
#pragma once

#include "substrate/aux/runtime/columnar_batch.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

// Converts the engine's change of one statement into a batch of `table`
// (replace the parameter with the engine's mutation event / transition table).
runtime::ColumnarBatch To{{EngineName}}ColumnarBatch(const void *engine_event, core::TableId table);

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
