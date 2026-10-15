// GENERATED BACKEND
#include "backends/postgres/state/shard_server.hpp"

#include <algorithm>
#include <csignal>
#include <exception>

#include <pthread.h>

namespace accelerator_codegen::aux_state::engine::postgres::shard {
namespace {

// Starts a thread with every signal blocked: Postgres's handlers must run on
// the worker's main thread, never here.
template <typename F>
std::thread StartThread(F &&body) {
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  std::thread thread(std::forward<F>(body));
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  return thread;
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct RowCollector final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

} // namespace

// ---- CommitArrivals ----

void CommitArrivals::Advance() {
  beyond.erase(beyond.begin(), beyond.upper_bound(through));
  while (!beyond.empty() && *beyond.begin() == through + 1) {
    through++;
    beyond.erase(beyond.begin());
  }
}

void CommitArrivals::Offered(runtime::Epoch epoch) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (epoch > through) {
      beyond.insert(epoch);
      Advance();
    }
  }
  arrived.notify_all();
}

void CommitArrivals::Covered(runtime::Epoch covered) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    through = std::max(through, covered);
    Advance();
  }
  arrived.notify_all();
}

bool CommitArrivals::WaitThrough(runtime::Epoch epoch, Clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mutex);
  return arrived.wait_until(lock, deadline, [&] { return through >= epoch; });
}

// ---- ShardServer ----

ShardServer::ShardServer(ShardSegment &segment_p, uint32_t index_p,
                         runtime::AcceleratorRuntime &runtime_p, CommitArrivals &arrivals_p)
    : segment(segment_p), index(index_p), runtime(runtime_p), arrivals(arrivals_p) {
  Touch();
}

ShardServer::~ShardServer() { Stop(); }

void ShardServer::Start(std::size_t read_threads) {
  threads.push_back(StartThread([this] { CommitLoop(); }));
  for (std::size_t i = 0; i < std::max<std::size_t>(read_threads, 1); i++) {
    threads.push_back(StartThread([this] { ReadLoop(); }));
  }
}

void ShardServer::Stop() {
  if (threads.empty()) {
    return;
  }
  stopping = true;
  segment.Close();
  for (auto &thread : threads) {
    thread.join();
  }
  threads.clear();
}

void ShardServer::WaitWork(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(work_mutex);
  work_cv.wait_for(lock, timeout, [&] { return work; });
  work = false;
}

void ShardServer::Wake() {
  {
    std::lock_guard<std::mutex> lock(work_mutex);
    work = true;
  }
  work_cv.notify_one();
}

Clock::time_point ShardServer::LastActivity() const {
  return Clock::time_point(std::chrono::nanoseconds(last_activity_ns.load()));
}

void ShardServer::Touch() { last_activity_ns = NowNs(); }

void ShardServer::CommitLoop() {
  std::string message;
  while (segment.ReceiveCommit(message, Clock::time_point::max())) {
    Touch();
    try {
      auto commit = DecodeCommit(message);
      if (commit.accelerator != index) {
        continue;
      }
      runtime.OfferCommit(runtime::CommittedChange{commit.epoch, commit.epoch, std::move(commit.batches),
                                                   commit.empty, commit.invalidates});
      segment.stats.commits++;
      arrivals.Offered(commit.epoch);
    } catch (const std::exception &e) {
      // The commit is missing: the runtime sees a gap and rebuilds.
      segment.stats.errors++;
      segment.SetText(ShardSegment::kLastError, e.what());
    }
    Wake();
  }
  failed = failed || !stopping;
}

void ShardServer::ReadLoop() {
  std::string message;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(request_mutex);
      if (!segment.ReceiveRequest(message, Clock::time_point::max())) {
        break;
      }
    }
    Touch();
    Serve(message);
  }
  failed = failed || !stopping;
}

void ShardServer::Serve(const std::string &message) {
  ReadRequest request;
  try {
    request = DecodeRequest(message);
  } catch (const std::exception &e) {
    // Without its slot nobody can be told; the worker restarts.
    segment.SetText(ShardSegment::kLastError, e.what());
    failed = true;
    return;
  }
  segment.stats.requests++;
  const auto deadline = Clock::time_point(std::chrono::nanoseconds(request.deadline_ns));
  if (segment.SlotWaiting(request.slot) && Clock::now() < deadline) {
    // A read that needs a build or a catch-up reaches the main thread only
    // through its next wake-up.
    if (!runtime.IsSynced()) {
      Wake();
    }
    ReadResponse response;
    try {
      runtime::ReadOptions options;
      options.epoch = request.epoch;
      options.require_applied_through = request.epoch;
      options.max_wait_us = std::min<uint64_t>(
          request.max_wait_us,
          std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count());
      RowCollector rows;
      response.served = runtime.Read(options, request.arguments, rows) == runtime::ReadResult::Served;
      response.rows = std::move(rows.rows);
    } catch (const std::exception &e) {
      segment.stats.errors++;
      segment.SetText(ShardSegment::kLastError, e.what());
      response = ReadResponse{};
    }
    (response.served ? segment.stats.served : segment.stats.fallback)++;
    std::string reply;
    EncodeResponse(response, reply);
    segment.Respond(request.slot, reply, deadline);
    if (!response.served) {
      Wake();
    }
  }
  segment.FinishRequest(request.slot);
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard
