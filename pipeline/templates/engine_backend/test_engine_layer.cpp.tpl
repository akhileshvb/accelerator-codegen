// GENERATED BACKEND — layer harness for {{layer_id}} ({{layer_title}}).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_{{engine_name}}_{{layer_id}}   CTest: engine_{{engine_name}}_{{layer_id}}
//
// Value asserts. No TPC-H / SEC modules. NEVER SKIP-then-PASS.
#include <cstdint>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_{{layer_id}}[{{engine_name}}] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

void TestLayer() {
  Fail("TestLayer ({{layer_id}}) not implemented");
}

} // namespace

int main() {
  TestLayer();
  std::cout << "engine_{{layer_id}}[{{engine_name}}] PASS\n";
  return 0;
}
