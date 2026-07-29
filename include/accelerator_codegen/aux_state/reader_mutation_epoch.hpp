#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace accelerator_codegen::aux_state {

// v1: statement-start committed visibility — pin per-accelerator mutation epoch at first aux access per statement.
class ReaderMutationEpochState final : public duckdb::ClientContextState {
public:
	static constexpr const char *STATE_KEY = "accel_reader_mutation_epoch";

	uint64_t PinVisibleMutationEpoch(const std::string &accelerator_id, uint64_t current_committed_epoch);
	uint64_t GetPinnedVisibleMutationEpoch(const std::string &accelerator_id) const;

private:
	std::unordered_map<std::string, uint64_t> pinned_epochs;
};

uint64_t GetReaderVisibleMutationEpoch(duckdb::ClientContext &context, const std::string &accelerator_id,
                                       uint64_t current_committed_epoch);

} // namespace accelerator_codegen::aux_state
