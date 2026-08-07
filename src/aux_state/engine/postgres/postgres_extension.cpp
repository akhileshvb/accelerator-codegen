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
}

#include <cctype>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "accelerator_codegen/aux_state/engine/postgres/postgres_mutation_capture.hpp"
#include "accelerator_codegen/aux_state/engine/postgres/postgres_xact_callbacks.hpp"

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
  case 1001: // TPCH Q1: shipdate,returnflag,linestatus,quantity,price,discount,tax
    return {10, 8, 9, 4, 5, 6, 7};
  case 1002: // TPCH Q3 orders
    return {0, 1, 4, 7};
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
  mutation.table = MutationTableId(trigger_data);
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
  case XACT_EVENT_PRE_COMMIT:
    HandleTransactionEvent(TransactionEvent::PreCommit);
    break;
  case XACT_EVENT_COMMIT:
    HandleTransactionEvent(TransactionEvent::Commit);
    break;
  case XACT_EVENT_ABORT:
    HandleTransactionEvent(TransactionEvent::Abort);
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

extern "C" void _PG_init(void) {
  // The SQL-callable prototype is loaded with LOAD/CREATE EXTENSION, where
  // PostgreSQL has already completed its shared-memory request phase. The
  // shared-memory state-store reservation belongs in a future
  // shared_preload_libraries deployment hook, not in this dynamically loaded
  // read-only prototype.
  RegisterXactCallback(AcceleratorXactCallback, nullptr);
  RegisterSubXactCallback(AcceleratorSubXactCallback, nullptr);
}

extern "C" void _PG_fini(void) {
  UnregisterXactCallback(AcceleratorXactCallback, nullptr);
  UnregisterSubXactCallback(AcceleratorSubXactCallback, nullptr);
}

#endif
