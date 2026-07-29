#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"
#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "duckdb.hpp"
#include <iostream>

int main() {
  duckdb::DuckDB db("/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb");
  duckdb::Connection c(db);
  accelerator_codegen::RegisterAcceleratorFunctions(c);
  c.Query("SELECT * FROM accel_q6_aux_build()");
  auto q25dec = c.Query("SELECT SUM(l_extendedprice * l_discount) FROM lineitem WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' AND l_discount >= 0.02 AND l_discount <= 0.04 AND l_quantity < 25.00");
  auto q25int = c.Query("SELECT SUM(l_extendedprice * l_discount) FROM lineitem WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' AND l_discount >= 0.02 AND l_discount <= 0.04 AND l_quantity < 25");
  auto q24 = c.Query("SELECT SUM(l_extendedprice * l_discount) FROM lineitem WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' AND l_discount >= 0.02 AND l_discount <= 0.04 AND l_quantity < 24.00");
  std::cout << "q25dec=" << q25dec->GetValue(0,0).ToString() << " q25int=" << q25int->GetValue(0,0).ToString() << " q24=" << q24->GetValue(0,0).ToString() << "\n";
  const auto start = accelerator_codegen::Q6PartitionKeyFromShipdate(duckdb::Date::FromString("1995-01-01"));
  const auto end = accelerator_codegen::Q6PartitionKeyFromShipdate(duckdb::Date::FromString("1996-01-01"));
  auto registry = accelerator_codegen::aux_state::AcceleratorStateRegistry::GetOrCreate(*c.context->db);
  auto &store = registry->GetStore(accelerator_codegen::kQ6AuxAcceleratorId);
  accelerator_codegen::Q6AccT b25=0,b24=0,b2499=0;
  store.VisitReadablePartitionsInRange(start,end,[&](auto, const std::shared_ptr<const void> &p){
    const auto &day = *std::static_pointer_cast<const accelerator_codegen::Q6DayPartition>(p);
    accelerator_codegen::MergeQ6DayRevenue(b25, day, 2, 4, 2500);
    accelerator_codegen::MergeQ6DayRevenue(b24, day, 2, 4, 2400);
    accelerator_codegen::MergeQ6DayRevenue(b2499, day, 2, 4, 2499);
  });
  std::cout << "store25=" << (long long)b25 << " store24=" << (long long)b24 << " store2499=" << (long long)b2499 << "\n";
}
