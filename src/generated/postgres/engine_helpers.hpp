// GENERATED BACKEND
//
// Engine-level helpers of the PostgreSQL backend (module only): SPI scan
// helpers, typed column readers, text normalisation, cost-model path
// resolution.  Per-query scan code (src/generated/<bench>/q<N>/postgres/
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

#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_cost_model_path.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"
#include "accelerator_codegen/stateful/postgres/snapshot_token.hpp"
#include "accelerator_codegen/stateful/postgres/aux_mutation_epoch.hpp"

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

#include "accelerator_codegen/stateful/postgres/aux_sync.hpp"


namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;

// The query's cost model: `env_name` when set, else the first of
// <stem>_postgres.json and <stem>.json found in ACCELERATOR_CODEGEN_CONFIG_DIR,
// the query's directory, then the source checkout in Docker (/accel-src, its
// copy of the query's directory, /tmp/accel-codegen-src); "" when none exists.
inline std::string ResolveCostModelPath(const char *env_name,
                                        const core::AuxCostModelLocation &location) {
  if (const char *path = std::getenv(env_name); path && *path) {
    return path;
  }
  core::AuxCostModelLookup lookup;
  lookup.engine = "postgres";
  if (const char *config_dir = std::getenv("ACCELERATOR_CODEGEN_CONFIG_DIR");
      config_dir && *config_dir) {
    lookup.dirs_before.push_back(config_dir);
  }
  lookup.dirs_after = {"/accel-src", std::string("/accel-src/") + location.dir,
                       "/tmp/accel-codegen-src"};
  return core::ResolveAuxCostModelPath(location, lookup);
}

// AuxDate uses Unix-epoch days, while PostgreSQL's DateADT uses days since
// 2000-01-01.
constexpr int32_t kPostgresDateEpochOffset = 10957;

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

// SQL this backend ran against the tables (a query that runs any answered
// from SQL, not from the state).
inline thread_local uint64_t t_table_scans = 0;

template <typename Callback>
inline void ScanTypedTuples(const std::string &sql, Callback &&callback) {
  ++t_table_scans;
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

// ScanTypedTuples on the database as this transaction leaves it: its own
// writes, including the last command's, and every committed transaction.
// For scans during PRE_COMMIT (an aux rescan must read the database after
// the transaction): without the command counter increment the last
// command's writes are invisible, and the statement / transaction snapshot
// can predate them.
template <typename Callback>
inline void ScanTypedTuplesAfterOwnWrites(const std::string &sql, Callback &&callback) {
  CommandCounterIncrement();
  PushActiveSnapshot(GetLatestSnapshot());
  try {
    ScanTypedTuples(sql, std::forward<Callback>(callback));
  } catch (...) {
    PopActiveSnapshot();
    throw;
  }
  PopActiveSnapshot();
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
