// Single-producer / single-consumer message ring over a caller-provided memory
// region (e.g. a DSM segment mapped in several processes).
//
// Pure C++/Linux: no Postgres APIs, so it is safe on threads that must never
// call into Postgres. Blocking uses cross-process futexes on words in the region.
//
// A message is sent as one or more chunks, so it may be larger than the ring;
// the receiver reassembles it. Each ring is one direction; use two per client.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace accel {

struct ShmRing {
	using Clock = std::chrono::steady_clock;

	// Bytes of memory needed for a ring with `capacity_bytes` of data space.
	static size_t Size(size_t capacity_bytes);

	// `memory` must be 64-byte aligned and Size(capacity_bytes) bytes long.
	// Returns nullptr if the capacity is too small to be useful.
	static ShmRing *Create(void *memory, size_t capacity_bytes);
	// Attaches to a ring set up by Create (in any process); nullptr if none.
	static ShmRing *Attach(void *memory);

	// Blocks until the whole message is queued. False on deadline, or if the
	// ring is closed. A deadline hit after part of the message was queued closes
	// the ring, since the receiver would otherwise see a truncated message.
	bool Send(std::string_view message, Clock::time_point deadline);

	// Blocks until a whole message is reassembled into `out`. False on deadline,
	// or if the ring is closed and drained. A deadline hit mid-message closes
	// the ring, as with Send.
	bool Receive(std::string &out, Clock::time_point deadline);

	// Blocks until a message (or the first chunk of one) can be received, the
	// ring is closed (both true), or the deadline passes (false). Lets a receiver
	// that must stay interruptible wait in slices without risking Receive's
	// mid-message close.
	bool WaitForData(Clock::time_point deadline);

	// Declares this side gone: wakes every waiter, and later Sends fail.
	// Messages already queued can still be received.
	void Close();

private:
	// A futex word plus a count of threads that may be sleeping on it, so the
	// waking side can skip the syscall when nobody waits.
	struct Signal {
		std::atomic<uint32_t> seq;
		std::atomic<uint32_t> waiters;
	};

	bool HasData() const;
	bool HasSpace() const;
	// Sleeps until `ready` holds, the ring is closed (both return true), or the
	// deadline passes (false).
	bool Wait(Signal &signal, bool (ShmRing::*ready)() const, Clock::time_point deadline);
	void Wake(Signal &signal);
	void CopyIn(uint64_t pos, const void *src, size_t n);
	void CopyOut(uint64_t pos, void *dst, size_t n) const;
	unsigned char *Data();
	const unsigned char *Data() const;

	std::atomic<uint64_t> magic_;
	uint64_t capacity_;
	std::atomic<uint32_t> closed_;

	// Producer-written, consumer-read; byte counters that only grow.
	alignas(64) std::atomic<uint64_t> head_;
	Signal data_signal_;
	// Consumer-written, producer-read.
	alignas(64) std::atomic<uint64_t> tail_;
	Signal space_signal_;

	// The data area follows the header (see Data()).
	alignas(64) unsigned char data_[1];
};

} // namespace accel
