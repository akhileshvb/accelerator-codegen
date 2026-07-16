#include "accelerator_codegen/duckdb_scan_adapter.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace accelerator_codegen {
namespace {

struct NativeTableScanBindData : public duckdb::FunctionData {
    explicit NativeTableScanBindData(NativeTableSpec spec_p, duckdb::LogicalDependency dependency_p,
                                     duckdb::unique_ptr<duckdb::TableFilterSet> table_filters_p)
        : spec(std::move(spec_p)), dependency(std::move(dependency_p)), table_filters(std::move(table_filters_p)) {
    }

    NativeTableSpec spec;
    duckdb::LogicalDependency dependency;
    duckdb::unique_ptr<duckdb::TableFilterSet> table_filters;

    duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
        return duckdb::make_uniq<NativeTableScanBindData>(
            spec, dependency, table_filters ? table_filters->Copy() : nullptr);
    }

    bool Equals(const duckdb::FunctionData &other_p) const override {
        auto &other = other_p.Cast<NativeTableScanBindData>();
        return spec.catalog_name == other.spec.catalog_name && spec.schema_name == other.spec.schema_name &&
               spec.table_name == other.spec.table_name && spec.projected_columns == other.spec.projected_columns &&
               duckdb::TableFilterSet::Equals(table_filters.get(), other.table_filters.get());
    }
};

std::string DescribeExpected(const NativeColumnContract &contract) {
    std::ostringstream out;
    out << contract.expected_logical_type.ToString() << " / " << duckdb::EnumUtil::ToString(contract.expected_physical_type);
    return out.str();
}

std::string DescribeObserved(const duckdb::LogicalType &type) {
    std::ostringstream out;
    out << type.ToString() << " / " << duckdb::EnumUtil::ToString(type.InternalType());
    return out.str();
}

void ValidateColumnContract(duckdb::TableCatalogEntry &table, const NativeColumnContract &contract) {
    if (!table.ColumnExists(contract.name)) {
        throw duckdb::BinderException("Required column \"%s\" is missing from %s.%s.%s", contract.name,
                                      table.catalog.GetName(), table.schema.name, table.name);
    }

    const auto &column = table.GetColumn(contract.name);
    const auto &type = column.Type();
    if (type != contract.expected_logical_type || type.InternalType() != contract.expected_physical_type) {
        throw duckdb::BinderException(
            "Unsupported schema for %s.%s.%s column \"%s\": observed %s, expected %s", table.catalog.GetName(),
            table.schema.name, table.name, contract.name, DescribeObserved(type), DescribeExpected(contract));
    }
}

const NativeColumnContract &FindContract(const NativeTableSpec &spec, const std::string &column_name) {
    for (const auto &contract : spec.schema_contract) {
        if (contract.name == column_name) {
            return contract;
        }
    }
    throw duckdb::BinderException("Unsupported projected column \"%s\" for accelerator scan", column_name);
}

NativeTableSpec BuildLineitemSpec(duckdb::TableCatalogEntry &table,
                                  const duckdb::vector<std::string> &projected_column_names) {
    NativeTableSpec spec;
    spec.catalog_name = table.catalog.GetName();
    spec.schema_name = table.schema.name;
    spec.table_name = table.name;

    spec.schema_contract = {
        {"l_shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
        {"l_discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
    };

    for (const auto &contract : spec.schema_contract) {
        ValidateColumnContract(table, contract);
    }

    for (const auto &column_name : projected_column_names) {
        const auto &contract = FindContract(spec, column_name);
        auto mutable_column_name = column_name;
        auto column_index = table.GetColumnIndex(mutable_column_name);
        spec.projected_columns.emplace_back(table.GetStorageIndex(duckdb::ColumnIndex(column_index.index)));
        spec.projected_types.emplace_back(contract.expected_logical_type);
    }
    return spec;
}

duckdb::TableCatalogEntry &LookupLineitem(duckdb::ClientContext &context) {
    return duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, INVALID_CATALOG, DEFAULT_SCHEMA, "lineitem");
}

const NativeTableScanBindData &GetNativeTableBindData(const duckdb::FunctionData *bind_data) {
    if (!bind_data) {
        throw std::runtime_error("native table bind data is missing");
    }
    return bind_data->Cast<NativeTableScanBindData>();
}

class NativeChunkSource : public ChunkSource {
public:
    NativeChunkSource(duckdb::ClientContext &context_p, const NativeTableSpec &spec,
                      duckdb::TableFilterSet *table_filters)
        : context(context_p),
          transaction(duckdb::DuckTransaction::Get(context_p, duckdb::Catalog::GetCatalog(context_p, spec.catalog_name))),
          table(ResolveTable(context_p, spec)), storage(table.GetStorage()) {
        for (const auto &contract : spec.schema_contract) {
            ValidateColumnContract(table, contract);
        }
        chunk.Initialize(context, spec.projected_types);
        storage.InitializeScan(context, transaction, scan_state, spec.projected_columns, table_filters);
    }

    bool Next() override {
        if (context.interrupted) {
            throw duckdb::InterruptException();
        }
        chunk.Reset();
        storage.Scan(transaction, chunk, scan_state);
        return chunk.size() != 0;
    }

    duckdb::DataChunk &Chunk() override {
        return chunk;
    }

private:
    static duckdb::TableCatalogEntry &ResolveTable(duckdb::ClientContext &context, const NativeTableSpec &spec) {
        return duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, spec.catalog_name, spec.schema_name,
                                                                    spec.table_name);
    }

private:
    duckdb::ClientContext &context;
    duckdb::DuckTransaction &transaction;
    duckdb::TableCatalogEntry &table;
    duckdb::DataTable &storage;
    duckdb::TableScanState scan_state;
    duckdb::DataChunk chunk;
};

template <class Consumer>
void ScanLineitem(duckdb::ClientContext &context,
                  const duckdb::vector<std::string> &projected_columns,
                  Consumer &&consume) {
    auto bind_data = CreateLineitemScanBindData(context, projected_columns);
    auto source = CreateChunkSource(context, *bind_data);
    while (source->Next()) {
        consume(source->Chunk());
    }
}

} // namespace

duckdb::unique_ptr<duckdb::FunctionData> CreateLineitemScanBindData(
    duckdb::ClientContext &context,
    const duckdb::vector<std::string> &projected_column_names,
    duckdb::unique_ptr<duckdb::TableFilterSet> table_filters) {
    auto &table_entry = LookupLineitem(context);
    auto spec = BuildLineitemSpec(table_entry, projected_column_names);
    duckdb::LogicalDependency dependency(table_entry);
    return duckdb::make_uniq<NativeTableScanBindData>(std::move(spec), std::move(dependency), std::move(table_filters));
}

void AddNativeTableDependencies(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data) {
    dependencies.AddDependency(GetNativeTableBindData(bind_data).dependency);
}

duckdb::unique_ptr<ChunkSource> CreateChunkSource(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
    auto &scan_bind = bind_data.Cast<NativeTableScanBindData>();
    return duckdb::make_uniq<NativeChunkSource>(context, scan_bind.spec, scan_bind.table_filters.get());
}

uint64_t CountLineitemRows(duckdb::Connection &connection) {
    const bool started_transaction = !connection.HasActiveTransaction();
    if (started_transaction) {
        connection.BeginTransaction();
    }
    try {
        auto count = CountLineitemRows(*connection.context);
        if (started_transaction) {
            connection.Commit();
        }
        return count;
    } catch (...) {
        if (started_transaction && connection.HasActiveTransaction()) {
            connection.Rollback();
        }
        throw;
    }
}

uint64_t CountLineitemRows(duckdb::ClientContext &context) {
    uint64_t count = 0;
    ScanLineitem(context, {"l_shipdate"}, [&](duckdb::DataChunk &chunk) { count += chunk.size(); });
    return count;
}

std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::Connection &connection) {
    const bool started_transaction = !connection.HasActiveTransaction();
    if (started_transaction) {
        connection.BeginTransaction();
    }
    try {
        auto shipdates = CollectLineitemShipdates(*connection.context);
        if (started_transaction) {
            connection.Commit();
        }
        return shipdates;
    } catch (...) {
        if (started_transaction && connection.HasActiveTransaction()) {
            connection.Rollback();
        }
        throw;
    }
}

std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::ClientContext &context) {
    std::vector<duckdb::date_t> result;
    ScanLineitem(context, {"l_shipdate"}, [&](duckdb::DataChunk &chunk) {
        duckdb::UnifiedVectorFormat format;
        chunk.data[0].ToUnifiedFormat(chunk.size(), format);
        auto values = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(format);
        for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
            auto index = format.sel->get_index(row);
            if (!format.validity.RowIsValid(index)) {
                throw std::runtime_error("lineitem.l_shipdate unexpectedly contains NULL");
            }
            result.push_back(values[index]);
        }
    });
    return result;
}

} // namespace accelerator_codegen
