#include "tpch/q19/postgres/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// The query's three OR branches, as (container family, p_size range) pairs.
std::string TpchQ19FamilySql() {
  return "CASE "
         "WHEN p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
         "AND p_size BETWEEN 1 AND 5 THEN 1 "
         "WHEN p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
         "AND p_size BETWEEN 1 AND 10 THEN 2 "
         "WHEN p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
         "AND p_size BETWEEN 1 AND 15 THEN 3 END";
}

std::string TpchQ19ShippingSql() {
  return "l_shipmode IN ('AIR', 'AIR REG') "
         "AND l_shipinstruct = 'DELIVER IN PERSON'";
}

} // namespace accelerator_codegen::aux_state::query
