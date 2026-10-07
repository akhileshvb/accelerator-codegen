#pragma once

// Query-local helper for TPC-H Q3: batched branch-free bitmap membership tests over (optionally
// selection-indirect) int64 keys.  Used for the customer-segment and qualifying-order probes.
// (An AVX-512 gather variant measured slower than this scalar loop on SF1.)
#include <cstddef>
#include <cstdint>

#include "accelerator_codegen/runtime/simd.hpp"

namespace accelerator_codegen {
namespace q3simd {

//! Writes to out (capacity n + simd::kSelSlack) every position i in [0, n) whose key
//! keys[idx ? idx[i] : i] is a member of the bitmap (bit k of bits[], valid for k < nbits).
//! Keys outside [0, nbits) are never members.  Positions are ascending.
inline std::size_t SelectBits(const int64_t *ACCEL_RESTRICT keys, const uint32_t *ACCEL_RESTRICT idx, std::size_t n,
                              const uint64_t *ACCEL_RESTRICT bits, uint64_t nbits, uint32_t *ACCEL_RESTRICT out) {
	std::size_t count = 0, i = 0;
	for (; i < n; i++) {
		const uint64_t k = static_cast<uint64_t>(keys[idx ? idx[i] : i]);
		const bool member = k < nbits && ((bits[k >> 6] >> (k & 63)) & 1);
		out[count] = static_cast<uint32_t>(i);
		count += member ? 1 : 0;
	}
	return count;
}

} // namespace q3simd
} // namespace accelerator_codegen
