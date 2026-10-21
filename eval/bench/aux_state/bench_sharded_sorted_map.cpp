// Per-operation cost of ShardedSortedMap against per-shard occupancy.
//
// An SF3000 map holds ~34k entries per shard when its shard count is fixed at 2^17; SF1 holds
// ~25. The map is built with Builder(entries, target) so that occupancy is the target, then
// a batch of fresh keys is inserted and a batch of existing keys is erased, serially through
// Txn and through Txn::ApplyGrouped on a thread pool, followed by Finish and a Find sweep.
//
//   bench_sharded_sorted_map [entries=8000000] [batch=200000] [threads=8]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#include "prims/sharded_sorted_map.hpp"

using namespace accelerator_codegen::aux_state::core;
using Map = ShardedSortedKvMap<int64_t, int64_t, ShardedSortedMapHash<int64_t>, 26, 8>;
using Clock = std::chrono::steady_clock;

static double Ms(Clock::time_point from) {
  return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

struct Edit {
  int64_t key;
  bool erase;
};

int main(int argc, char **argv) {
  const std::size_t entries = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8000000;
  const std::size_t batch = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 200000;
  const std::size_t threads = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 8;
  const auto par = [&](std::size_t n, const auto &fn) {
    std::vector<std::thread> pool;
    for (std::size_t t = 0; t < std::min(threads, n); ++t)
      pool.emplace_back([&, t] {
        for (std::size_t i = t; i < n; i += threads)
          fn(i);
      });
    for (auto &th : pool)
      th.join();
  };
  std::printf("%zu entries, batch %zu (half insert, half erase), %zu threads\n", entries, batch,
              threads);
  std::printf("%-10s %-8s %11s %11s %11s %11s %11s\n", "target/shd", "shards", "find ns",
              "serial ns", "batch1 ns", "batchN ns", "finish ms");
  for (const std::size_t target : {std::size_t{25}, std::size_t{256}, std::size_t{4000},
                                   std::size_t{34000}, std::size_t{69000}}) {
    Map::Builder builder(entries, target);
    for (std::size_t k = 0; k < entries; ++k)
      builder.Add({int64_t(2 * k), 1});
    builder.Sort(par);
    const Map base = builder.Finish(par);

    std::mt19937_64 rng(1);
    std::vector<Edit> edits;
    for (std::size_t i = 0; i < batch; ++i)
      edits.push_back(i % 2 ? Edit{int64_t(2 * (rng() % entries)), true}
                            : Edit{int64_t(2 * (rng() % entries) + 1), false});

    auto t0 = Clock::now();
    std::size_t hits = 0;
    for (const auto &e : edits)
      hits += base.Find(e.key & ~int64_t{1}) != nullptr;
    const double find_ns = Ms(t0) * 1e6 / batch;

    t0 = Clock::now();
    Map::Txn serial(base);
    for (const auto &e : edits) {
      if (e.erase)
        serial.Erase(e.key);
      else
        serial.Upsert({e.key, 2});
    }
    const double serial_ns = Ms(t0) * 1e6 / batch;
    const std::size_t expect = serial.size();
    t0 = Clock::now();
    const Map done = serial.Finish(par);
    const double finish_ms = Ms(t0);

    double batched[2];
    for (int run = 0; run < 2; ++run) {
      const std::size_t workers = run ? threads : 1;
      const auto hook = [&](std::size_t n, const auto &fn) {
        std::vector<std::thread> pool;
        for (std::size_t t = 0; t < std::min(workers, n); ++t)
          pool.emplace_back([&, t] {
            for (std::size_t i = t; i < n; i += workers)
              fn(i);
          });
        for (auto &th : pool)
          th.join();
      };
      std::vector<std::vector<Edit>> pieces{edits};
      t0 = Clock::now();
      Map::Txn txn(base);
      txn.ApplyGrouped(
          pieces, [](const Edit &e) -> const int64_t & { return e.key; }, hook,
          [](std::size_t, Map::Txn::Scope &scope, std::vector<Edit> &rows) {
            for (const auto &e : rows) {
              if (e.erase)
                scope.Erase(e.key);
              else
                scope.Upsert({e.key, 2});
            }
          });
      batched[run] = Ms(t0) * 1e6 / batch;
      if (txn.size() != expect || (void(txn.Finish(hook)), false))
        std::printf("MISMATCH batched size\n");
    }
    std::printf("%-10zu %-8u %11.0f %11.0f %11.0f %11.0f %11.1f%s\n", target,
                1u << base.shard_bits(), find_ns, serial_ns, batched[0], batched[1], finish_ms,
                hits == batch ? "" : "  (find misses)");
    (void)done;
  }
  return 0;
}
