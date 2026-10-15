// Standalone test for ShardSegment, the transport between backends and a shard
// worker: needs neither a Postgres server nor its headers.
//
//  1. commits: many producers, one consumer; messages up to larger than the ring
//  2. requests and replies: many clients, a few server threads, replies larger
//     than a response ring
//  3. a client that gives up (deadline) never sees a later reply on the slot's
//     next use, and the slot comes back once the worker is done with it
//  4. all slots taken; worker restart (Reset) releases the dead worker's claims
#include "backends/postgres/state/shard_segment.hpp"

#include <sys/mman.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace accelerator_codegen::aux_state::engine::postgres::shard;
using std::chrono::milliseconds;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                \
      std::exit(1);                                                                                \
    }                                                                                              \
  } while (0)

namespace {

Clock::time_point In(milliseconds d) { return Clock::now() + d; }

ShardSegment *Map() {
  void *memory = mmap(nullptr, ShardSegment::Size(), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(memory != MAP_FAILED);
  ShardSegment *segment = ShardSegment::Create(memory);
  segment->Reset();
  return segment;
}

// Message (producer, index): length and content derive from them.
std::string Message(int producer, int index) {
  std::mt19937_64 rng(static_cast<uint64_t>(producer) * 1000003 + index);
  const std::size_t length = rng() % 50 == 0 ? rng() % (12u << 20) : rng() % 4096;
  std::string message(length, static_cast<char>('a' + producer));
  if (length >= 8) {
    std::memcpy(&message[0], &index, sizeof(index));
  }
  return message;
}

void Commits() {
  ShardSegment *segment = Map();
  constexpr int kProducers = 4, kPerProducer = 400;
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; p++) {
    producers.emplace_back([=] {
      for (int i = 0; i < kPerProducer; i++) {
        CHECK(segment->SendCommit(Message(p, i), In(milliseconds(30000))));
      }
    });
  }
  std::vector<int> next(kProducers, 0);
  std::string message;
  for (int n = 0; n < kProducers * kPerProducer; n++) {
    CHECK(segment->ReceiveCommit(message, In(milliseconds(30000))));
    const int producer = message.empty() ? -1 : message.back() - 'a';
    CHECK(producer >= 0 && producer < kProducers);
    CHECK(message == Message(producer, next[producer]++)); // one producer's messages stay in order
  }
  for (auto &t : producers) {
    t.join();
  }
}

// The server: replies to request "slot|size" with `size` bytes derived from the slot.
void Serve(ShardSegment *segment, std::atomic<bool> &stop, std::mutex &receive) {
  std::string request;
  while (!stop) {
    {
      std::lock_guard<std::mutex> lock(receive);
      if (!segment->ReceiveRequest(request, In(milliseconds(50)))) {
        continue;
      }
    }
    unsigned slot, size;
    std::sscanf(request.c_str(), "%u|%u", &slot, &size);
    segment->Respond(slot, std::string(size, static_cast<char>('A' + slot % 26)), In(milliseconds(5000)));
    segment->FinishRequest(slot);
  }
}

void RequestsAndReplies() {
  ShardSegment *segment = Map();
  std::atomic<bool> stop{false};
  std::mutex receive;
  std::vector<std::thread> servers;
  for (int i = 0; i < 3; i++) {
    servers.emplace_back([&] { Serve(segment, stop, receive); });
  }
  std::vector<std::thread> clients;
  std::atomic<int> done{0};
  for (int c = 0; c < 16; c++) {
    clients.emplace_back([&, c] {
      std::mt19937_64 rng(c);
      for (int i = 0; i < 200; i++) {
        int slot;
        while ((slot = segment->ClaimSlot()) < 0) {
          std::this_thread::yield();
        }
        const unsigned size = rng() % 20 == 0 ? rng() % (1u << 20) : rng() % 2000; // up to 8 rings
        CHECK(segment->SendRequest(slot, std::to_string(slot) + "|" + std::to_string(size), In(milliseconds(5000))));
        std::string reply;
        CHECK(segment->WaitResponse(slot, In(milliseconds(5000))));
        CHECK(segment->ReceiveResponse(slot, reply, In(milliseconds(5000))));
        CHECK(reply == std::string(size, static_cast<char>('A' + slot % 26)));
        segment->ReleaseSlot(slot);
      }
      done++;
    });
  }
  for (auto &t : clients) {
    t.join();
  }
  CHECK(done == 16);
  stop = true;
  for (auto &t : servers) {
    t.join();
  }
}

void GivingUp() {
  ShardSegment *segment = Map();
  const int slot = segment->ClaimSlot();
  CHECK(slot >= 0);
  CHECK(segment->SendRequest(slot, "1|10", In(milliseconds(1000))));
  // The worker is slow: the client gives up.
  std::string reply;
  CHECK(!segment->WaitResponse(slot, In(milliseconds(50))));
  segment->ReleaseSlot(slot);
  CHECK(!segment->SlotWaiting(slot));
  // The slot is not free for the next client while the worker still holds it.
  std::vector<int> taken;
  for (int i = 0; i < static_cast<int>(ShardSegment::kClientSlots) - 1; i++) {
    taken.push_back(segment->ClaimSlot());
  }
  CHECK(segment->ClaimSlot() < 0); // exactly the abandoned slot is missing
  // The worker gets to the request, finds the client gone, and replies anyway.
  CHECK(segment->ReceiveRequest(reply, In(milliseconds(1000))));
  segment->Respond(slot, "late reply", In(milliseconds(1000)));
  segment->FinishRequest(slot);
  // Now the slot is back, empty: its next user sees only its own reply.
  const int again = segment->ClaimSlot();
  CHECK(again == slot);
  CHECK(!segment->WaitResponse(slot, In(milliseconds(20))));
  CHECK(segment->SendRequest(slot, "2|3", In(milliseconds(1000))));
  CHECK(segment->ReceiveRequest(reply, In(milliseconds(1000))));
  segment->Respond(slot, "own", In(milliseconds(1000)));
  segment->FinishRequest(slot);
  CHECK(segment->ReceiveResponse(slot, reply, In(milliseconds(1000))));
  CHECK(reply == "own");
  segment->ReleaseSlot(slot);
  for (int t : taken) {
    segment->ReleaseSlot(t);
  }
}

void WorkerRestart() {
  ShardSegment *segment = Map();
  const int slot = segment->ClaimSlot();
  CHECK(segment->SendRequest(slot, "x", In(milliseconds(1000))));
  segment->ReleaseSlot(slot); // client gone, worker still owed
  segment->Close();           // the worker dies...
  CHECK(!segment->SendCommit("late", In(milliseconds(100))));
  segment->Reset();           // ...and a new one starts
  CHECK(segment->SendCommit("fresh", In(milliseconds(1000))));
  std::string message;
  CHECK(segment->ReceiveCommit(message, In(milliseconds(1000))));
  CHECK(message == "fresh");
  // The dead worker's claim on the slot is gone: it can be claimed again.
  std::vector<int> all;
  for (uint32_t i = 0; i < ShardSegment::kClientSlots; i++) {
    all.push_back(segment->ClaimSlot());
    CHECK(all.back() >= 0);
  }
  CHECK(segment->ClaimSlot() < 0);
  segment->SetText(ShardSegment::kLastError, "text");
  CHECK(segment->GetText(ShardSegment::kLastError) == "text");
}

} // namespace

int main() {
  Commits();
  std::puts("PASS commits");
  RequestsAndReplies();
  std::puts("PASS requests_and_replies");
  GivingUp();
  std::puts("PASS giving_up");
  WorkerRestart();
  std::puts("PASS worker_restart");
  return 0;
}
