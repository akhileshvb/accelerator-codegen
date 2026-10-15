#include "backends/postgres/state/shm_ring.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <new>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace accel {

static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared-memory atomics must be lock-free");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared-memory atomics must be lock-free");

namespace {

constexpr uint64_t kMagic = 0x53484d52494e4731ULL; // "SHMRING1"
constexpr size_t kAlignment = 64;
constexpr int kSpinIterations = 100;

// Each chunk is a 4-byte header followed by its payload. The header holds the
// payload length, with the top bit set on a message's last chunk.
constexpr size_t kChunkHeaderBytes = sizeof(uint32_t);
constexpr uint32_t kLastChunkFlag = 0x80000000u;
constexpr uint32_t kMaxChunkPayload = kLastChunkFlag - 1;

void FutexWait(std::atomic<uint32_t> &word, uint32_t expected, ShmRing::Clock::time_point deadline) {
	auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - ShmRing::Clock::now());
	if (remaining.count() <= 0) {
		return;
	}
	timespec timeout;
	timeout.tv_sec = static_cast<time_t>(remaining.count() / 1000000000);
	timeout.tv_nsec = static_cast<long>(remaining.count() % 1000000000);
	// Not FUTEX_PRIVATE: the word is shared between processes. Spurious wake-ups,
	// EAGAIN and timeouts are all handled by the caller re-checking its condition.
	syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAIT, expected, &timeout, nullptr, 0);
}

} // namespace

size_t ShmRing::Size(size_t capacity_bytes) {
	return offsetof(ShmRing, data_) + capacity_bytes;
}

ShmRing *ShmRing::Create(void *memory, size_t capacity_bytes) {
	if (capacity_bytes <= kChunkHeaderBytes || reinterpret_cast<uintptr_t>(memory) % kAlignment != 0) {
		return nullptr;
	}
	auto *ring = new (memory) ShmRing;
	ring->capacity_ = capacity_bytes;
	ring->closed_.store(0, std::memory_order_relaxed);
	ring->head_.store(0, std::memory_order_relaxed);
	ring->tail_.store(0, std::memory_order_relaxed);
	for (Signal *signal : {&ring->data_signal_, &ring->space_signal_}) {
		signal->seq.store(0, std::memory_order_relaxed);
		signal->waiters.store(0, std::memory_order_relaxed);
	}
	// Published last so an Attach that sees the magic sees an initialised ring.
	ring->magic_.store(kMagic, std::memory_order_release);
	return ring;
}

ShmRing *ShmRing::Attach(void *memory) {
	auto *ring = static_cast<ShmRing *>(memory);
	return ring->magic_.load(std::memory_order_acquire) == kMagic ? ring : nullptr;
}

unsigned char *ShmRing::Data() {
	return data_;
}

const unsigned char *ShmRing::Data() const {
	return data_;
}

bool ShmRing::HasData() const {
	return tail_.load(std::memory_order_relaxed) != head_.load(std::memory_order_acquire);
}

bool ShmRing::HasSpace() const {
	uint64_t used = head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire);
	return capacity_ - used > kChunkHeaderBytes;
}

// Wake protocol: a waiter registers in `waiters` and then re-checks its
// condition against the futex word it read; the waker publishes its change,
// bumps `seq`, and issues FUTEX_WAKE only if `waiters` is non-zero. The
// seq_cst operations on both sides guarantee that either the waker sees the
// waiter or the waiter sees the change, so no wake-up is lost.
bool ShmRing::Wait(Signal &signal, bool (ShmRing::*ready)() const, Clock::time_point deadline) {
	for (int i = 0; i < kSpinIterations; i++) {
		if ((this->*ready)() || closed_.load(std::memory_order_acquire)) {
			return true;
		}
	}
	signal.waiters.fetch_add(1, std::memory_order_seq_cst);
	bool ok = false;
	while (true) {
		uint32_t seq = signal.seq.load(std::memory_order_seq_cst);
		if ((this->*ready)() || closed_.load(std::memory_order_seq_cst)) {
			ok = true;
			break;
		}
		if (Clock::now() >= deadline) {
			break;
		}
		FutexWait(signal.seq, seq, deadline);
	}
	signal.waiters.fetch_sub(1, std::memory_order_seq_cst);
	return ok;
}

void ShmRing::Wake(Signal &signal) {
	signal.seq.fetch_add(1, std::memory_order_seq_cst);
	if (signal.waiters.load(std::memory_order_seq_cst) != 0) {
		syscall(SYS_futex, reinterpret_cast<uint32_t *>(&signal.seq), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
	}
}

void ShmRing::CopyIn(uint64_t pos, const void *src, size_t n) {
	size_t offset = pos % capacity_;
	size_t first = std::min<size_t>(n, capacity_ - offset);
	std::memcpy(Data() + offset, src, first);
	std::memcpy(Data(), static_cast<const unsigned char *>(src) + first, n - first);
}

void ShmRing::CopyOut(uint64_t pos, void *dst, size_t n) const {
	size_t offset = pos % capacity_;
	size_t first = std::min<size_t>(n, capacity_ - offset);
	std::memcpy(dst, Data() + offset, first);
	std::memcpy(static_cast<unsigned char *>(dst) + first, Data(), n - first);
}

bool ShmRing::Send(std::string_view message, Clock::time_point deadline) {
	size_t sent = 0;
	do {
		if (!Wait(space_signal_, &ShmRing::HasSpace, deadline) || closed_.load(std::memory_order_acquire)) {
			if (sent > 0) {
				Close();
			}
			return false;
		}
		uint64_t head = head_.load(std::memory_order_relaxed);
		uint64_t used = head - tail_.load(std::memory_order_acquire);
		size_t room = static_cast<size_t>(capacity_ - used) - kChunkHeaderBytes;
		size_t remaining = message.size() - sent;
		uint32_t length = static_cast<uint32_t>(std::min<size_t>({remaining, room, kMaxChunkPayload}));
		bool last = length == remaining;
		uint32_t header = length | (last ? kLastChunkFlag : 0);

		CopyIn(head, &header, kChunkHeaderBytes);
		CopyIn(head + kChunkHeaderBytes, message.data() + sent, length);
		head_.store(head + kChunkHeaderBytes + length, std::memory_order_release);
		Wake(data_signal_);
		sent += length;
	} while (sent < message.size());
	return true;
}

bool ShmRing::Receive(std::string &out, Clock::time_point deadline) {
	out.clear();
	bool started = false;
	while (true) {
		if (!Wait(data_signal_, &ShmRing::HasData, deadline) || !HasData()) {
			if (started) {
				Close();
			}
			return false;
		}
		uint64_t tail = tail_.load(std::memory_order_relaxed);
		uint32_t header;
		CopyOut(tail, &header, kChunkHeaderBytes);
		size_t length = header & kMaxChunkPayload;

		size_t old_size = out.size();
		out.resize(old_size + length);
		CopyOut(tail + kChunkHeaderBytes, &out[old_size], length);
		tail_.store(tail + kChunkHeaderBytes + length, std::memory_order_release);
		Wake(space_signal_);
		if (header & kLastChunkFlag) {
			return true;
		}
		started = true;
	}
}

bool ShmRing::WaitForData(Clock::time_point deadline) {
	return Wait(data_signal_, &ShmRing::HasData, deadline);
}

void ShmRing::Close() {
	closed_.store(1, std::memory_order_seq_cst);
	Wake(data_signal_);
	Wake(space_signal_);
}

} // namespace accel
