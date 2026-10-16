// SCAFFOLDING
// BasicAuxTxnBuffer: a pending-changes flag for modules that keep their own buffers.
#pragma once

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

// A small common base deliberately leaves the contents query-owned. Query
// modules can use typed buffers without making the runtime know about their
// state layout.
class BasicAuxTxnBuffer final : public AuxTxnBuffer {
public:
  bool HasPendingChanges() const override { return pending_changes; }

  void MarkPending() { pending_changes = true; }

  void Clear() override { pending_changes = false; }

private:
  bool pending_changes = false;
};

} // namespace accelerator_codegen::aux_state::core
