// GENERATED BACKEND
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

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q1/postgres/aux/module.hpp"
#include "tpch/q2/postgres/aux/module.hpp"
#include "tpch/q3/postgres/aux/module.hpp"
#include "tpch/q4/postgres/aux/module.hpp"
#include "tpch/q5/postgres/aux/module.hpp"
#include "tpch/q6/postgres/aux/module.hpp"
#include "tpch/q7/postgres/aux/module.hpp"
#include "tpch/q8/postgres/aux/module.hpp"
#include "tpch/q9/postgres/aux/module.hpp"
#include "tpch/q10/postgres/aux/module.hpp"
#include "tpch/q11/postgres/aux/module.hpp"
#include "tpch/q12/postgres/aux/module.hpp"
#include "tpch/q13/postgres/aux/module.hpp"
#include "tpch/q14/postgres/aux/module.hpp"
#include "tpch/q15/postgres/aux/module.hpp"
#include "tpch/q16/postgres/aux/module.hpp"
#include "tpch/q17/postgres/aux/module.hpp"
#include "tpch/q18/postgres/aux/module.hpp"
#include "tpch/q19/postgres/aux/module.hpp"
#include "tpch/q20/postgres/aux/module.hpp"
#include "tpch/q21/postgres/aux/module.hpp"
#include "tpch/q22/postgres/aux/module.hpp"
#if defined(ACCEL_HAVE_Q21_VERUS)
#include "tpch/q21/verus/module.hpp"
#endif

#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"
#include "accelerator_codegen/stateful/postgres/snapshot_token.hpp"
#include "accelerator_codegen/stateful/postgres/aux_mutation_epoch.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"

// Declared in postgres_bridge.cpp (MODULE).
extern "C" int accel_pg_get_observer_mutation_count();

// xact + mutation registration uses the current substrate APIs
// (SetPostgresActiveBackend / RegisterPostgresXactCallbacks).

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
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
// From utils/pg_locale.h, which is not included because it pulls in ICU headers.
extern bool lc_collate_is_c(Oid collation);
}

#include "accelerator_codegen/stateful/postgres/aux_sync.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;

bool FileReadable(const std::string &path) {
  std::ifstream input(path);
  return input.good();
}

std::string Basename(const char *path) {
  if (!path || !*path) {
    return "";
  }
  const std::string text(path);
  const auto slash = text.find_last_of('/');
  return slash == std::string::npos ? text : text.substr(slash + 1);
}

std::string PostgresCostModelBasename(const std::string &basename) {
  // q1_aux_cost_model.json -> q1_aux_cost_model_postgres.json
  static constexpr char kSuffix[] = "_aux_cost_model.json";
  const auto pos = basename.rfind(kSuffix);
  if (pos == std::string::npos || pos + sizeof(kSuffix) - 1 != basename.size()) {
    return "";
  }
  return basename.substr(0, pos) + "_aux_cost_model_postgres.json";
}

std::string ResolveCostModelPath(const char *env_name,
                                 const char *default_relative_path) {
  if (const char *path = std::getenv(env_name); path && *path) {
    return path;
  }
  if (!default_relative_path || !*default_relative_path) {
    return "";
  }
  std::vector<std::string> candidates;
  const std::string basename = Basename(default_relative_path);
  const std::string pg_basename = PostgresCostModelBasename(basename);
  auto PushPair = [&](const std::string &dir_prefix, const std::string &rel) {
    if (!pg_basename.empty()) {
      // Prefer engine-specific calibration when present.
      if (!dir_prefix.empty()) {
        candidates.push_back(dir_prefix + pg_basename);
      } else if (rel.size() >= basename.size()) {
        candidates.push_back(rel.substr(0, rel.size() - basename.size()) +
                             pg_basename);
      }
    }
    if (!dir_prefix.empty()) {
      candidates.push_back(dir_prefix + basename);
    } else {
      candidates.push_back(rel);
    }
  };
  if (const char *config_dir = std::getenv("ACCELERATOR_CODEGEN_CONFIG_DIR");
      config_dir && *config_dir) {
    const std::string prefix = std::string(config_dir) + "/";
    PushPair(prefix, prefix + basename);
  }
  PushPair("", default_relative_path);
  PushPair("/accel-src/", std::string("/accel-src/") + default_relative_path);
  PushPair("/accel-src/config/",
           std::string("/accel-src/config/") + basename);
  PushPair("/tmp/accel-codegen-src/",
           std::string("/tmp/accel-codegen-src/") + default_relative_path);
  for (const auto &candidate : candidates) {
    if (FileReadable(candidate)) {
      return candidate;
    }
  }
  return "";
}

// AuxDate uses Unix-epoch days, while PostgreSQL's DateADT uses days since
// 2000-01-01.
constexpr int32_t kPostgresDateEpochOffset = 10957;

std::string DatumText(Datum datum, Oid type_oid) {
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

std::string TextDatum(Datum datum) {
  char *cstring = text_to_cstring(
      reinterpret_cast<::text *>(PG_DETOAST_DATUM_PACKED(datum)));
  std::string result(cstring ? cstring : "");
  if (cstring) {
    pfree(cstring);
  }
  return result;
}

core::AuxDate PostgresDateArg(int32_t arg_index, FunctionCallInfo fcinfo) {
  return core::AuxDate{PG_GETARG_DATEADT(arg_index) + kPostgresDateEpochOffset};
}

template <typename Callback>
void ScanTypedTuples(const std::string &sql, Callback &&callback) {
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

std::string TypedText(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  const Datum datum = SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  return is_null ? std::string() :
                   DatumText(datum, TupleDescAttr(tuple_desc, attribute)->atttypid);
}

// bpchar -> text, as PostgreSQL's cast does it: trailing spaces removed.
void StripCharPadding(std::string &text) {
  text.erase(text.find_last_not_of(' ') + 1);
}

// TypedText, except that a bpchar value loses its padding (bpchar comparisons
// ignore trailing spaces, so this is the form they compare).
std::string TypedCharText(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  std::string text = TypedText(tuple, tuple_desc, attribute);
  if (TupleDescAttr(tuple_desc, attribute)->atttypid == BPCHAROID)
    StripCharPadding(text);
  return text;
}

int64_t TypedInt(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
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

double TypedDouble(HeapTuple tuple, TupleDesc tuple_desc, int attribute,
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

int64_t TypedNumericRaw(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
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

int32_t TypedDate(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
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

int64_t TypedDecimalRaw(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  return TypedNumericRaw(tuple, tuple_desc, attribute);
}

char TypedChar(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  const auto text = TypedText(tuple, tuple_desc, attribute);
  return text.empty() ? '\0' : text[0];
}

namespace scans {

void ScanTpchQ1Lineitems(
    const std::function<void(const query::TpchQ1LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, l_tax, "
      "l_returnflag, l_linestatus FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedDate(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 4), 2,
                  TypedChar(tuple, tuple_desc, 5),
                  TypedChar(tuple, tuple_desc, 6)});
      });
}

void ScanTpchQ3Customers(
    const std::function<void(const query::TpchQ3CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_mktsegment FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ3Orders(
    const std::function<void(const query::TpchQ3OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate, o_custkey, o_shippriority FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDate(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ3Lineitems(
    const std::function<void(const query::TpchQ3LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDate(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ6Lineitems(
    const std::function<void(const query::TpchQ6LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedDate(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2});
      });
}

void ScanTpchQ10Customers(
    const std::function<void(const query::TpchQ10CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
      "c_phone, c_comment FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3)),
                  TypedText(tuple, tuple_desc, 4),
                  TypedText(tuple, tuple_desc, 5),
                  TypedText(tuple, tuple_desc, 6)});
      });
}

void LookupTpchQ10Customers(
    const std::vector<int64_t> &custkeys,
    std::unordered_map<int64_t, query::TpchQ10Customer> &out) {
  out.clear();
  if (custkeys.empty())
    return;
  std::ostringstream sql;
  sql << "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
         "c_phone, c_comment FROM customer WHERE c_custkey IN (";
  for (std::size_t i = 0; i < custkeys.size(); ++i) {
    if (i > 0)
      sql << ',';
    sql << custkeys[i];
  }
  sql << ')';
  ScanTypedTuples(sql.str(), [&](HeapTuple tuple, TupleDesc tuple_desc) {
    const auto key = TypedInt(tuple, tuple_desc, 0);
    out.emplace(key, query::TpchQ10Customer{
                         TypedText(tuple, tuple_desc, 1),
                         TypedDecimalRaw(tuple, tuple_desc, 2),
                         static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3)),
                         TypedText(tuple, tuple_desc, 4),
                         TypedText(tuple, tuple_desc, 5),
                         TypedText(tuple, tuple_desc, 6)});
  });
}

void ScanTpchQ10Nations(
    const std::function<void(const query::TpchQ10NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ10Orders(
    const std::function<void(const query::TpchQ10OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ10Lineitems(
    const std::function<void(const query::TpchQ10LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_extendedprice, l_discount, l_returnflag "
      "FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedText(tuple, tuple_desc, 3) == "R"});
      });
}

void ScanTpchQ12Orders(
    const std::function<void(const query::TpchQ12OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderpriority FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ12Lineitems(
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, l_shipdate "
      "FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDate(tuple, tuple_desc, 3),
                  TypedDate(tuple, tuple_desc, 4)});
      });
}

void ScanTpchQ14Parts(
    const std::function<void(const query::TpchQ14PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_type FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ14Lineitems(
    const std::function<void(const query::TpchQ14LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDate(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ9Parts(
    const std::function<void(const query::TpchQ9PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_name FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ9Suppliers(
    const std::function<void(const query::TpchQ9SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ9Nations(
    const std::function<void(const query::TpchQ9NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ9Orders(
    const std::function<void(const query::TpchQ9OrdersInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDate(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ9Partsupps(
    const std::function<void(const query::TpchQ9PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ9Lineitems(
    const std::function<void(const query::TpchQ9LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_suppkey, l_orderkey, l_quantity, l_extendedprice, "
      "l_discount FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3),
                  TypedDecimalRaw(tuple, tuple_desc, 4),
                  TypedDecimalRaw(tuple, tuple_desc, 5)});
      });
}

void ScanTpchQ11Partsupp(
    const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost FROM partsupp",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2});
      });
}

void ScanTpchQ11Suppliers(
    const std::function<void(const query::TpchQ11SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ11Nations(
    const std::function<void(const query::TpchQ11NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ17Parts(
    const std::function<void(const query::TpchQ17PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ17Lineitems(
    const std::function<void(const query::TpchQ17LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ17PartQuantityAggregatesForPart(
    int64_t partkey,
    const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
        &consumer) {
  const std::string sql =
      "SELECT CAST(round(l_quantity * 100) AS BIGINT), "
      "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
      "COUNT(*)::BIGINT "
      "FROM lineitem WHERE l_partkey = " +
      std::to_string(partkey) + " GROUP BY 1";
  ScanTypedTuples(sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
    consumer({partkey, TypedInt(tuple, tuple_desc, 0),
              TypedInt(tuple, tuple_desc, 1), TypedInt(tuple, tuple_desc, 2)});
  });
}

void ScanTpchQ18Lineitems(
    const std::function<void(const query::TpchQ18LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_quantity FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ18Orders(
    const std::function<void(const query::TpchQ18OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ18Customers(
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

bool LookupTpchQ18Order(int64_t orderkey, query::TpchQ18OrderInput &out) {
  if (SPI_connect() != SPI_OK_CONNECT) {
    return false;
  }
  char sql[256];
  snprintf(sql, sizeof(sql),
                "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice "
                "FROM orders WHERE o_orderkey = %lld",
                static_cast<long long>(orderkey));
  const int status = SPI_execute(sql, true, 1);
  bool found = false;
  if (status == SPI_OK_SELECT && SPI_processed > 0 && SPI_tuptable) {
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tuple_desc = SPI_tuptable->tupdesc;
    out = {TypedInt(tuple, tuple_desc, 0), TypedInt(tuple, tuple_desc, 1),
           TypedDate(tuple, tuple_desc, 2),
           TypedDecimalRaw(tuple, tuple_desc, 3)};
    found = out.key == orderkey;
  }
  SPI_finish();
  return found;
}

void ScanTpchQ21Suppliers(
    const std::function<void(const query::TpchQ21SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanTpchQ21Nations(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ21Orders(
    const std::function<void(const query::TpchQ21OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderstatus = 'F'",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedChar(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ21Lineitems(
    const std::function<void(const query::TpchQ21LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
      "FROM lineitem l "
      "INNER JOIN orders o ON l.l_orderkey = o.o_orderkey AND "
      "o.o_orderstatus = 'F'",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDate(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ22Customers(
    const std::function<void(const query::TpchQ22CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_phone, c_acctbal FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2});
      });
}

void ScanTpchQ22Orders(
    const std::function<void(const query::TpchQ22OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_custkey FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0)});
      });
}

void ScanTpchQ2Parts(
    const std::function<void(const query::TpchQ2PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_mfgr, p_type, p_size FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ2Suppliers(
    const std::function<void(const query::TpchQ2SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, "
      "s_comment FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3)),
                  TypedText(tuple, tuple_desc, 4),
                  TypedDecimalRaw(tuple, tuple_desc, 5),
                  TypedText(tuple, tuple_desc, 6)});
      });
}

void ScanTpchQ2Partsupps(
    const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ2Nations(
    const std::function<void(const query::TpchQ2NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanTpchQ2Regions(
    const std::function<void(const query::TpchQ2RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ4Orders(
    const std::function<void(const query::TpchQ4OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate, o_orderpriority FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDate(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ4Lineitems(
    const std::function<void(const query::TpchQ4LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_commitdate, l_receiptdate FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDate(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ5Regions(
    const std::function<void(const query::TpchQ5RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ5Nations(
    const std::function<void(const query::TpchQ5NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanTpchQ5Customers(
    const std::function<void(const query::TpchQ5CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ5Suppliers(
    const std::function<void(const query::TpchQ5SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ5Orders(
    const std::function<void(const query::TpchQ5OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ5Lineitems(
    const std::function<void(const query::TpchQ5LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2});
      });
}

void ScanTpchQ7Nations(
    const std::function<void(const query::TpchQ7NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ7Suppliers(
    const std::function<void(const query::TpchQ7SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ7Customers(
    const std::function<void(const query::TpchQ7CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ7Orders(
    const std::function<void(const query::TpchQ7OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ7Lineitems(
    const std::function<void(const query::TpchQ7LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate "
      "FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2,
                  TypedDate(tuple, tuple_desc, 4)});
      });
}

void ScanTpchQ8Parts(
    const std::function<void(const query::TpchQ8PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_type FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ8Suppliers(
    const std::function<void(const query::TpchQ8SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ8Customers(
    const std::function<void(const query::TpchQ8CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ8Orders(
    const std::function<void(const query::TpchQ8OrdersInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ8Lineitems(
    const std::function<void(const query::TpchQ8LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_orderkey, l_suppkey, l_extendedprice, l_discount "
      "FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3),
                  TypedDecimalRaw(tuple, tuple_desc, 4)});
      });
}

void ScanTpchQ8Nations(
    const std::function<void(const query::TpchQ8NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanTpchQ8Regions(
    const std::function<void(const query::TpchQ8RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ13Customers(
    const std::function<void(const query::TpchQ13CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0)});
      });
}

void ScanTpchQ13Orders(
    const std::function<void(const query::TpchQ13OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_custkey, o_comment FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ15Suppliers(
    const std::function<void(const query::TpchQ15SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_phone FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedText(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ15Lineitems(
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedDate(tuple, tuple_desc, 3),
                  TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2});
      });
}

void ScanTpchQ15LineitemsForShipdateRange(
    int32_t start_days, int32_t end_days,
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  if (end_days <= start_days) {
    return;
  }
  const std::string sql =
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem "
      "WHERE l_shipdate >= (DATE '1970-01-01' + " +
      std::to_string(start_days) +
      ") AND l_shipdate < (DATE '1970-01-01' + " + std::to_string(end_days) +
      ")";
  ScanTypedTuples(sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
    consumer({TypedDate(tuple, tuple_desc, 3), TypedInt(tuple, tuple_desc, 0),
              TypedDecimalRaw(tuple, tuple_desc, 1), 2,
              TypedDecimalRaw(tuple, tuple_desc, 2), 2});
  });
}

void ScanTpchQ16Suppliers(
    const std::function<void(const query::TpchQ16SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_comment FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ16Parts(
    const std::function<void(const query::TpchQ16PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_type, p_size FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ16Partsupp(
    const std::function<void(const query::TpchQ16PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey FROM partsupp",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ19Parts(
    const std::function<void(const query::TpchQ19PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container, p_size FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ19Lineitems(
    const std::function<void(const query::TpchQ19LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice, l_discount, l_shipmode, "
      "l_shipinstruct FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3),
                  TypedText(tuple, tuple_desc, 4),
                  TypedText(tuple, tuple_desc, 5)});
      });
}

bool LookupTpchQ19Part(int64_t partkey, query::TpchQ19PartInput &out) {
  if (SPI_connect() != SPI_OK_CONNECT) {
    return false;
  }
  char sql[256];
  snprintf(sql, sizeof(sql),
           "SELECT p_brand, p_container, p_size FROM part WHERE p_partkey = %lld",
           static_cast<long long>(partkey));
  const int status = SPI_execute(sql, true, 1);
  bool found = false;
  if (status == SPI_OK_SELECT && SPI_processed > 0 && SPI_tuptable) {
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tuple_desc = SPI_tuptable->tupdesc;
    out = {partkey, TypedText(tuple, tuple_desc, 0),
           TypedText(tuple, tuple_desc, 1),
           static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))};
    found = true;
  }
  SPI_finish();
  return found;
}

void ScanTpchQ20Lineitems(
    const std::function<void(const query::TpchQ20LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_partkey, l_suppkey, l_quantity FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedDate(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ20Parts(
    const std::function<void(const query::TpchQ20PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_name FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ20Partsupps(
    const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_availqty FROM partsupp",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ20Suppliers(
    const std::function<void(const query::TpchQ20SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ20Nations(
    const std::function<void(const query::TpchQ20NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ1Scan final : public query::TpchQ1InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ1LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ1Lineitems(consumer);
  }
};

class TpchQ3Scan final : public query::TpchQ3InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ3CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ3Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ3OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ3Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ3LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ3Lineitems(consumer);
  }
};

class TpchQ6Scan final : public query::TpchQ6InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ6LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ6Lineitems(consumer);
  }
};

class TpchQ10Scan final : public query::TpchQ10InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ10CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ10Customers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ10NationInput &)> &consumer)
      override {
    scans::ScanTpchQ10Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ10OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ10Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ10LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ10Lineitems(consumer);
  }
  bool LookupCustomers(
      const std::vector<int64_t> &custkeys,
      std::unordered_map<int64_t, query::TpchQ10Customer> &out) override {
    scans::LookupTpchQ10Customers(custkeys, out);
    return true;
  }
};

class TpchQ12Scan final : public query::TpchQ12InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ12OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ12Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ12LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ12Lineitems(consumer);
  }
};

class TpchQ14Scan final : public query::TpchQ14InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ14PartInput &)> &consumer)
      override {
    scans::ScanTpchQ14Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ14LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ14Lineitems(consumer);
  }
};

class TpchQ9Scan final : public query::TpchQ9InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ9PartInput &)> &consumer)
      override {
    scans::ScanTpchQ9Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ9SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ9Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ9NationInput &)> &consumer)
      override {
    scans::ScanTpchQ9Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ9OrdersInput &)> &consumer)
      override {
    scans::ScanTpchQ9Orders(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ9PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ9Partsupps(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ9LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ9Lineitems(consumer);
  }
};

class TpchQ11Scan final : public query::TpchQ11InitialScan {
public:
  void ScanPartsupp(
      const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ11Partsupp(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ11SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ11Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ11NationInput &)> &consumer)
      override {
    scans::ScanTpchQ11Nations(consumer);
  }
};

class TpchQ17Scan final : public query::TpchQ17InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ17PartInput &)> &consumer)
      override {
    scans::ScanTpchQ17Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ17LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ17Lineitems(consumer);
  }
  bool ScanPartQuantityAggregatesForPart(
      int64_t partkey,
      const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
          &consumer) override {
    scans::ScanTpchQ17PartQuantityAggregatesForPart(partkey, consumer);
    return true;
  }
};

class TpchQ18Scan final : public query::TpchQ18InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ18LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ18Lineitems(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ18OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ18Orders(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ18CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ18Customers(consumer);
  }
  bool LookupOrder(int64_t orderkey,
                   query::TpchQ18OrderInput &out) override {
    // SPI is unsafe inside PostgreSQL xact callbacks (PreCommit maintenance).
    // Returning false makes StageMaintenance fall back to RebuildAll.
    (void)orderkey;
    (void)out;
    return false;
  }
};

class TpchQ21Scan final : public query::TpchQ21InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ21SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ21Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ21NationInput &)> &consumer)
      override {
    scans::ScanTpchQ21Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ21OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ21Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ21LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ21Lineitems(consumer);
  }
};

#if defined(ACCEL_HAVE_Q21_VERUS)
// Scans for the Verus Q21 kernel.  Each implements the SQL its interface
// comment names (the kernel's trusted inputs).
class TpchQ21VerusScanImpl final : public query::TpchQ21VerusScan {
public:
  void ScanSuppliers(
      const std::function<void(int64_t, const std::string &, int32_t)> &consumer)
      override {
    ScanTypedTuples("SELECT s_suppkey, s_name, s_nationkey FROM supplier",
                    [&](HeapTuple tuple, TupleDesc desc) {
                      consumer(TypedInt(tuple, desc, 0), TypedCharText(tuple, desc, 1),
                               static_cast<int32_t>(TypedInt(tuple, desc, 2)));
                    });
  }
  void ScanNations(
      const std::function<void(int32_t, const std::string &)> &consumer) override {
    ScanTypedTuples("SELECT n_nationkey, n_name FROM nation",
                    [&](HeapTuple tuple, TupleDesc desc) {
                      consumer(static_cast<int32_t>(TypedInt(tuple, desc, 0)),
                               TypedCharText(tuple, desc, 1));
                    });
  }
  // Capture renders bpchar padded; strip it when the column is bpchar.
  void NormalizeCapturedName(bool supplier, std::string &name) const override {
    const Oid relid = RelnameGetRelid(supplier ? "supplier" : "nation");
    if (!OidIsValid(relid))
      return;
    const AttrNumber attnum = get_attnum(relid, supplier ? "s_name" : "n_name");
    if (attnum != InvalidAttrNumber && get_atttype(relid, attnum) == BPCHAROID)
      StripCharPadding(name);
  }
  void ScanFOrderKeys(const std::function<void(int64_t)> &consumer) override {
    ScanTypedTuples("SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'",
                    [&](HeapTuple tuple, TupleDesc desc) {
                      consumer(TypedInt(tuple, desc, 0));
                    });
  }
  void ScanFLineitems(const std::function<void(const q21v_li_row &)> &consumer) override {
    ScanTypedTuples(
        "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
        "FROM lineitem l WHERE EXISTS (SELECT 1 FROM orders o "
        "WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')",
        [&](HeapTuple tuple, TupleDesc desc) {
          consumer(q21v_li_row{TypedInt(tuple, desc, 0), TypedInt(tuple, desc, 1),
                               TypedDate(tuple, desc, 2), TypedDate(tuple, desc, 3)});
        });
  }
  void ScanLineitemsForKeys(const std::vector<int64_t> &keys,
                            const std::function<void(const q21v_li_row &)> &consumer)
      override {
    std::string sql =
        "SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate FROM lineitem "
        "WHERE l_orderkey = ANY('{";
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i) sql += ',';
      sql += std::to_string(keys[i]);
    }
    sql += "}'::bigint[])";
    // Runs in PRE_COMMIT: make this transaction's last command visible and
    // scan under a fresh snapshot (db + own writes; the sync protocol rules
    // out other writers for a store that stays synced).
    CommandCounterIncrement();
    PushActiveSnapshot(GetLatestSnapshot());
    try {
      ScanTypedTuples(sql, [&](HeapTuple tuple, TupleDesc desc) {
        consumer(q21v_li_row{TypedInt(tuple, desc, 0), TypedInt(tuple, desc, 1),
                             TypedDate(tuple, desc, 2), TypedDate(tuple, desc, 3)});
      });
    } catch (...) {
      PopActiveSnapshot();
      throw;
    }
    PopActiveSnapshot();
  }
  void RunReference(const std::string &nation,
                    const std::function<void(const std::string &, int64_t)> &consumer)
      override {
    static const char *kSql =
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = $1 GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100";
    if (SPI_connect() != SPI_OK_CONNECT)
      throw std::runtime_error("SPI_connect failed");
    Oid argtypes[1] = {TEXTOID};
    Datum values[1] = {CStringGetTextDatum(nation.c_str())};
    if (SPI_execute_with_args(kSql, 1, argtypes, values, nullptr, true, 0) !=
        SPI_OK_SELECT) {
      SPI_finish();
      throw std::runtime_error("Q21 reference query failed");
    }
    std::vector<std::pair<std::string, int64_t>> rows;
    for (uint64 i = 0; i < SPI_processed; ++i) {
      HeapTuple tuple = SPI_tuptable->vals[i];
      TupleDesc desc = SPI_tuptable->tupdesc;
      rows.emplace_back(TypedCharText(tuple, desc, 0), TypedInt(tuple, desc, 1));
    }
    SPI_finish();
    for (const auto &[name, numwait] : rows)
      consumer(name, numwait);
  }
};
#endif

class TpchQ22Scan final : public query::TpchQ22InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ22CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ22Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ22OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ22Orders(consumer);
  }
};

class TpchQ2Scan final : public query::TpchQ2InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ2PartInput &)> &consumer)
      override {
    scans::ScanTpchQ2Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ2SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ2Suppliers(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ2Partsupps(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ2NationInput &)> &consumer)
      override {
    scans::ScanTpchQ2Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ2RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ2Regions(consumer);
  }
};

class TpchQ4Scan final : public query::TpchQ4InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ4OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ4Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ4LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ4Lineitems(consumer);
  }
};

class TpchQ5Scan final : public query::TpchQ5InitialScan {
public:
  void ScanRegions(
      const std::function<void(const query::TpchQ5RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ5Regions(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ5NationInput &)> &consumer)
      override {
    scans::ScanTpchQ5Nations(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ5CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ5Customers(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ5SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ5Suppliers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ5OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ5Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ5Lineitems(consumer);
  }
};

class TpchQ7Scan final : public query::TpchQ7InitialScan {
public:
  void ScanNations(
      const std::function<void(const query::TpchQ7NationInput &)> &consumer)
      override {
    scans::ScanTpchQ7Nations(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ7SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ7Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ7CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ7Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ7OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ7Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ7LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ7Lineitems(consumer);
  }
};

class TpchQ8Scan final : public query::TpchQ8InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ8PartInput &)> &consumer)
      override {
    scans::ScanTpchQ8Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ8SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ8Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ8CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ8Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ8OrdersInput &)> &consumer)
      override {
    scans::ScanTpchQ8Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ8LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ8Lineitems(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ8NationInput &)> &consumer)
      override {
    scans::ScanTpchQ8Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ8RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ8Regions(consumer);
  }
};

class TpchQ13Scan final : public query::TpchQ13InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ13CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ13Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ13OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ13Orders(consumer);
  }
};

class TpchQ15Scan final : public query::TpchQ15InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15Lineitems(consumer);
  }
  void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15LineitemsForShipdateRange(start_days, end_days, consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ15SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ15Suppliers(consumer);
  }
};

class TpchQ16Scan final : public query::TpchQ16InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ16SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ16Suppliers(consumer);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ16PartInput &)> &consumer)
      override {
    scans::ScanTpchQ16Parts(consumer);
  }
  void ScanPartsupp(
      const std::function<void(const query::TpchQ16PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ16Partsupp(consumer);
  }
};

class TpchQ19Scan final : public query::TpchQ19InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ19PartInput &)> &consumer)
      override {
    scans::ScanTpchQ19Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ19LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ19Lineitems(consumer);
  }
  bool LookupPart(int64_t partkey, query::TpchQ19PartInput &out) override {
    return scans::LookupTpchQ19Part(partkey, out);
  }
};

class TpchQ20Scan final : public query::TpchQ20InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ20LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ20Lineitems(consumer);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ20PartInput &)> &consumer)
      override {
    scans::ScanTpchQ20Parts(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ20Partsupps(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ20SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ20Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ20NationInput &)> &consumer)
      override {
    scans::ScanTpchQ20Nations(consumer);
  }
};

class Builder final : public core::StateBuilder {
public:
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

class PgSink final : public core::AuxRowSink {
public:
  PgSink(Tuplestorestate *tuplestore_p, TupleDesc tuple_desc_p)
      : tuplestore(tuplestore_p), tuple_desc(tuple_desc_p) {}

  void Begin(const core::OutputSchema &) override {}

  void WriteRow(const core::AuxRow &row) override {
    std::array<Datum, 12> values{};
    std::array<bool, 12> nulls{};
    if (row.values.size() > values.size()) {
      ereport(ERROR, (errmsg("auxiliary output row exceeds PostgreSQL schema")));
    }
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (row.values[column].is_null) {
        nulls[column] = true;
        continue;
      }
      values[column] = ToDatum(
          row.values[column], TupleDescAttr(tuple_desc, column)->atttypid);
    }
    tuplestore_putvalues(tuplestore, tuple_desc, values.data(), nulls.data());
  }

  void End() override {}

private:
  static std::string DecimalText(const core::ScaledDecimal &decimal) {
    const bool negative = decimal.value < 0;
    uint64_t magnitude = negative
                            ? static_cast<uint64_t>(-(decimal.value + 1)) + 1
                            : static_cast<uint64_t>(decimal.value);
    std::string digits = std::to_string(magnitude);
    if (decimal.scale > 0) {
      if (digits.size() <= static_cast<std::size_t>(decimal.scale)) {
        digits.insert(0, static_cast<std::size_t>(decimal.scale) + 1 -
                               digits.size(),
                     '0');
      }
      digits.insert(digits.size() - decimal.scale, 1, '.');
    }
    if (negative) {
      digits.insert(digits.begin(), '-');
    }
    return digits;
  }

  static Datum ToDatum(const core::AuxValue &value, Oid type_oid) {
    if (std::holds_alternative<int64_t>(value.payload)) {
      return Int64GetDatum(std::get<int64_t>(value.payload));
    }
    if (std::holds_alternative<double>(value.payload)) {
      const Datum as_float8 =
          Float8GetDatum(std::get<double>(value.payload));
      // Q7 (and similar) emit Double while the SQL function returns numeric.
      // A float8 Datum is not a valid Numeric pointer and SIGSEGVs on output.
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall1(float8_numeric, as_float8);
      }
      return as_float8;
    }
    if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
      const auto text = DecimalText(
          std::get<core::ScaledDecimal>(value.payload));
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall3(
            numeric_in, CStringGetDatum(text.c_str()),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
      }
      return Float8GetDatum(std::stod(text));
    }
    if (std::holds_alternative<core::AuxDate>(value.payload)) {
      return DateADTGetDatum(std::get<core::AuxDate>(value.payload)
                                 .days_since_epoch - kPostgresDateEpochOffset);
    }
    if (std::holds_alternative<std::string>(value.payload)) {
      const auto &text = std::get<std::string>(value.payload);
      // Exact numeric results are emitted as numeric text (pg_numeric).
      if (type_oid == NUMERICOID) {
        return DirectFunctionCall3(
            numeric_in, CStringGetDatum(text.c_str()),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
      }
      return CStringGetTextDatum(text.c_str());
    }
    return (Datum)0;
  }

  Tuplestorestate *tuplestore;
  TupleDesc tuple_desc;
};

namespace auxsync = accelerator_codegen::aux_state::engine::postgres::aux_sync;

// Per-backend Runtime.  The Aux store is process-local; aux_sync decides when
// it may serve a hit for the statement snapshot (cross-backend protocol, see
// aux_sync.hpp and tests/verification/q21/dafny/Q21Runtime.dfy):
//
//   Sync marker  (base, own_commits, seq): the store equals the Aux of the
//                database whose dependency-table writers are exactly those
//                committed-visible in `base` plus `own_commits`, and `seq[i]`
//                counts exactly those writers for dependency slot i.
//   Hit(S)       marker synced, store fully valid, this transaction has no
//                captured dependency writes, S covers `base`, S sees every
//                own commit, and (read after S) inflight == 0 and
//                seq == marker.seq on every dependency slot.
//   Rebuild      only in a transaction without an xid (so the build snapshot
//                contains no uncommitted writes of this backend), from a
//                fresh snapshot taken between two counter reads.
//   Own commit   the locally maintained store stays synced iff no other
//                writer was in flight or committed on a dependency slot
//                between PRE_COMMIT (after the maintenance snapshot) and this
//                commit's counter increment.
template <typename Module, typename Config, typename ScanT>
class Runtime final : public accelerator_codegen::aux_state::engine::postgres::
                          aux_sync::Participant {
public:
  // A table/column whose text ordering the verified result depends on.
  struct CollationKey {
    const char *relation;
    const char *column;
  };

  Runtime(const char *cost_model_env, const char *default_config_path,
          core::AcceleratorId accelerator, Config config,
          std::vector<CollationKey> c_collation_keys_p = {})
      : c_collation_keys(std::move(c_collation_keys_p)),
        store([]() {
          return accelerator_codegen::aux_state::engine::postgres::
              AuxMutationEpoch();
        }),
        coordinator(),
        backend(
            coordinator, store,
            [] {
              return accelerator_codegen::aux_state::engine::postgres::
                  PinTokenForActiveSnapshot();
            },
            [cost_model_env, default_config_path] {
              return core::LoadAuxCostModelJson(
                  ResolveCostModelPath(cost_model_env, default_config_path));
            }),
        scan(), module(config, &scan), accelerator(std::move(accelerator)) {
    auto module_view = std::shared_ptr<Module>(&module, [](Module *) {});
    coordinator.Register(std::move(module_view));
    std::vector<core::TableId> tables;
    for (const auto &dependency : module.Dependencies()) {
      tables.push_back(dependency.table);
    }
    dep_slots = auxsync::SlotsOf(tables);
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresMutationBackend(&backend);
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresXactCallbacks();
    auxsync::RegisterParticipant(this);
  }

  void Execute(const core::QueryArguments &arguments, FunctionCallInfo fcinfo) {
    // TokenForSnapshot: freeze (S, epoch) at the earliest UDF entry.
    accelerator_codegen::aux_state::engine::postgres::ForcePinActiveSnapshot();

    RequireCCollation();
    RequireMaterialize(fcinfo);
    // Prefer this Runtime for subsequent trigger captures in this session.
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresMutationBackend(&backend);

    Snapshot statement = GetActiveSnapshot();
    bool hit = HitAllowed(statement);
    if (!hit && NeedsSync() && CanRebuildNow()) {
      Rebuild();
      hit = HitAllowed(statement);
    }
    observed_mutations = accel_pg_get_observer_mutation_count();
    auto *return_set_info =
        reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    PgSink sink(return_set_info->setResult, return_set_info->setDesc);
    elog(DEBUG1, "accelerator %s: %s", accelerator.c_str(),
         hit ? "aux hit" : "fallback");
    if (hit) {
      module.Execute(backend, arguments, sink);
    } else {
      module.ExecuteFallback(backend, arguments, sink);
    }
  }

  void BeforePreCommit() override {
    precommit_ok = false;
    if (!auxsync::OwnRegisteredAnyOf(dep_slots) || !marker.synced) {
      return;
    }
    std::vector<uint64_t> seq;
    precommit_ok = auxsync::ReadCounters(dep_slots, /*allow_own=*/true, seq) &&
                   seq == marker.seq;
  }

  void AfterCommit(const auxsync::CommitInfo &info) override {
    if (info.maintenance_failed) {
      marker.synced = false;
    }
    if (auxsync::OwnRegisteredAnyOf(dep_slots) &&
        TransactionIdIsValid(info.xid)) {
      bool ok = marker.synced && precommit_ok;
      for (std::size_t i = 0; ok && i < dep_slots.size(); ++i) {
        const auto slot = dep_slots[i];
        if (!info.wrote[slot]) {
          continue;
        }
        if (info.old_seq[slot] != marker.seq[i]) {
          ok = false;
        } else {
          marker.seq[i] += 1;
        }
      }
      if (ok) {
        if (marker.own_commits.size() >= kMaxOwnCommits) {
          ok = false;
        } else {
          marker.own_commits.push_back(info.xid);
        }
      }
      marker.synced = ok;
    }
    if (store.NeedsRebuild() || auxsync::OwnTruncatedAnyOf(dep_slots)) {
      marker.synced = false;
    }
    precommit_ok = false;
  }

private:
  static constexpr std::size_t kMaxOwnCommits = 1024;
  struct SyncMarker {
    bool synced = false;
    auxsync::SnapMark base;
    std::vector<TransactionId> own_commits;
    std::vector<uint64_t> seq;
  };

  bool CountersMatch() const {
    std::vector<uint64_t> seq;
    return auxsync::ReadCounters(dep_slots, /*allow_own=*/false, seq) &&
           seq == marker.seq;
  }

  bool NeedsSync() const {
    return !built || !marker.synced || store.NeedsRebuild() || !CountersMatch();
  }

  bool HitAllowed(Snapshot statement) const {
    if (!built || !marker.synced || store.NeedsRebuild() ||
        store.IsTransactionDirty() || auxsync::OwnRegisteredAnyOf(dep_slots)) {
      return false;
    }
    if (!auxsync::Covers(statement, marker.base)) {
      return false;
    }
    for (auto xid : marker.own_commits) {
      if (!auxsync::Sees(statement, xid)) {
        return false;
      }
    }
    // Read after the statement snapshot was taken.
    return CountersMatch();
  }

  bool CanRebuildNow() const {
    return !TransactionIdIsValid(GetTopTransactionIdIfAny()) &&
           !auxsync::OwnRegisteredAny() && !store.IsTransactionDirty() &&
           !IsInParallelMode() && !RecoveryInProgress();
  }

  void Rebuild() {
    marker = SyncMarker{};
    std::vector<uint64_t> before;
    if (!auxsync::ReadCounters(dep_slots, /*allow_own=*/false, before)) {
      return; // a writer is in flight: the result could not be certified
    }
    Snapshot fresh = GetLatestSnapshot();
    auxsync::SnapMark base = auxsync::MarkOf(fresh);
    PushActiveSnapshot(fresh);
    Builder builder;
    try {
      module.Build(backend, builder);
    } catch (...) {
      PopActiveSnapshot();
      throw;
    }
    PopActiveSnapshot();
    if (!builder.finished) {
      throw std::runtime_error("auxiliary build did not finish");
    }
    store.Install(std::move(builder.images), accelerator);
    built = true;
    std::vector<uint64_t> after;
    if (base.ok && auxsync::ReadCounters(dep_slots, /*allow_own=*/false, after) &&
        after == before) {
      marker.synced = true;
      marker.base = std::move(base);
      marker.seq = std::move(before);
    }
    elog(DEBUG1, "accelerator %s: rebuild (%s)", accelerator.c_str(),
         marker.synced ? "synced" : "unsynced");
    accelerator_codegen::aux_state::engine::postgres::
        RegisterPostgresBackendForMutations();
  }

  // Verified domain: the reference semantics orders text byte-wise
  // (TextCLeb), and so do Execute and ExecuteFallback.  PostgreSQL compares
  // text with memcmp exactly when lc_collate_is_c(collation) holds, so every
  // configured ORDER BY key column must have such a collation; otherwise fail
  // closed rather than return an ordering that may differ from PostgreSQL.
  void RequireCCollation() const {
    for (const auto &key : c_collation_keys) {
      const Oid relid = RelnameGetRelid(key.relation);
      if (!OidIsValid(relid)) {
        ereport(ERROR, (errmsg("accelerator %s: relation \"%s\" not found",
                               accelerator.c_str(), key.relation)));
      }
      const AttrNumber attnum = get_attnum(relid, key.column);
      if (attnum == InvalidAttrNumber) {
        ereport(ERROR, (errmsg("accelerator %s: column %s.%s not found",
                               accelerator.c_str(), key.relation, key.column)));
      }
      Oid type_oid = InvalidOid;
      int32 typmod = -1;
      Oid collation = InvalidOid;
      get_atttypetypmodcoll(relid, attnum, &type_oid, &typmod, &collation);
      if (!OidIsValid(collation) || !lc_collate_is_c(collation)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("accelerator %s requires C collation on %s.%s",
                        accelerator.c_str(), key.relation, key.column),
                 errdetail("Its verified result orders text byte-wise; under "
                           "this column's collation PostgreSQL may order "
                           "differently."),
                 errhint("ALTER TABLE %s ALTER COLUMN %s TYPE character "
                         "varying COLLATE \"C\"",
                         key.relation, key.column)));
      }
    }
  }

  void RequireMaterialize(FunctionCallInfo fcinfo) {
    auto *return_set_info =
        reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    if (!return_set_info ||
        !(return_set_info->allowedModes & SFRM_Materialize)) {
      ereport(ERROR,
              (errmsg("accelerator auxiliary functions require materialize mode")));
    }
    InitMaterializedSRF(fcinfo, 0);
  }

  std::vector<CollationKey> c_collation_keys;
  PostgresStateStore store;
  core::AuxTransactionCoordinator coordinator;
  accelerator_codegen::aux_state::engine::postgres::PostgresAuxBackend backend;
  ScanT scan;
  Module module;
  std::string accelerator;
  std::vector<std::size_t> dep_slots;
  SyncMarker marker;
  bool built = false;
  bool precommit_ok = false;
  int observed_mutations = 0;
};


namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan> &Q1Runtime() {
  static Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q1_COST_MODEL",
              "config/q1_aux_cost_model.json",
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan> &Q3Runtime() {
  static Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q3_COST_MODEL",
              "config/q3_aux_cost_model.json",
              core::TpchQ3Layout::kAccelerator,
              query::TpchQ3Config{pg_tbl::kCustomer, pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan> &Q6Runtime() {
  static Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q6_COST_MODEL",
              "config/q6_aux_cost_model.json",
              core::TpchQ6Layout::kAccelerator, query::TpchQ6Config{pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan> &
Q10Runtime() {
  static Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL",
              "config/q10_aux_cost_model.json",
              core::TpchQ10Layout::kAccelerator,
              query::TpchQ10Config{pg_tbl::kCustomer, pg_tbl::kNation, pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan> &
Q12Runtime() {
  static Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q12_COST_MODEL",
              "config/q12_aux_cost_model.json",
              core::TpchQ12Layout::kAccelerator, query::TpchQ12Config{pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan> &
Q14Runtime() {
  static Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q14_COST_MODEL",
              "config/q14_aux_cost_model.json",
              core::TpchQ14Layout::kAccelerator, query::TpchQ14Config{pg_tbl::kPart, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan> &Q9Runtime() {
  static Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q9_COST_MODEL",
              "config/q9_aux_cost_model.json",
              query::TpchQ9Layout::kAccelerator,
              query::TpchQ9Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kNation, pg_tbl::kOrders, pg_tbl::kPartsupp, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan> &
Q11Runtime() {
  static Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q11_COST_MODEL",
              "config/q11_aux_cost_model.json",
              query::TpchQ11Layout::kAccelerator,
              query::TpchQ11Config{pg_tbl::kPartsupp, pg_tbl::kSupplier, pg_tbl::kNation});
  return runtime;
}

Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan> &
Q17Runtime() {
  static Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q17_COST_MODEL",
              "config/q17_aux_cost_model.json",
              query::TpchQ17Layout::kAccelerator,
              query::TpchQ17Config{pg_tbl::kPart, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan> &
Q18Runtime() {
  static Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q18_COST_MODEL",
              "config/q18_aux_cost_model.json",
              query::TpchQ18Layout::kAccelerator,
              query::TpchQ18Config{pg_tbl::kLineitem, pg_tbl::kOrders, pg_tbl::kCustomer});
  return runtime;
}

Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan> &
Q21Runtime() {
  static Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21Layout::kAccelerator,
              query::TpchQ21Config{pg_tbl::kSupplier, pg_tbl::kOrders, pg_tbl::kLineitem, pg_tbl::kNation},
              {{"supplier", "s_name"}});
  return runtime;
}

#if defined(ACCEL_HAVE_Q21_VERUS)
Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig, TpchQ21VerusScanImpl> &
Q21VerusRuntime() {
  static Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig,
                 TpchQ21VerusScanImpl>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21VerusLayout::kAccelerator,
              query::TpchQ21VerusConfig{pg_tbl::kSupplier, pg_tbl::kOrders,
                                        pg_tbl::kLineitem, pg_tbl::kNation},
              {{"supplier", "s_name"}});
  return runtime;
}
#endif

Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan> &
Q22Runtime() {
  static Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q22_COST_MODEL",
              "config/q22_aux_cost_model.json",
              query::TpchQ22Layout::kAccelerator,
              query::TpchQ22Config{pg_tbl::kOrders, pg_tbl::kCustomer});
  return runtime;
}

Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan> &Q2Runtime() {
  static Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q2_COST_MODEL",
              "config/q2_aux_cost_model.json", core::TpchQ2Layout::kAccelerator,
              query::TpchQ2Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kPartsupp, pg_tbl::kNation, pg_tbl::kRegion});
  return runtime;
}

Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan> &Q4Runtime() {
  static Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q4_COST_MODEL",
              "config/q4_aux_cost_model.json", query::TpchQ4Layout::kAccelerator,
              query::TpchQ4Config{pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan> &Q5Runtime() {
  static Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q5_COST_MODEL",
              "config/q5_aux_cost_model.json", core::TpchQ5Layout::kAccelerator,
              query::TpchQ5Config{pg_tbl::kRegion, pg_tbl::kNation, pg_tbl::kCustomer, pg_tbl::kSupplier, pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan> &Q7Runtime() {
  static Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q7_COST_MODEL",
              "config/q7_aux_cost_model.json", core::TpchQ7Layout::kAccelerator,
              query::TpchQ7Config{pg_tbl::kNation, pg_tbl::kSupplier, pg_tbl::kCustomer, pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan> &Q8Runtime() {
  static Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q8_COST_MODEL",
              "config/q8_aux_cost_model.json", query::TpchQ8Layout::kAccelerator,
              query::TpchQ8Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kCustomer, pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}

Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan> &
Q13Runtime() {
  static Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q13_COST_MODEL",
              "config/q13_aux_cost_model.json",
              core::TpchQ13Layout::kAccelerator, query::TpchQ13Config{pg_tbl::kCustomer, pg_tbl::kOrders});
  return runtime;
}

Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan> &
Q15Runtime() {
  static Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q15_COST_MODEL",
              "config/q15_aux_cost_model.json",
              query::TpchQ15Layout::kAccelerator,
              query::TpchQ15Config{pg_tbl::kLineitem, pg_tbl::kSupplier});
  return runtime;
}

Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan> &
Q16Runtime() {
  static Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q16_COST_MODEL",
              "config/q16_aux_cost_model.json",
              query::TpchQ16Layout::kAccelerator,
              query::TpchQ16Config{pg_tbl::kPart, pg_tbl::kPartsupp, pg_tbl::kSupplier});
  return runtime;
}

Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan> &
Q19Runtime() {
  static Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q19_COST_MODEL",
              "config/q19_aux_cost_model.json",
              query::TpchQ19Layout::kAccelerator,
              query::TpchQ19Config{pg_tbl::kLineitem, pg_tbl::kPart});
  return runtime;
}

Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan> &
Q20Runtime() {
  static Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q20_COST_MODEL",
              "config/q20_aux_cost_model.json",
              query::TpchQ20Layout::kAccelerator,
              query::TpchQ20Config{pg_tbl::kLineitem, pg_tbl::kPart, pg_tbl::kPartsupp, pg_tbl::kSupplier, pg_tbl::kNation});
  return runtime;
}

} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

void ExecutePostgresTpchQ1(FunctionCallInfo fcinfo) {
  detail::Q1Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Int64, static_cast<int64_t>(PG_GETARG_INT32(0)))}},
      fcinfo);
}

void ExecutePostgresTpchQ3(FunctionCallInfo fcinfo) {
  detail::Q3Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::Date,
                                detail::PostgresDateArg(1, fcinfo))}},
      fcinfo);
}

void ExecutePostgresTpchQ6(FunctionCallInfo fcinfo) {
  detail::Q6Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::Date,
                                detail::PostgresDateArg(0, fcinfo)),
                            core::AuxValue::Of(core::AuxType::Double,
                                               PG_GETARG_FLOAT8(1)),
                            core::AuxValue::Of(core::AuxType::Double,
                                               PG_GETARG_FLOAT8(2))}},
      fcinfo);
}

void ExecutePostgresTpchQ10(FunctionCallInfo fcinfo) {
  detail::Q10Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Date, detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

void ExecutePostgresTpchQ12(FunctionCallInfo fcinfo) {
  detail::Q12Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1))),
                            core::AuxValue::Of(
                                core::AuxType::Date,
                                detail::PostgresDateArg(2, fcinfo))}},
      fcinfo);
}

void ExecutePostgresTpchQ14(FunctionCallInfo fcinfo) {
  detail::Q14Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Date, detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

void ExecutePostgresTpchQ9(FunctionCallInfo fcinfo) {
  detail::Q9Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}

void ExecutePostgresTpchQ11(FunctionCallInfo fcinfo) {
  detail::Q11Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(core::AuxType::Double,
                                               PG_GETARG_FLOAT8(1))}},
      fcinfo);
}

void ExecutePostgresTpchQ17(FunctionCallInfo fcinfo) {
  detail::Q17Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1)))}},
      fcinfo);
}

void ExecutePostgresTpchQ18(FunctionCallInfo fcinfo) {
  detail::Q18Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Int64, static_cast<int64_t>(PG_GETARG_INT64(0)))}},
      fcinfo);
}

void ExecutePostgresTpchQ21(FunctionCallInfo fcinfo) {
  detail::Q21Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}

#if defined(ACCEL_HAVE_Q21_VERUS)
void ExecutePostgresTpchQ21Verus(FunctionCallInfo fcinfo) {
  detail::Q21VerusRuntime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}
#endif

void ExecutePostgresTpchQ22(FunctionCallInfo fcinfo) {
  detail::Q22Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(2))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(3))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(4))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(5))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(6)))}},
      fcinfo);
}

void ExecutePostgresTpchQ2(FunctionCallInfo fcinfo) {
  detail::Q2Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(2)))}},
      fcinfo);
}

void ExecutePostgresTpchQ4(FunctionCallInfo fcinfo) {
  detail::Q4Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Date, detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

void ExecutePostgresTpchQ5(FunctionCallInfo fcinfo) {
  detail::Q5Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::Date,
                                detail::PostgresDateArg(1, fcinfo))}},
      fcinfo);
}

void ExecutePostgresTpchQ7(FunctionCallInfo fcinfo) {
  detail::Q7Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1)))}},
      fcinfo);
}

void ExecutePostgresTpchQ8(FunctionCallInfo fcinfo) {
  detail::Q8Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(2)))}},
      fcinfo);
}

void ExecutePostgresTpchQ13(FunctionCallInfo fcinfo) {
  detail::Q13Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}

void ExecutePostgresTpchQ15(FunctionCallInfo fcinfo) {
  detail::Q15Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Date, detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

void ExecutePostgresTpchQ16(FunctionCallInfo fcinfo) {
  detail::Q16Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(2))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(3))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(4))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(5))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(6))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(7))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(8))),
                            core::AuxValue::Of(
                                core::AuxType::Int64,
                                static_cast<int64_t>(PG_GETARG_INT32(9)))}},
      fcinfo);
}

void ExecutePostgresTpchQ19(FunctionCallInfo fcinfo) {
  detail::Q19Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(core::AuxType::Int64,
                                               PG_GETARG_INT64(1)),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(2))),
                            core::AuxValue::Of(core::AuxType::Int64,
                                               PG_GETARG_INT64(3)),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(4))),
                            core::AuxValue::Of(core::AuxType::Int64,
                                               PG_GETARG_INT64(5))}},
      fcinfo);
}

void ExecutePostgresTpchQ20(FunctionCallInfo fcinfo) {
  detail::Q20Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::Date,
                                detail::PostgresDateArg(0, fcinfo)),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(2)))}},
      fcinfo);
}


} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
