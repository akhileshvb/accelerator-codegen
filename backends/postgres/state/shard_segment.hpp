// GENERATED BACKEND
//
// One shard's shared memory (S6): the transport between a database's backends
// and the shard worker, and the worker's published status. Pure C++/Linux, no
// Postgres headers, so the worker's non-Postgres threads use it directly (the
// DSM segment is only where the memory comes from).
//
//   commit ring    backends -> worker. Many producers (SendCommit), one
//                  consumer (the worker's commit thread).
//   request ring   backends -> worker. Many producers, consumed by the
//                  worker's read threads (which serialise among themselves).
//   response ring  one per client slot, worker -> backend. A backend claims a
//                  slot for one request and reads its reply from the slot's
//                  ring.
//
// Producers of one ring serialise on a robust process-shared mutex, held for
// the whole message (ShmRing is single-producer). A producer that dies while
// holding it may have left half a message: the ring is closed, and the worker
// (which then exits and is restarted on demand) sees it.
//
// A slot has two owners, the backend that claimed it and the worker while a
// request sent on it is queued or running. Each releases its own claim and
// whoever releases last empties the slot's ring and frees it, so a reply that
// arrives after its backend gave up (timeout, cancel) can never be read by the
// slot's next user.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <string>
#include <string_view>

#include "backends/postgres/state/shm_ring.hpp"

namespace accelerator_codegen::aux_state::engine::postgres::shard {

using Clock = accel::ShmRing::Clock;

// A mutex in shared memory that survives the death of its holder.
class SharedMutex {
public:
  void Init();
  enum class Result { Locked, Timeout, OwnerDied };
  // OwnerDied: the mutex is held, but what it protects may be inconsistent.
  Result Lock(Clock::time_point deadline);
  void Unlock();

private:
  pthread_mutex_t mutex;
};

class ShardSegment {
public:
  static constexpr uint32_t kClientSlots = 64;
  static constexpr std::size_t kCommitRingBytes = 8u << 20;
  static constexpr std::size_t kRequestRingBytes = 256u << 10;
  static constexpr std::size_t kResponseRingBytes = 128u << 10;

  static std::size_t Size();
  // Initialises `memory` (Size() bytes, 64-byte aligned, any contents).
  static ShardSegment *Create(void *memory);

  // ---- worker lifecycle ----
  // kStopping: the worker is leaving; its rings are closing, nothing is owed to it.
  enum State : uint32_t { kStopped, kStarting, kRunning, kStopping };
  std::atomic<uint32_t> state;
  std::atomic<uint32_t> pid;
  std::atomic<uint32_t> generation;
  std::atomic<int64_t> started_ns; // when `state` became kStarting (steady clock)

  // The worker's counters (Postgres-free threads update them).
  struct Stats {
    std::atomic<uint64_t> requests, served, fallback, errors, commits;
    // Published by the main thread from AcceleratorStats.
    std::atomic<uint64_t> applied, builds, gaps, synced, waited, declined;
    std::atomic<int64_t> decision_ns; // when kLastDecision was published
  } stats;

  // Text the worker publishes (and, for kForcedAction, readers set).
  enum Text : std::size_t { kForcedAction, kLastAction, kLastDecision, kSearchPath, kLastError, kTextCount };
  void SetText(Text which, std::string_view value);
  std::string GetText(Text which);

  // Worker start (state kStarting): zeroes the counters, empties the commit and request rings,
  // drops the worker's claims on slots (the previous worker is gone), and
  // empties the response rings of slots no backend holds.
  void Reset();
  // The worker is going away: wakes everyone blocked on any ring and makes
  // later sends fail.
  void Close();

  // ---- backends ----
  // Queues a commit message. False: not queued (full past the deadline, lock
  // timeout, ring closed).
  bool SendCommit(std::string_view message, Clock::time_point deadline);
  // A free client slot, now held by the caller; -1 if all are taken.
  int ClaimSlot();
  // Queues a request for the worker on `slot`; false: not queued (the worker
  // has no claim then).
  bool SendRequest(uint32_t slot, std::string_view message, Clock::time_point deadline);
  // Blocks until a reply (or its first chunk) is waiting, the ring is closed
  // (true), or the deadline passes (false).
  bool WaitResponse(uint32_t slot, Clock::time_point deadline);
  bool ReceiveResponse(uint32_t slot, std::string &out, Clock::time_point deadline);
  // The caller is done with `slot`, whether or not it got its reply.
  void ReleaseSlot(uint32_t slot);

  // ---- worker ----
  bool ReceiveCommit(std::string &out, Clock::time_point deadline);
  bool ReceiveRequest(std::string &out, Clock::time_point deadline);
  // The backend is still waiting for `slot`'s reply.
  bool SlotWaiting(uint32_t slot) const;
  bool Respond(uint32_t slot, std::string_view message, Clock::time_point deadline);
  // The worker is done with the request it received for `slot`.
  void FinishRequest(uint32_t slot);

private:
  ShardSegment() = default;
  void ClearStats();
  accel::ShmRing &CommitRing();
  accel::ShmRing &RequestRing();
  accel::ShmRing &ResponseRing(uint32_t slot);
  bool Send(SharedMutex &lock, accel::ShmRing &ring, std::string_view message,
            Clock::time_point deadline);
  // Gives up one owner's claim on `slot`; the last one out empties its ring.
  void Release(uint32_t slot, uint32_t claim);

  SharedMutex commit_lock;
  SharedMutex request_lock;
  SharedMutex text_lock;
  static constexpr std::size_t kTextBytes = 192;
  std::array<std::array<char, kTextBytes>, kTextCount> texts;
  std::atomic<uint32_t> next_slot;
  struct Slot {
    std::atomic<uint32_t> state;
  };
  std::array<Slot, kClientSlots> slots;
};

} // namespace accelerator_codegen::aux_state::engine::postgres::shard
