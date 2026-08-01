#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/parallel/parallel_runtime.hpp"

#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group_reorderer.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace accelerator_codegen {
namespace {

struct BoundNativeColumnSpec {
    std::string source_name;
    std::string kernel_column_id;
    duckdb::LogicalType logical_type;
    duckdb::PhysicalType physical_type;
    duckdb::StorageIndex storage_index;
};

struct BoundNativeScanSpec {
    std::string catalog_name;
    std::string schema_name;
    std::string table_name;
    duckdb::vector<BoundNativeColumnSpec> projected_columns;
    duckdb::vector<BoundNativeColumnSpec> scan_columns;
};

struct NativeScanBindData : public duckdb::FunctionData {
    NativeScanBindData(BoundNativeScanSpec bound_spec_p,
                       duckdb::LogicalDependency dependency_p,
                       duckdb::unique_ptr<duckdb::TableFilterSet> translated_filters_p)
        : bound_spec(std::move(bound_spec_p)),
          dependency(std::move(dependency_p)),
          translated_filters(std::move(translated_filters_p)) {
    }

    BoundNativeScanSpec bound_spec;
    duckdb::LogicalDependency dependency;
    duckdb::unique_ptr<duckdb::TableFilterSet> translated_filters;

    duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
        return duckdb::make_uniq<NativeScanBindData>(
            bound_spec, dependency, translated_filters ? translated_filters->Copy() : nullptr);
    }

    bool Equals(const duckdb::FunctionData &other_p) const override {
        auto &other = other_p.Cast<NativeScanBindData>();
        return bound_spec.catalog_name == other.bound_spec.catalog_name &&
               bound_spec.schema_name == other.bound_spec.schema_name &&
               bound_spec.table_name == other.bound_spec.table_name &&
               ProjectedColumnsEqual(bound_spec.projected_columns, other.bound_spec.projected_columns) &&
               ProjectedColumnsEqual(bound_spec.scan_columns, other.bound_spec.scan_columns) &&
               duckdb::TableFilterSet::Equals(translated_filters.get(), other.translated_filters.get());
    }

private:
    static bool ProjectedColumnsEqual(const duckdb::vector<BoundNativeColumnSpec> &lhs,
                                      const duckdb::vector<BoundNativeColumnSpec> &rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (duckdb::idx_t i = 0; i < lhs.size(); i++) {
            if (lhs[i].source_name != rhs[i].source_name || lhs[i].kernel_column_id != rhs[i].kernel_column_id ||
                lhs[i].logical_type != rhs[i].logical_type || lhs[i].physical_type != rhs[i].physical_type ||
                lhs[i].storage_index != rhs[i].storage_index) {
                return false;
            }
        }
        return true;
    }
};

std::string DescribeExpected(const NativeColumnSpec &contract) {
    std::ostringstream out;
    out << contract.expected_logical_type.ToString() << " / "
        << duckdb::EnumUtil::ToString(contract.expected_physical_type);
    return out.str();
}

std::string DescribeObserved(const duckdb::LogicalType &type) {
    std::ostringstream out;
    out << type.ToString() << " / " << duckdb::EnumUtil::ToString(type.InternalType());
    return out.str();
}

std::string GetLookupCatalogName(const NativeScanSpec &scan_spec) {
    return scan_spec.catalog_name.empty() ? std::string(INVALID_CATALOG) : scan_spec.catalog_name;
}

std::string GetLookupSchemaName(const NativeScanSpec &scan_spec) {
    return scan_spec.schema_name.empty() ? std::string(DEFAULT_SCHEMA) : scan_spec.schema_name;
}

duckdb::TableCatalogEntry &LookupTable(duckdb::ClientContext &context, const NativeScanSpec &scan_spec) {
    return duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, GetLookupCatalogName(scan_spec),
                                                                GetLookupSchemaName(scan_spec), scan_spec.table_name);
}

void ValidateProjectedColumn(duckdb::TableCatalogEntry &table, const NativeColumnSpec &column_spec) {
    if (!table.ColumnExists(column_spec.source_name)) {
        throw duckdb::BinderException("Required column \"%s\" is missing from %s.%s.%s", column_spec.source_name,
                                      table.catalog.GetName(), table.schema.name, table.name);
    }

    const auto &column = table.GetColumn(column_spec.source_name);
    const auto &type = column.Type();
    if (type != column_spec.expected_logical_type || type.InternalType() != column_spec.expected_physical_type) {
        throw duckdb::BinderException("Unsupported schema for %s.%s.%s column \"%s\": observed %s, expected %s",
                                      table.catalog.GetName(), table.schema.name, table.name, column_spec.source_name,
                                      DescribeObserved(type), DescribeExpected(column_spec));
    }
}

const BoundNativeColumnSpec &FindScanColumn(const BoundNativeScanSpec &bound_spec, const std::string &source_name) {
    for (const auto &column : bound_spec.scan_columns) {
        if (column.source_name == source_name) {
            return column;
        }
    }
    throw duckdb::BinderException("Scan filter references undeclared scan column \"%s\" on %s.%s.%s", source_name,
                                  bound_spec.catalog_name, bound_spec.schema_name, bound_spec.table_name);
}

BoundNativeScanSpec AppendFilterOnlyScanColumns(duckdb::TableCatalogEntry &table, BoundNativeScanSpec bound_spec,
                                                const NativeScanSpec &scan_spec) {
    bound_spec.scan_columns = bound_spec.projected_columns;
    std::unordered_map<std::string, bool> scan_names;
    scan_names.reserve(bound_spec.scan_columns.size() + scan_spec.filters.size());
    for (const auto &column : bound_spec.scan_columns) {
        scan_names.emplace(column.source_name, true);
    }
    for (const auto &filter_spec : scan_spec.filters) {
        if (scan_names.find(filter_spec.source_name) != scan_names.end()) {
            continue;
        }
        if (!table.ColumnExists(filter_spec.source_name)) {
            throw duckdb::BinderException("Scan filter references missing column \"%s\" on %s.%s.%s",
                                          filter_spec.source_name, table.catalog.GetName(), table.schema.name,
                                          table.name);
        }
        const auto &column = table.GetColumn(filter_spec.source_name);
        auto mutable_column_name = filter_spec.source_name;
        auto column_index = table.GetColumnIndex(mutable_column_name);
        bound_spec.scan_columns.push_back(
            {filter_spec.source_name, "", column.Type(), column.Type().InternalType(),
             table.GetStorageIndex(duckdb::ColumnIndex(column_index.index))});
        scan_names.emplace(filter_spec.source_name, true);
    }
    return bound_spec;
}

BoundNativeScanSpec BindProjectedColumns(duckdb::TableCatalogEntry &table, const NativeScanSpec &scan_spec) {
    if (scan_spec.table_name.empty()) {
        throw duckdb::BinderException("Native scan requires a non-empty table name");
    }
    if (scan_spec.projected_columns.empty()) {
        throw duckdb::BinderException("Native scan for %s.%s.%s must project at least one column", table.catalog.GetName(),
                                      table.schema.name, table.name);
    }

    BoundNativeScanSpec bound_spec;
    bound_spec.catalog_name = table.catalog.GetName();
    bound_spec.schema_name = table.schema.name;
    bound_spec.table_name = table.name;

    std::unordered_map<std::string, bool> kernel_ids;
    kernel_ids.reserve(scan_spec.projected_columns.size());

    for (const auto &column_spec : scan_spec.projected_columns) {
        if (column_spec.source_name.empty()) {
            throw duckdb::BinderException("Native scan for %s.%s.%s has an empty projected source column name",
                                          table.catalog.GetName(), table.schema.name, table.name);
        }
        if (column_spec.kernel_column_id.empty()) {
            throw duckdb::BinderException("Projected column \"%s\" on %s.%s.%s must declare a non-empty kernel column id",
                                          column_spec.source_name, table.catalog.GetName(), table.schema.name, table.name);
        }
        if (!kernel_ids.emplace(column_spec.kernel_column_id, true).second) {
            throw duckdb::BinderException("Duplicate kernel column id \"%s\" in native scan for %s.%s.%s",
                                          column_spec.kernel_column_id, table.catalog.GetName(), table.schema.name,
                                          table.name);
        }

        ValidateProjectedColumn(table, column_spec);
        auto mutable_column_name = column_spec.source_name;
        auto column_index = table.GetColumnIndex(mutable_column_name);

        bound_spec.projected_columns.push_back(
            {column_spec.source_name, column_spec.kernel_column_id, column_spec.expected_logical_type,
             column_spec.expected_physical_type, table.GetStorageIndex(duckdb::ColumnIndex(column_index.index))});
    }
    bound_spec.scan_columns = bound_spec.projected_columns;
    return bound_spec;
}

duckdb::unique_ptr<duckdb::TableFilterSet> TranslateFilters(const BoundNativeScanSpec &bound_spec,
                                                            const NativeScanSpec &scan_spec) {
    if (scan_spec.filters.empty()) {
        return nullptr;
    }

    auto filters = duckdb::make_uniq<duckdb::TableFilterSet>();
    for (const auto &filter_spec : scan_spec.filters) {
        (void)FindScanColumn(bound_spec, filter_spec.source_name);

        duckdb::idx_t scan_index = 0;
        bool found_index = false;
        for (duckdb::idx_t i = 0; i < bound_spec.scan_columns.size(); i++) {
            if (bound_spec.scan_columns[i].source_name == filter_spec.source_name) {
                scan_index = i;
                found_index = true;
                break;
            }
        }
        if (!found_index) {
            throw std::runtime_error("scan filter column index lookup failed");
        }

        if (filter_spec.comparison_type == duckdb::ExpressionType::OPERATOR_IS_NOT_NULL) {
            filters->PushFilter(duckdb::ColumnIndex(scan_index), duckdb::make_uniq<duckdb::IsNotNullFilter>());
            continue;
        }
        filters->PushFilter(duckdb::ColumnIndex(scan_index),
                            duckdb::make_uniq<duckdb::ConstantFilter>(filter_spec.comparison_type, filter_spec.constant));
    }
    return filters;
}

const NativeScanBindData &GetNativeScanBindData(const duckdb::FunctionData *bind_data) {
    if (!bind_data) {
        throw std::runtime_error("native scan bind data is missing");
    }
    return bind_data->Cast<NativeScanBindData>();
}

class NativeChunkSource : public ChunkSource {
public:
    explicit NativeChunkSource(duckdb::ClientContext &context_p, const NativeScanBindData &bind_data,
                               const duckdb::TableFilterSet *additional_filters_p = nullptr)
        : context(context_p),
          transaction(
              duckdb::DuckTransaction::Get(context_p, duckdb::Catalog::GetCatalog(context_p, bind_data.bound_spec.catalog_name))),
          table(ResolveTable(context_p, bind_data.bound_spec)),
          storage(table.GetStorage()) {
        duckdb::vector<duckdb::LogicalType> projected_types;
        for (const auto &column : bind_data.bound_spec.scan_columns) {
            ValidateResolvedColumn(table, column);
            projected_types.emplace_back(column.logical_type);
            storage_indexes.emplace_back(column.storage_index);
        }
        chunk.Initialize(context, projected_types);

        duckdb::optional_ptr<duckdb::TableFilterSet> filters;
        if (additional_filters_p) {
            if (bind_data.translated_filters) {
                merged_filters = bind_data.translated_filters->Copy();
                for (auto &entry : additional_filters_p->filters) {
                    merged_filters->PushFilter(duckdb::ColumnIndex(entry.first), entry.second->Copy());
                }
            } else {
                merged_filters = additional_filters_p->Copy();
            }
            filters = merged_filters.get();
        } else if (bind_data.translated_filters) {
            filters = bind_data.translated_filters.get();
        }
        storage.InitializeScan(context, transaction, scan_state, storage_indexes, filters);
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
    static duckdb::TableCatalogEntry &ResolveTable(duckdb::ClientContext &context, const BoundNativeScanSpec &bound_spec) {
        return duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, bound_spec.catalog_name,
                                                                    bound_spec.schema_name, bound_spec.table_name);
    }

    static void ValidateResolvedColumn(duckdb::TableCatalogEntry &table, const BoundNativeColumnSpec &column_spec) {
        NativeColumnSpec check_spec {column_spec.source_name, column_spec.kernel_column_id, column_spec.logical_type,
                                     column_spec.physical_type};
        ValidateProjectedColumn(table, check_spec);
    }

private:
    duckdb::ClientContext &context;
    duckdb::DuckTransaction &transaction;
    duckdb::TableCatalogEntry &table;
    duckdb::DataTable &storage;
    duckdb::vector<duckdb::StorageIndex> storage_indexes;
    duckdb::unique_ptr<duckdb::TableFilterSet> merged_filters;
    duckdb::TableScanState scan_state;
    duckdb::DataChunk chunk;
};

} // namespace

duckdb::unique_ptr<duckdb::FunctionData> BindNativeScan(duckdb::ClientContext &context, const NativeScanSpec &scan_spec) {
    auto &table_entry = LookupTable(context, scan_spec);
    auto bound_spec = BindProjectedColumns(table_entry, scan_spec);
    bound_spec = AppendFilterOnlyScanColumns(table_entry, std::move(bound_spec), scan_spec);
    auto translated_filters = TranslateFilters(bound_spec, scan_spec);
    duckdb::LogicalDependency dependency(table_entry);
    return duckdb::make_uniq<NativeScanBindData>(std::move(bound_spec), std::move(dependency),
                                                 std::move(translated_filters));
}

void AddNativeTableDependencies(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data) {
    dependencies.AddDependency(GetNativeScanBindData(bind_data).dependency);
}

duckdb::unique_ptr<ChunkSource> CreateNativeChunkSource(duckdb::ClientContext &context,
                                                        const duckdb::FunctionData &bind_data,
                                                        const duckdb::TableFilterSet *additional_filters) {
    return duckdb::make_uniq<NativeChunkSource>(context, bind_data.Cast<NativeScanBindData>(), additional_filters);
}

bool NativeScanHasFilters(const duckdb::FunctionData &bind_data) {
    const auto &native = GetNativeScanBindData(&bind_data);
    return native.translated_filters && !native.translated_filters->filters.empty();
}

duckdb::StorageIndex GetNativeScanColumnStorageIndex(const duckdb::FunctionData &bind_data, const std::string &source_name) {
    const auto &native = GetNativeScanBindData(&bind_data);
    for (const auto &column : native.bound_spec.projected_columns) {
        if (column.source_name == source_name) {
            return column.storage_index;
        }
    }
    throw std::runtime_error("native scan column not found: " + source_name);
}

duckdb::idx_t GetNativeScanTableRowCount(duckdb::ClientContext &context, const NativeScanSpec &scan_spec) {
    auto &table = LookupTable(context, scan_spec);
    return table.GetStorage().GetTotalRows();
}

duckdb::idx_t GetNativeScanTableRowCount(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
    const auto &native = GetNativeScanBindData(&bind_data);
    NativeScanSpec scan_spec;
    scan_spec.catalog_name = native.bound_spec.catalog_name;
    scan_spec.schema_name = native.bound_spec.schema_name;
    scan_spec.table_name = native.bound_spec.table_name;
    return GetNativeScanTableRowCount(context, scan_spec);
}

bool RunNativeFilteredScanConsumer(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data,
                                   const duckdb::FilteredScanConsumer &consumer,
                                   const NativeScanConsumerOptions *options) {
    const auto &native = GetNativeScanBindData(&bind_data);
    auto &table = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, native.bound_spec.catalog_name,
                                                                       native.bound_spec.schema_name,
                                                                       native.bound_spec.table_name);
    auto &transaction = duckdb::DuckTransaction::Get(context, duckdb::Catalog::GetCatalog(context, native.bound_spec.catalog_name));
    auto &storage = table.GetStorage();

    duckdb::vector<duckdb::StorageIndex> storage_indexes;
    storage_indexes.reserve(native.bound_spec.scan_columns.size());
    for (const auto &column : native.bound_spec.scan_columns) {
        storage_indexes.emplace_back(column.storage_index);
    }

    duckdb::unique_ptr<duckdb::TableFilterSet> merged_filters;
    if (options && options->additional_filters) {
        if (native.translated_filters) {
            merged_filters = native.translated_filters->Copy();
            for (auto &entry : options->additional_filters->filters) {
                merged_filters->PushFilter(duckdb::ColumnIndex(entry.first), entry.second->Copy());
            }
        } else {
            merged_filters = options->additional_filters->Copy();
        }
    }
    duckdb::optional_ptr<duckdb::TableFilterSet> filters;
    if (merged_filters) {
        filters = merged_filters.get();
    } else if (native.translated_filters) {
        filters = native.translated_filters.get();
    }

    duckdb::TableScanState scan_state;
    if (options && options->row_group_order) {
        const duckdb::TransactionData transaction_data(transaction);
        scan_state.table_state.reorderer = duckdb::make_uniq<duckdb::RowGroupReorderer>(
            *options->row_group_order, transaction_data);
    }
    storage.InitializeScan(context, transaction, scan_state, storage_indexes, filters);

    duckdb::FilteredScanConsumer wrapped =
        [&context, &consumer](duckdb::TransactionData transaction_data, duckdb::CollectionScanState &state,
                              duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
                              duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
        if (context.interrupted) {
            return duckdb::FilteredScanConsumerResult::STOP;
        }
        return consumer(transaction_data, state, row_group, sel, count);
    };

    auto result = storage.ScanFilteredConsumer(transaction, scan_state, wrapped);
    return result != duckdb::FilteredScanConsumerResult::STOP;
}

void InitializeNativeParallelScanHandle(NativeParallelScanHandle &handle, duckdb::ClientContext &context,
                                        const duckdb::FunctionData &bind_data) {
    const auto &native = GetNativeScanBindData(&bind_data);
    auto &table = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(context, native.bound_spec.catalog_name,
                                                                       native.bound_spec.schema_name,
                                                                       native.bound_spec.table_name);
    auto &transaction =
        duckdb::DuckTransaction::Get(context, duckdb::Catalog::GetCatalog(context, native.bound_spec.catalog_name));
    auto &storage = table.GetStorage();

    handle.context = &context;
    handle.transaction = &transaction;
    handle.storage = &storage;
    handle.storage_indexes.clear();
    handle.projected_types.clear();
    handle.storage_indexes.reserve(native.bound_spec.scan_columns.size());
    handle.projected_types.reserve(native.bound_spec.scan_columns.size());
    for (const auto &column : native.bound_spec.scan_columns) {
        handle.storage_indexes.emplace_back(column.storage_index);
        handle.projected_types.emplace_back(column.logical_type);
    }
    handle.filters = native.translated_filters.get();
    handle.tasks_claimed.store(0, std::memory_order_relaxed);
    handle.failed.store(false, std::memory_order_relaxed);

    // InitializeParallelScan's column_indexes argument is unused by storage; pass empty.
    duckdb::vector<duckdb::ColumnIndex> column_indexes;
    storage.InitializeParallelScan(context, handle.parallel_state, column_indexes);
}

} // namespace accelerator_codegen
