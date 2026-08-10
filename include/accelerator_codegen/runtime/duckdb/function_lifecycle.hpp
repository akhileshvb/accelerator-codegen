#pragma once

namespace accelerator_codegen {

// Documents the intended separation between bind-time metadata and execution-time state.
//
// Bind phase:
// - validates user-visible arguments
// - determines output schema
// - resolves stable identifiers and conversion metadata
//
// Global/local init:
// - acquires execution-context-specific state
// - creates scan state tied to the current transaction
//
// Execution:
// - drives scans and kernels
// - checks interruption at chunk boundaries
// - produces output DataChunks
//
// Transaction-specific objects must not be stored in bind data.
struct FunctionLifecycle final {};

} // namespace accelerator_codegen
