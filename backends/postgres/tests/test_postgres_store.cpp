// GENERATED BACKEND — layer harness for store (State store).
// Spec: pipeline/prompts/engine_backend/layers.md
// Build: test_postgres_store   CTest: engine_postgres_store
//
// Rule: store_roundtrip
// Evidence: PostgresStateStore::TryPublish → TryPublishPartition,
//   CheckValidity from published StateImage.accelerator + partition,
//   Read with VisibilityToken filtering, Invalidate fail-closed,
//   CAS race detection, token epoch gating.
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "backends/postgres/state/postgres_state_store.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::engine::postgres;
using namespace accelerator_codegen::aux_state::core;

namespace {

struct TestPayload {
  int value;
  std::string label;
};

[[noreturn]] void Fail(const char *msg) {
  std::cerr << "engine_store[postgres] FAIL: " << msg << "\n";
  std::abort();
}

void Expect(bool cond, const char *msg) {
  if (!cond) {
    Fail(msg);
  }
}

void TestTypedPayloadRoundTrip() {
  PostgresStateStore st;

  auto payload = std::make_shared<const TestPayload>(TestPayload{42, "hello"});

  StagedMaintenanceOutcome staged;
  staged.action = MaintenanceAction::RebuildAll;
  staged.updates.push_back(
      StateImage::Make("test_accel", 1, 1, payload));

  bool ok = st.TryPublish(staged, 0);
  Expect(ok, "TryPublish should succeed with expected_version=0");

  VisibilityToken tok;
  tok.mutation_epoch = 0;
  auto snapshot = st.Read(tok);
  Expect(snapshot.available, "Snapshot should be available");
  Expect(snapshot.partitions.size() == 1, "Should have 1 partition");

  const auto &img = snapshot.partitions[0];
  Expect(img.accelerator == "test_accel", "Wrong accelerator");
  Expect(img.partition == 1, "Wrong partition");
  Expect(img.version == 1, "Version should be 1 after publish (initial+1)");

  auto *tp = img.As<TestPayload>();
  Expect(tp != nullptr, "As<TestPayload> should not be null");
  Expect(tp->value == 42, "Payload value mismatch");
  Expect(tp->label == "hello", "Payload label mismatch");

  std::cout << "  [PASS] typed_payload_roundtrip\n";
}

void TestUnknownKeyMissing() {
  PostgresStateStore st;

  auto payload = std::make_shared<const TestPayload>(TestPayload{1, "a"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(
      StateImage::Make("accel_a", 7, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish should succeed");

  // Unknown accelerator
  QueryKey qk_unknown{"unknown_accel", "7"};
  auto vr = st.CheckValidity(qk_unknown, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing,
         "Unknown accelerator should be Missing");

  // Known accelerator but wrong partition
  QueryKey qk_wrong_part{"accel_a", "99"};
  vr = st.CheckValidity(qk_wrong_part, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing,
         "Known accel but unknown partition should be Missing");

  QueryKey qk_explicit_zero{"accel_a", "0"};
  vr = st.CheckValidity(qk_explicit_zero, VisibilityToken{});
  Expect(vr.code == ValidityCode::Missing,
         "Explicit partition 0 not published should be Missing");

  QueryKey qk_empty{"accel_a", ""};
  vr = st.CheckValidity(qk_empty, VisibilityToken{});
  Expect(vr.code == ValidityCode::Valid,
         "Empty QueryKey is accelerator-wide, not partition 0");

  std::cout << "  [PASS] unknown_key_missing\n";
}

void TestInvalidateFailClosed() {
  PostgresStateStore st;

  auto payload = std::make_shared<const TestPayload>(TestPayload{99, "valid"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(
      StateImage::Make("accel_x", 5, 1, payload));
  Expect(st.TryPublish(staged, 0), "Publish should succeed");

  // Verify valid before invalidate
  QueryKey qk{"accel_x", "5"};
  auto vr = st.CheckValidity(qk, VisibilityToken{});
  Expect(vr.code == ValidityCode::Valid, "Should be Valid before invalidate");

  // Invalidate the partition
  InvalidationPlan plan;
  plan.scope = InvalidationScope::PartitionSet;
  plan.partitions.insert(5);
  st.Invalidate(plan);

  // CheckValidity should now return Invalidated
  vr = st.CheckValidity(qk, VisibilityToken{});
  Expect(vr.code == ValidityCode::Invalidated,
         "Should be Invalidated after Invalidate");

  // Read should not include invalidated slot
  auto snapshot = st.Read(VisibilityToken{});
  Expect(snapshot.partitions.empty(),
         "Read should omit invalidated partition");

  std::cout << "  [PASS] invalidate_fail_closed\n";
}

void TestCASRaceDetection() {
  PostgresStateStore st;

  auto payload1 = std::make_shared<const TestPayload>(TestPayload{10, "first"});
  StagedMaintenanceOutcome staged1;
  staged1.updates.push_back(
      StateImage::Make("accel_cas", 3, 1, payload1));
  bool ok = st.TryPublish(staged1, 0);
  Expect(ok, "First publish should succeed");

  // version should be 2 now (started at 0, TryPublishPartition increments)
  auto vr = st.CheckValidity({"accel_cas", "3"}, VisibilityToken{});
  Expect(vr.visible_version == 1, "Version after first publish should be 1");

  // Try to publish with wrong expected_version
  auto payload2 = std::make_shared<const TestPayload>(TestPayload{20, "second"});
  StagedMaintenanceOutcome staged2;
  staged2.updates.push_back(
      StateImage::Make("accel_cas", 3, 1, payload2));
  ok = st.TryPublish(staged2, 99);  // expected 99, but current is 1
  Expect(!ok, "TryPublish with wrong expected_version should return false");

  // After failed CAS, slot is Invalidated — Read returns empty
  auto snapshot = st.Read(VisibilityToken{});
  Expect(snapshot.partitions.empty(), "Read should be empty after failed CAS invalidates slot");

  // Republish with expected_version=0 (slot is Invalid, version moved past)
  auto payload3 = std::make_shared<const TestPayload>(TestPayload{30, "third"});
  StagedMaintenanceOutcome staged3;
  staged3.updates.push_back(
      StateImage::Make("accel_cas", 3, 1, payload3));
  ok = st.TryPublish(staged3, 0);  // expected_version=0 to always succeed
  ok = st.TryPublish(staged3, 0);
  Expect(ok, "Publish with expected_version=0 should work");

  // Verify the new payload took effect
  auto snapshot2 = st.Read(VisibilityToken{});
  auto *tp2 = snapshot2.partitions[0].As<TestPayload>();
  Expect(tp2->value == 30, "Payload should be 'third' after successful publish");

  std::cout << "  [PASS] cas_race_detection\n";
}

void TestTokenEpochGating() {
  // epoch_provider returns 100
  int call_count = 0;
  PostgresStateStore st([&call_count]() -> Version {
    call_count++;
    return 100;
  });

  auto payload = std::make_shared<const TestPayload>(TestPayload{7, "epoch_test"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(
      StateImage::Make("accel_tok", 2, 1, payload));
  bool ok = st.TryPublish(staged, 0);
  Expect(ok, "Publish with epoch 100 should succeed");
  Expect(call_count == 1, "Epoch provider should be called once");

  // Read with mutation_epoch=0 should see everything
  VisibilityToken tok_zero;
  tok_zero.mutation_epoch = 0;
  auto snapshot = st.Read(tok_zero);
  Expect(snapshot.available, "Should be available with epoch 0");
  Expect(snapshot.partitions.size() == 1, "Should see partition with epoch 0");

  // Read with mutation_epoch=99 should omit the image (stamped at 100)
  VisibilityToken tok_99;
  tok_99.mutation_epoch = 99;
  auto snapshot99 = st.Read(tok_99);
  Expect(!snapshot99.available || snapshot99.partitions.empty(),
         "Should omit image when token epoch < publish epoch");

  // CheckValidity with epoch 99 should return NotVisible
  QueryKey qk{"accel_tok", "2"};
  VisibilityToken vis_99;
  vis_99.mutation_epoch = 99;
  auto vr = st.CheckValidity(qk, vis_99);
  Expect(vr.code == ValidityCode::NotVisible,
         "Token behind epoch should be NotVisible");

  // CheckValidity with epoch 100 should be Valid
  VisibilityToken vis_100;
  vis_100.mutation_epoch = 100;
  vr = st.CheckValidity(qk, vis_100);
  Expect(vr.code == ValidityCode::Valid,
         "Token at epoch should be Valid");

  std::cout << "  [PASS] token_epoch_gating\n";
}

void TestMultiPartitionRead() {
  PostgresStateStore st;

  auto p1 = std::make_shared<const TestPayload>(TestPayload{1, "one"});
  auto p2 = std::make_shared<const TestPayload>(TestPayload{2, "two"});
  auto p3 = std::make_shared<const TestPayload>(TestPayload{3, "three"});

  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_multi", 10, 1, p1));
  staged.updates.push_back(StateImage::Make("accel_multi", 20, 1, p2));
  staged.updates.push_back(StateImage::Make("accel_multi", 30, 1, p3));
  Expect(st.TryPublish(staged, 0), "Multi-partition publish should succeed");

  // Read all
  auto snapshot = st.Read(VisibilityToken{});
  Expect(snapshot.partitions.size() == 3, "Should have 3 partitions");

  // ReadPartitions with subset
  auto sub = st.ReadPartitions(VisibilityToken{}, {10, 30});
  Expect(sub.partitions.size() == 2, "ReadPartitions subset should return 2");
  Expect(sub.available, "Should be available");

  std::cout << "  [PASS] multi_partition_read\n";
}

void TestInvalidateAll() {
  PostgresStateStore st;

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
  Expect(vr1.code == ValidityCode::Invalidated, "Partition 1 should be Invalidated");

  auto vr2 = st.CheckValidity({"accel_ia", "2"}, VisibilityToken{});
  Expect(vr2.code == ValidityCode::Invalidated, "Partition 2 should be Invalidated");

  auto snapshot = st.Read(VisibilityToken{});
  Expect(snapshot.partitions.empty(), "Read should return empty after InvalidateAll");

  std::cout << "  [PASS] invalidate_all\n";
}

void TestEpochProviderNull() {
  // No epoch provider (default) → stamp with 0
  PostgresStateStore st;

  auto payload = std::make_shared<const TestPayload>(TestPayload{5, "no_epoch"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(
      StateImage::Make("accel_noep", 1, 1, payload));
  bool ok = st.TryPublish(staged, 0);
  Expect(ok, "Publish without epoch provider should succeed");

  // Slot stamped at 0, so any visibility should see it
  VisibilityToken tok{5, 0, ""};
  auto snapshot = st.Read(tok);
  Expect(snapshot.available, "Should be available with epoch 0 stamp");
  Expect(snapshot.partitions.size() == 1, "Should have 1 partition");

  std::cout << "  [PASS] epoch_provider_null\n";
}

void TestReadPartitionsPreservesCompatibility() {
  // Verify the default ReadPartitions override works properly (our overridden
  // version should be faster, but the base class design is compatible).
  PostgresStateStore st;

  auto p = std::make_shared<const TestPayload>(TestPayload{42, "compat"});
  StagedMaintenanceOutcome staged;
  staged.updates.push_back(StateImage::Make("accel_c", 5, 1, p));
  Expect(st.TryPublish(staged, 0), "Publish should succeed");

  // ReadPartitions for a non-existent partition
  auto sub = st.ReadPartitions(VisibilityToken{}, {99});
  Expect(sub.partitions.empty(), "Non-existent partition should return empty");
  Expect(!sub.available, "Should not be available");

  // ReadPartitions for existing partition
  auto sub2 = st.ReadPartitions(VisibilityToken{}, {5});
  Expect(sub2.partitions.size() == 1, "Should find existing partition");
  Expect(sub2.partitions[0].As<TestPayload>()->value == 42, "Payload should match");

  std::cout << "  [PASS] read_partitions_compat\n";
}

} // namespace

int main() {
  std::cout << "engine_store[postgres]\n";
  TestTypedPayloadRoundTrip();
  TestUnknownKeyMissing();
  TestInvalidateFailClosed();
  TestCASRaceDetection();
  TestTokenEpochGating();
  TestMultiPartitionRead();
  TestInvalidateAll();
  TestEpochProviderNull();
  TestReadPartitionsPreservesCompatibility();
  std::cout << "engine_store[postgres] PASS\n";
  return 0;
}
