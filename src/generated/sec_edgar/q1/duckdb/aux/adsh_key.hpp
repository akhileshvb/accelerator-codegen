#pragma once

// Packing of SEC accession numbers ("0001234567-22-000123", 20 chars) into a
// 60-bit integer key. This removes the 20-byte string from the aux state and
// turns every adsh comparison/hash into a single 64-bit operation.
//
// The SSE4.1 kernel is selected at runtime (__builtin_cpu_supports); the
// scalar kernel is the portable reference and is always compiled.

#include <cstddef>
#include <cstdint>

namespace duckdb {
struct string_t;
}

namespace accelerator_codegen::aux_state::query::q1 {

constexpr uint64_t kInvalidAdshKey = ~uint64_t(0);

// Scalar reference. Returns false when `s` is not exactly DDDDDDDDDD-DD-DDDDDD.
bool ParseAdshKeyScalar(const char *s, std::size_t len, uint64_t &key);

// Dispatching single-value parse (SIMD when available).
bool ParseAdshKey(const char *s, std::size_t len, uint64_t &key);

// Batch parse of `count` string_t values; out_keys[i] = kInvalidAdshKey when the
// value is not in canonical form (caller falls back to string interning).
// `sel` (optional) maps output slot -> input index.
void ParseAdshKeyBatch(const duckdb::string_t *values, const uint32_t *sel,
                       std::size_t count, uint64_t *out_keys);

// For tests: force the scalar / SIMD path.
bool AdshKeySimdAvailable();
void ParseAdshKeyBatchScalar(const duckdb::string_t *values, const uint32_t *sel,
                             std::size_t count, uint64_t *out_keys);

} // namespace accelerator_codegen::aux_state::query::q1
