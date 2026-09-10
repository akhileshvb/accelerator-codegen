// GENERATED BACKEND - PostgreSQL scan adapter.
// Real table-AM path is compiled into accelerator_codegen_pg (MODULE).
// Host builds get a fail-closed stub (never a silent no-op).

#include "accelerator_codegen/runtime/postgres/scan_adapter.hpp"

#ifdef ACCELERATOR_CODEGEN_POSTGRES_EXTENSION

extern "C" {
#include <postgres.h>
#include <access/htup_details.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/tupdesc.h>
#include <catalog/namespace.h>
#include <executor/tuptable.h>
#include <miscadmin.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>
}

#include <cctype>
#include <cstring>
#include <stdexcept>

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

std::string DatumToText(Datum d, Oid typid, bool is_null) {
  if (is_null) return {};
  Oid typoutput = InvalidOid;
  bool typisvarlena = false;
  getTypeOutputInfo(typid, &typoutput, &typisvarlena);
  Datum out_datum = OidFunctionCall1(typoutput, d);
  char *str = DatumGetCString(out_datum);
  std::string result(str);
  pfree(str);
  return result;
}

bool NameEquals(const char *a, const std::string &b) {
  return strcasecmp(a, b.c_str()) == 0;
}

PostgresScanRow MakeCell(bool is_null, const std::string &text) {
  PostgresScanRow cell;
  cell.is_null = is_null;
  if (!is_null) {
    cell.data.assign(text.begin(), text.end());
  }
  return cell;
}

} // namespace

void ScanPostgresTableForRebuild(const PostgresScanSpec &scan_spec,
                                 const PostgresScanBatchConsumer &consumer) {
  if (!consumer) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("ScanPostgresTableForRebuild: null consumer")));
  }
  if (scan_spec.table_name.empty() || scan_spec.projected_columns.empty()) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("ScanPostgresTableForRebuild: table/projection required")));
  }

  const char *schema =
      scan_spec.schema_name.empty() ? "public" : scan_spec.schema_name.c_str();
  Oid nspid = get_namespace_oid(schema, false);
  Oid relid = get_relname_relid(scan_spec.table_name.c_str(), nspid);
  if (!OidIsValid(relid)) {
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                    errmsg("ScanPostgresTableForRebuild: relation not found")));
  }

  Relation rel = table_open(relid, AccessShareLock);
  TupleDesc tupdesc = RelationGetDescr(rel);

  std::vector<int> proj_attrs;
  proj_attrs.reserve(scan_spec.projected_columns.size());
  for (const auto &col : scan_spec.projected_columns) {
    bool found = false;
    for (int a = 1; a <= tupdesc->natts; ++a) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, a - 1);
      if (attr->attisdropped) continue;
      if (NameEquals(NameStr(attr->attname), col.name)) {
        proj_attrs.push_back(a);
        found = true;
        break;
      }
    }
    if (!found) {
      table_close(rel, AccessShareLock);
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
                      errmsg("ScanPostgresTableForRebuild: column \"%s\" not found",
                             col.name.c_str())));
    }
  }

  int filter_attnum = 0;
  if (!scan_spec.filter_column.empty()) {
    for (int a = 1; a <= tupdesc->natts; ++a) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, a - 1);
      if (attr->attisdropped) continue;
      if (NameEquals(NameStr(attr->attname), scan_spec.filter_column)) {
        filter_attnum = a;
        break;
      }
    }
  }

  const std::size_t batch_size =
      scan_spec.batch_size == 0 ? 1 : scan_spec.batch_size;
  const std::size_t column_count = proj_attrs.size();

  TableScanDesc scan =
      table_beginscan_strat(rel, GetActiveSnapshot(), 0, nullptr, true, false);
  TupleTableSlot *slot = table_slot_create(rel, nullptr);

  std::vector<PostgresScanRow> batch;
  batch.reserve(batch_size * column_count);
  std::size_t rows_in_batch = 0;

  auto flush = [&]() {
    if (rows_in_batch == 0) return;
    consumer(batch, column_count);
    batch.clear();
    rows_in_batch = 0;
  };

  while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
    if (filter_attnum > 0 && !scan_spec.filter_value.empty()) {
      bool isnull = false;
      Datum fd = slot_getattr(slot, filter_attnum, &isnull);
      Form_pg_attribute fattr = TupleDescAttr(tupdesc, filter_attnum - 1);
      std::string cv =
          isnull ? "NULL" : DatumToText(fd, fattr->atttypid, false);
      bool match = false;
      switch (scan_spec.filter_op) {
      case 0:
        match = (cv == scan_spec.filter_value);
        break;
      case 1:
        match = (cv != scan_spec.filter_value);
        break;
      case 2:
        match = (cv < scan_spec.filter_value);
        break;
      case 3:
        match = (cv > scan_spec.filter_value);
        break;
      default:
        match = false;
        break;
      }
      if (!match) continue;
    }

    for (int attnum : proj_attrs) {
      bool isnull = false;
      Datum d = slot_getattr(slot, attnum, &isnull);
      Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
      if (isnull) {
        batch.push_back(MakeCell(true, {}));
      } else {
        batch.push_back(MakeCell(false, DatumToText(d, attr->atttypid, false)));
      }
    }
    ++rows_in_batch;
    if (rows_in_batch >= batch_size) {
      flush();
    }
  }

  flush();

  ExecDropSingleTupleTableSlot(slot);
  table_endscan(scan);
  table_close(rel, AccessShareLock);
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#else // !ACCELERATOR_CODEGEN_POSTGRES_EXTENSION

#include <stdexcept>

namespace accelerator_codegen::aux_state::engine::postgres {

void ScanPostgresTableForRebuild(const PostgresScanSpec &scan_spec,
                                 const PostgresScanBatchConsumer &consumer) {
  (void)scan_spec;
  (void)consumer;
  throw std::runtime_error(
      "ScanPostgresTableForRebuild requires MODULE accelerator_codegen_pg "
      "(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION); host stub is fail-closed, "
      "not a silent no-op");
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
