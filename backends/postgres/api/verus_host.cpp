// GENERATED BACKEND
//
// Verus kits on Postgres (verus_host.hpp).  The kit's accelerator is the generic Runtime
// (engine_runtime.hpp) over the kit's module: captures, shard workers, epochs and reads are the
// accelerators' own.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/api/verus_host.hpp"

#include <pthread.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

extern "C" {
#include "catalog/pg_type.h"
#include "utils/array.h"
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

namespace core = accelerator_codegen::aux_state::core;
using verus_kit::Glue;
using verus_kit::Kind;

const pthread_t g_main_thread = pthread_self();

// The kit's cells as the DuckDB host delivers them: a numeric that fits 64 bits as a ScaledDecimal,
// CHAR without its padding (trailing spaces are not significant to bpchar).
core::AuxValue KitCell(HeapTuple tuple, TupleDesc desc, int column) {
  const Oid type = TupleDescAttr(desc, column)->atttypid;
  if (type == BPCHAROID) {
    char *text = SPI_getvalue(tuple, desc, column + 1);
    if (!text)
      return core::AuxValue::Null(core::AuxType::String);
    std::string out(text);
    pfree(text);
    out.erase(out.find_last_not_of(' ') + 1);
    return core::AuxValue::Of(core::AuxType::String, std::move(out));
  }
  auto cell = detail::CellOf(tuple, desc, column);
  if (const auto *wide = std::get_if<core::ScaledDecimal128>(&cell.payload);
      wide && wide->value >= INT64_MIN && wide->value <= INT64_MAX)
    return core::AuxValue::Of(core::AuxType::ScaledDecimal,
                              core::ScaledDecimal{static_cast<int64_t>(wide->value), wide->scale});
  return cell;
}

class SpiHost final : public verus_kit::Host {
public:
  void Query(const std::string &sql, const verus_kit::RowFn &row) override { Run(sql, {}, {}, {}, row); }

  void QueryParams(const std::string &sql, const std::vector<core::AuxValue> &params,
                   const verus_kit::RowFn &row) override {
    core::QueryArguments arguments;
    arguments.values = params;
    auto bound = detail::ToSpiParameters(arguments);
    Run(sql, bound.types, bound.values, bound.nulls, row);
  }

  bool QueryBigintList(const std::string &sql, const std::vector<int64_t> &list,
                       const verus_kit::RowFn &row) override {
    std::vector<Datum> elements;
    elements.reserve(list.size());
    for (const auto key : list)
      elements.push_back(Int64GetDatum(key));
    ArrayType *array = construct_array_builtin(elements.data(), static_cast<int>(elements.size()), INT8OID);
    Run(sql, {INT8ARRAYOID}, {PointerGetDatum(array)}, {' '}, row);
    return true;
  }

  std::vector<int32_t> Attnums(const std::string &table, const std::vector<std::string> &columns) override {
    const Oid relid = RelnameGetRelid(table.c_str());
    if (!OidIsValid(relid))
      throw std::runtime_error("relation " + table + " not found");
    std::vector<int32_t> out;
    for (const auto &column : columns) {
      const AttrNumber attnum = get_attnum(relid, column.c_str());
      if (attnum == InvalidAttrNumber)
        throw std::runtime_error("table " + table + " has no column " + column);
      out.push_back(attnum - 1);
    }
    return out;
  }

  std::string ArrayLiteral(const std::vector<std::string> &items, const std::string &type) const override {
    std::string out = "ARRAY[";
    for (std::size_t i = 0; i < items.size(); ++i)
      out += (i ? "," : "") + items[i];
    return out + "]::" + type + "[]";
  }

private:
  // Rows in chunks of a cursor, so a big scan does not materialize in the SPI context.
  static void Run(const std::string &sql, const std::vector<Oid> &types, const std::vector<Datum> &values,
                  const std::vector<char> &nulls, const verus_kit::RowFn &row) {
    if (!pthread_equal(pthread_self(), g_main_thread))
      throw std::runtime_error("Postgres is called from the main thread only");
    if (SPI_connect() != SPI_OK_CONNECT)
      throw std::runtime_error("SPI_connect failed");
    struct Spi {
      SPIPlanPtr plan = nullptr;
      Portal portal = nullptr;
      ~Spi() {
        if (portal)
          SPI_cursor_close(portal);
        if (plan)
          SPI_freeplan(plan);
        SPI_finish();
      }
    } spi;
    spi.plan = SPI_prepare_cursor(sql.c_str(), static_cast<int>(types.size()), const_cast<Oid *>(types.data()),
                                  CURSOR_OPT_PARALLEL_OK);
    if (!spi.plan)
      throw std::runtime_error("SPI_prepare failed (" + std::string(SPI_result_code_string(SPI_result)) + "): " + sql);
    spi.portal = SPI_cursor_open(nullptr, spi.plan, const_cast<Datum *>(values.data()),
                                 nulls.empty() ? nullptr : nulls.data(), true);
    if (!spi.portal)
      throw std::runtime_error("SPI_cursor_open failed: " + sql);
    core::AuxRow out;
    for (;;) {
      SPI_cursor_fetch(spi.portal, true, 65536);
      if (SPI_processed == 0) {
        if (SPI_tuptable)
          SPI_freetuptable(SPI_tuptable);
        return;
      }
      TupleDesc desc = SPI_tuptable->tupdesc;
      for (uint64 r = 0; r < SPI_processed; ++r) {
        out.values.clear();
        for (int c = 0; c < desc->natts; ++c)
          out.values.push_back(KitCell(SPI_tuptable->vals[r], desc, c));
        row(out);
      }
      SPI_freetuptable(SPI_tuptable);
    }
  }
};

// ---- the kit as an accelerator ----

struct KitConfig {
  const Glue *glue;
};
struct KitScan {};

// The kit's module, built on the SPI host (the Runtime owns its module by value).
class KitModule final : public core::AuxQueryModule {
public:
  KitModule(const KitConfig &config, const KitScan *)
      : inner(verus_kit::MakeModule(*config.glue, SpiVerusHost())) {}
  core::AcceleratorId Id() const override { return inner->Id(); }
  std::vector<core::TableDependency> Dependencies() const override { return inner->Dependencies(); }
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override { return inner->CreateTxnBuffer(); }
  void Build(core::AuxEngineServices &s, core::StateBuilder &b) override { inner->Build(s, b); }
  core::InvalidationPlan InterpretMutation(const core::MutationBatch &m) const override {
    return inner->InterpretMutation(m);
  }
  void AccumulateMutation(const core::MutationBatch &m, core::AuxEngineServices &s, core::AuxTxnBuffer &b) override {
    inner->AccumulateMutation(m, s, b);
  }
  core::StagedMaintenanceOutcome StageMaintenance(core::AuxEngineServices &s, const core::InvalidationPlan &p,
                                                  core::AuxTxnBuffer &b) override {
    return inner->StageMaintenance(s, p, b);
  }
  bool CheckValidity(core::AuxEngineServices &s, const core::QueryArguments &a) const override {
    return inner->CheckValidity(s, a);
  }
  void Execute(core::AuxEngineServices &s, const core::QueryArguments &a, core::AuxRowSink &k) override {
    inner->Execute(s, a, k);
  }
  void ExecuteFallback(core::AuxEngineServices &s, const core::QueryArguments &a, core::AuxRowSink &k) override {
    inner->ExecuteFallback(s, a, k);
  }
  std::vector<core::CalibrationWrite> CalibrationWrites() const override { return inner->CalibrationWrites(); }

private:
  std::unique_ptr<core::AuxQueryModule> inner;
};

using KitRuntime = detail::Runtime<KitModule, KitConfig, KitScan>;

struct KitNames {
  std::string cost_model_env, cost_model_dir, cost_model_stem;
};

std::string Upper(std::string s) {
  for (auto &c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool IsText(Oid type) { return type == TEXTOID || type == VARCHAROID || type == BPCHAROID; }

// The verified result orders text byte-wise: every captured text column must be COLLATE "C".
std::vector<KitRuntime::CollationKey> TextColumns(const Glue &glue) {
  std::vector<KitRuntime::CollationKey> keys;
  for (const auto &table : glue.tables) {
    const Oid relid = RelnameGetRelid(table.name);
    for (const char *column : table.columns)
      if (OidIsValid(relid) && IsText(get_atttype(relid, get_attnum(relid, column))))
        keys.push_back({table.name, column});
  }
  return keys;
}

// The services' store of a member: its view of the verified store (empty: the default store).
runtime::StoreHandle VerusStores(const core::AcceleratorId &id, std::function<runtime::Epoch()> epoch,
                                 std::function<runtime::Epoch()> floor) {
  runtime::StoreHandle handle;
  if (auto verus = aux_state::MakeVerusAuxStore(id, epoch, floor)) {
    auto *store = verus.get();
    handle.install = [store, id](std::vector<core::StateImage> images, runtime::Epoch at) {
      store->Install(std::move(images), id, at);
    };
    handle.store = std::move(verus);
  }
  return handle;
}

KitRuntime &KitAccelerator(const Glue &glue) {
  static std::map<std::string, std::unique_ptr<KitRuntime>> runtimes;
  static std::deque<KitNames> names;
  auto &slot = runtimes[glue.id];
  if (!slot) {
    const std::string query = "q" + std::to_string(glue.query_id);
    auto &n = names.emplace_back();
    n.cost_model_env = "ACCELERATOR_CODEGEN_POSTGRES_" + Upper(std::string(glue.workload) + "_" + query) + "_COST_MODEL";
    n.cost_model_dir = std::string("accelerators/") + glue.workload + "/" + query + "/config";
    n.cost_model_stem = query + "_aux_cost_model";
    std::vector<AcceleratorTable> tables;
    for (std::size_t i = 0; i < glue.tables.size(); ++i)
      tables.push_back({glue.tables[i].name, verus_kit::TableIdOf(i)});
    slot = std::make_unique<KitRuntime>(n.cost_model_env.c_str(),
                                        core::AuxCostModelLocation{n.cost_model_dir.c_str(), n.cost_model_stem.c_str()},
                                        glue.id, KitConfig{&glue}, std::move(tables), TextColumns(glue));
    // A world's kits share one worker, one commit sequence and the world's store.
    if (const char *world = verus_kit::WorldOf(glue.id); world && aux_state::VerusAuxStoreRequested())
      slot->JoinShard(std::string("verus_world:") + world, VerusStores);
    slot->Dependencies(); // resolves the attnums while a transaction can read the catalog
  }
  return *slot;
}

// Accelerators by id, for the shard worker and the capture path.
const struct KitAcceleratorRegistrar {
  KitAcceleratorRegistrar() {
    for (const auto *glue : verus_kit::All())
      RegisterPostgresAccelerator(glue->id, [glue]() -> PostgresAccelerator & { return KitAccelerator(*glue); });
  }
} kRegistrar;

// ---- SQL entry ----

Oid ParamType(Kind kind) {
  switch (kind) {
  case Kind::Int: return INT8OID;
  case Kind::Dec: return NUMERICOID;
  case Kind::Date: return DATEOID;
  case Kind::Bool: return BOOLOID;
  case Kind::Text: break;
  }
  return TEXTOID;
}

core::AuxType ArgType(Kind kind) {
  switch (kind) {
  case Kind::Int: return core::AuxType::Int64;
  case Kind::Dec: return core::AuxType::ScaledDecimal;
  case Kind::Date: return core::AuxType::Date;
  case Kind::Bool: return core::AuxType::Boolean;
  case Kind::Text: break;
  }
  return core::AuxType::String;
}

core::AuxValue Argument(FunctionCallInfo fcinfo, int i, Kind kind) {
  if (PG_ARGISNULL(i))
    return core::AuxValue::Null(ArgType(kind));
  switch (kind) {
  case Kind::Int: return core::AuxValue::Of(core::AuxType::Int64, PG_GETARG_INT64(i));
  case Kind::Date: return core::AuxValue::Of(core::AuxType::Date, detail::PostgresDateArg(i, fcinfo));
  case Kind::Bool: return core::AuxValue::Of(core::AuxType::Boolean, PG_GETARG_BOOL(i));
  case Kind::Dec: {
    if (numeric_is_nan(PG_GETARG_NUMERIC(i)) || numeric_is_inf(PG_GETARG_NUMERIC(i)))
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("a kit's numeric argument must be finite")));
    char *text = DatumGetCString(DirectFunctionCall1(numeric_out, PG_GETARG_DATUM(i)));
    auto cell = detail::NumericCell(text);
    pfree(text);
    const auto &wide = std::get<core::ScaledDecimal128>(cell.payload);
    if (wide.value >= INT64_MIN && wide.value <= INT64_MAX)
      return core::AuxValue::Of(core::AuxType::ScaledDecimal,
                                core::ScaledDecimal{static_cast<int64_t>(wide.value), wide.scale});
    return cell;
  }
  case Kind::Text: break;
  }
  return core::AuxValue::Of(core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(i)));
}

void ExecuteKit(FunctionCallInfo fcinfo) {
  constexpr char kPrefix[] = "accel_";
  const char *name = get_func_name(fcinfo->flinfo->fn_oid);
  const auto *glue = name && std::strncmp(name, kPrefix, sizeof kPrefix - 1) == 0
                         ? verus_kit::Find(name + sizeof kPrefix - 1)
                         : nullptr;
  if (!glue)
    ereport(ERROR, (errmsg("no Verus kit behind function %s", name ? name : "?")));
  core::QueryArguments arguments;
  for (std::size_t i = 0; i < glue->params.size(); ++i)
    arguments.values.push_back(Argument(fcinfo, static_cast<int>(i), glue->params[i]));
  KitAccelerator(*glue).Execute(arguments, fcinfo);
}

// "CREATE FUNCTION accel_<id>(p_1 type, ...) RETURNS TABLE (...)", the result's columns taken from
// the query as the database types it.
std::string FunctionSql(const Glue &glue) {
  std::string query = glue.query_sql;
  while (!query.empty() && (query.back() == ';' || std::isspace(static_cast<unsigned char>(query.back()))))
    query.pop_back();
  std::vector<Oid> types;
  std::vector<char> nulls(glue.params.size(), 'n');
  std::vector<Datum> values(glue.params.size(), 0);
  std::string params;
  for (std::size_t i = 0; i < glue.params.size(); ++i) {
    types.push_back(ParamType(glue.params[i]));
    params += (i ? ", p_" : "p_") + std::to_string(i + 1) + " " + format_type_be(types.back());
  }
  if (SPI_execute_with_args(("SELECT * FROM (" + query + ") AS verus_kit_q LIMIT 0").c_str(),
                            static_cast<int>(types.size()), types.data(), values.data(), nulls.data(), true, 0) !=
      SPI_OK_SELECT)
    throw std::runtime_error(std::string("the query of kit ") + glue.id + " does not prepare");
  std::string columns;
  TupleDesc desc = SPI_tuptable->tupdesc;
  for (int c = 0; c < desc->natts; ++c) {
    const auto *attr = TupleDescAttr(desc, c);
    columns += (c ? ", " : "") + std::string(quote_identifier(NameStr(attr->attname))) + " " + format_type_be(attr->atttypid);
  }
  SPI_freetuptable(SPI_tuptable);
  const std::string function = std::string("accel_") + glue.id;
  return "DROP FUNCTION IF EXISTS " + function + " CASCADE; CREATE FUNCTION " + function + "(" + params +
         ") RETURNS TABLE (" + columns +
         ") AS 'accelerator_codegen_pg', 'AcceleratorCodegenVerusKit' LANGUAGE C;";
}

} // namespace

verus_kit::Host &SpiVerusHost() {
  static SpiHost host;
  return host;
}

int RegisterVerusKitFunctions() {
  int created = 0;
  if (SPI_connect() != SPI_OK_CONNECT)
    ereport(ERROR, (errmsg("SPI_connect failed")));
  for (const auto *glue : verus_kit::All()) {
    const bool present = std::all_of(glue->tables.begin(), glue->tables.end(),
                                     [](const verus_kit::TableDecl &t) { return OidIsValid(RelnameGetRelid(t.name)); });
    if (!present)
      continue;
    std::string sql;
    try {
      sql = FunctionSql(*glue);
    } catch (const std::exception &e) {
      ereport(ERROR, (errmsg("registering kit %s: %s", glue->id, e.what())));
    }
    if (SPI_execute(sql.c_str(), false, 0) < 0)
      ereport(ERROR, (errmsg("registering kit %s failed", glue->id)));
    ++created;
  }
  SPI_finish();
  return created;
}

} // namespace accelerator_codegen::aux_state::engine::postgres

extern "C" {

PG_FUNCTION_INFO_V1(AcceleratorCodegenVerusKit);
Datum AcceleratorCodegenVerusKit(PG_FUNCTION_ARGS) {
  accelerator_codegen::aux_state::engine::postgres::ExecuteKit(fcinfo);
  PG_RETURN_NULL();
}

} // extern "C"

#endif
