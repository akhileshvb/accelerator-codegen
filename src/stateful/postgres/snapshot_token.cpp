#include "accelerator_codegen/stateful/postgres/snapshot_token.hpp"

#include "accelerator_codegen/stateful/postgres/aux_mutation_epoch.hpp"

#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

extern "C" {
#include "postgres.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "utils/snapmgr.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {

namespace {

std::mutex g_pin_mu;
// Per-snapshot freeze: snap_id → mutation_epoch at first observation of S.
// Nested SPI / ExecutorStart for a different S' must not clobber S's pin
// (single-slot overwrite would re-pin S at a later epoch after SPI pops).
std::unordered_map<std::string, core::Version> g_pins;

std::string EncodeSnapshot(Snapshot snap) {
  if (snap == nullptr) {
    return "snapshot=none";
  }
  std::ostringstream oss;
  oss << "xmin=" << snap->xmin << ";xmax=" << snap->xmax
      << ";xcnt=" << snap->xcnt;
  if (snap->xip != nullptr && snap->xcnt > 0) {
    oss << ";xip=";
    for (uint32 i = 0; i < snap->xcnt; ++i) {
      if (i)
        oss << ',';
      oss << snap->xip[i];
    }
  }
  oss << ";suboverflowed=" << (snap->suboverflowed ? 1 : 0)
      << ";takenDuringRecovery=" << (snap->takenDuringRecovery ? 1 : 0);
  return oss.str();
}

} // namespace

std::string EncodeActiveSnapshotIdentity() {
  return EncodeSnapshot(GetActiveSnapshot());
}

core::VisibilityToken
MakeTokenForActiveSnapshot(core::Version mutation_epoch) {
  core::VisibilityToken tok;
  tok.mutation_epoch = mutation_epoch;
  tok.backend_token = EncodeActiveSnapshotIdentity();
  return tok;
}

core::VisibilityToken PinTokenForActiveSnapshot() {
  const std::string snap_id = EncodeActiveSnapshotIdentity();
  std::lock_guard<std::mutex> lock(g_pin_mu);
  auto it = g_pins.find(snap_id);
  if (it != g_pins.end()) {
    // Same S: reuse frozen epoch (Commit/PreCommit bumps do not move the pin).
    return MakeTokenForActiveSnapshot(it->second);
  }
  // First observation of this S: freeze current aux epoch with S.
  // Concurrent Commit that already bumped epoch means this pin takes the new
  // epoch; TokenForSnapshot_stable_under_concurrent_publish covers the
  // complementary case (pin before bump ⇒ Hit filters newer publishes).
  const core::Version epoch = AuxMutationEpoch();
  g_pins.emplace(snap_id, epoch);
  return MakeTokenForActiveSnapshot(epoch);
}

void ClearSnapshotPins() {
  std::lock_guard<std::mutex> lock(g_pin_mu);
  g_pins.clear();
}

namespace {

ExecutorStart_hook_type prev_ExecutorStart = nullptr;
bool g_executor_hook_registered = false;

void AccelExecutorStart_hook(QueryDesc *queryDesc, int eflags) {
  // PortalStart/ProcessQuery has already PushActiveSnapshot'd statement S.
  // Freeze TokenForSnapshot(S, t) at plan start — before set-returning UDF
  // bodies run — so concurrent Commit between S establishment and UDF entry
  // cannot widen the pin window beyond ExecutorStart.
  if (ActiveSnapshotSet()) {
    (void)PinTokenForActiveSnapshot();
  }
  if (prev_ExecutorStart) {
    prev_ExecutorStart(queryDesc, eflags);
  } else {
    standard_ExecutorStart(queryDesc, eflags);
  }
}

} // namespace

void RegisterSnapshotPinExecutorHook() {
  if (g_executor_hook_registered) {
    return;
  }
  prev_ExecutorStart = ExecutorStart_hook;
  ExecutorStart_hook = AccelExecutorStart_hook;
  g_executor_hook_registered = true;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
