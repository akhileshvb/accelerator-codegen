#include "accelerator_codegen/aux_state/execution_gate.hpp"

#include "accelerator_codegen/aux_state/txn_invalidation_buffer.hpp"

namespace accelerator_codegen::aux_state {

ExecutionGateResult CheckExecutionGate(const PartitionedStateStore &store, const TxnInvalidationBuffer &txn_buffer,
                                       PartitionKey key, uint64_t reader_visible_mutation_epoch) {
	ExecutionGateResult result;
	if (txn_buffer.IsAllBypassed()) {
		result.bypassed_by_txn = true;
		result.committed_invalid = true;
		return result;
	}
	if (txn_buffer.IsPartitionBypassed(key)) {
		result.bypassed_by_txn = true;
		return result;
	}
	if (store.GetStatus(key) != PartitionStatus::Valid) {
		result.committed_invalid = true;
		return result;
	}
	if (!store.GetReadablePayload(key)) {
		result.committed_invalid = true;
		return result;
	}
	// v1: Valid status is authoritative — maintenance invalidates or republishes on relevant
	// mutations. valid_through_mutation_epoch is publish metadata; reader_visible_mutation_epoch
	// is pinned at statement start for future txn-snapshot integration (see substrate_contract.md).
	(void)reader_visible_mutation_epoch;
	result.use_aux = true;
	return result;
}

} // namespace accelerator_codegen::aux_state
