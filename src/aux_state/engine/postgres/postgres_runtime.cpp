#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q1_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q2_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q3_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q4_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q6_common.hpp"
#include "accelerator_codegen/aux_state/query/sec_edgar/sec_edgar_q24_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q1_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q3_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q6_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q10_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q12_common.hpp"
#include "accelerator_codegen/aux_state/query/tpch/tpch_q14_common.hpp"

#include "accelerator_codegen/aux_state/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/aux_state/core/aux_calibration.hpp"
#include "accelerator_codegen/aux_state/engine/postgres/postgres_mutation_capture.hpp"
#include "accelerator_codegen/aux_state/engine/postgres/postgres_xact_callbacks.hpp"
#include "accelerator_codegen/aux_state/engine/postgres/postgres_aux_backend.hpp"

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
}

#include "accelerator_codegen/aux_state/engine/postgres/postgres_runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {

namespace core = accelerator_codegen::aux_state::core;
namespace maintenance = accelerator_codegen::aux_state::maintenance;
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
  if (const char *config_dir = std::getenv("ACCELERATOR_CODEGEN_CONFIG_DIR");
      config_dir && *config_dir) {
    candidates.push_back(std::string(config_dir) + "/" + basename);
  }
  candidates.emplace_back(default_relative_path);
  candidates.push_back(std::string("/tmp/accel-codegen-src/") +
                       default_relative_path);
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

class LocalStateStore final : public core::AuxStateStore {
public:
  core::StateSnapshot Read(const core::VisibilityToken &visibility) const override {
    core::StateSnapshot snapshot;
    snapshot.accelerator = accelerator;
    snapshot.visibility = visibility;
    if (!valid || transaction_dirty) {
      return snapshot;
    }
    for (const auto &entry : images) {
      snapshot.partitions.push_back(entry.second);
    }
    snapshot.available = !images.empty();
    return snapshot;
  }

  core::StateSnapshot ReadPartitions(
      const core::VisibilityToken &visibility,
      const std::vector<core::PartitionId> &partitions) const override {
    core::StateSnapshot snapshot;
    snapshot.accelerator = accelerator;
    snapshot.visibility = visibility;
    if (!valid) {
      return snapshot;
    }
    for (const auto partition : partitions) {
      const auto it = images.find(partition);
      if (it != images.end()) {
        snapshot.partitions.push_back(it->second);
      }
    }
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }

  core::ValidityResult CheckValidity(
      const core::QueryKey &key,
      const core::VisibilityToken &) const override {
    if (key.accelerator != accelerator) {
      return {core::ValidityCode::Missing, 0};
    }
    if (transaction_dirty) {
      return {core::ValidityCode::NotVisible, 0};
    }
    return {valid ? core::ValidityCode::Valid : core::ValidityCode::Missing,
            valid ? version : 0};
  }

  void Invalidate(const core::InvalidationPlan &) override {
    valid = false;
  }

  void MarkTransactionDirty() override { transaction_dirty = true; }
  void ClearTransactionDirty() override { transaction_dirty = false; }

  bool TryPublish(const core::StagedMaintenanceOutcome &outcome,
                  core::Version expected_version) override {
    if (expected_version != 0 && expected_version != version) {
      valid = false;
      return false;
    }
    for (const auto &image : outcome.updates) {
      auto published = image;
      published.version = ++version;
      images[published.partition] = std::move(published);
    }
    valid = !images.empty();
    return true;
  }

  void Install(std::vector<core::StateImage> built,
               std::string accelerator_id) {
    images.clear();
    accelerator = std::move(accelerator_id);
    ++version;
    for (auto &image : built) {
      image.version = version;
      images.emplace(image.partition, std::move(image));
    }
    valid = !images.empty();
  }

private:
  std::string accelerator;
  std::map<core::PartitionId, core::StateImage> images;
  core::Version version = 0;
  bool valid = false;
  bool transaction_dirty = false;
};

class ServicesBase : public core::AuxEngineServices {
public:
  ServicesBase(LocalStateStore &store_p, const char *cost_model_env,
               const char *default_config_path)
      : store(store_p),
        cost_model(core::LoadAuxCostModelJson(
            ResolveCostModelPath(cost_model_env, default_config_path))) {}

  core::VisibilityToken CurrentVisibility() const override {
    return {};
  }
  core::AuxStateStore &StateStore() override { return store; }
  const maintenance::CostModelParams &MaintenanceCostModel() const override {
    return cost_model;
  }

private:
  LocalStateStore &store;
  maintenance::CostModelParams cost_model;
};

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

void ScanSecEdgarQ1Pre(
    const std::function<void(const query::SecEdgarQ1PreInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT stmt, rfile, adsh, line FROM pre WHERE stmt IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        bool stmt_is_null = false;
        bool rfile_is_null = false;
        SPI_getbinval(tuple, tuple_desc, 1, &stmt_is_null);
        SPI_getbinval(tuple, tuple_desc, 2, &rfile_is_null);
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  rfile_is_null,
                  stmt_is_null,
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanSecEdgarQ2Num(
    const std::function<void(const query::SecEdgarQ2NumInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, tag, uom, value FROM num",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        bool value_is_null = false;
        (void)TypedDouble(tuple, tuple_desc, 3, value_is_null);
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedDouble(tuple, tuple_desc, 3, value_is_null),
                  value_is_null});
      });
}

void ScanSecEdgarQ2Sub(
    const std::function<void(const query::SecEdgarQ2SubInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, name, fy FROM sub",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanSecEdgarQ3Num(
    const std::function<void(const query::SecEdgarQ3NumInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, uom, value FROM num",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        bool value_is_null = false;
        (void)TypedDouble(tuple, tuple_desc, 2, value_is_null);
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDouble(tuple, tuple_desc, 2, value_is_null),
                  value_is_null});
      });
}

void ScanSecEdgarQ3Sub(
    const std::function<void(const query::SecEdgarQ3SubInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, name, cik, fy FROM sub",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2)),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanSecEdgarQ4Sub(
    const std::function<void(const query::SecEdgarQ4SubInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, cik, sic FROM sub",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1)),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanSecEdgarQ4Tag(
    const std::function<void(const query::SecEdgarQ4TagInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT tag, version, abstract, tlabel FROM tag",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2)),
                  TypedText(tuple, tuple_desc, 3)});
      });
}

void ScanSecEdgarQ4Pre(
    const std::function<void(const query::SecEdgarQ4PreInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, tag, version, stmt FROM pre",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedText(tuple, tuple_desc, 3)});
      });
}

void ScanSecEdgarQ4Num(
    const std::function<void(const query::SecEdgarQ4NumInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, tag, version, uom, value FROM num",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        bool value_is_null = false;
        (void)TypedDouble(tuple, tuple_desc, 4, value_is_null);
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedText(tuple, tuple_desc, 3),
                  TypedDouble(tuple, tuple_desc, 4, value_is_null),
                  value_is_null});
      });
}

void ScanSecEdgarQ6Num(
    const std::function<void(const query::SecEdgarQ6NumInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, tag, version, uom, value FROM num",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        bool value_is_null = false;
        (void)TypedDouble(tuple, tuple_desc, 4, value_is_null);
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedText(tuple, tuple_desc, 3),
                  TypedDouble(tuple, tuple_desc, 4, value_is_null),
                  value_is_null});
      });
}

void ScanSecEdgarQ6Sub(
    const std::function<void(const query::SecEdgarQ6SubInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, name, fy FROM sub",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanSecEdgarQ6Pre(
    const std::function<void(const query::SecEdgarQ6PreInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, tag, version, stmt, plabel FROM pre",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedText(tuple, tuple_desc, 3),
                  TypedText(tuple, tuple_desc, 4)});
      });
}

void ScanSecEdgarQ24Num(
    const std::function<void(const query::SecEdgarQ24NumInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, tag, version, uom, value, ddate FROM num",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        bool value_is_null = false;
        (void)TypedDouble(tuple, tuple_desc, 4, value_is_null);
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedText(tuple, tuple_desc, 3),
                  TypedDouble(tuple, tuple_desc, 4, value_is_null),
                  value_is_null,
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 5))});
      });
}

void ScanSecEdgarQ24Pre(
    const std::function<void(const query::SecEdgarQ24PreInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT adsh, tag, version FROM pre",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedText(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2)});
      });
}

} // namespace scans

class TpchQ1Services final : public ServicesBase,
                             public query::TpchQ1InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanLineitems(
      const std::function<void(const query::TpchQ1LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ1Lineitems(consumer);
  }
};

class TpchQ3Services final : public ServicesBase,
                             public query::TpchQ3InitialScan {
public:
  using ServicesBase::ServicesBase;
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

class TpchQ6Services final : public ServicesBase,
                             public query::TpchQ6InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanLineitems(
      const std::function<void(const query::TpchQ6LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ6Lineitems(consumer);
  }
};

class TpchQ10Services final : public ServicesBase,
                              public query::TpchQ10InitialScan {
public:
  using ServicesBase::ServicesBase;
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
};

class TpchQ12Services final : public ServicesBase,
                              public query::TpchQ12InitialScan {
public:
  using ServicesBase::ServicesBase;
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

class TpchQ14Services final : public ServicesBase,
                              public query::TpchQ14InitialScan {
public:
  using ServicesBase::ServicesBase;
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

class SecEdgarQ1Services final : public ServicesBase,
                                 public query::SecEdgarQ1InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanPre(
      const std::function<void(const query::SecEdgarQ1PreInput &)> &consumer)
      const override {
    scans::ScanSecEdgarQ1Pre(consumer);
  }
};

class SecEdgarQ2Services final : public ServicesBase,
                               public query::SecEdgarQ2InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanNum(
      const std::function<void(const query::SecEdgarQ2NumInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ2Num(consumer);
  }
  void ScanSub(
      const std::function<void(const query::SecEdgarQ2SubInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ2Sub(consumer);
  }
};

class SecEdgarQ3Services final : public ServicesBase,
                                 public query::SecEdgarQ3InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanNum(
      const std::function<void(const query::SecEdgarQ3NumInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ3Num(consumer);
  }
  void ScanSub(
      const std::function<void(const query::SecEdgarQ3SubInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ3Sub(consumer);
  }
};

class SecEdgarQ4Services final : public ServicesBase,
                                 public query::SecEdgarQ4InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanSub(
      const std::function<void(const query::SecEdgarQ4SubInput &)> &consumer)
      const override {
    scans::ScanSecEdgarQ4Sub(consumer);
  }
  void ScanTag(
      const std::function<void(const query::SecEdgarQ4TagInput &)> &consumer)
      const override {
    scans::ScanSecEdgarQ4Tag(consumer);
  }
  void ScanPre(
      const std::function<void(const query::SecEdgarQ4PreInput &)> &consumer)
      const override {
    scans::ScanSecEdgarQ4Pre(consumer);
  }
  void ScanNum(
      const std::function<void(const query::SecEdgarQ4NumInput &)> &consumer)
      const override {
    scans::ScanSecEdgarQ4Num(consumer);
  }
};

class SecEdgarQ6Services final : public ServicesBase,
                                 public query::SecEdgarQ6InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanNum(
      const std::function<void(const query::SecEdgarQ6NumInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ6Num(consumer);
  }
  void ScanSub(
      const std::function<void(const query::SecEdgarQ6SubInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ6Sub(consumer);
  }
  void ScanPre(
      const std::function<void(const query::SecEdgarQ6PreInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ6Pre(consumer);
  }
};

class SecEdgarQ24Services final : public ServicesBase,
                                  public query::SecEdgarQ24InitialScan {
public:
  using ServicesBase::ServicesBase;
  void ScanNum(
      const std::function<void(const query::SecEdgarQ24NumInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ24Num(consumer);
  }
  void ScanPre(
      const std::function<void(const query::SecEdgarQ24PreInput &)> &consumer)
      override {
    scans::ScanSecEdgarQ24Pre(consumer);
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
      return Float8GetDatum(std::get<double>(value.payload));
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
      return CStringGetTextDatum(
          std::get<std::string>(value.payload).c_str());
    }
    return (Datum)0;
  }

  Tuplestorestate *tuplestore;
  TupleDesc tuple_desc;
};

template <typename Module, typename Config, typename ServicesT>
class Runtime final {
public:
  Runtime(const char *cost_model_env, const char *default_config_path,
          core::AcceleratorId accelerator, Config config)
      : store(), services(store, cost_model_env, default_config_path),
        module(config, &services), coordinator(),
        backend(
            coordinator, store,
            [this] { return services.CurrentVisibility(); },
            [this] { return services.MaintenanceCostModel(); }),
        capture(backend), accelerator(std::move(accelerator)) {
    auto module_view = std::shared_ptr<Module>(&module, [](Module *) {});
    coordinator.Register(std::move(module_view));
    accelerator_codegen::aux_state::engine::postgres::
        RegisterTransactionBackend(&backend);
  }

  void EnsureBuilt() {
    if (built) {
      return;
    }
    Builder builder;
    module.Build(services, builder);
    if (!builder.finished) {
      throw std::runtime_error("auxiliary build did not finish");
    }
    store.Install(std::move(builder.images), accelerator);
    built = true;
    capture.Register();
  }

  void Execute(const core::QueryArguments &arguments, FunctionCallInfo fcinfo) {
    RequireMaterialize(fcinfo);
    auto *return_set_info =
        reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    EnsureBuilt();
    PgSink sink(return_set_info->setResult, return_set_info->setDesc);
    module.Execute(services, arguments, sink);
  }

  LocalStateStore store;
  ServicesT services;
  Module module;

private:
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

  core::AuxTransactionCoordinator coordinator;
  accelerator_codegen::aux_state::engine::postgres::PostgresAuxBackend backend;
  accelerator_codegen::aux_state::engine::postgres::PostgresMutationCapture
      capture;
  std::string accelerator;
  bool built = false;
};

Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Services> &Q1Runtime() {
  static Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q1_COST_MODEL",
              "config/q1_aux_cost_model.json",
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{4});
  return runtime;
}

Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Services> &Q3Runtime() {
  static Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q3_COST_MODEL",
              "config/q3_aux_cost_model.json",
              core::TpchQ3Layout::kAccelerator,
              query::TpchQ3Config{1003, 1002, 1004});
  return runtime;
}

Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Services> &Q6Runtime() {
  static Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q6_COST_MODEL",
              "config/q6_aux_cost_model.json",
              core::TpchQ6Layout::kAccelerator, query::TpchQ6Config{1006});
  return runtime;
}

Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Services> &
Q10Runtime() {
  static Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL",
              "config/q10_aux_cost_model.json",
              core::TpchQ10Layout::kAccelerator,
              query::TpchQ10Config{1, 2, 3, 4});
  return runtime;
}

Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Services> &
Q12Runtime() {
  static Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q12_COST_MODEL",
              "config/q12_aux_cost_model.json",
              core::TpchQ12Layout::kAccelerator, query::TpchQ12Config{1012, 1013});
  return runtime;
}

Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Services> &
Q14Runtime() {
  static Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q14_COST_MODEL",
              "config/q14_aux_cost_model.json",
              core::TpchQ14Layout::kAccelerator, query::TpchQ14Config{1015, 1014});
  return runtime;
}

Runtime<query::SecEdgarQ1Module, query::SecEdgarQ1Config, SecEdgarQ1Services> &
SecQ1Runtime() {
  static Runtime<query::SecEdgarQ1Module, query::SecEdgarQ1Config,
                 SecEdgarQ1Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_Q1_COST_MODEL",
              "config/sec_edgar_q1_aux_cost_model.json",
              core::SecEdgarQ1Layout::kAccelerator,
              query::SecEdgarQ1Config{102});
  return runtime;
}

Runtime<query::SecEdgarQ2Module, query::SecEdgarQ2Config, SecEdgarQ2Services> &
SecQ2Runtime() {
  static Runtime<query::SecEdgarQ2Module, query::SecEdgarQ2Config,
                 SecEdgarQ2Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_Q2_COST_MODEL",
              "config/sec_edgar_q2_aux_cost_model.json",
              core::SecEdgarQ2Layout::kAccelerator,
              query::SecEdgarQ2Config{103, 101});
  return runtime;
}

Runtime<query::SecEdgarQ3Module, query::SecEdgarQ3Config, SecEdgarQ3Services> &
SecQ3Runtime() {
  static Runtime<query::SecEdgarQ3Module, query::SecEdgarQ3Config,
                 SecEdgarQ3Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_Q3_COST_MODEL",
              "config/sec_edgar_q3_aux_cost_model.json",
              core::SecEdgarQ3Layout::kAccelerator,
              query::SecEdgarQ3Config{103, 101});
  return runtime;
}

Runtime<query::SecEdgarQ4Module, query::SecEdgarQ4Config, SecEdgarQ4Services> &
SecQ4Runtime() {
  static Runtime<query::SecEdgarQ4Module, query::SecEdgarQ4Config,
                 SecEdgarQ4Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_Q4_COST_MODEL",
              "config/sec_edgar_q4_aux_cost_model.json",
              core::SecEdgarQ4Layout::kAccelerator,
              query::SecEdgarQ4Config{103, 101, 104, 102});
  return runtime;
}

Runtime<query::SecEdgarQ6Module, query::SecEdgarQ6Config, SecEdgarQ6Services> &
SecQ6Runtime() {
  static Runtime<query::SecEdgarQ6Module, query::SecEdgarQ6Config,
                 SecEdgarQ6Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_Q6_COST_MODEL",
              "config/sec_edgar_q6_aux_cost_model.json",
              core::SecEdgarQ6Layout::kAccelerator,
              query::SecEdgarQ6Config{103, 101, 102});
  return runtime;
}

Runtime<query::SecEdgarQ24Module, query::SecEdgarQ24Config, SecEdgarQ24Services> &
Q24Runtime() {
  static Runtime<query::SecEdgarQ24Module, query::SecEdgarQ24Config,
                 SecEdgarQ24Services>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q24_COST_MODEL",
              "config/sec_edgar_q24_aux_cost_model.json",
              core::SecEdgarQ24Layout::kAccelerator,
              query::SecEdgarQ24Config{103, 102});
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

void ExecutePostgresSecEdgarQ1(FunctionCallInfo fcinfo) {
  detail::SecQ1Runtime().Execute({}, fcinfo);
}

void ExecutePostgresSecEdgarQ2(FunctionCallInfo fcinfo) {
  detail::SecQ2Runtime().Execute({}, fcinfo);
}

void ExecutePostgresSecEdgarQ3(FunctionCallInfo fcinfo) {
  detail::SecQ3Runtime().Execute({}, fcinfo);
}

void ExecutePostgresSecEdgarQ4(FunctionCallInfo fcinfo) {
  detail::SecQ4Runtime().Execute({}, fcinfo);
}

void ExecutePostgresSecEdgarQ6(FunctionCallInfo fcinfo) {
  detail::SecQ6Runtime().Execute({}, fcinfo);
}

void ExecutePostgresSecEdgarQ24(FunctionCallInfo fcinfo) {
  detail::Q24Runtime().Execute({}, fcinfo);
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
