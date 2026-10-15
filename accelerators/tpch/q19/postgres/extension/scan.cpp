// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q19 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q19/postgres/aux/module.hpp"
#include "tpch/q19/cost_model.hpp"
#include <stdexcept>


#include "prims/decimal_text.hpp"
#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ19Parts(
    const std::function<void(const query::TpchQ19PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container, p_size FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

// lineitem JOIN part with the shipping predicate and family classification
// applied and grouped in the engine: one row per (brand, family, quantity)
// with exact numeric sums (text).
void ScanTpchQ19GroupAggregates(
    const std::function<void(const query::TpchQ19GroupAggregate &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  const std::string family = query::TpchQ19FamilySql();
  ScanTypedTuples(
      "SELECT p_brand, " + family +
          " AS fam, l_quantity, sum(l_extendedprice * (1 - l_discount)), "
          "count(*) FROM lineitem, part WHERE l_partkey = p_partkey AND " +
          query::TpchQ19ShippingSql() + " AND " + family +
          " IS NOT NULL GROUP BY p_brand, fam, l_quantity",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        query::TpchQ19GroupAggregate agg;
        agg.brand = TypedText(tuple, tuple_desc, 0);
        agg.family = static_cast<int>(TypedInt(tuple, tuple_desc, 1));
        __int128 quantity = 0;
        if (!dec::Parse(TypedText(tuple, tuple_desc, 2), 2, quantity) ||
            !dec::Parse(TypedText(tuple, tuple_desc, 3), 4, agg.revenue_raw))
          throw std::runtime_error("TPC-H Q19: unparsable numeric");
        agg.quantity_raw = static_cast<int64_t>(quantity);
        agg.count = TypedInt(tuple, tuple_desc, 4);
        consumer(agg);
      });
}

// Point re-aggregation of the given parts' qualifying lineitems (l_partkey is
// indexed).
void ScanTpchQ19PartQuantityRevenue(
    const std::vector<int64_t> &partkeys,
    const std::function<void(const query::TpchQ19PartQuantityRevenue &)>
        &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  ScanTypedTuplesAfterOwnWrites(
      "SELECT l_partkey, l_quantity, sum(l_extendedprice * (1 - l_discount)), "
      "count(*) FROM lineitem WHERE " +
          KeyListPredicate("l_partkey", partkeys) + " AND " +
          query::TpchQ19ShippingSql() + " GROUP BY l_partkey, l_quantity",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        query::TpchQ19PartQuantityRevenue row;
        row.partkey = TypedInt(tuple, tuple_desc, 0);
        __int128 quantity = 0;
        if (!dec::Parse(TypedText(tuple, tuple_desc, 1), 2, quantity) ||
            !dec::Parse(TypedText(tuple, tuple_desc, 2), 4, row.revenue_raw))
          throw std::runtime_error("TPC-H Q19: unparsable numeric");
        row.quantity_raw = static_cast<int64_t>(quantity);
        row.count = TypedInt(tuple, tuple_desc, 3);
        consumer(row);
      });
}

} // namespace scans

class TpchQ19Scan final : public query::TpchQ19InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ19PartInput &)> &consumer)
      override {
    scans::ScanTpchQ19Parts(consumer);
  }
  void ScanGroupAggregates(
      const std::function<void(const query::TpchQ19GroupAggregate &)> &consumer)
      override {
    scans::ScanTpchQ19GroupAggregates(consumer);
  }
  void ScanPartQuantityRevenue(
      const std::vector<int64_t> &partkeys,
      const std::function<void(const query::TpchQ19PartQuantityRevenue &)>
          &consumer) override {
    scans::ScanTpchQ19PartQuantityRevenue(partkeys, consumer);
  }
};
Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan> &
Q19Runtime() {
  static Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q19_COST_MODEL",
              core::kTpchQ19CostModel,
              query::TpchQ19Layout::kAccelerator,
              query::TpchQ19Config{pg_tbl::kLineitem, pg_tbl::kPart});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ19(FunctionCallInfo fcinfo) {
  // A NULL argument is passed on as a NULL value (its OR branch is then NULL).
  const auto text = [&](int index) {
    return PG_ARGISNULL(index)
               ? core::AuxValue::Null(core::AuxType::String)
               : core::AuxValue::Of(core::AuxType::String,
                                    detail::TextDatum(PG_GETARG_DATUM(index)));
  };
  const auto integer = [&](int index) {
    return PG_ARGISNULL(index)
               ? core::AuxValue::Null(core::AuxType::Int64)
               : core::AuxValue::Of(core::AuxType::Int64, PG_GETARG_INT64(index));
  };
  detail::Q19Runtime().Execute(
      core::QueryArguments{{text(0), integer(1), text(2), integer(3), text(4),
                            integer(5)}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 19, kPostgresVariantAux, &ExecutePostgresTpchQ19});
const detail::RuntimeRegistrar<&detail::Q19Runtime> kAcceleratorRegistrar(
    query::TpchQ19Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
