#include "tpch/q19/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

int64_t TpchQ19RevenueRaw(int64_t extendedprice_raw, int64_t discount_raw) {
  // l_extendedprice DECIMAL(15,2) and l_discount DECIMAL(15,2).
  // l_extendedprice * (1 - l_discount) has DECIMAL(31,4).
  return extendedprice_raw * (100 - discount_raw);
}

} // namespace accelerator_codegen::aux_state::query
