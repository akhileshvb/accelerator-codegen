// GENERATED BACKEND — mutation capture helpers for MySQL triggers.
// Parses trigger arguments (per-column strings) into typed MutationBatch.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

struct CaptureColumn {
  core::AuxType type = core::AuxType::String;
  bool nullable = true;
};

void RegisterCaptureTableSchema(core::TableId table_id,
                                std::vector<CaptureColumn> cols);

std::optional<std::vector<CaptureColumn>>
LookupCaptureSchema(core::TableId table_id);

core::AuxValue ParseCaptureValue(const char *raw, std::size_t len,
                                 const CaptureColumn &col);

// Build a MutationBatch from trigger argument strings.
// args[0] = table_id, args[1] = kind ("0"/"1"/"2"),
// then old-row column values followed by new-row column values.
// Uses registered schema for typed parsing.
// Returns nullopt on parse failure.
std::optional<core::MutationBatch>
BuildMutationBatchFromTriggerArgs(std::size_t arg_count, char **args,
                                   unsigned long *lengths);

} // namespace accelerator_codegen::aux_state::engine::mysql
