// The Verus world's commit groups under concurrent commits (WorldKitStore / WorldShared) over a
// fake world ABI: commits with overlapping and disjoint accelerator sets, each thread locking its
// accelerators' commit locks in id order as RuntimeHost::PreCommit does, publishing (Begin,
// Finish) and unlocking each, then ending the commit.  The fake store refuses a second commit
// in flight, so the test fails if two groups ever overlap; a deadlock fails by timeout.
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "substrate/verus/kit/verus_kit.hpp"

namespace vk = accelerator_codegen::verus_kit;
namespace core = accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::VerusAuxStore;

namespace {

int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n";     \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

thread_local uint64_t this_commit = 0; // the epoch the store reports to its commit
std::atomic<bool> pending{false};
std::atomic<int> refused{0}, aborts{0};

int32_t Begin(const void *, uint64_t) {
  if (pending.exchange(true)) {
    ++refused;
    return 0;
  }
  return 1;
}
void Abort(const void *) {
  ++aborts;
  pending = false;
}

vk::World MakeFake(const char *bench) {
  vk::World w{};
  w.bench = bench;
  w.new_store = [] { return static_cast<void *>(new int(0)); };
  w.free_store = [](void *s) { delete static_cast<int *>(s); };
  w.begin = Begin;
  w.abort = Abort;
  w.newest = [](const void *) -> uint64_t { return 0; };
  w.version_count = [](const void *) -> size_t { return 1; };
  for (const char *id : {"fake_a", "fake_b", "fake_c"}) {
    vk::WorldKitOps k{};
    k.accelerator = id;
    w.kits.push_back(k);
  }
  return w;
}

vk::Glue MakeGlue(const char *id) {
  vk::Glue g{};
  g.id = id;
  g.answer = [](const void *, const core::QueryArguments &, std::vector<core::AuxRow> &) { return false; };
  g.prepare_apply = [](const void *, const vk::Changes &, vk::Host &, int32_t *) -> void * { return nullptr; };
  g.prepare_build = [](const vk::HeadFn &, vk::Host &) -> void * { return nullptr; };
  return g;
}

} // namespace

int main() {
  vk::World world = MakeFake("fake");
  vk::RegisterWorld(&world);
  const char *ids[] = {"fake_a", "fake_b", "fake_c"};
  std::vector<vk::Glue> glues;
  for (const char *id : ids)
    glues.push_back(MakeGlue(id));
  std::vector<std::unique_ptr<VerusAuxStore>> stores;
  std::mutex commit_locks[3];
  for (auto &g : glues) {
    stores.push_back(vk::MakeWorldStore(g, [] { return VerusAuxStore::Epoch{this_commit}; }, nullptr));
    CHECK(stores.back() != nullptr);
  }
  if (failures)
    return 1;

  std::atomic<uint64_t> epochs{0};
  // One transaction: PreCommit locks its accelerators in id order; each publishes then
  // releases its lock; the commit ends after all published.
  const auto commit = [&](std::vector<int> touched, uint64_t epoch) {
    this_commit = epoch;
    for (int a : touched)
      commit_locks[a].lock();
    for (int a : touched) {
      stores[a]->BeginCommit(ids[a], core::InvalidationPlan{});
      stores[a]->FinishCommit(ids[a], core::InvalidationPlan{}, {}, 0);
      commit_locks[a].unlock();
    }
    for (int a : touched)
      stores[a]->EndCommit(epoch);
  };

  const std::vector<std::pair<std::vector<int>, std::vector<int>>> cases = {
      {{0, 2}, {1, 2}}, // overlapping on C
      {{0}, {1}},       // disjoint
      {{0, 1, 2}, {2}}, // nested
  };
  for (const auto &[s1, s2] : cases) {
    refused = 0;
    aborts = 0;
    const int rounds = 300;
    auto run = std::async(std::launch::async, [&] {
      auto worker = [&](const std::vector<int> &set) {
        for (int i = 0; i < rounds; ++i)
          commit(set, ++epochs);
      };
      std::thread t1(worker, s1), t2(worker, s2);
      t1.join();
      t2.join();
    });
    if (run.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
      std::cerr << "deadlock: concurrent commits did not finish\n";
      return 1;
    }
    CHECK(refused == 0);
    CHECK(!pending); // every commit group ended
    CHECK(aborts == 2 * rounds); // no batch: each commit's group ended by abort, once
  }
  std::cout << (failures ? "FAIL" : "PASS") << "\n";
  return failures ? 1 : 0;
}
