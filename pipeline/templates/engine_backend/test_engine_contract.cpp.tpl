// GENERATED BACKEND — engine ↔ substrate CONTRACT harness (implementation-owned).
// Spec: pipeline/prompts/engine_backend/contract_tests.md
// Build: test_{{engine_name}}_contract   CTest: engine_{{engine_name}}_contract
//
// Hard rules: real native path enabled (NOT client-protocol SQL for scan); value asserts;
// real mutation hooks (NOT hand-built MutationBatch-only); no TODO Fail left in main;
// no TPC-H/SEC modules; NEVER SKIP-then-PASS — if engine/SDK unavailable, Fail/abort.
// Soft "throw when flag OFF" is NOT scan coverage.
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_registry.hpp"
#include "substrate/aux/core/aux_types.hpp"
// TODO: #include this engine's scan adapter, AuxBackend, StateStore, MutationCapture.

namespace {

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_contract[{{engine_name}}] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

// ---------------------------------------------------------------------------
// 1) Native scan: seed ≥3 rows, project subset, filter drops ≥1 row, assert values.
//    Force multi-chunk when the adapter batches. Must call the REAL scan path.
// ---------------------------------------------------------------------------
void TestNativeScanContract() {
  // TODO: seed tiny base table via {{engine_name}} native APIs (include a NULL).
  // TODO: ScanNativeBaseTable with projection + residual/pushdown filter.
  // TODO: assert filtered row count + exact projected cell values (golden).
  // TODO: if batched, assert ≥2 consumer callbacks whose concat matches golden.
  Fail("TestNativeScanContract not implemented");
}

// ---------------------------------------------------------------------------
// 2) Mutation: drive insert/update/delete through the REAL capture hook
//    (not only hand-built MutationBatch). Assert table/kind/old/new shape.
// ---------------------------------------------------------------------------
void TestMutationCaptureContract() {
  Fail("TestMutationCaptureContract not implemented");
}

// ---------------------------------------------------------------------------
// 3) Txn: Abort drops work; Commit publishes/invalidates once; recover after abort.
// ---------------------------------------------------------------------------
void TestTxnLifecycleContract() {
  Fail("TestTxnLifecycleContract not implemented");
}

// ---------------------------------------------------------------------------
// 4) Visibility: pre-commit token must not observe committed image; post-commit does.
// ---------------------------------------------------------------------------
void TestVisibilityContract() {
  Fail("TestVisibilityContract not implemented");
}

// ---------------------------------------------------------------------------
// 5) StateStore round-trip for chosen store_mode; Invalidate fails closed.
// ---------------------------------------------------------------------------
void TestStateStoreRoundTrip() {
  Fail("TestStateStoreRoundTrip not implemented");
}

} // namespace

int main() {
  TestNativeScanContract();
  TestMutationCaptureContract();
  TestTxnLifecycleContract();
  TestVisibilityContract();
  TestStateStoreRoundTrip();
  std::cout << "engine_contract[{{engine_name}}] PASS\n";
  return 0;
}
