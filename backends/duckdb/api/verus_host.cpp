// GENERATED BACKEND
#include "backends/duckdb/api/verus_host.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "backends/duckdb/engine/capture.hpp"
#include "backends/duckdb/engine/decimal128.hpp"
#include "backends/duckdb/engine/scan.hpp"
#include "substrate/verus/kit/verus_kit.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

using namespace scan;

// ---- Verus accelerators registered through the host kit (verus_kit.hpp) ----

// One column of a DataChunk, read in place (see DuckVerusHost::Query).
struct ChunkColumn {
  enum class Kind { Bool, Int8, Int16, Int32, Int64, Date, Float, Double, Dec16, Dec32, Dec64, Dec128, Varchar, Other };
  Kind kind = Kind::Other;
  int32_t scale = 0;
  ::duckdb::UnifiedVectorFormat uvf;

  void Init(::duckdb::DataChunk &chunk, ::duckdb::idx_t col) {
    using ::duckdb::LogicalTypeId;
    using ::duckdb::PhysicalType;
    auto &vec = chunk.data[col];
    const auto &type = vec.GetType();
    kind = Kind::Other;
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: kind = Kind::Bool; break;
    case LogicalTypeId::TINYINT: kind = Kind::Int8; break;
    case LogicalTypeId::SMALLINT: kind = Kind::Int16; break;
    case LogicalTypeId::INTEGER: kind = Kind::Int32; break;
    case LogicalTypeId::BIGINT: kind = Kind::Int64; break;
    case LogicalTypeId::DATE: kind = Kind::Date; break;
    case LogicalTypeId::FLOAT: kind = Kind::Float; break;
    case LogicalTypeId::DOUBLE: kind = Kind::Double; break;
    case LogicalTypeId::VARCHAR: kind = Kind::Varchar; break;
    case LogicalTypeId::DECIMAL:
      scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(type));
      switch (type.InternalType()) {
      case PhysicalType::INT16: kind = Kind::Dec16; break;
      case PhysicalType::INT32: kind = Kind::Dec32; break;
      case PhysicalType::INT64: kind = Kind::Dec64; break;
      case PhysicalType::INT128: kind = Kind::Dec128; break;
      default: break;
      }
      break;
    default: break;
    }
    if (kind != Kind::Other)
      vec.ToUnifiedFormat(chunk.size(), uvf);
  }

  void Get(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t col, ::duckdb::idx_t r, core::AuxValue &out) const {
    using core::AuxType;
    const auto i = uvf.sel->get_index(r);
    const bool valid = kind == Kind::Other ? true : uvf.validity.RowIsValid(i);
    const auto set = [&](AuxType t, auto v) {
      out.type = t;
      out.is_null = false;
      out.payload = std::move(v);
    };
    const auto null = [&](AuxType t) {
      out.type = t;
      out.is_null = true;
      out.payload = std::string();
    };
    switch (kind) {
    case Kind::Bool:
      return valid ? set(AuxType::Boolean, static_cast<bool>(::duckdb::UnifiedVectorFormat::GetData<bool>(uvf)[i]))
                   : null(AuxType::Boolean);
    case Kind::Int8:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int8_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Int16:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int16_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Int32:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int32_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Int64:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int64_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Date:
      return valid ? set(AuxType::Date, core::AuxDate{::duckdb::UnifiedVectorFormat::GetData<::duckdb::date_t>(uvf)[i].days})
                   : null(AuxType::Date);
    case Kind::Float:
      return valid ? set(AuxType::Double, static_cast<double>(::duckdb::UnifiedVectorFormat::GetData<float>(uvf)[i]))
                   : null(AuxType::Double);
    case Kind::Double:
      return valid ? set(AuxType::Double, ::duckdb::UnifiedVectorFormat::GetData<double>(uvf)[i])
                   : null(AuxType::Double);
    case Kind::Dec16:
      return valid ? set(AuxType::ScaledDecimal, core::ScaledDecimal{::duckdb::UnifiedVectorFormat::GetData<int16_t>(uvf)[i], scale})
                   : null(AuxType::ScaledDecimal);
    case Kind::Dec32:
      return valid ? set(AuxType::ScaledDecimal, core::ScaledDecimal{::duckdb::UnifiedVectorFormat::GetData<int32_t>(uvf)[i], scale})
                   : null(AuxType::ScaledDecimal);
    case Kind::Dec64:
      return valid ? set(AuxType::ScaledDecimal, core::ScaledDecimal{::duckdb::UnifiedVectorFormat::GetData<int64_t>(uvf)[i], scale})
                   : null(AuxType::ScaledDecimal);
    case Kind::Dec128: {
      if (!valid)
        return null(AuxType::ScaledDecimal);
      const auto huge = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::hugeint_t>(uvf)[i];
      if (!HugeintFitsInt64(huge)) // beyond 64 bits: the exact 128-bit cell
        out = Decimal128Cell(huge, scale);
      else
        set(AuxType::ScaledDecimal, core::ScaledDecimal{static_cast<int64_t>(huge.lower), scale});
      return;
    }
    case Kind::Varchar: {
      if (!valid)
        return null(AuxType::String);
      const auto &str = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(uvf)[i];
      return set(AuxType::String, std::string(str.GetData(), str.GetSize()));
    }
    case Kind::Other:
      out = CapturedValue(chunk.GetValue(col, r));
      return;
    }
  }
};

// The kit's engine services on a runtime's connection: SQL as the
// connection's transaction sees it (a writer's own writes included).
struct DuckVerusHost final : verus_kit::Host {
  explicit DuckVerusHost(::duckdb::Connection &connection_p) : connection(connection_p) {}

  // Chunk at a time: each column is read through its UnifiedVectorFormat
  // (flat, constant or dictionary), no Value boxing per cell. The produced
  // AuxValues are exactly CapturedValue(chunk.GetValue(c, r)).
  static void DeliverChunks(::duckdb::QueryResult &result, const verus_kit::RowFn &row) {
    Require(!result.HasError(), result.GetError());
    std::vector<ChunkColumn> columns;
    while (true) {
      auto chunk = result.Fetch();
      if (!chunk || chunk->size() == 0)
        break;
      const auto n = chunk->size();
      columns.resize(chunk->ColumnCount());
      for (::duckdb::idx_t c = 0; c < columns.size(); ++c)
        columns[c].Init(*chunk, c);
      core::AuxRow out;
      for (::duckdb::idx_t r = 0; r < n; ++r) {
        out.values.resize(columns.size());
        for (::duckdb::idx_t c = 0; c < columns.size(); ++c)
          columns[c].Get(*chunk, c, r, out.values[c]);
        row(out);
      }
    }
    Require(!result.HasError(), result.GetError());
  }

  void Query(const std::string &sql, const verus_kit::RowFn &row) override {
    auto lock = LockScan(connection);
    auto result = ScanConnection(connection).SendQuery(sql);
    Require(result != nullptr, "DuckDB scan returned null");
    DeliverChunks(*result, row);
  }

  bool QueryBigintList(const std::string &sql, const std::vector<int64_t> &list,
                       const verus_kit::RowFn &row) override {
    auto lock = LockScan(connection);
    auto stmt = ScanConnection(connection).Prepare(sql);
    Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed: " + sql);
    ::duckdb::vector<::duckdb::Value> items;
    items.reserve(list.size());
    for (auto v : list)
      items.push_back(::duckdb::Value::BIGINT(v));
    ::duckdb::vector<::duckdb::Value> values;
    values.push_back(::duckdb::Value::LIST(::duckdb::LogicalType::BIGINT, std::move(items)));
    auto result = stmt->Execute(values, /*allow_stream_result=*/true);
    Require(result != nullptr, "query failed: " + sql);
    DeliverChunks(*result, row);
    return true;
  }

  void QueryParams(const std::string &sql, const std::vector<core::AuxValue> &params,
                   const verus_kit::RowFn &row) override {
    auto scan_lock = LockScan(connection);
    auto stmt = ScanConnection(connection).Prepare(sql);
    Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed: " + sql);
    ::duckdb::vector<::duckdb::Value> values;
    for (const auto &p : params) {
      if (p.is_null) {
        values.emplace_back();
        continue;
      }
      values.push_back(std::visit(
          [](const auto &payload) -> ::duckdb::Value {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, int64_t>)
              return ::duckdb::Value::BIGINT(payload);
            else if constexpr (std::is_same_v<T, uint64_t>)
              return ::duckdb::Value::UBIGINT(payload);
            else if constexpr (std::is_same_v<T, double>)
              return ::duckdb::Value::DOUBLE(payload);
            else if constexpr (std::is_same_v<T, bool>)
              return ::duckdb::Value::BOOLEAN(payload);
            else if constexpr (std::is_same_v<T, core::ScaledDecimal>)
              return ::duckdb::Value::DECIMAL(payload.value, 18, payload.scale);
            else if constexpr (std::is_same_v<T, core::ScaledDecimal128>)
              return Decimal128ToValue(payload);
            else if constexpr (std::is_same_v<T, core::AuxDate>) {
              ::duckdb::date_t d{};
              d.days = payload.days_since_epoch;
              return ::duckdb::Value::DATE(d);
            } else
              return ::duckdb::Value(payload);
          },
          p.payload));
    }
    auto result = stmt->Execute(values, /*allow_stream_result=*/false);
    Require(result && !result->HasError(), result ? result->GetError() : "query failed: " + sql);
    auto &m = dynamic_cast<::duckdb::MaterializedQueryResult &>(*result);
    for (::duckdb::idx_t r = 0; r < m.RowCount(); ++r) {
      core::AuxRow out;
      for (::duckdb::idx_t c = 0; c < m.ColumnCount(); ++c)
        out.values.push_back(CapturedValue(m.GetValue(c, r)));
      row(out);
    }
  }

  std::vector<int32_t> Attnums(const std::string &table, const std::vector<std::string> &columns) override {
    std::vector<std::pair<std::string, int32_t>> cids;
    auto scan_lock = LockScan(connection);
    auto result = ScanConnection(connection).Query("SELECT name, cid FROM pragma_table_info(" +
                                                   ::duckdb::KeywordHelper::WriteQuoted(table, '\'') + ")");
    Require(result && !result->HasError(), result ? result->GetError() : "table_info failed");
    for (::duckdb::idx_t r = 0; r < result->RowCount(); ++r)
      cids.emplace_back(result->GetValue(0, r).ToString(),
                        static_cast<int32_t>(result->GetValue(1, r).GetValue<int64_t>()));
    auto lower = [](std::string x) {
      for (auto &c : x)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return x;
    };
    std::vector<int32_t> out;
    for (const auto &col : columns) {
      int32_t found = -1;
      for (const auto &[name, cid] : cids)
        if (name == col)
          found = cid;
      if (found < 0)
        for (const auto &[name, cid] : cids)
          if (lower(name) == lower(col))
            found = cid;
      Require(found >= 0, "table " + table + " has no column " + col +
                              " (a capture lists the table's own columns; compute a derived delta field, e.g. "
                              "`... AS " + col + "`, in the glue's Apply from the base columns it reads)");
      out.push_back(found);
    }
    return out;
  }

  ::duckdb::Connection &connection;
};

struct KitBinding final : DuckdbRuntime::Binding {
  KitBinding(::duckdb::Connection &connection, const verus_kit::Glue &glue_p)
      : host(connection), glue(glue_p), module(verus_kit::MakeModule(glue_p, host)) {
    // Resolve the captured columns now: capture asks for them mid-statement.
    module->Dependencies();
  }
  core::AuxQueryModule &Module() override { return *module; }
  void Build(DuckDBAuxBackend &backend, core::StateBuilder &builder) override { module->Build(backend, builder); }
  void Execute(DuckDBAuxBackend &backend, const core::QueryArguments &arguments, core::AuxRowSink &sink) override {
    module->Execute(backend, arguments, sink);
  }
  std::vector<std::pair<std::string, core::TableId>> CaptureTables() const override {
    std::vector<std::pair<std::string, core::TableId>> out;
    for (std::size_t i = 0; i < glue.tables.size(); ++i)
      out.emplace_back(glue.tables[i].name, verus_kit::TableIdOf(i));
    return out;
  }
  DuckVerusHost host;
  const verus_kit::Glue &glue;
  std::unique_ptr<core::AuxQueryModule> module;
};

} // namespace

std::unique_ptr<DuckdbRuntime::Binding> MakeVerusKitBinding(::duckdb::Connection &connection,
                                                            const std::string &accelerator_id) {
  if (const auto *glue = verus_kit::Find(accelerator_id)) {
    return std::make_unique<KitBinding>(connection, *glue);
  }
  return nullptr;
}

bool IsVerusKitAccelerator(const std::string &accelerator_id) {
  return verus_kit::Find(accelerator_id) != nullptr;
}

std::unique_ptr<VerusAuxStore> MakeRequestedVerusStore(const core::AcceleratorId &accelerator,
                                                       VerusAuxStore::EpochProvider epoch,
                                                       VerusAuxStore::FloorProvider floor) {
  return VerusAuxStoreRequested() ? MakeVerusAuxStore(accelerator, std::move(epoch), std::move(floor))
                                  : nullptr;
}

std::vector<VerusKitTableFunction> VerusKitTableFunctions() {
  std::vector<VerusKitTableFunction> out;
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
    out.push_back({kit_entry, std::string("accel_") + glue->id, std::move(types)});
  }
  return out;
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
