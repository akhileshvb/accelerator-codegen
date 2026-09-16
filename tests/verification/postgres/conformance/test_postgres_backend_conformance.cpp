// Conformance: PostgresAuxBackend routing + AllowsSynchronousRebuildOnCommit.
#include <cassert>
#include <memory>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"

using accelerator_codegen::aux_state::core::AuxTransactionCoordinator;
using accelerator_codegen::aux_state::core::MutationBatch;
using accelerator_codegen::aux_state::core::MutationKind;
using accelerator_codegen::aux_state::engine::postgres::PostgresAuxBackend;
using accelerator_codegen::aux_state::engine::postgres::PostgresStateStore;

namespace {

void CheckNoSyncRebuild() {
  AuxTransactionCoordinator coord;
  PostgresStateStore store([]() { return uint64_t{0}; });
  PostgresAuxBackend backend(coord, store, nullptr, nullptr, nullptr);
  assert(backend.AllowsSynchronousRebuildOnCommit() == false);
}

void CheckCaptureMutationRoutesToCoordinator() {
  AuxTransactionCoordinator coord;
  PostgresStateStore store([]() { return uint64_t{0}; });
  PostgresAuxBackend backend(coord, store, nullptr, nullptr, nullptr);
  // No modules registered: OnMutation still runs (marks dirty path / empty loop).
  MutationBatch mut;
  mut.table = 0;
  mut.kind = MutationKind::Insert;
  backend.CaptureMutation(mut);
  // Abort clears transaction state
  backend.OnAbort();
  assert(!coord.HasPendingWork());
}

} // namespace

int main() {
  CheckNoSyncRebuild();
  CheckCaptureMutationRoutesToCoordinator();
  return 0;
}
