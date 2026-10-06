// Typed statement-level transition-table mutation capture (restored from
// exp/interface-v1). Projects physical PG rows into module AuxRow layouts and
// dispatches MutationBatch into registered Runtime coordinators.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include <postgres.h>
#include <access/htup_details.h>
#include <access/xact.h>
#include <commands/trigger.h>
#include <executor/spi.h>
#include <fmgr.h>
#include <lib/stringinfo.h>
#include <nodes/bitmapset.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
}

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"
#include "accelerator_codegen/stateful/postgres/aux_server.hpp"
#include "accelerator_codegen/stateful/postgres/aux_sync.hpp"

extern "C" {
void accel_pg_observer_note_mutation(void);
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


std::vector<int32_t> ResolveAttnums(const std::vector<int32_t> &requested,
                                    TupleDesc tuple_desc) {
  if (!requested.empty()) {
    return requested;
  }
  std::vector<int32_t> all;
  for (int attribute = 0; attribute < tuple_desc->natts; ++attribute) {
    if (TupleDescAttr(tuple_desc, attribute)->attisdropped) {
      continue;
    }
    all.push_back(attribute);
  }
  return all;
}

std::vector<AuxRow> ProjectTuples(SPITupleTable *tuptable, TupleDesc tuple_desc,
                                  const std::vector<int32_t> &attnums) {
  std::vector<AuxRow> rows;
  if (!tuptable) {
    return rows;
  }
  rows.reserve(SPI_processed);
  for (uint64 row_index = 0; row_index < SPI_processed; ++row_index) {
    HeapTuple tuple = tuptable->vals[row_index];
    AuxRow row;
    row.values.reserve(attnums.size());
    for (const int32_t attribute : attnums) {
      if (attribute < 0 || attribute >= tuple_desc->natts) {
        ereport(ERROR,
                (errmsg("auxiliary mutation projection attnum %d out of range",
                        attribute)));
      }
      Form_pg_attribute metadata = TupleDescAttr(tuple_desc, attribute);
      if (metadata->attisdropped) {
        ereport(ERROR,
                (errmsg("auxiliary mutation projection references dropped column")));
      }
      bool is_null = false;
      const Datum datum =
          SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
      row.values.push_back(DatumToAuxValue(datum, is_null, metadata->atttypid));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<ColumnId> UpdatedColumnsForProjection(
    const TriggerData *trigger_data, const std::vector<int32_t> &attnums) {
  std::vector<ColumnId> columns;
  if (!trigger_data->tg_updatedcols) {
    return columns;
  }
  for (int bit = -1;
       (bit = bms_next_member(trigger_data->tg_updatedcols, bit)) >= 0;) {
    const int attribute_number = bit + FirstLowInvalidHeapAttributeNumber;
    if (attribute_number <= 0) {
      continue;
    }
    const int physical = attribute_number - 1;
    const auto it = std::find(attnums.begin(), attnums.end(), physical);
    if (it != attnums.end()) {
      columns.push_back(static_cast<ColumnId>(it - attnums.begin()));
    }
  }
  return columns;
}

TableId TableIdFromTrigger(const TriggerData *trigger_data) {
  // The trigger names the table id the query modules use in Dependencies().
  // Without an argument fall back to the relation OID.
  const auto *trigger = trigger_data->tg_trigger;
  if (trigger && trigger->tgnargs > 0 && trigger->tgargs &&
      trigger->tgargs[0] && trigger->tgargs[0][0] != '\0') {
    char *end = nullptr;
    errno = 0;
    const auto value = std::strtoull(trigger->tgargs[0], &end, 10);
    if (errno == 0 && end != trigger->tgargs[0] && *end == '\0' && value != 0) {
      return static_cast<TableId>(value);
    }
  }
  return static_cast<TableId>(RelationGetRelid(trigger_data->tg_relation));
}

SPITupleTable *ExecuteTransitionSelect(const char *name) {
  if (!name) {
    return nullptr;
  }
  const char *quoted_name = quote_identifier(name);
  StringInfoData query;
  initStringInfo(&query);
  appendStringInfo(&query, "SELECT * FROM %s", quoted_name);
  const int status = SPI_execute(query.data, true, 0);
  pfree(query.data);
  if (status != SPI_OK_SELECT) {
    ereport(ERROR,
            (errmsg("could not read PostgreSQL trigger transition table")));
  }
  return SPI_tuptable;
}

Datum CaptureTransitionImpl(FunctionCallInfo fcinfo) {
  using accelerator_codegen::aux_state::engine::postgres::PostgresAuxBackend;
  using accelerator_codegen::aux_state::engine::postgres::
      RegisteredPostgresMutationBackends;

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

  const TableId table = TableIdFromTrigger(trigger_data);
  if (TRIGGER_FIRED_BY_TRUNCATE(trigger_data->tg_event)) {
    // No transition tables: announce the write and make this backend's own
    // stores resync at commit (and use the fallback until then).
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresXactCallbacks();
    accelerator_codegen::aux_state::engine::postgres::aux_sync::
        RegisterTruncate(table);
    PG_RETURN_POINTER(nullptr);
  }
  // Cross-backend sync: every write to a tracked table is announced (in
  // flight until this transaction ends), whether or not this backend runs an
  // accelerator itself.  Other backends' stores stop hitting until they resync.
  accelerator_codegen::aux_state::engine::postgres::
      RegisterPostgresXactCallbacks();
  accelerator_codegen::aux_state::engine::postgres::aux_sync::RegisterWrite(
      table);

  namespace srv = accelerator_codegen::aux_state::engine::postgres::aux_server;
  // Per-backend mode: the accelerators this backend holds.  Shared mode: the
  // accelerators the state worker hosts (their captured rows are staged and
  // shipped at commit); this backend's own copies are not maintained.
  auto backends = RegisteredPostgresMutationBackends();
  std::vector<srv::CaptureTarget> shared_targets = srv::CaptureTargets(table);
  if (srv::SharedEnabled()) {
    backends.clear();
  }
  if (backends.empty() && shared_targets.empty()) {
    PG_RETURN_POINTER(nullptr);
  }

  TupleDesc tuple_desc = RelationGetDescr(trigger_data->tg_relation);

  struct Target {
    PostgresAuxBackend *backend; // null: a shared-state accelerator
    std::string key;
    std::vector<int32_t> attnums;
  };
  std::vector<Target> targets;
  for (auto *backend : backends) {
    if (!backend) {
      continue;
    }
    auto attnums = backend->MutationCaptureAttnums(table);
    if (!attnums) {
      continue;
    }
    targets.push_back(
        Target{backend, std::string(), ResolveAttnums(*attnums, tuple_desc)});
  }
  for (auto &shared : shared_targets) {
    targets.push_back(Target{nullptr, shared.key,
                             ResolveAttnums(shared.attnums, tuple_desc)});
    srv::StageExamined(shared.key,
                       accelerator_codegen::aux_state::engine::postgres::
                           aux_sync::SlotOf(table));
  }
  if (targets.empty()) {
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

  MutationKind kind = MutationKind::Insert;
  std::vector<std::vector<AuxRow>> old_by_target(targets.size());
  std::vector<std::vector<AuxRow>> new_by_target(targets.size());
  std::vector<std::vector<ColumnId>> upd_by_target(targets.size());

  if (TRIGGER_FIRED_BY_INSERT(trigger_data->tg_event)) {
    kind = MutationKind::Insert;
    ExecuteTransitionSelect(trigger_data->tg_trigger->tgnewtable);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      new_by_target[i] =
          ProjectTuples(SPI_tuptable, tuple_desc, targets[i].attnums);
    }
  } else if (TRIGGER_FIRED_BY_DELETE(trigger_data->tg_event)) {
    kind = MutationKind::Delete;
    ExecuteTransitionSelect(trigger_data->tg_trigger->tgoldtable);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      old_by_target[i] =
          ProjectTuples(SPI_tuptable, tuple_desc, targets[i].attnums);
    }
  } else if (TRIGGER_FIRED_BY_UPDATE(trigger_data->tg_event)) {
    kind = MutationKind::Update;
    ExecuteTransitionSelect(trigger_data->tg_trigger->tgoldtable);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      old_by_target[i] =
          ProjectTuples(SPI_tuptable, tuple_desc, targets[i].attnums);
      upd_by_target[i] =
          UpdatedColumnsForProjection(trigger_data, targets[i].attnums);
    }
    ExecuteTransitionSelect(trigger_data->tg_trigger->tgnewtable);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      new_by_target[i] =
          ProjectTuples(SPI_tuptable, tuple_desc, targets[i].attnums);
    }
  } else {
    SPI_finish();
    ereport(
        ERROR,
        (errmsg("unsupported PostgreSQL trigger event for auxiliary capture")));
  }

  // When PostgreSQL reports which columns were updated, skip backends whose
  // mutation projection does not intersect that set. Otherwise an UPDATE that
  // only touches unwatched columns still runs AccumulateMutation /
  // StageMaintenance and can force deferred rebuilds.
  const bool updated_columns_known =
      kind == MutationKind::Update && trigger_data->tg_updatedcols != nullptr;
  bool dispatched = false;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    if (updated_columns_known && upd_by_target[i].empty()) {
      continue;
    }
    MutationBatch mutation;
    mutation.table = table;
    mutation.kind = kind;
    mutation.old_rows = std::move(old_by_target[i]);
    mutation.new_rows = std::move(new_by_target[i]);
    mutation.updated_columns = std::move(upd_by_target[i]);
    const auto depth =
        static_cast<std::size_t>(GetCurrentTransactionNestLevel() - 1);
    if (targets[i].backend) {
      targets[i].backend->CaptureMutationAtDepth(mutation, depth);
    } else {
      srv::StageBatch(targets[i].key, std::move(mutation), depth);
    }
    dispatched = true;
  }
  if (dispatched) {
    accel_pg_observer_note_mutation();
  }
  SPI_finish();
  PG_RETURN_POINTER(nullptr);
}

} // namespace

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenCaptureTransition);
Datum AcceleratorCodegenCaptureTransition(PG_FUNCTION_ARGS) {
  return CaptureTransitionImpl(fcinfo);
}

} // extern "C"

#endif
