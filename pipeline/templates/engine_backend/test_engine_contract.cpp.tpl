// GENERATED BACKEND — engine <-> runtime-skeleton CONTRACT harness (implementation-owned).
// Spec: pipeline/prompts/engine_backend/contract_tests.md
// Build: test_{{engine_name}}_contract   CTest: engine_{{engine_name}}_contract
//
// The toy module (per-key sum of one table; reference: SumModule in
// eval/tests/test_aux_runtime.cpp) on a runtime::RuntimeHost with THIS backend's
// EngineSlots, driven by real engine writes. Value asserts; no TPC-H/SEC modules;
// NEVER SKIP-then-PASS — if the engine/SDK is unavailable, Fail/abort.
#include <cstdint>
#include <iostream>

#include "substrate/aux/runtime/runtime_host.hpp"
// TODO: #include this backend's host, capture and scan, and the toy module.

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

// S1: INSERT/UPDATE/DELETE from two connections -> one batch per statement, old/new values.
void TestCapture() { Fail("TestCapture not implemented"); }
// S2-S4: epochs in visibility order, exact reader epoch, floor <= every open reader.
void TestEpochs() { Fail("TestEpochs not implemented"); }
// S5/S8: snapshot at epoch b contains exactly the commits <= b; typed chunked scan values.
void TestBuildSnapshotAndScan() { Fail("TestBuildSnapshotAndScan not implemented"); }
// S6: seed -> background build -> read at epoch; writes during a build are caught up.
void TestRuntime() { Fail("TestRuntime not implemented"); }

} // namespace

int main() {
  TestCapture();
  TestEpochs();
  TestBuildSnapshotAndScan();
  TestRuntime();
  std::cout << "engine_contract[{{engine_name}}] PASS\n";
  return 0;
}
