// VersionedAuxStore: version memory cap (Options::max_version_bytes) and the
// background reclaimer (Options::async_reclaim).
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "substrate/aux/state/versioned_aux_store.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::core;
using Epoch = VersionedAuxStore::Epoch;

namespace {

void Check(bool ok, const std::string &what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << "\n";
    std::exit(1);
  }
}

std::atomic<int> g_alive {0};
std::atomic<int> g_destroyed {0};

// A payload with a size and a destructor counter.
struct Blob {
  explicit Blob(std::size_t b) : bytes(b) { ++g_alive; }
  ~Blob() {
    --g_alive;
    ++g_destroyed;
  }
  std::size_t bytes;
};

StateImage Image(PartitionId partition, std::size_t bytes) {
  StateImage image;
  image.accelerator = "a";
  image.partition = partition;
  image.payload = std::make_shared<const Blob>(bytes);
  return image;
}

InvalidationPlan Parts(std::initializer_list<PartitionKey> keys) {
  InvalidationPlan plan;
  plan.scope = InvalidationScope::PartitionSet;
  plan.partitions.insert(keys.begin(), keys.end());
  return plan;
}

VisibilityToken At(Epoch e) {
  VisibilityToken t;
  t.mutation_epoch = e;
  return t;
}

struct Fixture {
  std::atomic<Epoch> epoch {0};
  std::atomic<Epoch> floor {0};
  std::unique_ptr<VersionedAuxStore> store;
  explicit Fixture(uint64_t cap, bool with_estimator = true, bool async = true) {
    VersionedAuxStore::Options o;
    o.epoch = [this] { return epoch.load(); };
    o.floor = [this] { return floor.load(); };
    o.max_version_bytes = cap;
    o.async_reclaim = async;
    if (with_estimator) {
      o.payload_bytes = [](const std::shared_ptr<const void> &p) {
        return static_cast<const Blob *>(p.get())->bytes;
      };
    }
    store = std::make_unique<VersionedAuxStore>(std::move(o));
    store->Install({Image(1, 100)}, "a", VersionedAuxStore::kAllSnapshots);
  }
  // Commit at epoch e replacing partition 1 with `bytes`.
  void Commit(Epoch e, std::size_t bytes) {
    epoch = e;
    store->BeginCommit("a", Parts({1}));
    store->FinishCommit("a", {}, {Image(1, bytes)});
  }
  ValidityCode CodeAt(Epoch e) { return store->CheckValidity({"a", ""}, At(e)).code; }
};

void TestDefaultsUnchanged() {
  // No cap, no estimator: floor 0 keeps every version (epoch 0 floor =
  // the kAllSnapshots version), nothing dropped by a cap.
  Fixture f(/*cap=*/0, /*with_estimator=*/false);
  for (Epoch e = 10; e <= 50; e += 10) {
    f.Commit(e, 1000);
  }
  Check(f.store->TotalVersionCount() == 6, "defaults keep every version the floor needs");
  Check(f.store->VersionsDroppedByCap() == 0, "defaults drop nothing by cap");
  Check(f.CodeAt(5) == ValidityCode::Valid, "oldest reader still served");

  // A cap without an estimator is inactive.
  Fixture g(/*cap=*/1, /*with_estimator=*/false);
  for (Epoch e = 10; e <= 50; e += 10) {
    g.Commit(e, 1000);
  }
  Check(g.store->TotalVersionCount() == 6 && g.store->VersionsDroppedByCap() == 0,
        "cap without estimator is inactive");
}

void TestCapEnforcement() {
  // Each commit replaces partition 1 (1000 bytes): old versions are exclusive
  // 100 (initial) + 1000 each. Cap 2500: at most two 1000-byte old versions.
  Fixture f(/*cap=*/2500);
  f.Commit(10, 1000);
  f.Commit(20, 1000);
  f.Commit(30, 1000);
  // versions: init(100), v10, v20, v30(newest). Old = 100+1000+1000 = 2100 <= 2500.
  Check(f.store->TotalVersionCount() == 4 && f.store->VersionsDroppedByCap() == 0,
        "under cap: nothing dropped");
  f.Commit(40, 1000);
  // old = 100+3*1000 = 3100 > 2500: drop init -> 3000 still > 2500: drop v10 -> 2000.
  Check(f.store->VersionsDroppedByCap() == 2, "two oldest dropped");
  Check(f.store->TotalVersionCount() == 3, "three versions retained");
  Check(f.CodeAt(5) == ValidityCode::NotVisible, "reader below oldest retained: NotVisible");
  Check(f.CodeAt(15) == ValidityCode::NotVisible, "reader of a dropped version: NotVisible");
  Check(f.CodeAt(25) == ValidityCode::Valid, "retained old version still served");
  Check(f.CodeAt(45) == ValidityCode::Valid, "newest served");
  Check(f.store->Read(At(45)).partitions.size() == 1, "newest readable");
  Check(f.CodeAt(VersionedAuxStore::kNewestSnapshot) == ValidityCode::Valid, "kNewestSnapshot reads the newest");
}

void TestSharedPartitionsNotDoubleCounted() {
  // Two partitions; each commit replaces only partition 1. Partition 2 (a huge
  // payload) is shared by every version and must not count toward the cap.
  Fixture f(/*cap=*/5000);
  f.store->Install({Image(1, 100), Image(2, 1000000)}, "a", VersionedAuxStore::kAllSnapshots);
  for (Epoch e = 10; e <= 40; e += 10) {
    f.Commit(e, 1000);
  }
  Check(f.store->VersionsDroppedByCap() == 0, "shared payloads are not counted");
  Check(f.store->TotalVersionCount() == 5, "all versions retained");
}

void TestReclaim() {
  g_destroyed = 0;
  {
    Fixture f(/*cap=*/2500, true, /*async=*/true);
    const auto before = VersionedAuxStore::ReclaimedBatchCount();
    f.Commit(10, 1000);
    f.Commit(20, 1000);
    f.Commit(30, 1000);
    f.Commit(40, 1000);
    f.Commit(50, 1000);
    VersionedAuxStore::DrainReclaimer();
    Check(VersionedAuxStore::ReclaimedBatchCount() > before, "reclaimer ran");
    // Retained: newest + old versions within the cap; everything else freed.
    Check(g_alive.load() == static_cast<int>(f.store->TotalVersionCount()),
          "dropped payloads were destroyed (alive == retained versions)");
    Check(g_destroyed.load() >= 3, "destructors ran");
  }
  VersionedAuxStore::DrainReclaimer();
  Check(g_alive.load() == 0, "everything freed after the store is gone");

  // Inline mode frees by the time the commit returns.
  g_destroyed = 0;
  {
    Fixture f(/*cap=*/1, true, /*async=*/false);
    f.Commit(10, 1000);
    f.Commit(20, 1000);
    Check(g_destroyed.load() >= 2, "inline reclaim destroys before return");
  }
  Check(g_alive.load() == 0, "inline: all freed");
}

void TestPinnedReaderKeepsDroppedVersion() {
  Fixture f(/*cap=*/1000);
  f.Commit(10, 1000);
  {
    AuxReadScope scope;
    auto snap = f.store->Read(At(5)); // pins the initial version
    Check(snap.partitions.size() == 1, "pinned read");
    f.Commit(20, 1000);                // cap drops the initial version
    auto again = f.store->Read(At(5)); // same scope: still the pin
    Check(again.partitions.size() == 1, "pinned scope keeps its version");
  }
  Check(f.CodeAt(5) == ValidityCode::NotVisible, "new readers: NotVisible");
  VersionedAuxStore::DrainReclaimer();
}

} // namespace

int main() {
  TestDefaultsUnchanged();
  TestCapEnforcement();
  TestSharedPartitionsNotDoubleCounted();
  TestReclaim();
  TestPinnedReaderKeepsDroppedVersion();
  std::cout << "OK\n";
  return 0;
}
