// GENERATED BACKEND — engine-local scan helper for Postgres native table-AM
// Uses table_openrv_extended, table_beginscan, table_scan_getnextslot,
// slot_getattr to stream rows. No client-SQL round-trip. No portable chunk IR.

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/htup_details.h>
#include <catalog/pg_type_d.h>
#include <utils/builtins.h>
#include <utils/rel.h>
#include <utils/lsyscache.h>
#include <utils/snapmgr.h>
#include <executor/tuptable.h>
#include <miscadmin.h>
}

#include <cstring>
#include <string>
#include <vector>

#include "accelerator_codegen/runtime/postgres/scan_helper.hpp"

namespace accelerator_codegen::aux_state::runtime::postgres {

namespace {

char *FormatDatumAsCString(Datum d, Oid typid, bool isnull) {
  if (isnull) {
    return nullptr;
  }
  if (typid == INT4OID) {
    int32_t v = DatumGetInt32(d);
    char *buf = (char *)palloc(32);
    snprintf(buf, 32, "%d", v);
    return buf;
  }
  if (typid == INT8OID) {
    int64_t v = DatumGetInt64(d);
    char *buf = (char *)palloc(32);
    snprintf(buf, 32, "%ld", (long)v);
    return buf;
  }
  if (typid == INT2OID) {
    int16_t v = DatumGetInt16(d);
    char *buf = (char *)palloc(32);
    snprintf(buf, 32, "%d", (int)v);
    return buf;
  }
  if (typid == BOOLOID) {
    bool v = DatumGetBool(d);
    char *buf = (char *)palloc(8);
    snprintf(buf, 8, "%s", v ? "true" : "false");
    return buf;
  }
  if (typid == FLOAT4OID) {
    float4 v = DatumGetFloat4(d);
    char *buf = (char *)palloc(64);
    snprintf(buf, 64, "%.10g", (double)v);
    return buf;
  }
  if (typid == FLOAT8OID) {
    float8 v = DatumGetFloat8(d);
    char *buf = (char *)palloc(64);
    snprintf(buf, 64, "%.15g", v);
    return buf;
  }
  return TextDatumGetCString(d);
}

} // namespace

void ScanNativeTable(const PostgresScanSpec &spec,
                     const PostgresScanRowConsumer &consumer) {
  RangeVar rv = {};
  rv.relname = const_cast<char *>(spec.table_name.c_str());

  Relation rel = table_openrv_extended(&rv, AccessShareLock, true);
  if (!rel) {
    return;
  }

  TupleDesc tupdesc = RelationGetDescr(rel);
  int natts = tupdesc->natts;

  // Resolve projected column indices and types
  std::vector<int> proj_attnums;
  std::vector<Oid> proj_types;
  std::vector<std::string> proj_names;
  for (const auto &col_name : spec.projected_columns) {
    for (int i = 0; i < natts; ++i) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
      if (attr->attisdropped) {
        continue;
      }
      if (NameStr(attr->attname) == col_name) {
        proj_attnums.push_back(i + 1);
        proj_types.push_back(attr->atttypid);
        proj_names.push_back(col_name);
        break;
      }
    }
  }

  // Resolve filter column index
  int filter_attnum = -1;
  Oid filter_type = InvalidOid;
  if (!spec.filter_column.empty()) {
    for (int i = 0; i < natts; ++i) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
      if (attr->attisdropped) {
        continue;
      }
      if (NameStr(attr->attname) == spec.filter_column) {
        filter_attnum = i + 1;
        filter_type = attr->atttypid;
        break;
      }
    }
  }

  TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, nullptr);
  TupleTableSlot *slot = table_slot_create(rel, nullptr);

  while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
    bool slot_is_null = false;

    // Apply filter
    bool passes_filter = true;
    if (filter_attnum > 0 && spec.filter_op == "eq") {
      Datum filter_datum = slot_getattr(slot, filter_attnum, &slot_is_null);
      if (slot_is_null) {
        passes_filter = false;
      } else {
        char *filter_text = FormatDatumAsCString(filter_datum, filter_type, false);
        if (!filter_text) {
          passes_filter = false;
        } else {
          passes_filter = (strcmp(filter_text, spec.filter_value.c_str()) == 0);
          if (filter_type == INT4OID || filter_type == INT8OID ||
              filter_type == INT2OID || filter_type == FLOAT4OID ||
              filter_type == FLOAT8OID || filter_type == BOOLOID) {
            pfree(filter_text);
          }
        }
      }
    }

    if (!passes_filter) {
      ExecClearTuple(slot);
      continue;
    }

    // Build projected row
    std::vector<const char *> col_values;
    std::vector<char *> allocd_values;
    for (size_t pi = 0; pi < proj_attnums.size(); ++pi) {
      int attnum = proj_attnums[pi];
      Oid typid = proj_types[pi];
      bool isnull = false;
      Datum d = slot_getattr(slot, attnum, &isnull);

      char *val = FormatDatumAsCString(d, typid, isnull);
      col_values.push_back(val);
      if (!isnull && val) {
        if (typid == INT4OID || typid == INT8OID ||
            typid == INT2OID || typid == FLOAT4OID ||
            typid == FLOAT8OID || typid == BOOLOID) {
          allocd_values.push_back(val);
        }
      }
    }

    std::vector<const char *> c_names;
    for (const auto &n : proj_names) {
      c_names.push_back(n.c_str());
    }

    consumer(c_names, col_values, (int)col_values.size());

    // Free palloc'd buffers
    for (char *buf : allocd_values) {
      pfree(buf);
    }

    ExecClearTuple(slot);
  }

  ExecDropSingleTupleTableSlot(slot);
  table_endscan(scan);
  table_close(rel, AccessShareLock);
}

} // namespace accelerator_codegen::aux_state::runtime::postgres
