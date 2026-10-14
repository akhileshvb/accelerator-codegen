// GENERATED QUERY (sec_edgar, shared by its queries): probes that update a
// `num` row's value by rowid.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine::sec_edgar_probes {

namespace core = accelerator_codegen::aux_state::core;
using namespace catalog_values;

// Physical DuckDB `num` layout used by SecEdgarQ{2,3,4,6,24}::ReadNumRow:
//   0 adsh, 1 tag, 2 version, 3 ddate, 4 qtrs, 5 uom, 6 coreg, 7 value
// Compact probe AuxRows previously caused MarkRebuild on every write.
inline core::AuxRow MakeNumTableAuxRow(const ::duckdb::Value &adsh,
                                const ::duckdb::Value &tag,
                                const ::duckdb::Value &version,
                                const ::duckdb::Value &ddate,
                                const ::duckdb::Value &qtrs,
                                const ::duckdb::Value &uom,
                                const ::duckdb::Value &coreg,
                                const ::duckdb::Value &value) {
  return {{Text(adsh), Text(tag), Text(version), Int(ddate), Int(qtrs), Text(uom),
           Text(coreg), Number(value)}};
}

// +0.001 is lost in double ULP for large USD facts and is far too small to
// enter a LIMIT-N result when the probe row is not already in that result.
inline double VisibleSecNumDelta(double value) {
  return std::max(1.0, std::abs(value) * 0.01);
}

inline SharedProbe BuildSecNumRowidProbe(::duckdb::MaterializedQueryResult &result,
                                  ::duckdb::idx_t row, int table) {
  // probe_select: rowid, adsh, tag, version, ddate, qtrs, uom, coreg, value
  const auto rowid = result.GetValue(0, row).ToString();
  const double value = std::stod(result.GetValue(8, row).ToString());
  const double delta = VisibleSecNumDelta(value);
  SharedProbe probe;
  probe.table = table;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {7}; // num.value
  probe.write_delta = delta;
  probe.current_value = value;
  probe.old_row =
      MakeNumTableAuxRow(result.GetValue(1, row), result.GetValue(2, row),
                         result.GetValue(3, row), result.GetValue(4, row),
                         result.GetValue(5, row), result.GetValue(6, row),
                         result.GetValue(7, row), result.GetValue(8, row));
  probe.new_row = probe.old_row;
  probe.new_row.values[7] =
      core::AuxValue::Of(core::AuxType::Double, value + delta);
  probe.update_sql = "UPDATE num SET value = value + " + std::to_string(delta) +
                     " WHERE rowid = " + rowid;
  probe.restore_sql = "UPDATE num SET value = " + std::to_string(value) +
                      " WHERE rowid = " + rowid;
  return probe;
}

// Filler rows for batched writes (CollectBatchWritePool), used when a query's
// probe_select_sql (the rows feeding its result) has fewer distinct rows than
// a batch run touches: rows of the same table/column that pass the query's
// own predicate on that table, so every write still moves a value the query
// reads. Same columns as the query's probe_select_sql. Null: none.
inline const char *BatchFillerSelectSql(int query_id) {
  switch (query_id) {
  case 1:
    return "SELECT adsh, line, stmt, rfile, rowid FROM pre WHERE stmt IS NOT NULL "
           "AND line IS NOT NULL LIMIT 1";
  case 2:
    return "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
           "n.value FROM num n WHERE n.uom = 'pure' AND n.value IS NOT NULL LIMIT 1";
  case 3:
  case 4:
  case 6:
    return "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
           "n.value FROM num n WHERE n.uom = 'USD' AND n.value IS NOT NULL LIMIT 1";
  case 24:
    return "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
           "n.value FROM num n WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 "
           "AND 20231231 AND n.value IS NOT NULL LIMIT 1";
  default:
    return nullptr;
  }
}

} // namespace accelerator_codegen::aux_state::engine::sec_edgar_probes
