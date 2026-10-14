#pragma once

// TPC-H table-id vocabulary for the per-query MySQL modules
// (accelerators/tpch/q<N>/mysql/**), which name their tables. It is NOT used
// by the generic MySQL host layer (src/stateful/mysql, backends/mysql/api):
// capture decodes rows from self-describing trigger calls
// (mysql_accel_capture_row, scripts/gen_mysql_capture_triggers.py) and keys on


#include "substrate/aux/core/aux_types.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace accelerator_codegen::aux_state::engine::mysql::tpch_tables {

inline constexpr core::TableId kLineitem = 1;
inline constexpr core::TableId kOrders = 2;
inline constexpr core::TableId kCustomer = 3;
inline constexpr core::TableId kPart = 4;
inline constexpr core::TableId kPartsupp = 5;
inline constexpr core::TableId kSupplier = 6;
inline constexpr core::TableId kNation = 7;
inline constexpr core::TableId kRegion = 8;

inline core::TableId IdFromRelname(std::string_view name) {
  if (name == "lineitem")
    return kLineitem;
  if (name == "orders")
    return kOrders;
  if (name == "customer")
    return kCustomer;
  if (name == "part")
    return kPart;
  if (name == "partsupp")
    return kPartsupp;
  if (name == "supplier")
    return kSupplier;
  if (name == "nation")
    return kNation;
  if (name == "region")
    return kRegion;
  return 0;
}

} // namespace accelerator_codegen::aux_state::engine::mysql::tpch_tables
