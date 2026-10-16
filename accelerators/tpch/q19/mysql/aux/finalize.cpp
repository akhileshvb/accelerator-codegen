#include "tpch/q19/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

std::string TpchQ19ShippingSql() {
  return "l_shipmode IN ('AIR', 'AIR REG') "
         "AND l_shipinstruct = 'DELIVER IN PERSON'";
}

} // namespace accelerator_codegen::aux_state::query
