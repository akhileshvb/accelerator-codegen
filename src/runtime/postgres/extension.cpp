// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include "postgres.h"
#include "access/htup_details.h"
#include "access/subtrans.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/guc.h"
}

#include <cctype>
#include <cmath>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/postgres/postgres_mutation_capture.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_session_gucs.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
void EnterPostgresXactCallback();
void LeavePostgresXactCallback();
}

extern "C" {
PG_MODULE_MAGIC;
}

namespace {

// AuxDate uses Unix-epoch days; PostgreSQL DateADT is days since 2000-01-01.
constexpr int32_t kPostgresDateEpochOffset = 10957;

using accelerator_codegen::aux_state::core::AuxRow;
using accelerator_codegen::aux_state::core::AuxType;
using accelerator_codegen::aux_state::core::AuxValue;
using accelerator_codegen::aux_state::core::ColumnId;
using accelerator_codegen::aux_state::core::MutationBatch;
using accelerator_codegen::aux_state::core::MutationKind;
using accelerator_codegen::aux_state::core::TableId;

AuxType TypeForOid(Oid type_oid) {
  switch (type_oid) {
  case BOOLOID:
    return AuxType::Boolean;
  case INT2OID:
  case INT4OID:
  case INT8OID:
    return AuxType::Int64;
  case FLOAT4OID:
  case FLOAT8OID:
  case NUMERICOID:
    return AuxType::Double;
  case DATEOID:
    return AuxType::Date;
  default:
    return AuxType::String;
  }
}

char *DatumToCString(Datum datum, Oid type_oid) {
  Oid output_function = InvalidOid;
  bool is_varlena = false;
  getTypeOutputInfo(type_oid, &output_function, &is_varlena);
  return OidOutputFunctionCall(output_function, datum);
}

bool ParseScaledDecimal(
    const char *text,
    accelerator_codegen::aux_state::core::ScaledDecimal &decimal) {
  if (!text || *text == '\0') {
    return false;
  }
  const char *cursor = text;
  bool negative = false;
  if (*cursor == '+' || *cursor == '-') {
    negative = *cursor == '-';
    ++cursor;
  }
  int64_t value = 0;
  int32_t scale = 0;
  bool saw_digit = false;
  bool after_decimal = false;
  for (; *cursor != '\0'; ++cursor) {
    if (*cursor == '.') {
      if (after_decimal) {
        return false;
      }
      after_decimal = true;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(*cursor))) {
      return false;
    }
    saw_digit = true;
    const int digit = *cursor - '0';
    if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
    if (after_decimal) {
      if (scale == std::numeric_limits<int32_t>::max()) {
        return false;
      }
      ++scale;
    }
  }
  if (!saw_digit ||
      (negative && value == std::numeric_limits<int64_t>::min())) {
    return false;
  }
  decimal.value = negative ? -value : value;
  decimal.scale = scale;
  return true;
}

AuxValue DatumToAuxValue(Datum datum, bool is_null, Oid type_oid) {
  const auto type = TypeForOid(type_oid);
  if (is_null) {
    return AuxValue::Null(type);
  }
  switch (type_oid) {
  case BOOLOID:
    return AuxValue::Of(type, DatumGetBool(datum));
  case INT2OID:
    return AuxValue::Of(type, static_cast<int64_t>(DatumGetInt16(datum)));
  case INT4OID:
  case DATEOID:
    return type_oid == DATEOID
               ? AuxValue::Of(type,
                              accelerator_codegen::aux_state::core::AuxDate{
                                  DatumGetInt32(datum) + kPostgresDateEpochOffset})
               : AuxValue::Of(type, static_cast<int64_t>(DatumGetInt32(datum)));
  case INT8OID:
    return AuxValue::Of(type, DatumGetInt64(datum));
  case FLOAT4OID:
    return AuxValue::Of(type, static_cast<double>(DatumGetFloat4(datum)));
  case FLOAT8OID:
    return AuxValue::Of(type, DatumGetFloat8(datum));
  case NUMERICOID: {
    char *text = DatumToCString(datum, type_oid);
    accelerator_codegen::aux_state::core::ScaledDecimal decimal;
    if (ParseScaledDecimal(text, decimal)) {
      return AuxValue::Of(AuxType::ScaledDecimal, decimal);
    }
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
      ereport(
          ERROR,
          (errmsg("could not convert PostgreSQL numeric to auxiliary double")));
    }
    return AuxValue::Of(type, value);
  }
  default: {
    char *text = DatumToCString(datum, type_oid);
    return AuxValue::Of(type, std::string(text));
  }
  }
}

std::vector<int> MutationProjection(TableId table, int natts) {
  switch (table) {
  case 1: // customer: c_custkey,c_name,c_acctbal,c_nationkey,c_address,c_phone,c_comment
    return {0, 1, 5, 3, 2, 4, 7};
  case 2: // nation: n_nationkey,n_name
    return {0, 1};
  case 3: // orders: o_orderkey,o_custkey,o_orderdate
    return {0, 1, 4};
  case 4: // lineitem: l_orderkey,l_extendedprice,l_discount,l_returnflag
    return {0, 5, 6, 8};
  case 101: // sub (generic; row parsing unused on rebuild-only paths)
    return {0, 1, 2, 3, 10};
  case 102: // pre: adsh,tag,version
    return {0, 1, 2};
  case 103: // num: full table row (adsh..footnote)
    return {0, 1, 2, 3, 4, 5, 6, 7, 8};
  case 1001: // TPCH Q1: shipdate,quantity,price,discount,tax,returnflag,linestatus
    return {10, 4, 5, 6, 7, 8, 9};
  case 1002: // TPCH Q3 orders: o_orderkey,o_orderdate,o_custkey,o_shippriority
    return {0, 4, 1, 7};
  case 1003: // TPCH Q3 customer
    return {6, 0};
  case 1004: // TPCH Q3 lineitem
    return {0, 5, 6, 10};
  case 1006: // TPCH Q6: shipdate,discount,quantity,extendedprice
    return {10, 6, 4, 5};
  case 1012: // TPCH Q12 orders
    return {0, 5};
  case 1013: // TPCH Q12 lineitem
    return {0, 14, 11, 12, 10};
  case 1014: // TPCH Q14 lineitem: partkey,extendedprice,discount,shipdate
    return {1, 5, 6, 10};
  case 1015: // TPCH Q14 part
    return {0, 3};
  case 1091: // TPCH Q9 part
    return {0, 1};
  case 1092: // TPCH Q9 supplier
    return {0, 3};
  case 1093: // TPCH Q9 nation
    return {0, 1};
  case 1094: // TPCH Q9 orders
    return {0, 4};
  case 1095: // TPCH Q9 partsupp
    return {0, 1, 3};
  case 1096: // TPCH Q9 lineitem: partkey,suppkey,orderkey,qty,price,discount
    return {1, 2, 0, 4, 5, 6};
  case 1111: // TPCH Q11 partsupp
    return {0, 1, 2, 3};
  case 1112: // TPCH Q11 supplier
    return {0, 3};
  case 1113: // TPCH Q11 nation
    return {0, 1};
  case 1171: // TPCH Q17 part
    return {0, 3, 6};
  case 1172: // TPCH Q17 lineitem: partkey,quantity,extendedprice
    return {1, 4, 5};
  case 1180: // TPCH Q18 lineitem: orderkey,quantity
    return {0, 4};
  case 1181: // TPCH Q18 orders: orderkey,custkey,orderdate,totalprice
    return {0, 1, 4, 3};
  case 1182: // TPCH Q18 customer
    return {0, 1};
  case 1211: // TPCH Q21 supplier
    return {0, 1, 3};
  case 1212: // TPCH Q21 orders: orderkey,orderstatus
    return {0, 2};
  case 1213: // TPCH Q21 lineitem: orderkey,suppkey,commit,receipt
    return {0, 2, 11, 12};
  case 1214: // TPCH Q21 nation
    return {0, 1};
  case 1221: // TPCH Q22 orders: o_custkey
    return {1};
  case 1222: // TPCH Q22 customer: c_custkey,c_phone,c_acctbal
    return {0, 4, 5};
  case 1021: // TPCH Q2 part: partkey,mfgr,type,size
    return {0, 2, 4, 5};
  case 1022: // TPCH Q2 supplier: key,name,address,nationkey,phone,acctbal,comment
    return {0, 1, 2, 3, 4, 5, 6};
  case 1023: // TPCH Q2 partsupp: partkey,suppkey,supplycost
    return {0, 1, 3};
  case 1024: // TPCH Q2 nation: nationkey,name,regionkey
    return {0, 1, 2};
  case 1025: // TPCH Q2 region: regionkey,name
    return {0, 1};
  case 1041: // TPCH Q4 orders: orderkey,orderdate,priority
    return {0, 4, 5};
  case 1042: // TPCH Q4 lineitem: orderkey,commitdate,receiptdate
    return {0, 11, 12};
  case 1051: // TPCH Q5 region: regionkey,name
    return {0, 1};
  case 1052: // TPCH Q5 nation: nationkey,name,regionkey
    return {0, 1, 2};
  case 1053: // TPCH Q5 customer: custkey,nationkey
    return {0, 3};
  case 1054: // TPCH Q5 supplier: suppkey,nationkey
    return {0, 3};
  case 1055: // TPCH Q5 orders: orderkey,custkey,orderdate
    return {0, 1, 4};
  case 1056: // TPCH Q5 lineitem: orderkey,suppkey,price,discount
    return {0, 2, 5, 6};
  case 1071: // TPCH Q7 nation: nationkey,name
    return {0, 1};
  case 1072: // TPCH Q7 supplier: suppkey,nationkey
    return {0, 3};
  case 1073: // TPCH Q7 customer: custkey,nationkey
    return {0, 3};
  case 1074: // TPCH Q7 orders: orderkey,custkey
    return {0, 1};
  case 1075: // TPCH Q7 lineitem: orderkey,suppkey,price,discount,shipdate
    return {0, 2, 5, 6, 10};
  case 1081: // TPCH Q8 part: partkey,type
    return {0, 4};
  case 1082: // TPCH Q8 supplier: suppkey,nationkey
    return {0, 3};
  case 1083: // TPCH Q8 customer: custkey,nationkey
    return {0, 3};
  case 1084: // TPCH Q8 orders: orderkey,custkey,orderdate
    return {0, 1, 4};
  case 1085: // TPCH Q8 lineitem: partkey,orderkey,suppkey,price,discount
    return {1, 0, 2, 5, 6};
  case 1131: // TPCH Q13 customer: custkey
    return {0};
  case 1132: // TPCH Q13 orders: custkey,comment
    return {1, 8};
  case 1151: // TPCH Q15 lineitem: shipdate,suppkey,price,discount
    return {10, 2, 5, 6};
  case 1152: // TPCH Q15 supplier: suppkey,name,address,phone
    return {0, 1, 2, 4};
  case 1161: // TPCH Q16 part: partkey,brand,type,size
    return {0, 3, 4, 5};
  case 1162: // TPCH Q16 partsupp: partkey,suppkey
    return {0, 1};
  case 1163: // TPCH Q16 supplier: suppkey,comment
    return {0, 6};
  case 1191: // TPCH Q19 lineitem: partkey,qty,price,discount,shipmode,shipinstruct
    return {1, 4, 5, 6, 14, 13};
  case 1192: // TPCH Q19 part: partkey,brand,container,size
    return {0, 3, 6, 5};
  case 1201: // TPCH Q20 lineitem: shipdate,partkey,suppkey,quantity
    return {10, 1, 2, 4};
  case 1202: // TPCH Q20 part: partkey,name
    return {0, 1};
  case 1203: // TPCH Q20 partsupp: partkey,suppkey,availqty
    return {0, 1, 2};
  case 1204: // TPCH Q20 supplier: suppkey,name,address,nationkey
    return {0, 1, 2, 3};
  case 1205: // TPCH Q20 nation: nationkey,name
    return {0, 1};
  case 2001: // SEC Q1 pre: stmt,rfile,adsh,line
    return {3, 5, 0, 2};
  case 2002: // SEC Q2 sub: adsh,name,cik,fy
  case 2004: // SEC Q3 sub: adsh,name,cik,fy
    return {0, 2, 1, 10};
  case 2003: // SEC Q2 num: adsh,tag,uom,value
    return {0, 1, 5, 7};
  case 2005: // SEC Q3 num: adsh,uom,value
    return {0, 5, 7};
  case 2009: // SEC Q4 num: adsh,tag,version,value,uom
    return {0, 1, 2, 7, 5};
  case 2012: // SEC Q6 num: adsh,tag,version,uom,value
    return {0, 1, 2, 5, 7};
  case 2013: // SEC Q24 num: adsh,tag,version,uom,value,ddate
    return {0, 1, 2, 5, 7, 3};
  case 2006: // SEC Q4 sub: adsh,sic,name,cik
    return {0, 3, 2, 1};
  case 2007: // SEC Q4 tag: tag,abstract,tlabel
    return {0, 3, 7};
  case 2008: // SEC Q4 pre: adsh,tag,version,stmt,plabel
  case 2011: // SEC Q6 pre: adsh,tag,version,stmt,plabel
    return {0, 6, 7, 3, 8};
  case 2010: // SEC Q6 sub: adsh,name,fy
    return {0, 2, 10};
  default: {
    std::vector<int> physical;
    physical.reserve(natts);
    for (int attribute = 0; attribute < natts; ++attribute) {
      physical.push_back(attribute);
    }
    return physical;
  }
  }
}

std::vector<AuxRow> ReadTransitionTable(const char *table_name,
                                        TableId table, TupleDesc tuple_desc) {
  std::vector<AuxRow> rows;
  if (!table_name) {
    return rows;
  }
  const char *quoted_name = quote_identifier(table_name);
  StringInfoData query;
  initStringInfo(&query);
  appendStringInfo(&query, "SELECT * FROM %s", quoted_name);
  const int status = SPI_execute(query.data, true, 0);
  pfree(query.data);
  if (status != SPI_OK_SELECT) {
    ereport(ERROR,
            (errmsg("could not read PostgreSQL trigger transition table")));
  }
  if (!SPI_tuptable) {
    return rows;
  }
  const auto projection = MutationProjection(table, tuple_desc->natts);
  rows.reserve(SPI_processed);
  for (uint64 row_index = 0; row_index < SPI_processed; ++row_index) {
    HeapTuple tuple = SPI_tuptable->vals[row_index];
    AuxRow row;
    row.values.reserve(projection.size());
    for (const int attribute : projection) {
      if (attribute < 0 || attribute >= tuple_desc->natts) {
        ereport(ERROR,
                (errmsg("auxiliary mutation projection is incompatible with PostgreSQL table")));
      }
      Form_pg_attribute metadata = TupleDescAttr(tuple_desc, attribute);
      bool is_null = false;
      const Datum datum =
          SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
      row.values.push_back(DatumToAuxValue(datum, is_null, metadata->atttypid));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<ColumnId> UpdatedColumns(const TriggerData *trigger_data,
                                     TableId table) {
  std::vector<ColumnId> columns;
  if (!trigger_data->tg_updatedcols) {
    return columns;
  }
  for (int bit = -1;
       (bit = bms_next_member(trigger_data->tg_updatedcols, bit)) >= 0;) {
    const int attribute_number = bit + FirstLowInvalidHeapAttributeNumber;
    if (attribute_number > 0) {
      const auto projection =
          MutationProjection(table, trigger_data->tg_relation->rd_att->natts);
      const auto physical_attribute = attribute_number - 1;
      const auto it = std::find(projection.begin(), projection.end(),
                                physical_attribute);
      if (it != projection.end()) {
        columns.push_back(static_cast<ColumnId>(it - projection.begin()));
      }
    }
  }
  return columns;
}

uint64_t MutationTableId(const TriggerData *trigger_data) {
  const auto *trigger = trigger_data->tg_trigger;
  if (trigger && trigger->tgnargs > 0 && trigger->tgargs &&
      trigger->tgargs[0] && trigger->tgargs[0][0] != '\0') {
    char *end = nullptr;
    errno = 0;
    const auto value = std::strtoull(trigger->tgargs[0], &end, 10);
    if (errno == 0 && end != trigger->tgargs[0] && *end == '\0') {
      return static_cast<uint64_t>(value);
    }
  }
  return static_cast<uint64_t>(RelationGetRelid(trigger_data->tg_relation));
}

extern "C" {
PG_FUNCTION_INFO_V1(AcceleratorCodegenCaptureTransition);
Datum AcceleratorCodegenCaptureTransition(PG_FUNCTION_ARGS) {
  if (!CALLED_AS_TRIGGER(fcinfo)) {
    ereport(
        ERROR,
        (errmsg("accelerator transition capture must be called as a trigger")));
  }
  TriggerData *trigger_data =
      reinterpret_cast<TriggerData *>(fcinfo->context);
  if (!TRIGGER_FIRED_AFTER(trigger_data->tg_event) ||
      !TRIGGER_FIRED_FOR_STATEMENT(trigger_data->tg_event)) {
    ereport(ERROR, (errmsg("accelerator transition capture requires an AFTER "
                           "STATEMENT trigger")));
  }
  const TableId table = MutationTableId(trigger_data);
  if (!accelerator_codegen::aux_state::engine::postgres::
          HasRegisteredCaptureForTable(table)) {
    PG_RETURN_POINTER(nullptr);
  }
  if (SPI_connect() != SPI_OK_CONNECT) {
    ereport(ERROR, (errmsg("could not connect to PostgreSQL SPI")));
  }
  if (SPI_register_trigger_data(trigger_data) < 0) {
    SPI_finish();
    ereport(
        ERROR,
        (errmsg("could not register PostgreSQL transition tables with SPI")));
  }

  MutationBatch mutation;
  mutation.table = table;
  if (TRIGGER_FIRED_BY_INSERT(trigger_data->tg_event)) {
    mutation.kind = MutationKind::Insert;
    mutation.new_rows =
        ReadTransitionTable(trigger_data->tg_trigger->tgnewtable,
                            mutation.table, trigger_data->tg_relation->rd_att);
  } else if (TRIGGER_FIRED_BY_DELETE(trigger_data->tg_event)) {
    mutation.kind = MutationKind::Delete;
    mutation.old_rows =
        ReadTransitionTable(trigger_data->tg_trigger->tgoldtable,
                            mutation.table, trigger_data->tg_relation->rd_att);
  } else if (TRIGGER_FIRED_BY_UPDATE(trigger_data->tg_event)) {
    mutation.kind = MutationKind::Update;
    mutation.old_rows =
        ReadTransitionTable(trigger_data->tg_trigger->tgoldtable,
                            mutation.table, trigger_data->tg_relation->rd_att);
    mutation.new_rows =
        ReadTransitionTable(trigger_data->tg_trigger->tgnewtable,
                            mutation.table, trigger_data->tg_relation->rd_att);
    mutation.updated_columns = UpdatedColumns(trigger_data, mutation.table);
  } else {
    SPI_finish();
    ereport(
        ERROR,
        (errmsg("unsupported PostgreSQL trigger event for auxiliary capture")));
  }
  const bool dispatched = accelerator_codegen::aux_state::engine::postgres::
      DispatchPostgresMutation(mutation);
  SPI_finish();
  if (!dispatched) {
    ereport(
        ERROR,
        (errmsg("PostgreSQL auxiliary mutation capture is not initialized")));
  }
  PG_RETURN_POINTER(nullptr);
}
}

void AcceleratorXactCallback(XactEvent event, void *) {
  using accelerator_codegen::aux_state::engine::postgres::
      HandleTransactionEvent;
  using accelerator_codegen::aux_state::engine::postgres::TransactionEvent;
  switch (event) {
  case XACT_EVENT_PRE_COMMIT: {
    // Statement portals have already popped their snapshot; SPI scans in
    // StageMaintenance / PreCommit rebuild need an active one.
    PushActiveSnapshot(GetTransactionSnapshot());
    accelerator_codegen::aux_state::engine::postgres::EnterPostgresXactCallback();
    PG_TRY();
    {
      try {
        HandleTransactionEvent(TransactionEvent::PreCommit);
      } catch (const std::exception &ex) {
        ereport(ERROR,
                (errmsg("accelerator pre-commit failed: %s", ex.what())));
      }
    }
    PG_CATCH();
    {
      accelerator_codegen::aux_state::engine::postgres::LeavePostgresXactCallback();
      PopActiveSnapshot();
      PG_RE_THROW();
    }
    PG_END_TRY();
    accelerator_codegen::aux_state::engine::postgres::LeavePostgresXactCallback();
    PopActiveSnapshot();
    break;
  }
  case XACT_EVENT_COMMIT:
    accelerator_codegen::aux_state::engine::postgres::EnterPostgresXactCallback();
    HandleTransactionEvent(TransactionEvent::Commit);
    accelerator_codegen::aux_state::engine::postgres::LeavePostgresXactCallback();
    break;
  case XACT_EVENT_ABORT:
    accelerator_codegen::aux_state::engine::postgres::EnterPostgresXactCallback();
    HandleTransactionEvent(TransactionEvent::Abort);
    accelerator_codegen::aux_state::engine::postgres::LeavePostgresXactCallback();
    break;
  default:
    break;
  }
}

void AcceleratorSubXactCallback(SubXactEvent event, SubTransactionId,
                                 SubTransactionId, void *) {
  using accelerator_codegen::aux_state::engine::postgres::
      HandleTransactionEvent;
  using accelerator_codegen::aux_state::engine::postgres::TransactionEvent;
  switch (event) {
  case SUBXACT_EVENT_START_SUB:
    HandleTransactionEvent(TransactionEvent::SubtransactionStart);
    break;
  case SUBXACT_EVENT_COMMIT_SUB:
    HandleTransactionEvent(TransactionEvent::SubtransactionCommit);
    break;
  case SUBXACT_EVENT_ABORT_SUB:
    HandleTransactionEvent(TransactionEvent::SubtransactionAbort);
    break;
  default:
    break;
  }
}

} // namespace

#ifdef ACCEL_ENABLE_PARALLEL_AUX
int accel_aux_worker_count = 0;
#endif

static double accel_expected_queries_before_write = 0.0;
static bool accel_single_active_aux = false;

static void AssignExpectedQueriesBeforeWrite(double newvalue, void *extra) {
  (void)extra;
  accel_expected_queries_before_write = newvalue;
  accelerator_codegen::aux_state::engine::postgres::
      SetSessionExpectedQueriesBeforeWrite(newvalue > 0.0 ? newvalue : NAN);
}

static void AssignSingleActiveAux(bool newvalue, void *extra) {
  (void)extra;
  accel_single_active_aux = newvalue;
  accelerator_codegen::aux_state::engine::postgres::SetSessionSingleActiveAuxEnabled(
      newvalue);
}

extern "C" void _PG_init(void) {
  // The SQL-callable prototype is loaded with LOAD/CREATE EXTENSION, where
  // PostgreSQL has already completed its shared-memory request phase. The
  // shared-memory state-store reservation belongs in a future
  // shared_preload_libraries deployment hook, not in this dynamically loaded
  // read-only prototype.
  RegisterXactCallback(AcceleratorXactCallback, nullptr);
  RegisterSubXactCallback(AcceleratorSubXactCallback, nullptr);
  DefineCustomRealVariable(
      "accel.expected_queries_before_write",
      "Expected read queries before the next write for aux maintenance costing "
      "(0 = unset).",
      NULL, &accel_expected_queries_before_write, 0.0, 0.0, 1e9, PGC_USERSET, 0,
      NULL, AssignExpectedQueriesBeforeWrite, NULL);
  DefineCustomBoolVariable(
      "accel.single_active_aux",
      "When on, registering an auxiliary runtime unregisters other captures in "
      "this backend.",
      NULL, &accel_single_active_aux, false, PGC_USERSET, 0, NULL,
      AssignSingleActiveAux, NULL);
#ifdef ACCEL_ENABLE_PARALLEL_AUX
  DefineCustomIntVariable(
      "accel.aux_worker_count",
      "Worker count for multithreaded SEC EDGAR aux kernels (0 = max_parallel_workers_per_gather or 1).",
      NULL, &accel_aux_worker_count, 0, 0, 1024, PGC_USERSET, 0, NULL, NULL,
      NULL);
#endif
}

extern "C" void _PG_fini(void) {
  UnregisterXactCallback(AcceleratorXactCallback, nullptr);
  UnregisterSubXactCallback(AcceleratorSubXactCallback, nullptr);
}

#endif
