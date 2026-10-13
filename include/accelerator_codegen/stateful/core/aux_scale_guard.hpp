#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace accelerator_codegen::aux_state::core {

// Build-time refuse for aux modules whose published/hit state is fundamentally
// O(|orders|) / O(|parts|) / O(partsupp×lineitem) and cannot be lean-fixed.
//
// Threshold is above SF100 (orders ≈ 150M) and below SF300 (orders ≈ 450M):
// SF1–SF100 unit/bench builds proceed; SF300/SF3000 Build throws ScaleTooLarge.
constexpr int64_t kAuxScaleTooLargeOrders = 200'000'000LL;
// SF200 parts ≈ 40M; SF300 parts ≈ 60M.
constexpr int64_t kAuxScaleTooLargeParts = 40'000'000LL;
// SF200 partsupp ≈ 80M; pair_series grows with qualifying (part,supp)×ship dates.
constexpr int64_t kAuxScaleTooLargePartsuppPairs = 80'000'000LL;

[[noreturn]] inline void ThrowScaleTooLarge(const char *query,
                                            const char *detail) {
  throw std::runtime_error(std::string("ScaleTooLarge: ") + query +
                           " aux Build refused (" + detail +
                           "). Threshold: orders>" +
                           std::to_string(kAuxScaleTooLargeOrders) +
                           " or parts>" +
                           std::to_string(kAuxScaleTooLargeParts) + ".");
}

inline void GuardOrdersScale(const char *query, int64_t order_rows_or_max_key) {
  if (order_rows_or_max_key > kAuxScaleTooLargeOrders)
    ThrowScaleTooLarge(query, "orders/max_orderkey exceeds threshold");
}

inline void GuardPartsScale(const char *query, int64_t part_rows_or_max_key) {
  if (part_rows_or_max_key > kAuxScaleTooLargeParts)
    ThrowScaleTooLarge(query, "parts/max_partkey exceeds threshold");
}

inline void GuardPartsuppPairsScale(const char *query, int64_t pair_count) {
  if (pair_count > kAuxScaleTooLargePartsuppPairs)
    ThrowScaleTooLarge(query, "partsupp pair_series exceeds threshold");
}

} // namespace accelerator_codegen::aux_state::core
