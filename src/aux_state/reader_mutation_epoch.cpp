#include "accelerator_codegen/aux_state/reader_mutation_epoch.hpp"

namespace accelerator_codegen::aux_state {

uint64_t ReaderMutationEpochState::PinVisibleMutationEpoch(const std::string &accelerator_id,
                                                            uint64_t current_committed_epoch) {
	auto it = pinned_epochs.find(accelerator_id);
	if (it != pinned_epochs.end()) {
		return it->second;
	}
	pinned_epochs[accelerator_id] = current_committed_epoch;
	return current_committed_epoch;
}

uint64_t ReaderMutationEpochState::GetPinnedVisibleMutationEpoch(const std::string &accelerator_id) const {
	const auto it = pinned_epochs.find(accelerator_id);
	if (it == pinned_epochs.end()) {
		return 0;
	}
	return it->second;
}

void ReaderMutationEpochState::QueryEnd(duckdb::ClientContext &context) {
	(void)context;
	pinned_epochs.clear();
}

uint64_t GetReaderVisibleMutationEpoch(duckdb::ClientContext &context, const std::string &accelerator_id,
                                       uint64_t current_committed_epoch) {
	auto state = context.registered_state->GetOrCreate<ReaderMutationEpochState>(ReaderMutationEpochState::STATE_KEY);
	return state->PinVisibleMutationEpoch(accelerator_id, current_committed_epoch);
}

} // namespace accelerator_codegen::aux_state
