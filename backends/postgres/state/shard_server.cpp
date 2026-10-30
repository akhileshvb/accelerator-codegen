// GENERATED BACKEND
#include "backends/postgres/state/shard_server.hpp"

#include <algorithm>
#include <csignal>
#include <exception>
#include <utility>

#include <pthread.h>

#include "backends/postgres/state/shared.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"

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

runtime::Epoch CommitArrivals::Missing() {
  std::lock_guard<std::mutex> lock(mutex);
  return through + 1;
}

// ---- ShardServer ----

ShardServer::ShardServer(ShardSegment &segment_p, uint32_t index_p,
                         std::vector<runtime::AcceleratorRuntime *> runtimes_p, CommitArrivals &arrivals_p)
    : segment(segment_p), index(index_p), runtimes(std::move(runtimes_p)), arrivals(arrivals_p) {
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
  core::g_pending_waits_cancelled = true; // a pass that died mid-commit leaves it pending for good
  segment.Close();
  for (auto &thread : threads) {
    thread.join();
  }
  threads.clear();
}

bool ShardServer::WaitWork(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(work_mutex);
  work_cv.wait_for(lock, timeout, [&] { return work; });
  return std::exchange(work, false);
}

void ShardServer::SetPatient(std::size_t member) {
  patient.resize(runtimes.size());
  patient.at(member) = true;
  runtimes[member]->SetSqlFallbackUs(1e15);
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
      // Every member sees every epoch: the sequence is the shard's, and a commit that touched none of
      // a member's tables is an empty change for it.
      for (auto *runtime : runtimes) {
        runtime->OfferCommit(runtime::CommittedChange{commit.epoch, commit.epoch, commit.batches, commit.empty,
                                                      commit.invalidates});
      }
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

// The runtime waits for commits, builds and catch-ups up to the request's
// budget in one call; here in slices, so that a stopping worker is not held up
// by a reader that waits for a build that will not finish.
bool ShardServer::ReadWithin(runtime::AcceleratorRuntime &runtime, const ReadRequest &request, Clock::time_point deadline,
                             std::vector<core::AuxRow> &rows) {
  constexpr uint64_t kSliceUs = 100'000;
  const auto budget_end = Clock::now() + std::chrono::microseconds(request.max_wait_us);
  for (;;) {
    const auto now = Clock::now();
    const auto remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::min(budget_end, deadline) - now).count();
    const bool patient_member = request.member < patient.size() && patient[request.member];
    if (const double sql_us = postgres::shared::SqlFallbackUs(static_cast<int>(index), request.member);
        sql_us > 0 && !patient_member) {
      runtime.SetSqlFallbackUs(sql_us);
    }
    runtime::ReadOptions options;
    options.epoch = request.epoch;
    options.require_applied_through = request.epoch;
    options.max_wait_us = static_cast<uint64_t>(std::clamp<int64_t>(remaining_us, 0, kSliceUs));
    RowCollector collected;
    if (runtime.Read(options, request.arguments, collected) == runtime::ReadResult::Served) {
      rows = std::move(collected.rows);
      return true;
    }
    // Declined at once: waiting longer does not help. Otherwise the slice was
    // spent waiting, and the budget may allow another.
    const auto waited_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count();
    if (stopping || remaining_us <= static_cast<int64_t>(kSliceUs) || waited_us < static_cast<int64_t>(kSliceUs) / 2) {
      return false;
    }
  }
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
    runtime::AcceleratorRuntime *runtime = request.member < runtimes.size() ? runtimes[request.member] : nullptr;
    // A read that needs a build or a catch-up reaches the main thread only
    // through its next wake-up.
    if (runtime && !runtime->IsSynced()) {
      Wake();
    }
    ReadResponse response;
    try {
      core::TakeLastAuxFallbackCause();
      response.served = runtime && ReadWithin(*runtime, request, deadline, response.rows);
      if (!response.served) {
        response.cause = runtime ? core::TakeLastAuxFallbackCause() : "no_member";
        if (response.cause.empty()) {
          response.cause = stopping ? "worker_stopping" : "declined";
        }
      }
    } catch (const std::exception &e) {
      segment.stats.errors++;
      segment.SetText(ShardSegment::kLastError, e.what());
      response = ReadResponse{};
      response.cause = "worker_error";
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
