// GENERATED BACKEND - PostgreSQL extension entrypoint (serialized_boundary).
// Pure PostgreSQL C module. All substrate interactions happen
// through the base library (libaccelerator_codegen.so), not in this TU.
//
// Implements:
//  - Native scan helper using table-AM (table_open/table_beginscan/table_scan_getnextslot)
//  - Trigger: AFTER ROW value-level mutation capture
//  - Toy store: build/read
//  - Observer accessors

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <access/htup_details.h>
#include <access/relscan.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/tupdesc.h>
#include <catalog/namespace.h>
#include <catalog/pg_type_d.h>
#include <commands/trigger.h>
#include <executor/spi.h>
#include <executor/tuptable.h>
#include <funcapi.h>
#include <nodes/makefuncs.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>
#include <utils/tuplesort.h>
PG_MODULE_MAGIC;
}

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <functional>

#include "accelerator_codegen/runtime/postgres/scan_adapter.hpp"
#include <algorithm>
#include <limits>
#include <unordered_set>

extern "C" {
void accel_pg_init_backend(const char *backing_dir);
void accel_pg_capture_mutation_value(int table_oid, int kind,
                                     const char *old_jsn, const char *new_jsn,
                                     const char *updated_cols_jsn);
int  accel_pg_toy_build(const char *key, int64_t value);
int  accel_pg_toy_read(int64_t *out_value);
int  accel_pg_get_observer_mutation_count();
int  accel_pg_get_observer_pre_committed();
int  accel_pg_get_observer_committed();
int  accel_pg_get_observer_aborted();
void accel_pg_reset_observer();
const char *accel_pg_get_observer_mutation_at(int idx);
const char *accel_pg_get_observer_mutation_row(int idx, int row_num, int is_new);
const char *accel_pg_get_last_new_json();
const char *accel_pg_get_last_old_json();
void accel_pg_pre_commit_hook();
void accel_pg_commit_hook();
void accel_pg_abort_hook();
int  accel_pg_get_scan_row_count();
int  accel_pg_get_scan_column_count();
int  accel_pg_get_scan_callback_count();
const char *accel_pg_get_scan_row_cell(int row_idx, int col_idx);
void accel_pg_scan_clear();
void accel_pg_scan_callback_push(int column_count, const char *cells_json);
void accel_pg_scan_callback_inc();
int  accel_pg_toy_current_epoch(uint64_t *out_epoch);
int  accel_pg_toy_check_validity(const char *key, uint64_t visibility_epoch,
                                  int *out_code, uint64_t *out_version);
void accel_pg_toy_invalidate();
int  accel_pg_toy_stage_build(const char *key, int64_t value);
void accel_pg_flush_staged_toy_builds();
void accel_pg_clear_staged_toy_builds();
}

namespace {
bool s_initialized = false;
void EnsureInit() {
  if (!s_initialized) {
    accel_pg_init_backend("/tmp/accel_pg_aux_state");
    s_initialized = true;
  }
}

std::string DatumToCStr(Datum d, Oid typid, bool is_null) {
  if (is_null) return "NULL";
  Oid typoutput; bool typisvarlena;
  getTypeOutputInfo(typid, &typoutput, &typisvarlena);
  Datum out_datum = OidFunctionCall1(typoutput, d);
  char *str = DatumGetCString(out_datum);
  std::string result(str);
  pfree(str);
  return result;
}

// Convert a single column value to its string representation.
// Avoids OidFunctionCall1 / type output functions which may have
// side effects in trigger context.
static std::string AttrToStr(TupleTableSlot *slot, TupleDesc tupdesc, int attnum) {
  if (!slot) return "NULL";
  if (!tupdesc) return "NULL";
  bool isnull = false;
  slot_getallattrs(slot);
  Datum d = slot_getattr(slot, attnum, &isnull);
  if (isnull) return "NULL";
  Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
  Oid typid = attr->atttypid;
  switch (typid) {
    case INT2OID: {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", DatumGetInt16(d));
      return buf;
    }
    case INT4OID: {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", DatumGetInt32(d));
      return buf;
    }
    case INT8OID: {
      char buf[32];
      snprintf(buf, sizeof(buf), "%ld", (long)DatumGetInt64(d));
      return buf;
    }
    case FLOAT4OID: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.6g", (double)DatumGetFloat4(d));
      return buf;
    }
    case FLOAT8OID: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.15g", DatumGetFloat8(d));
      return buf;
    }
    case TEXTOID:
    case VARCHAROID:
    case BPCHAROID: {
      text *txt = DatumGetTextPP(d);
      char *cstr = text_to_cstring(txt);
      std::string result(cstr);
      pfree(cstr);
      return result;
    }
    case BOOLOID:
      return DatumGetBool(d) ? "t" : "f";
    default: {
      // Fallback for unsupported types: use the type output function.
      Oid typoutput; bool typisvarlena;
      getTypeOutputInfo(typid, &typoutput, &typisvarlena);
      Datum out_datum = OidFunctionCall1(typoutput, d);
      char *str = DatumGetCString(out_datum);
      std::string result(str);
      pfree(str);
      return result;
    }
  }
  return "?"; // unreachable
}

// Serialize all non-dropped columns of one tuple into the bridge's cell
// format: comma-separated cells ("NULL" for NULL).
static std::string SlotToRowCsv(TupleTableSlot *slot, TupleDesc tupdesc) {
  std::string out;
  if (!slot) return "NULL";
  for (int a = 1; a <= tupdesc->natts; ++a) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, a - 1);
    if (attr->attisdropped) continue;
    if (!out.empty()) out += ",";
    out += AttrToStr(slot, tupdesc, a);
  }
  return out;
}

// Comma-separated 1-based column numbers whose values differ between the old
// and new tuple of an UPDATE.
static std::string ComputeUpdatedCols(TupleDesc tupdesc,
                                      TupleTableSlot *old_slot,
                                      TupleTableSlot *new_slot) {
  std::string out;
  for (int a = 1; a <= tupdesc->natts; ++a) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, a - 1);
    if (attr->attisdropped) continue;
    std::string ov = AttrToStr(old_slot, tupdesc, a);
    std::string nv = AttrToStr(new_slot, tupdesc, a);
    if (ov != nv) {
      if (!out.empty()) out += ",";
      out += std::to_string(a);
    }
  }
  return out;
}
}

extern "C" {

PG_FUNCTION_INFO_V1(accel_postgres_capture_mutation_trigger);
Datum accel_postgres_capture_mutation_trigger(PG_FUNCTION_ARGS) {
  TriggerData *trigdata = reinterpret_cast<TriggerData *>(fcinfo->context);
  if (!CALLED_AS_TRIGGER(fcinfo) || !trigdata ||
      !TRIGGER_FIRED_FOR_ROW(trigdata->tg_event)) {
    ereport(ERROR,
            (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
             errmsg("capture_mutation_trigger: must be an AFTER ROW trigger")));
  }
  if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) &&
      !TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event) &&
      !TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) {
    return PointerGetDatum(nullptr);
  }
  EnsureInit();
  uint64_t table_oid = RelationGetRelid(trigdata->tg_relation);
  int kind = 0;
  if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event)) kind = 0;
  else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) kind = 1;
  else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event)) kind = 2;

  TupleDesc tupdesc = RelationGetDescr(trigdata->tg_relation);
  std::string old_row;
  std::string new_row;
  std::string updated_cols;
  if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event)) {
    // In PG 17, tg_newslot may be NULL for INSERT; tg_trigslot holds the new row
    TupleTableSlot *new_slot = trigdata->tg_newslot ? trigdata->tg_newslot
                                                     : trigdata->tg_trigslot;
    new_row = SlotToRowCsv(new_slot, tupdesc);
  } else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) {
    old_row = SlotToRowCsv(trigdata->tg_trigslot, tupdesc);
  } else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event)) {
    old_row = SlotToRowCsv(trigdata->tg_trigslot, tupdesc);
    TupleTableSlot *new_slot = trigdata->tg_newslot ? trigdata->tg_newslot
                                                     : trigdata->tg_trigslot;
    new_row = SlotToRowCsv(new_slot, tupdesc);
    updated_cols = ComputeUpdatedCols(tupdesc, trigdata->tg_trigslot,
                                      new_slot);
  }

  accel_pg_capture_mutation_value(
      static_cast<int>(table_oid), kind,
      old_row.c_str(), new_row.c_str(), updated_cols.c_str());
  return PointerGetDatum(nullptr);
}

PG_FUNCTION_INFO_V1(accel_postgres_native_scan);
Datum accel_postgres_native_scan(PG_FUNCTION_ARGS) {
  // Contract/host entrypoint: must call ScanPostgresTableForRebuild (named
  // engine-local helper), not a private inline table-AM copy.
  FuncCallContext *funcctx;
  if (SRF_IS_FIRSTCALL()) {
    funcctx = SRF_FIRSTCALL_INIT();
    MemoryContext oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    text *schema_txt = PG_GETARG_TEXT_PP(0);
    text *table_txt = PG_GETARG_TEXT_PP(1);
    std::string schema_name((char *)VARDATA_ANY(schema_txt),
                            VARSIZE_ANY_EXHDR(schema_txt));
    std::string table_name((char *)VARDATA_ANY(table_txt),
                           VARSIZE_ANY_EXHDR(table_txt));
    ArrayType *col_arr = PG_GETARG_ARRAYTYPE_P(2);
    accelerator_codegen::aux_state::engine::postgres::PostgresScanSpec spec;
    spec.schema_name = schema_name;
    spec.table_name = table_name;
    {
      Datum *elems;
      bool *nulls;
      int nelems;
      deconstruct_array(col_arr, TEXTOID, -1, false, 'i', &elems, &nulls, &nelems);
      for (int i = 0; i < nelems; ++i) {
        if (nulls[i]) continue;
        char *s = TextDatumGetCString(elems[i]);
        if (s) {
          accelerator_codegen::aux_state::engine::postgres::PostgresScanColumn c;
          c.name = s;
          spec.projected_columns.push_back(c);
          pfree(s);
        }
      }
      pfree(elems);
      pfree(nulls);
    }
    if (!PG_ARGISNULL(3)) {
      text *ft = PG_GETARG_TEXT_PP(3);
      spec.filter_column.assign((char *)VARDATA_ANY(ft), VARSIZE_ANY_EXHDR(ft));
    }
    spec.filter_op = PG_ARGISNULL(4) ? 0 : PG_GETARG_INT32(4);
    if (!PG_ARGISNULL(5)) {
      text *fvt = PG_GETARG_TEXT_PP(5);
      spec.filter_value.assign((char *)VARDATA_ANY(fvt), VARSIZE_ANY_EXHDR(fvt));
    }
    int batch_size = PG_ARGISNULL(6) ? 1 : PG_GETARG_INT32(6);
    if (batch_size < 1) batch_size = 1;
    spec.batch_size = static_cast<std::size_t>(batch_size);

    EnsureInit();
    accel_pg_scan_clear();
    int total_rows = 0;
    accelerator_codegen::aux_state::engine::postgres::ScanPostgresTableForRebuild(
        spec,
        [&](const std::vector<
                accelerator_codegen::aux_state::engine::postgres::PostgresScanRow>
                &cells,
            std::size_t column_count) {
          if (column_count == 0) return;
          const std::size_t nrows = cells.size() / column_count;
          for (std::size_t r = 0; r < nrows; ++r) {
            std::string row_str;
            for (std::size_t c = 0; c < column_count; ++c) {
              if (c > 0) row_str += "\t";
              const auto &cell = cells[r * column_count + c];
              if (cell.is_null) {
                row_str += "NULL";
              } else {
                row_str.append(reinterpret_cast<const char *>(cell.data.data()),
                               cell.data.size());
              }
            }
            accel_pg_scan_callback_push(static_cast<int>(column_count),
                                        row_str.c_str());
            ++total_rows;
          }
          accel_pg_scan_callback_inc();
        });
    funcctx->max_calls = total_rows;
    MemoryContextSwitchTo(oldctx);
  }
  funcctx = SRF_PERCALL_SETUP();
  if (funcctx->call_cntr < funcctx->max_calls) {
    int ri = funcctx->call_cntr;
    int nc = accel_pg_get_scan_column_count();
    std::string rl;
    for (int ci = 0; ci < nc; ++ci) {
      if (ci > 0) rl += "|";
      rl += accel_pg_get_scan_row_cell(ri, ci);
    }
    SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(rl.c_str()));
  }
  SRF_RETURN_DONE(funcctx);
}

PG_FUNCTION_INFO_V1(accel_postgres_toy_build);
Datum accel_postgres_toy_build(PG_FUNCTION_ARGS){
  text*k=PG_GETARG_TEXT_PP(0);int64_t v=PG_GETARG_INT64(1);
  EnsureInit();int rc=0;
  if(IsTransactionBlock()){
    rc=accel_pg_toy_stage_build(std::string((char*)VARDATA_ANY(k),VARSIZE_ANY_EXHDR(k)).c_str(),v);
  }else{
    rc=accel_pg_toy_build(std::string((char*)VARDATA_ANY(k),VARSIZE_ANY_EXHDR(k)).c_str(),v);
  }
  PG_RETURN_BOOL(rc);
}

PG_FUNCTION_INFO_V1(accel_postgres_toy_read);
Datum accel_postgres_toy_read(PG_FUNCTION_ARGS){
  EnsureInit();int64_t v=0;int ok=accel_pg_toy_read(&v);
  if(!ok)PG_RETURN_NULL();PG_RETURN_INT64(v);
}

PG_FUNCTION_INFO_V1(accel_postgres_toy_current_epoch);
Datum accel_postgres_toy_current_epoch(PG_FUNCTION_ARGS){EnsureInit();uint64_t e=0;accel_pg_toy_current_epoch(&e);PG_RETURN_INT64(e);}

PG_FUNCTION_INFO_V1(accel_postgres_toy_check_validity);
Datum accel_postgres_toy_check_validity(PG_FUNCTION_ARGS){
  text*k=PG_GETARG_TEXT_PP(0);
  uint64_t ep=(uint64_t)PG_GETARG_INT64(1);
  EnsureInit();int code=0;uint64_t ver=0;
  accel_pg_toy_check_validity(std::string((char*)VARDATA_ANY(k),VARSIZE_ANY_EXHDR(k)).c_str(),ep,&code,&ver);
  std::string result=std::to_string(code)+","+std::to_string(ver);
  PG_RETURN_TEXT_P(cstring_to_text(result.c_str()));
}

PG_FUNCTION_INFO_V1(accel_postgres_toy_invalidate);
Datum accel_postgres_toy_invalidate(PG_FUNCTION_ARGS){EnsureInit();accel_pg_toy_invalidate();PG_RETURN_VOID();}

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_pre_committed);
Datum accel_postgres_get_observer_pre_committed(PG_FUNCTION_ARGS){EnsureInit();PG_RETURN_BOOL(accel_pg_get_observer_pre_committed());}

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_committed);
Datum accel_postgres_get_observer_committed(PG_FUNCTION_ARGS){EnsureInit();PG_RETURN_BOOL(accel_pg_get_observer_committed());}

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_aborted);
Datum accel_postgres_get_observer_aborted(PG_FUNCTION_ARGS){EnsureInit();PG_RETURN_BOOL(accel_pg_get_observer_aborted());}

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_mutation_count);
Datum accel_postgres_get_observer_mutation_count(PG_FUNCTION_ARGS){EnsureInit();PG_RETURN_INT64(accel_pg_get_observer_mutation_count());}

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_mutation_at);
Datum accel_postgres_get_observer_mutation_at(PG_FUNCTION_ARGS){int idx=PG_GETARG_INT32(0);EnsureInit();const char*s=accel_pg_get_observer_mutation_at(idx);if(!s||!*s)PG_RETURN_NULL();PG_RETURN_TEXT_P(cstring_to_text(s));}

PG_FUNCTION_INFO_V1(accel_postgres_reset_observer);
Datum accel_postgres_reset_observer(PG_FUNCTION_ARGS){EnsureInit();accel_pg_reset_observer();PG_RETURN_VOID();}

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_mutation_row);
Datum accel_postgres_get_observer_mutation_row(PG_FUNCTION_ARGS){int idx=PG_GETARG_INT32(0);int rn=PG_GETARG_INT32(1);int isn=PG_GETARG_INT32(2);EnsureInit();const char*s=accel_pg_get_observer_mutation_row(idx,rn,isn);if(!s||!*s)PG_RETURN_NULL();PG_RETURN_TEXT_P(cstring_to_text(s));}

PG_FUNCTION_INFO_V1(accel_postgres_get_last_new_json);
Datum accel_postgres_get_last_new_json(PG_FUNCTION_ARGS){EnsureInit();const char*s=accel_pg_get_last_new_json();if(!s||!*s)PG_RETURN_NULL();PG_RETURN_TEXT_P(cstring_to_text(s));}

PG_FUNCTION_INFO_V1(accel_postgres_get_last_old_json);
Datum accel_postgres_get_last_old_json(PG_FUNCTION_ARGS){EnsureInit();const char*s=accel_pg_get_last_old_json();if(!s||!*s)PG_RETURN_NULL();PG_RETURN_TEXT_P(cstring_to_text(s));}

PG_FUNCTION_INFO_V1(accel_postgres_get_scan_row_count);
Datum accel_postgres_get_scan_row_count(PG_FUNCTION_ARGS){EnsureInit();PG_RETURN_INT64(accel_pg_get_scan_row_count());}

PG_FUNCTION_INFO_V1(accel_postgres_get_scan_column_count);
Datum accel_postgres_get_scan_column_count(PG_FUNCTION_ARGS){EnsureInit();PG_RETURN_INT64(accel_pg_get_scan_column_count());}

PG_FUNCTION_INFO_V1(accel_postgres_get_scan_callback_count);
Datum accel_postgres_get_scan_callback_count(PG_FUNCTION_ARGS){EnsureInit();PG_RETURN_INT64(accel_pg_get_scan_callback_count());}

PG_FUNCTION_INFO_V1(accel_postgres_get_scan_row_cell);
Datum accel_postgres_get_scan_row_cell(PG_FUNCTION_ARGS){int ri=PG_GETARG_INT32(0);int ci=PG_GETARG_INT32(1);EnsureInit();const char*s=accel_pg_get_scan_row_cell(ri,ci);if(!s||!*s)PG_RETURN_NULL();PG_RETURN_TEXT_P(cstring_to_text(s));}

static void AccelXactCB(XactEvent event, void *arg) {
  (void)arg;
  switch(event){case XACT_EVENT_PRE_COMMIT:accel_pg_pre_commit_hook();break;case XACT_EVENT_COMMIT:accel_pg_commit_hook();break;case XACT_EVENT_ABORT:accel_pg_abort_hook();break;default:break;}
}

void _PG_init(void){RegisterXactCallback(AccelXactCB,nullptr);}

// TPC-H SRFs (accel_tpch_qN / _aux) are in query_functions.cpp → runtime.cpp
// (SPI InitialScan over native Postgres tables; no DuckDB DataChunk IR).

// Alias for interface-v1 registration SQL.
PG_FUNCTION_INFO_V1(AcceleratorCodegenCaptureTransition);
Datum AcceleratorCodegenCaptureTransition(PG_FUNCTION_ARGS) {
  return accel_postgres_capture_mutation_trigger(fcinfo);
}

} // extern "C"

