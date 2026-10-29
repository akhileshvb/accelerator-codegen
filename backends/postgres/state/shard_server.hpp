// GENERATED BACKEND
//
// The worker's Postgres-free threads (S6): the commit thread that offers
// shipped commits to the shard's runtimes, and the read threads that serve reads at the
// reader's epoch. Neither ever calls Postgres (their signals are blocked);
// everything that must, runs on the worker's main thread.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "backends/postgres/state/messages.hpp"
#include "backends/postgres/state/shard_segment.hpp"
#include "substrate/aux/runtime/accelerator_runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres::shard {

// Which commits (by epoch) have reached the worker. A build at epoch b
// contains every commit through b, shipped or not, so those count as arrived.
class CommitArrivals {
public:
  void Offered(runtime::Epoch epoch);
  void Covered(runtime::Epoch through);
  // Waits until every commit through `epoch` has arrived; false on deadline.
  bool WaitThrough(runtime::Epoch epoch, Clock::time_point deadline);

private:
  void Advance();
  std::mutex mutex;
  std::condition_variable arrived;
  runtime::Epoch through = 0;
  std::set<runtime::Epoch> beyond; // arrived epochs after a hole
};

class ShardServer {
public:
  // `runtimes`: the shard's members, in ShardMembers order.
  ShardServer(ShardSegment &segment, uint32_t index, std::vector<runtime::AcceleratorRuntime *> runtimes,
              CommitArrivals &arrivals);
  ~ShardServer();

  void Start(std::size_t read_threads);
  // Closes the segment's rings, which ends the threads, and joins them
  // (reads in progress finish first, within their wait budget).
  void Stop();

  // The main thread's idle wait: returns when woken or after `timeout`.
  // True if woken (work arrived), false on timeout.
  bool WaitWork(std::chrono::milliseconds timeout);
  // Reads of `member` wait for its state however long its SQL fallback would take (verified
  // accelerators never answer from SQL).
  void SetPatient(std::size_t member);
  void Wake();
  // A ring was closed under the worker (a producer died mid-message) or a
  // message was malformed: the worker must restart.
  bool Failed() const { return failed; }
  // When the last commit or request arrived.
  Clock::time_point LastActivity() const;

private:
  void CommitLoop();
  void ReadLoop();
  void Serve(const std::string &message);
  bool ReadWithin(runtime::AcceleratorRuntime &runtime, const ReadRequest &request, Clock::time_point deadline, std::vector<core::AuxRow> &rows);
  void Touch();

  ShardSegment &segment;
  const uint32_t index;
  std::vector<runtime::AcceleratorRuntime *> runtimes;
  std::vector<bool> patient;
  CommitArrivals &arrivals;
  std::atomic<bool> stopping{false};
  std::atomic<bool> failed{false};
  std::atomic<int64_t> last_activity_ns{0};
  std::mutex request_mutex; // one read thread at a time receives
  std::mutex work_mutex;
  std::condition_variable work_cv;
  bool work = false;
  std::vector<std::thread> threads;
};

} // namespace accelerator_codegen::aux_state::engine::postgres::shard
