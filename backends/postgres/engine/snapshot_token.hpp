#pragma once

#include "substrate/aux/core/aux_types.hpp"

#include <string>

namespace accelerator_codegen::aux_state::engine::postgres {

// Pin VisibilityToken to the exact active PostgreSQL snapshot (TokenForSnapshot).
// Same S ⇒ same t (frozen mutation_epoch for that S encoding); new S' ⇒ new t'.
// mutation_epoch must be the aux epoch at first observation of S so concurrent
// Commit bumps hide newer Aux from readers still on S.
//
// Pin timing: RegisterSnapshotPinExecutorHook installs ExecutorStart_hook so
// the freeze happens when the plan begins under the already-pushed active
// snapshot S (PortalStart/ProcessQuery → PushActiveSnapshot → ExecutorStart).
// Runtime::Execute / CurrentVisibility also pin (idempotent per S). Pins are
// stored in a per-snapshot map so nested SPI with S' cannot clobber S.
// Residual: concurrent Commit bump *before* first observation of S — readers
// who pin after the bump take the new epoch; pin-before-bump is filtered by
// TokenForSnapshot_stable_under_concurrent_publish (publish epoch > pin epoch).
core::VisibilityToken MakeTokenForActiveSnapshot(
    core::Version mutation_epoch = 0);

// Encode the active PG snapshot identity (xmin/xmax/xip/…) without epoch.
std::string EncodeActiveSnapshotIdentity();

// Statement-stable TokenForSnapshot: freeze (S_encoding, epoch) on first see
// of S; reuse for that encoding forever in-process. Subsequent Commit/PreCommit
// epoch bumps do not alter an already-frozen pin for S.
core::VisibilityToken PinTokenForActiveSnapshot();

// Drop all frozen pins (call on backend xact end so a later xact with a reused
// snapshot encoding does not inherit a stale epoch from a prior xact).
void ClearSnapshotPins();

// Register ExecutorStart_hook that pins when ActiveSnapshot is set. Idempotent.
void RegisterSnapshotPinExecutorHook();

} // namespace accelerator_codegen::aux_state::engine::postgres
