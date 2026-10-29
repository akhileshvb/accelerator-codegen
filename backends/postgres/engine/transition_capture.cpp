// GENERATED BACKEND
//
// S1: the statement-level transition trigger. It reads the transition tables
// straight from their tuplestores into one runtime::ColumnarBatch per
// statement and table (typed columns: the union of what the active
// accelerators on the table watch) and stages it with the transaction
// (postgres_xact_callbacks.hpp). TRUNCATE stages an invalidation.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include <postgres.h>
#include <access/htup_details.h>
#include <access/xact.h>
#include <catalog/pg_type_d.h>
#include <commands/trigger.h>
#include <executor/tuptable.h>
#include <fmgr.h>
#include <nodes/bitmapset.h>
#include <utils/builtins.h>
#include <utils/date.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/tuplestore.h>
}

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

#include "backends/postgres/engine/accelerator_tables.hpp"
#include "backends/postgres/engine/postgres_xact_callbacks.hpp"
#include "substrate/aux/runtime/columnar_batch.hpp"

namespace {

using accelerator_codegen::aux_state::core::AuxType;
using accelerator_codegen::aux_state::core::MutationKind;
using accelerator_codegen::aux_state::core::TableId;
namespace runtime = accelerator_codegen::aux_state::runtime;
namespace engine = accelerator_codegen::aux_state::engine::postgres;

// Date: Unix-epoch days; PostgreSQL DateADT counts from 2000-01-01.
constexpr int32_t kPostgresDateEpochOffset = 10957;

using int128 = __int128;
constexpr int128 kInt128Max = static_cast<int128>(~static_cast<unsigned __int128>(0) >> 1);

[[noreturn]] void NumericUnsupported() {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("accelerator capture: numeric value is not a finite decimal of at most 38 digits")));
}

// numeric_out text ("-12.340") -> unscaled value (-12340) and scale (3).
void ParseDecimal(const char *text, int128 &value, int32_t &scale) {
  const bool negative = *text == '-';
  value = 0;
  scale = 0;
  bool after_point = false;
  bool saw_digit = false;
  for (const char *c = text + (negative ? 1 : 0); *c != '\0'; c++) {
    if (*c == '.' && !after_point) {
      after_point = true;
      continue;
    }
    if (*c < '0' || *c > '9') {
      NumericUnsupported(); // NaN, Infinity
    }
    const int digit = *c - '0';
    if (value > (kInt128Max - digit) / 10) {
      NumericUnsupported();
    }
    value = value * 10 + digit;
    scale += after_point;
    saw_digit = true;
  }
  if (!saw_digit) {
    NumericUnsupported();
  }
  if (negative) {
    value = -value;
  }
}

int128 ScaleUp(int128 value, int32_t digits) {
  for (; digits > 0; digits--) {
    if (value > kInt128Max / 10 || value < -(kInt128Max / 10)) {
      NumericUnsupported();
    }
    value *= 10;
  }
  return value;
}

// Collects one column of a transition table.
class ColumnBuilder {
public:
  explicit ColumnBuilder(Oid type) {
    switch (type) {
    case BOOLOID:
      kind_ = Kind::Bool;
      break;
    case INT2OID:
    case INT4OID:
    case INT8OID:
      kind_ = Kind::Int;
      break;
    case FLOAT4OID:
    case FLOAT8OID:
      kind_ = Kind::Float;
      break;
    case NUMERICOID:
      kind_ = Kind::Decimal;
      break;
    case DATEOID:
      kind_ = Kind::Date;
      break;
    case BPCHAROID:
      trim_ = true; // trailing spaces are not significant to bpchar
      [[fallthrough]];
    case TEXTOID:
    case VARCHAROID:
      kind_ = Kind::Text;
      break;
    default: { // anything else as its text form
      kind_ = Kind::Other;
      Oid output = InvalidOid;
      bool varlena = false;
      getTypeOutputInfo(type, &output, &varlena);
      fmgr_info(output, &output_);
    }
    }
    type_ = type;
    if (kind_ == Kind::Text || kind_ == Kind::Other) {
      text_.offsets.push_back(0);
    }
  }

  void Append(Datum datum, bool is_null) {
    if (is_null) {
      if (nulls_.empty()) {
        nulls_.assign(rows_, 0);
      }
      nulls_.push_back(1);
    } else if (!nulls_.empty()) {
      nulls_.push_back(0);
    }
    switch (kind_) {
    case Kind::Bool:
      bools_.push_back(!is_null && DatumGetBool(datum));
      break;
    case Kind::Int:
      ints_.push_back(is_null ? 0 : Int(datum));
      break;
    case Kind::Float:
      doubles_.push_back(is_null ? 0 : type_ == FLOAT4OID ? DatumGetFloat4(datum) : DatumGetFloat8(datum));
      break;
    case Kind::Decimal:
      AppendDecimal(datum, is_null);
      break;
    case Kind::Date: {
      const DateADT date = is_null ? 0 : DatumGetDateADT(datum);
      dates_.push_back(is_null || DATE_NOT_FINITE(date) ? date : date + kPostgresDateEpochOffset);
      break;
    }
    case Kind::Text:
      if (!is_null) {
        text *value = DatumGetTextPP(datum);
        std::size_t length = VARSIZE_ANY_EXHDR(value);
        while (trim_ && length > 0 && VARDATA_ANY(value)[length - 1] == ' ') {
          length--;
        }
        text_.bytes.append(VARDATA_ANY(value), length);
      }
      text_.offsets.push_back(text_.bytes.size());
      break;
    case Kind::Other:
      if (!is_null) {
        text_.bytes.append(OutputFunctionCall(&output_, datum));
      }
      text_.offsets.push_back(text_.bytes.size());
      break;
    }
    rows_++;
  }

  runtime::ColumnData Finish() {
    runtime::ColumnData column;
    column.null_flags = std::move(nulls_);
    switch (kind_) {
    case Kind::Bool:
      column.type = AuxType::Boolean;
      column.values = std::move(bools_);
      break;
    case Kind::Int:
      column.type = AuxType::Int64;
      column.values = std::move(ints_);
      break;
    case Kind::Float:
      column.type = AuxType::Double;
      column.values = std::move(doubles_);
      break;
    case Kind::Decimal:
      FinishDecimal(column);
      break;
    case Kind::Date:
      column.type = AuxType::Date;
      column.values = std::move(dates_);
      break;
    case Kind::Text:
    case Kind::Other:
      column.type = AuxType::String;
      column.values = std::move(text_);
      break;
    }
    return column;
  }

private:
  enum class Kind { Bool, Int, Float, Decimal, Date, Text, Other };

  int64_t Int(Datum datum) const {
    return type_ == INT2OID ? DatumGetInt16(datum) : type_ == INT4OID ? DatumGetInt32(datum) : DatumGetInt64(datum);
  }

  // The column's scale is that of its most precise value; the others are
  // scaled up to it (a constrained numeric(p, s) always has scale s).
  void AppendDecimal(Datum datum, bool is_null) {
    int128 value = 0;
    if (!is_null) {
      int32_t scale;
      ParseDecimal(DatumGetCString(DirectFunctionCall1(numeric_out, datum)), value, scale);
      if (scale > scale_) {
        for (auto &earlier : decimals_) {
          earlier = ScaleUp(earlier, scale - scale_);
        }
        scale_ = scale;
      }
      value = ScaleUp(value, scale_ - scale);
    }
    decimals_.push_back(value);
  }

  void FinishDecimal(runtime::ColumnData &column) {
    column.scale = scale_;
    const bool fits64 = std::all_of(decimals_.begin(), decimals_.end(), [](int128 v) {
      return v >= std::numeric_limits<int64_t>::min() && v <= std::numeric_limits<int64_t>::max();
    });
    if (fits64) {
      column.type = AuxType::ScaledDecimal;
      column.values = std::vector<int64_t>(decimals_.begin(), decimals_.end());
    } else {
      column.type = AuxType::ScaledDecimal128;
      column.values = std::move(decimals_);
    }
  }

  Kind kind_;
  bool trim_ = false;
  Oid type_;
  FmgrInfo output_;
  std::size_t rows_ = 0;
  std::vector<uint8_t> nulls_; // empty until the first NULL
  std::vector<uint8_t> bools_;
  std::vector<int64_t> ints_;
  std::vector<double> doubles_;
  std::vector<int128> decimals_;
  int32_t scale_ = 0;
  std::vector<int32_t> dates_;
  runtime::StringColumnData text_;
};

std::vector<int32_t> AllAttnums(TupleDesc desc) {
  std::vector<int32_t> attnums;
  for (int attnum = 0; attnum < desc->natts; attnum++) {
    if (!TupleDescAttr(desc, attnum)->attisdropped) {
      attnums.push_back(attnum);
    }
  }
  return attnums;
}

// Every row of a transition table, as the columns `attnums`.
runtime::ColumnSet ReadTable(Tuplestorestate *store, TupleDesc desc, const std::vector<int32_t> &attnums) {
  std::vector<ColumnBuilder> builders;
  for (const int32_t attnum : attnums) {
    if (attnum < 0 || attnum >= desc->natts || TupleDescAttr(desc, attnum)->attisdropped) {
      ereport(ERROR, (errmsg("accelerator capture: attribute %d does not exist", attnum + 1)));
    }
    builders.emplace_back(TupleDescAttr(desc, attnum)->atttypid);
  }
  TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
  // Per-row output strings and detoasted values go here and are dropped.
  MemoryContext scratch = AllocSetContextCreate(CurrentMemoryContext, "accel capture", ALLOCSET_DEFAULT_SIZES);
  tuplestore_rescan(store);
  while (tuplestore_gettupleslot(store, true, false, slot)) {
    slot_getallattrs(slot);
    MemoryContext previous = MemoryContextSwitchTo(scratch);
    for (std::size_t i = 0; i < builders.size(); i++) {
      builders[i].Append(slot->tts_values[attnums[i]], slot->tts_isnull[attnums[i]]);
    }
    MemoryContextSwitchTo(previous);
    MemoryContextReset(scratch);
  }
  tuplestore_rescan(store);
  MemoryContextDelete(scratch);
  ExecDropSingleTupleTableSlot(slot);

  runtime::ColumnSet columns;
  columns.attnums = attnums;
  for (auto &builder : builders) {
    columns.columns.push_back(builder.Finish());
  }
  return columns;
}

// The attnums the statement assigned, as the batch records them.
std::vector<int32_t> UpdatedAttnums(const Bitmapset *updated, const std::vector<int32_t> &captured) {
  std::vector<int32_t> attnums;
  bool outside = false;
  for (int bit = -1; (bit = bms_next_member(updated, bit)) >= 0;) {
    const int attno = bit + FirstLowInvalidHeapAttributeNumber;
    if (attno <= 0) {
      continue; // system columns
    }
    if (std::binary_search(captured.begin(), captured.end(), attno - 1)) {
      attnums.push_back(attno - 1);
    } else {
      outside = true;
    }
  }
  if (outside) {
    attnums.push_back(runtime::kOutsideProjection);
  }
  return attnums;
}

Datum CaptureTransition(FunctionCallInfo fcinfo) {
  if (!CALLED_AS_TRIGGER(fcinfo)) {
    ereport(ERROR, (errmsg("accelerator transition capture must be called as a trigger")));
  }
  const auto *trigger_data = reinterpret_cast<TriggerData *>(fcinfo->context);
  const TriggerEvent event = trigger_data->tg_event;
  if (!TRIGGER_FIRED_AFTER(event) || !TRIGGER_FIRED_FOR_STATEMENT(event)) {
    ereport(ERROR, (errmsg("accelerator transition capture requires an AFTER STATEMENT trigger")));
  }
  engine::RegisterPostgresXactCallbacks();
  const TableId table = static_cast<TableId>(RelationGetRelid(trigger_data->tg_relation));
  const engine::WatchedTable *watched = engine::WatchedBy(table);

  if (TRIGGER_FIRED_BY_TRUNCATE(event)) {
    if (watched != nullptr) {
      engine::StageInvalidation(table);
    } else {
      engine::NoteWrite(table);
    }
    return PointerGetDatum(nullptr);
  }

  Tuplestorestate *old_store = trigger_data->tg_oldtable;
  Tuplestorestate *new_store = trigger_data->tg_newtable;
  const bool is_insert = TRIGGER_FIRED_BY_INSERT(event);
  const bool is_delete = TRIGGER_FIRED_BY_DELETE(event);
  if ((!is_insert && old_store == nullptr) || (!is_delete && new_store == nullptr)) {
    ereport(ERROR, (errmsg("accelerator transition capture needs the REFERENCING transition tables")));
  }
  const int64 rows = tuplestore_tuple_count(is_delete ? old_store : new_store);
  if (rows == 0) {
    return PointerGetDatum(nullptr); // the statement wrote nothing
  }
  if (watched == nullptr) {
    engine::NoteWrite(table);
    return PointerGetDatum(nullptr);
  }

  TupleDesc desc = RelationGetDescr(trigger_data->tg_relation);
  const std::vector<int32_t> attnums = watched->whole_row ? AllAttnums(desc) : watched->attnums;
  auto batch = std::make_shared<runtime::ColumnarBatch>();
  batch->table = table;
  batch->row_count = static_cast<std::size_t>(rows);
  if (is_insert) {
    batch->kind = MutationKind::Insert;
    batch->new_rows = ReadTable(new_store, desc, attnums);
  } else if (is_delete) {
    batch->kind = MutationKind::Delete;
    batch->old_rows = ReadTable(old_store, desc, attnums);
  } else {
    batch->kind = MutationKind::Update;
    batch->old_rows = ReadTable(old_store, desc, attnums);
    batch->new_rows = ReadTable(new_store, desc, attnums);
    if (trigger_data->tg_updatedcols != nullptr) {
      batch->updated_attnums = UpdatedAttnums(trigger_data->tg_updatedcols, attnums);
    }
  }
  engine::StageBatch(std::move(batch));
  return PointerGetDatum(nullptr);
}

} // namespace

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenCaptureTransition);
Datum AcceleratorCodegenCaptureTransition(PG_FUNCTION_ARGS) { return CaptureTransition(fcinfo); }

} // extern "C"

#endif
