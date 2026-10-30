// GENERATED BACKEND
#include "backends/postgres/state/shard_segment.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <new>

namespace accelerator_codegen::aux_state::engine::postgres::shard {
namespace {

constexpr std::size_t Round(std::size_t bytes) { return (bytes + 63) & ~std::size_t{63}; }

// A slot's owners: the backend that claimed it, the worker while a request is
// queued or running, and the one who is emptying its ring.
constexpr uint32_t kBackend = 1;
constexpr uint32_t kWorker = 2;
constexpr uint32_t kEmptying = 4;

timespec ToTimespec(Clock::time_point deadline) {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();
  return {static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
}

} // namespace

// ---- SharedMutex ----

void SharedMutex::Init() {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
}

SharedMutex::Result SharedMutex::Lock(Clock::time_point deadline) {
  const timespec until = ToTimespec(deadline);
  // steady_clock is CLOCK_MONOTONIC in libstdc++.
  switch (pthread_mutex_clocklock(&mutex, CLOCK_MONOTONIC, &until)) {
  case 0:
    return Result::Locked;
  case EOWNERDEAD:
    pthread_mutex_consistent(&mutex);
    return Result::OwnerDied;
  default:
    return Result::Timeout;
  }
}

void SharedMutex::Unlock() { pthread_mutex_unlock(&mutex); }

// ---- layout: this header, then the rings ----

std::size_t ShardSegment::Size() {
  return Round(sizeof(ShardSegment)) + Round(accel::ShmRing::Size(kCommitRingBytes)) +
         Round(accel::ShmRing::Size(kRequestRingBytes)) +
         kClientSlots * Round(accel::ShmRing::Size(kResponseRingBytes));
}

accel::ShmRing &ShardSegment::CommitRing() {
  return *accel::ShmRing::Attach(reinterpret_cast<char *>(this) + Round(sizeof(ShardSegment)));
}

accel::ShmRing &ShardSegment::RequestRing() {
  return *accel::ShmRing::Attach(reinterpret_cast<char *>(this) + Round(sizeof(ShardSegment)) +
                                 Round(accel::ShmRing::Size(kCommitRingBytes)));
}

accel::ShmRing &ShardSegment::ResponseRing(uint32_t slot) {
  return *accel::ShmRing::Attach(reinterpret_cast<char *>(this) + Round(sizeof(ShardSegment)) +
                                 Round(accel::ShmRing::Size(kCommitRingBytes)) +
                                 Round(accel::ShmRing::Size(kRequestRingBytes)) +
                                 slot * Round(accel::ShmRing::Size(kResponseRingBytes)));
}

ShardSegment *ShardSegment::Create(void *memory) {
  auto *segment = new (memory) ShardSegment;
  segment->state = kStopped;
  segment->pid = 0;
  segment->generation = 0;
  segment->started_ns = 0;
  segment->ClearStats();
  segment->commit_lock.Init();
  segment->request_lock.Init();
  segment->text_lock.Init();
  for (auto &text : segment->texts) {
    text[0] = '\0';
  }
  segment->next_slot = 0;
  auto *base = static_cast<char *>(memory) + Round(sizeof(ShardSegment));
  accel::ShmRing::Create(base, kCommitRingBytes);
  base += Round(accel::ShmRing::Size(kCommitRingBytes));
  accel::ShmRing::Create(base, kRequestRingBytes);
  base += Round(accel::ShmRing::Size(kRequestRingBytes));
  for (auto &slot : segment->slots) {
    slot.state = 0;
    accel::ShmRing::Create(base, kResponseRingBytes);
    base += Round(accel::ShmRing::Size(kResponseRingBytes));
  }
  return segment;
}

// ---- text ----

void ShardSegment::SetText(Text which, std::string_view value) {
  text_lock.Lock(Clock::time_point::max());
  auto &text = texts[which];
  const std::size_t n = std::min(value.size(), kTextBytes - 1);
  std::memcpy(text.data(), value.data(), n);
  text[n] = '\0';
  text_lock.Unlock();
}

std::string ShardSegment::GetText(Text which) {
  text_lock.Lock(Clock::time_point::max());
  std::string value(texts[which].data());
  text_lock.Unlock();
  return value;
}

// ---- lifecycle ----

void ShardSegment::ClearStats() {
  for (auto *counter : {&stats.requests, &stats.served, &stats.fallback, &stats.errors, &stats.commits,
                        &stats.applied, &stats.builds, &stats.gaps, &stats.synced, &stats.members}) {
    *counter = 0;
  }
  stats.decision_ns = 0;
}

void ShardSegment::Reset() {
  ClearStats();
  accel::ShmRing::Create(&CommitRing(), kCommitRingBytes);
  accel::ShmRing::Create(&RequestRing(), kRequestRingBytes);
  for (uint32_t slot = 0; slot < kClientSlots; slot++) {
    const uint32_t current = slots[slot].state.load();
    if (current & kWorker) {
      Release(slot, kWorker);
    } else if (current == 0) {
      accel::ShmRing::Create(&ResponseRing(slot), kResponseRingBytes);
    }
  }
}

void ShardSegment::Close() {
  CommitRing().Close();
  RequestRing().Close();
  for (uint32_t slot = 0; slot < kClientSlots; slot++) {
    ResponseRing(slot).Close();
  }
}

// ---- producers ----

bool ShardSegment::Send(SharedMutex &lock, accel::ShmRing &ring, std::string_view message,
                        Clock::time_point deadline) {
  switch (lock.Lock(deadline)) {
  case SharedMutex::Result::Timeout:
    return false;
  case SharedMutex::Result::OwnerDied:
    ring.Close();
    lock.Unlock();
    return false;
  case SharedMutex::Result::Locked:
    break;
  }
  const bool sent = ring.Send(message, deadline);
  lock.Unlock();
  return sent;
}

bool ShardSegment::SendCommit(std::string_view message, Clock::time_point deadline) {
  return Send(commit_lock, CommitRing(), message, deadline);
}

int ShardSegment::ClaimSlot() {
  const uint32_t start = next_slot.fetch_add(1);
  for (uint32_t i = 0; i < kClientSlots; i++) {
    const uint32_t slot = (start + i) % kClientSlots;
    uint32_t free = 0;
    if (slots[slot].state.compare_exchange_strong(free, kBackend)) {
      return static_cast<int>(slot);
    }
  }
  return -1;
}

bool ShardSegment::SendRequest(uint32_t slot, std::string_view message, Clock::time_point deadline) {
  slots[slot].state.fetch_or(kWorker);
  if (Send(request_lock, RequestRing(), message, deadline)) {
    return true;
  }
  Release(slot, kWorker);
  return false;
}

bool ShardSegment::WaitResponse(uint32_t slot, Clock::time_point deadline) {
  return ResponseRing(slot).WaitForData(deadline);
}

bool ShardSegment::ReceiveResponse(uint32_t slot, std::string &out, Clock::time_point deadline) {
  return ResponseRing(slot).Receive(out, deadline);
}

void ShardSegment::ReleaseSlot(uint32_t slot) {
  if (slots[slot].state.load() & kWorker) {
    ResponseRing(slot).Close(); // a reply still being sent has no reader any more
  }
  Release(slot, kBackend);
}

// ---- worker ----

bool ShardSegment::ReceiveCommit(std::string &out, Clock::time_point deadline) {
  return CommitRing().Receive(out, deadline);
}

bool ShardSegment::ReceiveRequest(std::string &out, Clock::time_point deadline) {
  return RequestRing().Receive(out, deadline);
}

bool ShardSegment::SlotWaiting(uint32_t slot) const { return slots[slot].state.load() & kBackend; }

bool ShardSegment::Respond(uint32_t slot, std::string_view message, Clock::time_point deadline) {
  return ResponseRing(slot).Send(message, deadline);
}

void ShardSegment::FinishRequest(uint32_t slot) { Release(slot, kWorker); }

void ShardSegment::Release(uint32_t slot, uint32_t claim) {
  auto &state = slots[slot].state;
  uint32_t current = state.load();
  for (;;) {
    const uint32_t others = current & ~claim;
    if (state.compare_exchange_weak(current, others ? others : kEmptying)) {
      if (!others) {
        accel::ShmRing::Create(&ResponseRing(slot), kResponseRingBytes);
        state.store(0);
      }
      return;
    }
  }
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard
