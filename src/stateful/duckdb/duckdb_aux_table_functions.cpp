// GENERATED BACKEND
#include "accelerator_codegen/runtime/catalog/shared_query_engine.hpp"

#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/aux_query_registry.hpp"
#include "accelerator_codegen/verus/verus_kit.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

namespace core = accelerator_codegen::aux_state::core;
using duckdb_backend = accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime;

constexpr ::duckdb::idx_t kOutputBatchSize = 2048;

struct AuxFunctionInfo : public ::duckdb::TableFunctionInfo {
  explicit AuxFunctionInfo(SharedQueryCatalogEntry entry_p)
      : entry(std::move(entry_p)) {}

  SharedQueryCatalogEntry entry;
};

struct AuxBindData : public ::duckdb::FunctionData {
  AuxBindData(SharedQueryCatalogEntry entry_p, ::duckdb::vector<::duckdb::Value> arguments_p,
              ::duckdb::vector<::duckdb::LogicalType> types_p,
              ::duckdb::vector<::duckdb::string> names_p)
      : entry(std::move(entry_p)), arguments(std::move(arguments_p)), types(std::move(types_p)),
        names(std::move(names_p)) {}

  SharedQueryCatalogEntry entry;
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
  std::vector<core::AuxRow> rows;
  ::duckdb::idx_t row_index = 0;
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
    return core::AuxValue::Of(
        core::AuxType::Int64,
        static_cast<int64_t>(std::llround(std::stod(value.ToString()))));
  case core::AuxType::Double:
    return core::AuxValue::Of(core::AuxType::Double, std::stod(value.ToString()));
  case core::AuxType::String:
    return core::AuxValue::Of(core::AuxType::String, value.ToString());
  case core::AuxType::Date: {
    const auto date = value.GetValue<::duckdb::date_t>();
    return core::AuxValue::Of(core::AuxType::Date,
                              core::AuxDate{::duckdb::Date::EpochDays(date)});
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
          return ::duckdb::Value::DECIMAL(payload.value, 15, payload.scale);
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

::duckdb::unique_ptr<::duckdb::FunctionData>
AuxBind(::duckdb::ClientContext &context, ::duckdb::TableFunctionBindInput &input,
        ::duckdb::vector<::duckdb::LogicalType> &return_types,
        ::duckdb::vector<::duckdb::string> &names) {
  const auto &entry = input.info->Cast<AuxFunctionInfo>().entry;

  // Cache schema per accelerator so repeated SELECT * FROM accel_*_aux(...)
  // does not re-run base_sql solely to infer column types.
  struct SchemaCache {
    ::duckdb::vector<::duckdb::LogicalType> types;
    ::duckdb::vector<::duckdb::string> names;
  };
  static std::mutex schema_mutex;
  static std::unordered_map<std::string, SchemaCache> schemas;
  {
    std::lock_guard<std::mutex> lock(schema_mutex);
    const auto it = schemas.find(entry.accelerator_id ? entry.accelerator_id : "");
    if (it != schemas.end()) {
      return_types = it->second.types;
      names = it->second.names;
      return ::duckdb::make_uniq<AuxBindData>(entry, input.inputs, return_types,
                                              names);
    }
  }

  ::duckdb::Connection connection(*context.db);
  if (std::string(entry.base_sql).find('$') != std::string::npos) {
    // Parameterized SQL (an entry built from a Verus kit glue): the schema
    // from preparing it, without running it.
    auto prepared = connection.Prepare(entry.base_sql);
    Require(prepared != nullptr && !prepared->HasError(),
            prepared ? prepared->GetError() : "failed to infer aux schema");
    return_types = prepared->GetTypes();
    names = prepared->GetNames();
  } else {
    auto base_result = connection.Query(entry.base_sql);
    Require(base_result != nullptr && !base_result->HasError(),
            base_result ? base_result->GetError() : "failed to infer aux schema");
    auto &base = base_result->Cast<::duckdb::MaterializedQueryResult>();
    return_types.reserve(base.ColumnCount());
    names.reserve(base.ColumnCount());
    for (::duckdb::idx_t column = 0; column < base.ColumnCount(); ++column) {
      return_types.push_back(base.types[column]);
      names.push_back(base.names[column]);
    }
  }
  {
    std::lock_guard<std::mutex> lock(schema_mutex);
    schemas[entry.accelerator_id ? entry.accelerator_id : ""] =
        SchemaCache{return_types, names};
  }
  return ::duckdb::make_uniq<AuxBindData>(entry, input.inputs, return_types, names);
}

::duckdb::unique_ptr<::duckdb::GlobalTableFunctionState>
AuxInitGlobal(::duckdb::ClientContext &, ::duckdb::TableFunctionInitInput &) {
  return ::duckdb::make_uniq<AuxGlobalState>();
}

::duckdb::unique_ptr<::duckdb::LocalTableFunctionState>
AuxInitLocal(::duckdb::ExecutionContext &context, ::duckdb::TableFunctionInitInput &input,
             ::duckdb::GlobalTableFunctionState *) {
  auto &bind_data = input.bind_data->Cast<AuxBindData>();
  auto local_state = ::duckdb::make_uniq<AuxLocalState>();
  const auto arguments =
      BuildArguments(bind_data.entry, bind_data.arguments);
  if (auto *active = duckdb_backend::FindActive(
          *context.client.db, bind_data.entry.accelerator_id
                                  ? bind_data.entry.accelerator_id
                                  : "")) {
    // Read at this query's snapshot, on this query's context.
    ::accelerator_codegen::aux_state::engine::duckdb::DuckDBAuxBackend::ReaderScope
        reader(context.client);
    local_state->rows = active->ExecuteCollect(arguments);
    return local_state;
  }
  ::duckdb::Connection connection(*context.client.db);
  // No shared runtime: build a private one for this query only.
  duckdb_backend runtime(connection, bind_data.entry, 10.0);
  runtime.Build(/*register_active=*/false);
  local_state->rows = runtime.ExecuteCollect(arguments);
  return local_state;
}

void AuxExecute(::duckdb::ClientContext &, ::duckdb::TableFunctionInput &input,
                ::duckdb::DataChunk &output) {
  auto &local_state = input.local_state->Cast<AuxLocalState>();
  auto &bind_data = input.bind_data->Cast<AuxBindData>();
  ::duckdb::idx_t count = 0;
  while (count < kOutputBatchSize && local_state.row_index < local_state.rows.size()) {
    const auto &row = local_state.rows[local_state.row_index++];
    for (::duckdb::idx_t column = 0; column < bind_data.types.size(); ++column) {
      const auto value =
          column < row.values.size()
              ? AuxValueToDuckdb(row.values[column])
              : ::duckdb::Value();
      output.data[column].SetValue(count, value);
    }
    ++count;
  }
  output.SetCardinality(count);
}

void RegisterAuxFunction(::duckdb::Catalog &catalog, ::duckdb::ClientContext &context,
                         const SharedQueryCatalogEntry &entry, const char *function_name,
                         ::duckdb::vector<::duckdb::LogicalType> argument_types) {
  ::duckdb::TableFunction function(function_name, argument_types, AuxExecute, AuxBind,
                                   AuxInitGlobal, AuxInitLocal);
  function.function_info = ::duckdb::make_shared_ptr<AuxFunctionInfo>(entry);
  ::duckdb::CreateTableFunctionInfo info(std::move(function));
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
    ::duckdb::vector<::duckdb::LogicalType> types(query->argument_types.begin(),
                                                 query->argument_types.end());
    RegisterAuxFunction(catalog, context, *entry, query->table_function.c_str(), std::move(types));
  }
  // Verus accelerators registered through the host kit (generated glue).
  for (const auto *glue : verus_kit::All()) {
    const auto *kit_entry = VerusKitCatalogEntry(glue->id);
    if (!kit_entry) {
      continue;
    }
    ::duckdb::vector<::duckdb::LogicalType> types;
    for (const auto kind : glue->params) {
      switch (kind) {
      case verus_kit::Kind::Text:
        types.push_back(::duckdb::LogicalType::VARCHAR);
        break;
      case verus_kit::Kind::Date:
        types.push_back(::duckdb::LogicalType::DATE);
        break;
      case verus_kit::Kind::Dec:
        types.push_back(::duckdb::LogicalType::DOUBLE);
        break;
      case verus_kit::Kind::Bool:
        types.push_back(::duckdb::LogicalType::BOOLEAN);
        break;
      case verus_kit::Kind::Int:
        types.push_back(::duckdb::LogicalType::BIGINT);
        break;
      }
    }
    const std::string name = std::string("accel_") + glue->id;
    RegisterAuxFunction(catalog, context, *kit_entry, name.c_str(), std::move(types));
  }
}


} // namespace

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

#ifdef ACCEL_ENABLE_PARALLEL_AUX
void RegisterDuckdbAuxParallelTableFunctions(::duckdb::Connection &connection) {
  RegisterDuckdbAuxTableFunctions(connection);
}
#endif

} // namespace accelerator_codegen::aux_state::engine
