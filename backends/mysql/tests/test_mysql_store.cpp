// GENERATED BACKEND — layer harness for store (State store).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_mysql_store   CTest: engine_mysql_store
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "backends/mysql/state/mysql_state_store.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::engine::mysql;
using namespace accelerator_codegen::aux_state::core;

namespace {

struct TestPayload {
  int value;
  std::string label;
};

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_store[mysql] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) { Fail(msg); }
}

void TestTypedPayloadRoundTrip() {
  MysqlStateStore st;
  auto payload = std::make_shared<const TestPayload>(TestPayload{42, "hello"});
  StagedMaintenanceOutcome staged;
  staged.action = MaintenanceAction::RebuildAll;
  staged.updates.push_back(StateImage::Make("test_accel", 1, 1, payload));
  Expect(st.TryPublish(staged, 0), "TryPublish should succeed");

  VisibilityToken tok;
  auto snapshot = st.Read(tok);
  Expect(snapshot.available, "Snapshot should be available");
  Expect(snapshot.partitions.size() == 1, "Should have 1 partition");

  const auto &img = snapshot.partitions[0];
  Expect(img.accelerator == "test_accel", "Wrong accelerator");
  Expect(img.partition == 1, "Wrong partition");
  Expect(img.version == 1, "Version should be 1");

  auto *tp = img.As<TestPayload>();
  Expect(tp != nullptr, "As<TestPayload> should not be null");
  Expect(tp->value == 42, "Payload value mismatch");
  Expect(tp->label == "hello", "Payload label mismatch");
  std::cout << "  [PASS] typed_payload_roundtrip\n";
}

void TestUnknownKeyMissing() {
  MysqlStateStore st;
  auto payload = std::make_shared<const TestPayload>(TestPayload{1, "a"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_a", 7, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish should succeed");

  QueryKey qk_unknown{"unknown_accel", "7"};
  auto vr = st.CheckValidity(qk_unknown, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing, "Unknown accelerator should be Missing");

  QueryKey qk_wrong_part{"accel_a", "99"};
  vr = st.CheckValidity(qk_wrong_part, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing, "Known accel wrong partition Missing");

  QueryKey qk_explicit_zero{"accel_a", "0"};
  vr = st.CheckValidity(qk_explicit_zero, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing, "Explicit partition 0 Missing");

  QueryKey qk_empty{"accel_a", ""};
  vr = st.CheckValidity(qk_empty, VisibilityToken{});
  Expect(vr.code == ValidityCode::Valid,
         "Empty QueryKey is accelerator-wide, not partition 0");
  std::cout << "  [PASS] unknown_key_missing\n";
}

void TestInvalidateFailClosed() {
  MysqlStateStore st;
  auto payload = std::make_shared<const TestPayload>(TestPayload{99, "valid"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_x", 5, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish should succeed");

  QueryKey qk{"accel_x", "5"};
  auto vr = st.CheckValidity(qk, VisibilityToken{});
  Expect(vr.code == ValidityCode::Valid, "Should be Valid before invalidate");

  InvalidationPlan plan;
  plan.scope = InvalidationScope::PartitionSet;
  plan.partitions.insert(5);
  st.Invalidate(plan);

  vr = st.CheckValidity(qk, VisibilityToken{});
  Expect(vr.code == ValidityCode::Invalidated, "Should be Invalidated");
  auto snapshot = st.Read(VisibilityToken{});
  Expect(snapshot.partitions.empty(), "Read should omit invalidated");
  std::cout << "  [PASS] invalidate_fail_closed\n";
}

void TestCASRaceDetection() {
  MysqlStateStore st;
  auto payload1 = std::make_shared<const TestPayload>(TestPayload{10, "first"});
  StagedMaintenanceOutcome staged1;
  staged1.updates.push_back(StateImage::Make("accel_cas", 3, 1, payload1));
  Expect(st.TryPublish(staged1, 0), "First publish unconditional");

  auto vr = st.CheckValidity({"accel_cas", "3"}, VisibilityToken{});
  Expect(vr.visible_version == 1, "Version should be 1");

  // Try with non-matching expected_version (5 != 1) — should fail
  auto payload2 = std::make_shared<const TestPayload>(TestPayload{20, "second"});
  StagedMaintenanceOutcome staged2;
  staged2.updates.push_back(StateImage::Make("accel_cas", 3, 2, payload2));
  bool ok2 = st.TryPublish(staged2, 5);
  Expect(!ok2, "Stale expected_version should return false");

  // The Substrate's TryPublishPartition invalidates on CAS fail, so Read is empty now
  // Verify: the original slot is no longer Valid (CAS-fail invalidation)
  vr = st.CheckValidity({"accel_cas", "3"}, VisibilityToken{});
  Expect(vr.code == ValidityCode::Invalidated, "Slot should be Invalidated after CAS fail");

  // Re-publish with expected_version=0 (unconditional) — succeeds
  auto payload3 = std::make_shared<const TestPayload>(TestPayload{30, "third"});
  StagedMaintenanceOutcome staged3;
  staged3.updates.push_back(StateImage::Make("accel_cas", 3, 3, payload3));
  Expect(st.TryPublish(staged3, 0), "Re-publish unconditional should succeed");

  auto snapshot = st.Read(VisibilityToken{});
  Expect(snapshot.partitions.size() == 1, "Should have 1 partition");
  auto *tp = snapshot.partitions[0].As<TestPayload>();
  Expect(tp->value == 30, "Third payload should be visible");
  std::cout << "  [PASS] cas_race_detection\n";
}

void TestTokenEpochGating() {
  int call_count = 0;
  auto epoch_fn = [&call_count]() -> Version { call_count++; return 100; };
  MysqlStateStore st(epoch_fn);

  auto payload = std::make_shared<const TestPayload>(TestPayload{7, "epoch"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_tok", 2, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish with epoch provider");
  Expect(call_count == 1, "Epoch provider called once");

  VisibilityToken tok_99;
  tok_99.mutation_epoch = 99;
  auto snap = st.Read(tok_99);
  Expect(!snap.available || snap.partitions.empty(), "Omit img when token epoch < publish epoch");

  QueryKey qk{"accel_tok", "2"};
  VisibilityToken vis_99;
  vis_99.mutation_epoch = 99;
  auto vr = st.CheckValidity(qk, vis_99);
  Expect(vr.code == ValidityCode::NotVisible, "Token behind epoch NotVisible");

  VisibilityToken vis_100;
  vis_100.mutation_epoch = 100;
  vr = st.CheckValidity(qk, vis_100);
  Expect(vr.code == ValidityCode::Valid, "Token at epoch Valid");
  std::cout << "  [PASS] token_epoch_gating\n";
}

void TestMultiPartitionRead() {
  MysqlStateStore st;
  auto p1 = std::make_shared<const TestPayload>(TestPayload{1, "one"});
  auto p2 = std::make_shared<const TestPayload>(TestPayload{2, "two"});
  auto p3 = std::make_shared<const TestPayload>(TestPayload{3, "three"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_multi", 10, 1, p1));
  staged.updates.push_back(StateImage::Make("accel_multi", 20, 1, p2));
  staged.updates.push_back(StateImage::Make("accel_multi", 30, 1, p3));
  Expect(st.TryPublish(staged, 0), "Multi-partition publish");

  auto snapshot = st.Read(VisibilityToken{});
  Expect(snapshot.partitions.size() == 3, "Should have 3 partitions");

  auto sub = st.ReadPartitions(VisibilityToken{}, {10, 30});
  Expect(sub.partitions.size() == 2, "ReadPartitions subset return 2");
  Expect(sub.available, "Should be available");
  std::cout << "  [PASS] multi_partition_read\n";
}

void TestInvalidateAll() {
  MysqlStateStore st;
  auto p1 = std::make_shared<const TestPayload>(TestPayload{1, "a"});
  auto p2 = std::make_shared<const TestPayload>(TestPayload{2, "b"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_ia", 1, 1, p1));
  staged.updates.push_back(StateImage::Make("accel_ia", 2, 1, p2));
  Expect(st.TryPublish(staged, 0), "Publish should succeed");

  InvalidationPlan plan;
  plan.scope = InvalidationScope::AllPartitions;
  st.Invalidate(plan);

  auto vr1 = st.CheckValidity({"accel_ia", "1"}, VisibilityToken{});
  Expect(vr1.code == ValidityCode::Invalidated, "P1 Invalidated");
  auto vr2 = st.CheckValidity({"accel_ia", "2"}, VisibilityToken{});
  Expect(vr2.code == ValidityCode::Invalidated, "P2 Invalidated");
  Expect(st.Read(VisibilityToken{}).partitions.empty(), "Read empty after InvalidateAll");
  std::cout << "  [PASS] invalidate_all\n";
}

void TestEpochProviderNull() {
  MysqlStateStore st;
  auto payload = std::make_shared<const TestPayload>(TestPayload{5, "no_epoch"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_noep", 1, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish without epoch provider");

  VisibilityToken tok{5, 0, ""};
  auto snapshot = st.Read(tok);
  Expect(snapshot.available, "Should be available epoch 0 stamp");
  Expect(snapshot.partitions.size() == 1, "Should have 1 partition");
  std::cout << "  [PASS] epoch_provider_null\n";
}

void TestEmptyQueryKeyAcceleratorWide() {
  MysqlStateStore st;
  QueryKey empty{"accel_q6", ""};
  auto vr = st.CheckValidity(empty, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing, "Empty store empty key Missing");

  auto payload = std::make_shared<const TestPayload>(TestPayload{1, "month"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_q6", 1000000000ULL, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish non-zero partition");

  vr = st.CheckValidity(empty, VisibilityToken{});
  Expect(vr.code == ValidityCode::Valid, "Empty key Valid when any slot published");
  Expect(st.CheckValidity({"other", ""}, VisibilityToken{}).code ==
             ValidityCode::Missing,
         "Wrong accelerator empty key Missing");

  InvalidationPlan plan;
  plan.scope = InvalidationScope::AllPartitions;
  st.Invalidate(plan);
  vr = st.CheckValidity(empty, VisibilityToken{});
  Expect(vr.code == ValidityCode::Invalidated,
         "Empty key Invalidated after InvalidateAll");
  std::cout << "  [PASS] empty_query_key_accelerator_wide\n";
}

void TestReadPartitionsPreservesCompatibility() {
  MysqlStateStore st;
  auto p = std::make_shared<const TestPayload>(TestPayload{42, "compat"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_c", 5, 1, p));
  Expect(st.TryPublish(staged, 0), "Publish");

  auto sub = st.ReadPartitions(VisibilityToken{}, {99});
  Expect(sub.partitions.empty(), "Non-existent partition empty");
  Expect(!sub.available, "Not available");

  auto sub2 = st.ReadPartitions(VisibilityToken{}, {5});
  Expect(sub2.partitions.size() == 1, "Should find existing partition");
  Expect(sub2.partitions[0].As<TestPayload>()->value == 42, "Payload match");
  std::cout << "  [PASS] read_partitions_compat\n";
}

} // namespace

int main() {
  std::cout << "engine_store[mysql]\n";
  TestTypedPayloadRoundTrip();
  TestUnknownKeyMissing();
  TestInvalidateFailClosed();
  TestCASRaceDetection();
  TestTokenEpochGating();
  TestMultiPartitionRead();
  TestInvalidateAll();
  TestEpochProviderNull();
  TestEmptyQueryKeyAcceleratorWide();
  TestReadPartitionsPreservesCompatibility();
  std::cout << "engine_store[mysql] PASS\n";
  return 0;
}
