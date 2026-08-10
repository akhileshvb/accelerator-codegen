#pragma once

#include "accelerator_codegen/aux_state/invalidation_plan.hpp"

namespace accelerator_codegen::aux_state {

class IncrementalTxnBuffer {
public:
	virtual ~IncrementalTxnBuffer() = default;

	virtual void Clear() = 0;
	virtual bool HasPendingChanges() const = 0;
	virtual InvalidationPlan ExtraInvalidationPlan() const = 0;
};

} // namespace accelerator_codegen::aux_state
