#include "test_support/native_scan_test_utils.hpp"

#include <stdexcept>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen::test_support {
namespace {

NativeScanSpec CreateLineitemShipdateScanSpec() {
    NativeScanSpec scan_spec;
    scan_spec.table_name = "lineitem";
    scan_spec.projected_columns = {
        {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
    };
    return scan_spec;
}

template <class Callback>
void WithTransaction(duckdb::Connection &connection, Callback &&callback) {
    const bool started_transaction = !connection.HasActiveTransaction();
    if (started_transaction) {
        connection.BeginTransaction();
    }
    try {
        callback(*connection.context);
        if (started_transaction) {
            connection.Commit();
        }
    } catch (...) {
        if (started_transaction && connection.HasActiveTransaction()) {
            connection.Rollback();
        }
        throw;
    }
}

} // namespace

uint64_t CountLineitemRows(duckdb::Connection &connection) {
    uint64_t count = 0;
    WithTransaction(connection, [&](duckdb::ClientContext &context) { count = CountLineitemRows(context); });
    return count;
}

uint64_t CountLineitemRows(duckdb::ClientContext &context) {
    auto bind_data = BindNativeScan(context, CreateLineitemShipdateScanSpec());
    auto source = CreateNativeChunkSource(context, *bind_data);

    uint64_t count = 0;
    while (source->Next()) {
        count += source->Chunk().size();
    }
    return count;
}

std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::Connection &connection) {
    std::vector<duckdb::date_t> shipdates;
    WithTransaction(connection, [&](duckdb::ClientContext &context) { shipdates = CollectLineitemShipdates(context); });
    return shipdates;
}

std::vector<duckdb::date_t> CollectLineitemShipdates(duckdb::ClientContext &context) {
    auto bind_data = BindNativeScan(context, CreateLineitemShipdateScanSpec());
    auto source = CreateNativeChunkSource(context, *bind_data);

    std::vector<duckdb::date_t> result;
    while (source->Next()) {
        auto &chunk = source->Chunk();
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
    }
    return result;
}

} // namespace accelerator_codegen::test_support
