// GENERATED BACKEND
//
// Engine-level helpers of the PostgreSQL backend (module only): SPI scan
// helpers, typed column readers, text normalisation, cost-model path
// resolution.  Per-query scan code (accelerators/<bench>/q<N>/postgres/
// extension/scan.cpp) calls these.  Include the query's module.hpp BEFORE
// this header (finalize headers pull in DuckDB types, which must precede
// PostgreSQL headers).
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_cost_model_path.hpp"
#include "backends/postgres/api/base/pg_base.hpp"
#include "backends/postgres/api/pg_sink.hpp"

extern "C" {
#include "postgres.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/snapmgr.h"
#include "utils/guc.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
// From utils/pg_locale.h, which is not included because it pulls in ICU headers.
extern bool lc_collate_is_c(Oid collation);
}

// The per-query namespace (declared by each module.hpp); present for the
// alias below also where no module is included (the shard worker).
namespace accelerator_codegen::aux_state::query {}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace pg_base = accelerator_codegen::pg_base;

// The query's cost model: `env_name` when set, else the first of
// <stem>_postgres.json and <stem>.json found in the query's directory
// (relative to the working directory), then in the source checkout mounted
// in Docker (/accel-src/<dir>); "" when none exists.
inline std::string ResolveCostModelPath(const char *env_name,
                                        const core::AuxCostModelLocation &location) {
  if (const char *path = std::getenv(env_name); path && *path) {
    return path;
  }
  core::AuxCostModelLookup lookup;
  lookup.engine = "postgres";
  lookup.dirs_after = {std::string("/accel-src/") + location.dir};
  return core::ResolveAuxCostModelPath(location, lookup);
}

inline std::string DatumText(Datum datum, Oid type_oid) {
  Oid output_function = InvalidOid;
  bool is_varlena = false;
  getTypeOutputInfo(type_oid, &output_function, &is_varlena);
  char *text = OidOutputFunctionCall(output_function, datum);
  std::string result(text ? text : "");
  if (text) {
    pfree(text);
  }
  return result;
}

inline std::string TextDatum(Datum datum) {
  char *cstring = text_to_cstring(
      reinterpret_cast<::text *>(PG_DETOAST_DATUM_PACKED(datum)));
  std::string result(cstring ? cstring : "");
  if (cstring) {
    pfree(cstring);
  }
  return result;
}

inline core::AuxDate PostgresDateArg(int32_t arg_index, FunctionCallInfo fcinfo) {
  return core::AuxDate{PG_GETARG_DATEADT(arg_index) + kPostgresDateEpochOffset};
}

template <typename Callback>
inline void ScanTypedTuples(const std::string &sql, Callback &&callback) {
  if (SPI_connect() != SPI_OK_CONNECT) {
    throw std::runtime_error("SPI_connect failed");
  }
  SPIPlanPtr plan = SPI_prepare(sql.c_str(), 0, nullptr);
  if (!plan) {
    SPI_finish();
    throw std::runtime_error("SPI_prepare failed");
  }
  Portal portal = SPI_cursor_open(nullptr, plan, nullptr, nullptr, true);
  if (!portal) {
    SPI_freeplan(plan);
    SPI_finish();
    throw std::runtime_error("SPI_cursor_open failed");
  }
  for (;;) {
    SPI_cursor_fetch(portal, true, 65536);
    if (SPI_processed == 0) {
      if (SPI_tuptable) {
        SPI_freetuptable(SPI_tuptable);
      }
      break;
    }
    TupleDesc tuple_desc = SPI_tuptable->tupdesc;
    for (uint64 row_index = 0; row_index < SPI_processed; ++row_index) {
      callback(SPI_tuptable->vals[row_index], tuple_desc);
    }
    SPI_freetuptable(SPI_tuptable);
  }
  SPI_cursor_close(portal);
  SPI_freeplan(plan);
  SPI_finish();
}

// Participants of a build scan (S8), set through SetBuildScanParticipants.
inline int g_build_scan_participants = 0;

// Column descriptors of ScanColumns.  Integers arrive as int64, decimals as
// value * 10^scale, text as std::string; the columns must be NOT NULL.
inline pg_base::ColumnSpec IntColumn(const char *name) {
  return {name, pg_base::ColType::Int64, 0};
}
inline pg_base::ColumnSpec DateColumn(const char *name) {
  return {name, pg_base::ColType::Date, 0};
}
inline pg_base::ColumnSpec DecimalColumn(const char *name, int scale) {
  return {name, pg_base::ColType::Decimal, scale};
}
inline pg_base::ColumnSpec TextColumn(const char *name) {
  return {name, pg_base::ColType::Text, 0};
}

// One row of a ScanColumns batch; indexes are positions in the column list.
struct BuildRow {
  const pg_base::Batch &batch;
  std::size_t row;
  int64_t Int(int column) const { return batch.I64(column)[row]; }
  int32_t Date(int column) const { return batch.I32(column)[row]; }
  int64_t Decimal(int column) const { return batch.I64(column)[row]; }
  std::string Text(int column) const {
    return std::string(batch.Str(column)[row].View());
  }
  char Char(int column) const {
    const auto text = batch.Str(column)[row].View();
    return text.empty() ? '\0' : text[0];
  }
};

// The parallel counterpart of ScanTypedTuples for builds (S8): `row_fn`
// (BuildRow) sees every row of `table` under the active snapshot, in an
// unspecified order, called from this backend only; workers decode.  Same
// rows as `SELECT <columns> FROM <table>` through ScanTypedTuples.
template <typename RowFn>
inline void ScanColumns(const char *table, std::vector<pg_base::ColumnSpec> columns,
                        RowFn &&row_fn) {
  const int participants = g_build_scan_participants > 0
                               ? g_build_scan_participants
                               : pg_base::ParallelParticipants();
  pg_base::ScanTableParallel(
      pg_base::ScanSpec{table, std::move(columns), {}}, participants,
      [&](const pg_base::Batch &batch) {
        for (std::size_t row = 0; row < batch.count; ++row)
          row_fn(BuildRow{batch, row});
      });
}

inline std::string TypedText(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  return is_null ? std::string() :
                   DatumText(datum, TupleDescAttr(tuple_desc, attribute)->atttypid);
}

// bpchar -> text, as PostgreSQL's cast does it: trailing spaces removed.
inline void StripCharPadding(std::string &text) {
  text.erase(text.find_last_not_of(' ') + 1);
}

// `column = ANY('{k1,k2,...}'::bigint[])`, or FALSE for no keys.
inline std::string KeyListPredicate(const char *column, const std::vector<int64_t> &keys) {
  if (keys.empty())
    return "FALSE";
  std::string sql = std::string(column) + " = ANY('{";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i)
      sql += ',';
    sql += std::to_string(keys[i]);
  }
  return sql + "}'::bigint[])";
}

// ScanTypedTuples for an aggregate whose result is small (bounded by the
// value domain, not the table size): runs to completion through SPI_execute,
// which unlike a cursor may use a parallel plan (Gather over a parallel scan,
// as many workers as max_parallel_workers_per_gather allows), and holds the
// whole result in memory.
template <typename Callback>
inline void ScanTypedTuplesParallel(const std::string &sql, Callback &&callback) {
  if (SPI_connect() != SPI_OK_CONNECT) {
    throw std::runtime_error("SPI_connect failed");
  }
  if (SPI_execute(sql.c_str(), true, 0) != SPI_OK_SELECT) {
    SPI_finish();
    throw std::runtime_error("SPI_execute failed");
  }
  for (uint64 row_index = 0; row_index < SPI_processed; ++row_index) {
    callback(SPI_tuptable->vals[row_index], SPI_tuptable->tupdesc);
  }
  SPI_freetuptable(SPI_tuptable);
  SPI_finish();
}

inline int64_t TypedInt(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  const Datum datum =
      SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  if (is_null) {
    return 0;
  }
  const auto type_oid = TupleDescAttr(tuple_desc, attribute)->atttypid;
  if (type_oid == INT8OID) {
    return DatumGetInt64(datum);
  }
  if (type_oid == INT4OID) {
    return DatumGetInt32(datum);
  }
  const auto text = DatumText(datum, type_oid);
  return text.empty() ? 0 : std::stoll(text);
}

inline double TypedDouble(HeapTuple tuple, TupleDesc tuple_desc, int attribute,
                   bool &is_null) {
  bool null_value = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1,
                                    &null_value);
  is_null = null_value;
  if (is_null) {
    return 0.0;
  }
  return std::stod(DatumText(datum,
                             TupleDescAttr(tuple_desc, attribute)->atttypid));
}

inline int64_t TypedNumericRaw(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  const Datum datum =
      SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  if (is_null) {
    return 0;
  }
  const auto type_oid = TupleDescAttr(tuple_desc, attribute)->atttypid;
  if (type_oid == NUMERICOID) {
    const double value = DatumGetFloat8(
        DirectFunctionCall1(numeric_float8, datum));
    return static_cast<int64_t>(std::llround(value * 100.0));
  }
  const double value = TypedDouble(tuple, tuple_desc, attribute, is_null);
  return is_null ? 0 : static_cast<int64_t>(std::llround(value * 100.0));
}

inline int32_t TypedDate(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  const auto type_oid = TupleDescAttr(tuple_desc, attribute)->atttypid;
  if (is_null) {
    return 0;
  }
  if (type_oid == DATEOID) {
    return DatumGetInt32(datum) + kPostgresDateEpochOffset;
  }
  return static_cast<int32_t>(TypedInt(tuple, tuple_desc, attribute));
}

inline int64_t TypedDecimalRaw(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  return TypedNumericRaw(tuple, tuple_desc, attribute);
}

inline char TypedChar(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  const auto text = TypedText(tuple, tuple_desc, attribute);
  return text.empty() ? '\0' : text[0];
}

} // namespace detail

// Build scans (S8, ScanColumns) use this backend plus participants - 1
// PostgreSQL parallel workers.  The runtime sets it from its thread budget
// before a build; 0 = max_parallel_workers_per_gather + 1.
inline void SetBuildScanParticipants(int participants) {
  detail::g_build_scan_participants = participants;
}

// A NULL text parameter makes the query's comparison NULL (no match): pass it
// on as a NULL value.
inline accelerator_codegen::aux_state::core::AuxValue TextArg(
    FunctionCallInfo fcinfo, int index) {
  namespace core = accelerator_codegen::aux_state::core;
  return PG_ARGISNULL(index)
             ? core::AuxValue::Null(core::AuxType::String)
             : core::AuxValue::Of(core::AuxType::String,
                                  detail::TextDatum(PG_GETARG_DATUM(index)));
}

// Exceptions must not leave the extension function (an uncaught C++ exception
// would terminate the backend): report them as a SQL error.
template <typename F> void RunOrReport(F &&run) {
  std::string message;
  bool division_by_zero = false;
  try {
    run();
  } catch (const std::domain_error &e) {
    message = e.what();
    division_by_zero = true;
  } catch (const std::exception &e) {
    message = e.what();
  }
  if (!message.empty()) {
    if (division_by_zero)
      ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("%s", message.c_str())));
    ereport(ERROR, (errmsg("accelerator: %s", message.c_str())));
  }
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
