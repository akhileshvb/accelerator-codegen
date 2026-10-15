// Standalone test for ShmRing: needs neither a Postgres server nor its headers.
#include "state/shm_ring.hpp"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

using accel::ShmRing;
using Clock = ShmRing::Clock;
using std::chrono::milliseconds;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
			std::exit(1);                                                                                              \
		}                                                                                                              \
	} while (0)

namespace {

constexpr size_t kRingCapacity = 1 << 20;
constexpr size_t kMaxMessage = 4 << 20;
constexpr int kMessages = 100000;

Clock::time_point In(milliseconds d) {
	return Clock::now() + d;
}

// Deterministic message `index`: length and content derive from the index only,
// so both sides can generate it independently.
std::string MakeMessage(int index) {
	std::mt19937_64 rng(index);
	// Mostly small messages, occasionally multi-MB ones, 0 B .. 4 MB overall.
	size_t length = rng() % 64 == 0 ? rng() % (kMaxMessage + 1) : rng() % 4096;
	std::string message(length, '\0');
	for (size_t i = 0; i + 8 <= length; i += 8) {
		uint64_t word = rng();
		std::memcpy(&message[i], &word, 8);
	}
	return message;
}

ShmRing *MapRing(size_t capacity) {
	void *memory = mmap(nullptr, ShmRing::Size(capacity), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(memory != MAP_FAILED);
	ShmRing *ring = ShmRing::Create(memory, capacity);
	CHECK(ring != nullptr);
	return ring;
}

void Produce(ShmRing *ring, int count) {
	for (int i = 0; i < count; i++) {
		CHECK(ring->Send(MakeMessage(i), In(milliseconds(30000))));
	}
}

void Consume(ShmRing *ring, int count) {
	std::string message;
	for (int i = 0; i < count; i++) {
		CHECK(ring->Receive(message, In(milliseconds(30000))));
		CHECK(message == MakeMessage(i));
	}
}

void TestCrossProcessOrderedDelivery() {
	ShmRing *ring = MapRing(kRingCapacity);
	pid_t child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		ShmRing *attached = ShmRing::Attach(ring);
		CHECK(attached != nullptr);
		Produce(attached, kMessages);
		_exit(0);
	}
	Consume(ring, kMessages);
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void TestTimeouts() {
	ShmRing *ring = MapRing(4096);
	std::string message;
	auto start = Clock::now();
	CHECK(!ring->Receive(message, In(milliseconds(50))));
	CHECK(Clock::now() - start >= milliseconds(50));

	// Nobody reads, so a message larger than the ring cannot fit.
	CHECK(!ring->Send(std::string(8192, 'x'), In(milliseconds(50))));
}

void TestCloseWakesWaiter() {
	// Receiver blocked on an empty ring, sender closes from another process.
	ShmRing *ring = MapRing(4096);
	pid_t child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		usleep(100 * 1000);
		ShmRing::Attach(ring)->Close();
		_exit(0);
	}
	std::string message;
	auto start = Clock::now();
	CHECK(!ring->Receive(message, In(milliseconds(10000))));
	CHECK(Clock::now() - start < milliseconds(5000));
	CHECK(waitpid(child, nullptr, 0) == child);

	// Sender blocked on a full ring, receiver closes.
	ring = MapRing(4096);
	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		usleep(100 * 1000);
		ShmRing::Attach(ring)->Close();
		_exit(0);
	}
	start = Clock::now();
	CHECK(!ring->Send(std::string(8192, 'x'), In(milliseconds(10000))));
	CHECK(Clock::now() - start < milliseconds(5000));
	CHECK(waitpid(child, nullptr, 0) == child);
}

void TestCloseDrainsQueuedMessages() {
	ShmRing *ring = MapRing(4096);
	CHECK(ring->Send("queued", In(milliseconds(100))));
	ring->Close();
	std::string message;
	CHECK(ring->Receive(message, In(milliseconds(100))) && message == "queued");
	CHECK(!ring->Receive(message, In(milliseconds(100))));
	CHECK(!ring->Send("late", In(milliseconds(100))));
}

void TestThreadedStress() {
	// A small ring forces frequent blocking and chunking.
	ShmRing *ring = MapRing(1024);
	constexpr int count = 20000;
	std::thread producer([&] { Produce(ring, count); });
	std::thread consumer([&] { Consume(ring, count); });
	producer.join();
	consumer.join();
}

} // namespace

int main() {
	TestCrossProcessOrderedDelivery();
	TestTimeouts();
	TestCloseWakesWaiter();
	TestCloseDrainsQueuedMessages();
	TestThreadedStress();
	std::puts("shm_ring_test: all passed");
	return 0;
}
