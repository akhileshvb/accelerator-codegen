#pragma once

#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "accelerator_codegen/aux_state/partitioned_state_store.hpp"

namespace accelerator_codegen::aux_state {

class TxnInvalidationBuffer;

struct ExecutionGateResult {
	bool use_aux = false;
	bool bypassed_by_txn = false;
	bool committed_invalid = false;
};

ExecutionGateResult CheckExecutionGate(const PartitionedStateStore &store, const TxnInvalidationBuffer &txn_buffer,
                                       PartitionKey key);

} // namespace accelerator_codegen::aux_state
