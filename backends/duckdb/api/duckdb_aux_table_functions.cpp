// GENERATED BACKEND
#include "backends/duckdb/api/shared_query_engine.hpp"

#include <cctype>
#include <cstdint>
#include <cmath>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/verus_host.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/accelerator/snapshot_override.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

namespace core = accelerator_codegen::aux_state::core;
using duckdb_backend = accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime;

constexpr ::duckdb::idx_t kOutputBatchSize = 2048;

// No shared runtime registered for the accelerator: a private one, built for
// this one query on its own connection. That connection reads at the reading
// transaction's snapshot (its start time), not at the newest commit: two calls
// in one transaction must answer from the same data even when other
// connections commit in between (snapshot isolation).
template <typename Run>
void WithPrivateRuntime(::duckdb::ClientContext &reader, const SharedQueryCatalogEntry &entry, Run &&run) {
  struct StartOverride {
    StartOverride(::duckdb::ClientContext &context_p, ::duckdb::transaction_t start) : context(context_p) {
      if (start) {
        ::duckdb::SetTransactionStartOverride(context, start);
      }
    }
    ~StartOverride() { ::duckdb::ClearTransactionStartOverride(context); }
    ::duckdb::ClientContext &context;
  };
  ::duckdb::Connection connection(*reader.db);
  // Declared before the runtime, so cleared only after it is gone.
  const StartOverride snapshot(*connection.context,
                               static_cast<::duckdb::transaction_t>(
                                   duckdb::DuckDBAuxBackend::SnapshotEpoch(reader)));
  duckdb_backend runtime(connection, entry, 10.0);
  runtime.Build(/*register_active=*/false);
  run(runtime);
}

struct AuxFunctionInfo : public ::duckdb::TableFunctionInfo {
  explicit AuxFunctionInfo(SharedQueryCatalogEntry entry_p)
      : entry(std::move(entry_p)) {}

  SharedQueryCatalogEntry entry;
};

struct AuxBindData : public ::duckdb::FunctionData {
  // `entry_p` lives in the registered function's AuxFunctionInfo (catalog-owned for the database's
  // lifetime), so bind data refers to it instead of copying the entry (vector + std::function).
  AuxBindData(const SharedQueryCatalogEntry &entry_p, ::duckdb::vector<::duckdb::Value> arguments_p,
              ::duckdb::vector<::duckdb::LogicalType> types_p,
              ::duckdb::vector<::duckdb::string> names_p)
      : entry(entry_p), arguments(std::move(arguments_p)), types(std::move(types_p)),
        names(std::move(names_p)) {}

  const SharedQueryCatalogEntry &entry;
  ::duckdb::vector<::duckdb::Value> arguments;
  ::duckdb::vector<::duckdb::LogicalType> types;
  ::duckdb::vector<::duckdb::string> names;

  ::duckdb::unique_ptr<::duckdb::FunctionData> Copy() const override {
    return ::duckdb::make_uniq<AuxBindData>(entry, arguments, types, names);
  }

  bool Equals(const ::duckdb::FunctionData &other_p) const override {
    auto &other = other_p.Cast<AuxBindData>();
    return entry.query_id == other.entry.query_id &&
           entry.workload == other.entry.workload && arguments == other.arguments &&
           types == other.types && names == other.names;
  }
};

struct AuxGlobalState : public ::duckdb::GlobalTableFunctionState {
  ::duckdb::idx_t MaxThreads() const override { return 1; }
};

struct AuxLocalState : public ::duckdb::LocalTableFunctionState {
  // Materialized path (Verus stores, private runtimes): the rows, copied out per chunk.
  std::vector<core::AuxRow> rows;
  ::duckdb::idx_t row_index = 0;
  // Streaming path (C++ stores): the read runs at the first AuxExecute call and writes its rows
  // straight into DataChunks -- the first into the output chunk, the rest into `spill`, which
  // later calls hand over by reference.
  bool streaming = false;
  bool executed = false;
  core::QueryArguments arguments;
  std::vector<::duckdb::unique_ptr<::duckdb::DataChunk>> spill;
  std::size_t spill_index = 0;
};

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

core::AuxValue ToAuxValue(const ::duckdb::Value &value, core::AuxType type) {
  if (value.IsNull()) {
    return core::AuxValue::Null(type);
  }
  switch (type) {
  case core::AuxType::Int64:
    switch (value.type().InternalType()) {
    case ::duckdb::PhysicalType::INT8:
    case ::duckdb::PhysicalType::INT16:
    case ::duckdb::PhysicalType::INT32:
    case ::duckdb::PhysicalType::INT64:
      if (value.type().id() != ::duckdb::LogicalTypeId::DECIMAL) {
        return core::AuxValue::Of(core::AuxType::Int64, value.GetValue<int64_t>());
      }
      break;
    default:
      break;
    }
    return core::AuxValue::Of(
        core::AuxType::Int64,
        static_cast<int64_t>(std::llround(std::stod(value.ToString()))));
  case core::AuxType::Double:
    if (value.type().id() == ::duckdb::LogicalTypeId::DOUBLE) {
      return core::AuxValue::Of(core::AuxType::Double, value.GetValue<double>());
    }
    if (value.type().id() == ::duckdb::LogicalTypeId::DECIMAL) {
      // raw / 10^scale is the correctly rounded quotient, the same double stod() of the text gives
      const auto scale = ::duckdb::DecimalType::GetScale(value.type());
      int64_t raw = 0;
      bool have = true;
      switch (value.type().InternalType()) {
      case ::duckdb::PhysicalType::INT16: raw = value.GetValueUnsafe<int16_t>(); break;
      case ::duckdb::PhysicalType::INT32: raw = value.GetValueUnsafe<int32_t>(); break;
      case ::duckdb::PhysicalType::INT64: raw = value.GetValueUnsafe<int64_t>(); break;
      default: have = false; break;
      }
      static constexpr double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
      if (have && scale <= 15 && raw > -(int64_t(1) << 53) && raw < (int64_t(1) << 53)) {
        return core::AuxValue::Of(core::AuxType::Double, static_cast<double>(raw) / kPow10[scale]);
      }
    }
    return core::AuxValue::Of(core::AuxType::Double, std::stod(value.ToString()));
  case core::AuxType::String:
    return core::AuxValue::Of(core::AuxType::String, value.ToString());
  case core::AuxType::Date: {
    const auto date = value.GetValue<::duckdb::date_t>();
    return core::AuxValue::Of(core::AuxType::Date,
                              core::AuxDate{::duckdb::Date::EpochDays(date)});
  }
  case core::AuxType::ScaledDecimal: {
    // exact: a DECIMAL keeps its scale, an integer gets scale 0, a double the
    // largest scale (4 down to 0) whose DECIMAL(18, s) holds it -- unless it
    // needs more decimals (e.g. TPC-H Q11's fraction 0.0001/SF), then the
    // smallest scale 5..9 that holds it exactly; a value beyond all of them
    // stays a double
    const auto id = value.type().id();
    if (id == ::duckdb::LogicalTypeId::DECIMAL) {
      core::ScaledDecimal decimal;
      decimal.scale = ::duckdb::DecimalType::GetScale(value.type());
      switch (value.type().InternalType()) {
      case ::duckdb::PhysicalType::INT16: decimal.value = value.GetValueUnsafe<int16_t>(); return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
      case ::duckdb::PhysicalType::INT32: decimal.value = value.GetValueUnsafe<int32_t>(); return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
      case ::duckdb::PhysicalType::INT64: decimal.value = value.GetValueUnsafe<int64_t>(); return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
      default: break;
      }
    }
    std::vector<uint8_t> scales;
    if (id == ::duckdb::LogicalTypeId::DECIMAL)
      scales = {::duckdb::DecimalType::GetScale(value.type())};
    else if (value.type().IsIntegral())
      scales = {0};
    else {
      scales = {4, 3, 2, 1, 0};
      // a double with more than 4 decimals: DECIMAL(18, 4) would round it
      const double d = value.GetValue<double>();
      static constexpr double kTen[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9};
      const auto at = [](double x, int s) { return std::round(x * kTen[s]) / kTen[s]; };
      if (std::isfinite(d) && std::fabs(d) < 1e8 && at(d, 4) != d) {
        for (uint8_t s = 5; s <= 9; ++s) {
          if (at(d, s) == d) {
            scales = {s};
            break;
          }
        }
      }
    }
    for (const auto scale : scales) {
      try {
        const auto exact = value.DefaultCastAs(::duckdb::LogicalType::DECIMAL(18, scale));
        core::ScaledDecimal decimal;
        decimal.value = exact.GetValueUnsafe<int64_t>();
        decimal.scale = scale;
        return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
      } catch (const std::exception &) {
        // too wide at this scale: try the next
      }
    }
    return core::AuxValue::Of(core::AuxType::Double, std::stod(value.ToString()));
  }
  default:
    return core::AuxValue::Of(core::AuxType::String, value.ToString());
  }
}

core::QueryArguments BuildArguments(const SharedQueryCatalogEntry &entry,
                                    const ::duckdb::vector<::duckdb::Value> &inputs) {
  core::QueryArguments arguments;
  arguments.values.reserve(inputs.size());
  for (::duckdb::idx_t index = 0; index < inputs.size(); ++index) {
    core::AuxType type = core::AuxType::String;
    if (index < entry.benchmark_arguments.values.size()) {
      type = entry.benchmark_arguments.values[index].type;
    }
    arguments.values.push_back(ToAuxValue(inputs[index], type));
  }
  return arguments;
}

// `sql` with each $n replaced by the n-th value as a typed SQL literal (highest
// n first, so $1 never matches inside $10).
std::string WithLiterals(const std::string &sql, const ::duckdb::vector<::duckdb::Value> &values) {
  std::string out = sql;
  for (std::size_t n = values.size(); n >= 1; --n) {
    const std::string marker = "$" + std::to_string(n);
    const std::string literal = "(" + values[n - 1].ToSQLString() + ")";
    for (std::size_t at = out.find(marker); at != std::string::npos; at = out.find(marker, at + literal.size())) {
      const std::size_t after = at + marker.size();
      if (after < out.size() && std::isdigit(static_cast<unsigned char>(out[after]))) {
        at += marker.size() - literal.size(); // part of a longer $nn: skip
        continue;
      }
      out.replace(at, marker.size(), literal);
    }
  }
  return out;
}

::duckdb::Value AuxValueToDuckdb(const core::AuxValue &value) {
  if (value.is_null) {
    return ::duckdb::Value();
  }
  return std::visit(
      [](const auto &payload) -> ::duckdb::Value {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, int64_t>) {
          return ::duckdb::Value::BIGINT(payload);
        } else if constexpr (std::is_same_v<Payload, uint64_t>) {
          return ::duckdb::Value::UBIGINT(payload);
        } else if constexpr (std::is_same_v<Payload, double>) {
          return ::duckdb::Value::DOUBLE(payload);
        } else if constexpr (std::is_same_v<Payload, core::ScaledDecimal>) {
          return ::duckdb::Value::DECIMAL(payload.value, 18, payload.scale); // any int64 payload
        } else if constexpr (std::is_same_v<Payload, core::AuxDate>) {
          ::duckdb::date_t date{};
          date.days = payload.days_since_epoch;
          return ::duckdb::Value::DATE(date);
        } else {
          return ::duckdb::Value(payload);
        }
      },
      value.payload);
}

struct SchemaCache {
  ::duckdb::vector<::duckdb::LogicalType> types;
  ::duckdb::vector<::duckdb::string> names;
};
std::shared_mutex &SchemaMutex() {
  static std::shared_mutex m;
  return m;
}
std::unordered_map<std::string, SchemaCache> &SchemaMap() {
  static std::unordered_map<std::string, SchemaCache> m;
  return m;
}

// The result schema of an accelerator's base query, without running it over the data.
void InferAuxSchema(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                    const ::duckdb::vector<::duckdb::Value> &inputs,
                    ::duckdb::vector<::duckdb::LogicalType> &return_types,
                    ::duckdb::vector<::duckdb::string> &names) {
  if (std::string(entry.base_sql).find('$') != std::string::npos) {
    // Parameterized SQL (an entry built from a Verus kit glue): the schema from
    // running it with this call's values as literals, returning no rows. (Not by
    // preparing it with unbound parameters: a parameter whose type depends on its
    // value, e.g. `$2 - 0.01`, fails that and leaves this statement unbindable.)
    auto probe = connection.Query("SELECT * FROM (" + WithLiterals(entry.base_sql, inputs) +
                                  ") AS accel_schema LIMIT 0");
    Require(probe != nullptr && !probe->HasError(),
            probe ? probe->GetError() : "failed to infer aux schema");
    return_types = probe->types;
    names = probe->names;
  } else {
    // The schema from planning the query, without running it (running it costs a full
    // base query on the accelerator's first read).
    auto prepared = connection.Prepare(entry.base_sql);
    Require(prepared != nullptr && !prepared->HasError(),
            prepared ? prepared->GetError() : "failed to infer aux schema");
    return_types = prepared->GetTypes();
    names = prepared->GetNames();
  }
}

::duckdb::unique_ptr<::duckdb::FunctionData>
AuxBind(::duckdb::ClientContext &context, ::duckdb::TableFunctionBindInput &input,
        ::duckdb::vector<::duckdb::LogicalType> &return_types,
        ::duckdb::vector<::duckdb::string> &names) {
  const auto &entry = input.info->Cast<AuxFunctionInfo>().entry;

  // Cache schema per accelerator so repeated SELECT * FROM accel_*_aux(...)
  // does not re-run base_sql solely to infer column types.
  auto &schema_mutex = SchemaMutex();
  auto &schemas = SchemaMap();
  {
    std::shared_lock<std::shared_mutex> lock(schema_mutex);
    const auto it = schemas.find(entry.accelerator_id ? entry.accelerator_id : "");
    if (it != schemas.end()) {
      return_types = it->second.types;
      names = it->second.names;
      return ::duckdb::make_uniq<AuxBindData>(entry, input.inputs, return_types,
                                              names);
    }
  }

  ::duckdb::Connection connection(*context.db);
  InferAuxSchema(connection, entry, input.inputs, return_types, names);
  {
    std::unique_lock<std::shared_mutex> lock(schema_mutex);
    schemas[entry.accelerator_id ? entry.accelerator_id : ""] =
        SchemaCache{return_types, names};
  }
  return ::duckdb::make_uniq<AuxBindData>(entry, input.inputs, return_types, names);
}

::duckdb::unique_ptr<::duckdb::GlobalTableFunctionState>
AuxInitGlobal(::duckdb::ClientContext &, ::duckdb::TableFunctionInitInput &) {
  return ::duckdb::make_uniq<AuxGlobalState>();
}

// One cell straight into the output vector (no per-cell Value / heap string object): the common
// type pairs; anything else (e.g. a decimal at another scale) goes through a Value cast.
void WriteCell(::duckdb::Vector &vec, ::duckdb::idx_t i, const ::duckdb::LogicalType &type, const core::AuxValue &v) {
  using ::duckdb::LogicalTypeId;
  if (v.is_null) {
    ::duckdb::FlatVector::SetNull(vec, i, true);
    return;
  }
  const auto *s = std::get_if<std::string>(&v.payload);
  const auto *i64 = std::get_if<int64_t>(&v.payload);
  const auto *dec = std::get_if<core::ScaledDecimal>(&v.payload);
  const auto *dbl = std::get_if<double>(&v.payload);
  const auto *day = std::get_if<core::AuxDate>(&v.payload);
  switch (type.id()) {
  case LogicalTypeId::VARCHAR:
    if (s) {
      ::duckdb::FlatVector::GetData<::duckdb::string_t>(vec)[i] = ::duckdb::StringVector::AddString(vec, *s);
      return;
    }
    break;
  case LogicalTypeId::BIGINT:
    if (i64) {
      ::duckdb::FlatVector::GetData<int64_t>(vec)[i] = *i64;
      return;
    }
    break;
  case LogicalTypeId::INTEGER:
    if (i64 && *i64 >= INT32_MIN && *i64 <= INT32_MAX) {
      ::duckdb::FlatVector::GetData<int32_t>(vec)[i] = static_cast<int32_t>(*i64);
      return;
    }
    break;
  case LogicalTypeId::DOUBLE:
    if (dbl) {
      ::duckdb::FlatVector::GetData<double>(vec)[i] = *dbl;
      return;
    }
    break;
  case LogicalTypeId::DATE:
    if (day) {
      ::duckdb::FlatVector::GetData<::duckdb::date_t>(vec)[i] = ::duckdb::date_t(day->days_since_epoch);
      return;
    }
    break;
  case LogicalTypeId::DECIMAL:
    if (dec && dec->scale == ::duckdb::DecimalType::GetScale(type)) {
      switch (type.InternalType()) {
      case ::duckdb::PhysicalType::INT64:
        ::duckdb::FlatVector::GetData<int64_t>(vec)[i] = dec->value;
        return;
      case ::duckdb::PhysicalType::INT128:
        ::duckdb::FlatVector::GetData<::duckdb::hugeint_t>(vec)[i] = ::duckdb::hugeint_t(dec->value);
        return;
      default:
        break;
      }
    }
    break;
  default:
    break;
  }
  vec.SetValue(i, AuxValueToDuckdb(v)); // the general path: DuckDB casts the value to the column's type
}


// Writes each row the module emits straight into DataChunk columns: the first kOutputBatchSize
// rows into `first`, the rest into freshly allocated spill chunks (no AuxRow is retained).
struct ChunkSink final : core::AuxRowSink {
  ChunkSink(::duckdb::ClientContext &context_p, const ::duckdb::vector<::duckdb::LogicalType> &types_p,
            ::duckdb::DataChunk &first_p, std::vector<::duckdb::unique_ptr<::duckdb::DataChunk>> &spill_p)
      : context(context_p), types(types_p), current(&first_p), spill(spill_p) {}

  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override {
    if (count == kOutputBatchSize) {
      current->SetCardinality(count);
      auto chunk = ::duckdb::make_uniq<::duckdb::DataChunk>();
      chunk->Initialize(::duckdb::Allocator::Get(context), types);
      current = chunk.get();
      spill.push_back(std::move(chunk));
      count = 0;
    }
    const auto columns = types.size();
    const auto have = row.values.size();
    for (::duckdb::idx_t column = 0; column < columns; ++column) {
      if (column < have)
        WriteCell(current->data[column], count, types[column], row.values[column]);
      else
        ::duckdb::FlatVector::SetNull(current->data[column], count, true);
    }
    ++count;
  }
  void End() override {}
  void Finish() { current->SetCardinality(count); }

  ::duckdb::ClientContext &context;
  const ::duckdb::vector<::duckdb::LogicalType> &types;
  ::duckdb::DataChunk *current;
  std::vector<::duckdb::unique_ptr<::duckdb::DataChunk>> &spill;
  ::duckdb::idx_t count = 0;
};

void StreamRead(::duckdb::ClientContext &context, const AuxBindData &bind_data, AuxLocalState &local_state,
                ::duckdb::DataChunk &output) {
  ChunkSink sink(context, bind_data.types, output, local_state.spill);
  const std::string accelerator_id = bind_data.entry.accelerator_id ? bind_data.entry.accelerator_id : "";
  if (auto *active = duckdb_backend::FindActive(*context.db, accelerator_id)) {
    // Read at this query's snapshot, on this query's context.
    ::accelerator_codegen::aux_state::engine::duckdb::DuckDBAuxBackend::ReaderScope reader(context);
    active->ExecuteInto(local_state.arguments, sink);
  } else {
    WithPrivateRuntime(context, bind_data.entry,
                       [&](duckdb_backend &runtime) { runtime.ExecuteInto(local_state.arguments, sink); });
  }
  sink.Finish();
}

::duckdb::unique_ptr<::duckdb::LocalTableFunctionState>
AuxInitLocal(::duckdb::ExecutionContext &context, ::duckdb::TableFunctionInitInput &input,
             ::duckdb::GlobalTableFunctionState *) {
  auto &bind_data = input.bind_data->Cast<AuxBindData>();
  auto local_state = ::duckdb::make_uniq<AuxLocalState>();
  const auto arguments =
      BuildArguments(bind_data.entry, bind_data.arguments);
  const std::string accelerator_id = bind_data.entry.accelerator_id ? bind_data.entry.accelerator_id : "";
  if (auto *active = duckdb_backend::FindActive(*context.client.db, accelerator_id)) {
    if (!active->UsesVerusStore() && !duckdb::IsVerusKitAccelerator(accelerator_id)) {
      // C++ store: the read itself runs at the first AuxExecute call, streaming into chunks.
      local_state->streaming = true;
      local_state->arguments = arguments;
      return local_state;
    }
    // Read at this query's snapshot, on this query's context.  (Streaming
    // Verus reads too was measured 2026-10-05: no difference for Q16's ~7.5K
    // rows at SF1 -- the per-row cost is the AuxRow conversion and WriteCell,
    // which both paths pay.)
    ::accelerator_codegen::aux_state::engine::duckdb::DuckDBAuxBackend::ReaderScope
        reader(context.client);
    local_state->rows = active->ExecuteCollect(arguments);
    return local_state;
  }
  // No shared runtime: build a private one for this query only.
  WithPrivateRuntime(context.client, bind_data.entry,
                     [&](duckdb_backend &runtime) { local_state->rows = runtime.ExecuteCollect(arguments); });
  return local_state;
}

void AuxExecute(::duckdb::ClientContext &context, ::duckdb::TableFunctionInput &input,
                ::duckdb::DataChunk &output) {
  auto &local_state = input.local_state->Cast<AuxLocalState>();
  auto &bind_data = input.bind_data->Cast<AuxBindData>();
  if (local_state.streaming) {
    if (!local_state.executed) {
      local_state.executed = true;
      StreamRead(context, bind_data, local_state, output); // output holds the first rows
    } else if (local_state.spill_index < local_state.spill.size()) {
      auto &chunk = *local_state.spill[local_state.spill_index++];
      output.Reference(chunk);
    } else {
      output.SetCardinality(0);
    }
    return;
  }
  ::duckdb::idx_t count = 0;
  const auto columns = bind_data.types.size();
  while (count < kOutputBatchSize && local_state.row_index < local_state.rows.size()) {
    const auto &row = local_state.rows[local_state.row_index++];
    for (::duckdb::idx_t column = 0; column < columns; ++column) {
      if (column < row.values.size())
        WriteCell(output.data[column], count, bind_data.types[column], row.values[column]);
      else
        ::duckdb::FlatVector::SetNull(output.data[column], count, true);
    }
    ++count;
  }
  output.SetCardinality(count);
}

// One table function name; one TableFunction per signature (overloads share it).
void RegisterAuxFunction(::duckdb::Catalog &catalog, ::duckdb::ClientContext &context,
                         const SharedQueryCatalogEntry &entry, const std::string &function_name,
                         std::vector<::duckdb::vector<::duckdb::LogicalType>> signatures) {
  ::duckdb::TableFunctionSet set(function_name);
  for (auto &signature : signatures) {
    ::duckdb::TableFunction function(function_name, std::move(signature), AuxExecute, AuxBind,
                                     AuxInitGlobal, AuxInitLocal);
    function.function_info = ::duckdb::make_shared_ptr<AuxFunctionInfo>(entry);
    set.AddFunction(std::move(function));
  }
  ::duckdb::CreateTableFunctionInfo info(std::move(set));
  info.on_conflict = ::duckdb::OnCreateConflict::IGNORE_ON_CONFLICT;
  catalog.CreateTableFunction(context, info);
}

// Every registered aux query (aux_query_registry.hpp), then the Verus
// accelerators registered through the host kit (generated glue).
void RegisterRegisteredAuxFunctions(::duckdb::Catalog &catalog,
                                    ::duckdb::ClientContext &context) {
  for (const auto *query : duckdb::AuxQueries()) {
    const auto *entry = FindSharedQueryByAccelerator(query->accelerator_id);
    Require(entry != nullptr, "aux query " + query->accelerator_id + " has no catalog entry");
    std::vector<::duckdb::vector<::duckdb::LogicalType>> signatures;
    signatures.emplace_back(query->argument_types.begin(), query->argument_types.end());
    for (const auto &overload : query->overload_argument_types)
      signatures.emplace_back(overload.begin(), overload.end());
    RegisterAuxFunction(catalog, context, *entry, query->table_function, std::move(signatures));
  }
  // Verus accelerators registered through the host kit (generated glue).
  for (auto &kit : duckdb::VerusKitTableFunctions()) {
    std::vector<::duckdb::vector<::duckdb::LogicalType>> signatures;
    signatures.push_back(std::move(kit.argument_types));
    RegisterAuxFunction(catalog, context, *kit.entry, kit.name, std::move(signatures));
  }
}


} // namespace

core::QueryArguments AuxArgumentsFromValues(const SharedQueryCatalogEntry &entry,
                                            const std::vector<::duckdb::Value> &values) {
  return BuildArguments(entry, ::duckdb::vector<::duckdb::Value>(values.begin(), values.end()));
}

void PrimeAuxSchema(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry) {
  const std::string id = entry.accelerator_id ? entry.accelerator_id : "";
  {
    std::shared_lock<std::shared_mutex> lock(SchemaMutex());
    if (SchemaMap().count(id))
      return;
  }
  try {
    ::duckdb::vector<::duckdb::Value> inputs;
    for (const auto &v : entry.benchmark_arguments.values)
      inputs.push_back(AuxValueToDuckdb(v));
    ::duckdb::vector<::duckdb::LogicalType> types;
    ::duckdb::vector<::duckdb::string> names;
    InferAuxSchema(connection, entry, inputs, types, names);
    std::unique_lock<std::shared_mutex> lock(SchemaMutex());
    SchemaMap()[id] = SchemaCache{std::move(types), std::move(names)};
  } catch (...) {
    // left to the first bind
  }
}

void RegisterDuckdbAuxTableFunctions(::duckdb::Connection &connection) {
  auto &context = *connection.context;
  auto &catalog = ::duckdb::Catalog::GetSystemCatalog(context);
  const bool started_transaction = !connection.HasActiveTransaction();
  if (started_transaction) {
    connection.BeginTransaction();
  }
  RegisterRegisteredAuxFunctions(catalog, context);
  if (started_transaction) {
    connection.Commit();
  }
}

} // namespace accelerator_codegen::aux_state::engine
