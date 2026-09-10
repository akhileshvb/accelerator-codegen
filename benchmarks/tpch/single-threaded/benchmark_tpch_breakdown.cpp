#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/duckdb/register.hpp"
#include "tpch/q1/duckdb/base/query.hpp"
#include "tpch/q1/duckdb/base/fused.hpp"
#include "tpch/q6/duckdb/base/query.hpp"
#include "test_support/q1_manifest.hpp"
#include "test_support/q6_manifest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace {
using Clock = std::chrono::steady_clock;

std::string GetTpchDbPath() {
    const char *env_path = std::getenv("ACCEL_TPCH_DB");
    if (env_path && *env_path) {
        return env_path;
    }
    return "/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb";
}

int GetEnvInt(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return std::stoi(value);
}

std::string GetEnvString(const char *name) {
    const char *value = std::getenv(name);
    return value ? value : "";
}

void Require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string ReadFile(const std::string &path) {
    std::ifstream input(path);
    Require(input.good(), "failed to open file: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> SplitCommaSeparated(const std::string &input) {
    std::vector<std::string> result;
    std::stringstream stream(input);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

bool MatchesFilter(const std::string &label, const std::vector<std::string> &filters) {
    if (filters.empty()) {
        return true;
    }
    return std::find(filters.begin(), filters.end(), label) != filters.end();
}

double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(end - start).count();
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

struct TimingStats {
    double average_us = 0.0;
    double median_us = 0.0;
};

struct TimedSqlResult {
    TimingStats timing;
    uint64_t checksum = 0;
};

struct StageRunResult {
    uint64_t checksum = 0;
    duckdb::idx_t chunks = 0;
    duckdb::idx_t rows = 0;
    duckdb::idx_t qualifying_rows = 0;
    duckdb::idx_t output_rows = 0;
    double setup_us = 0.0;
    double scan_us = 0.0;
    double predicate_us = 0.0;
    double checksum_us = 0.0;
    double kernel_us = 0.0;
    double merge_us = 0.0;
    double output_us = 0.0;
};

struct StageStats {
    TimingStats total;
    TimingStats setup;
    TimingStats scan;
    TimingStats predicate;
    TimingStats checksum;
    TimingStats kernel;
    TimingStats merge;
    TimingStats output;
    StageRunResult sample;
};

double ComputeMedian(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

template <class Callback>
TimingStats BenchmarkOperation(int warmup_iterations, int measured_iterations, Callback &&callback) {
    for (int i = 0; i < warmup_iterations; i++) {
        callback();
    }

    std::vector<double> samples_us;
    samples_us.reserve(measured_iterations);
    for (int i = 0; i < measured_iterations; i++) {
        const auto start = Clock::now();
        callback();
        const auto end = Clock::now();
        samples_us.push_back(ElapsedUs(start, end));
    }

    TimingStats stats;
    for (const auto sample : samples_us) {
        stats.average_us += sample;
    }
    stats.average_us /= static_cast<double>(samples_us.size());
    stats.median_us = ComputeMedian(std::move(samples_us));
    return stats;
}

template <class Callback>
StageStats BenchmarkStage(int warmup_iterations, int measured_iterations, Callback &&callback) {
    for (int i = 0; i < warmup_iterations; i++) {
        auto warmup = callback();
        Require(warmup.checksum != std::numeric_limits<uint64_t>::max(), "unreachable warmup checksum guard");
    }

    StageStats stats;
    stats.sample = callback();

    std::vector<double> total_samples;
    std::vector<double> setup_samples;
    std::vector<double> scan_samples;
    std::vector<double> predicate_samples;
    std::vector<double> checksum_samples;
    std::vector<double> kernel_samples;
    std::vector<double> merge_samples;
    std::vector<double> output_samples;
    total_samples.reserve(measured_iterations);
    setup_samples.reserve(measured_iterations);
    scan_samples.reserve(measured_iterations);
    predicate_samples.reserve(measured_iterations);
    checksum_samples.reserve(measured_iterations);
    kernel_samples.reserve(measured_iterations);
    merge_samples.reserve(measured_iterations);
    output_samples.reserve(measured_iterations);

    for (int i = 0; i < measured_iterations; i++) {
        const auto start = Clock::now();
        auto result = callback();
        const auto end = Clock::now();

        total_samples.push_back(ElapsedUs(start, end));
        setup_samples.push_back(result.setup_us);
        scan_samples.push_back(result.scan_us);
        predicate_samples.push_back(result.predicate_us);
        checksum_samples.push_back(result.checksum_us);
        kernel_samples.push_back(result.kernel_us);
        merge_samples.push_back(result.merge_us);
        output_samples.push_back(result.output_us);
    }

    auto fill_stats = [](TimingStats &target, const std::vector<double> &samples) {
        for (const auto sample : samples) {
            target.average_us += sample;
        }
        target.average_us /= static_cast<double>(samples.size());
        target.median_us = ComputeMedian(samples);
    };

    fill_stats(stats.total, total_samples);
    fill_stats(stats.setup, setup_samples);
    fill_stats(stats.scan, scan_samples);
    fill_stats(stats.predicate, predicate_samples);
    fill_stats(stats.checksum, checksum_samples);
    fill_stats(stats.kernel, kernel_samples);
    fill_stats(stats.merge, merge_samples);
    fill_stats(stats.output, output_samples);
    return stats;
}

uint64_t MaterializedResultChecksum(duckdb::MaterializedQueryResult &materialized) {
    uint64_t checksum = 0;
    for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
        for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
            checksum += materialized.GetValue(col, row).ToString().size();
        }
    }
    return checksum;
}

uint64_t ExecuteSqlChecksum(duckdb::Connection &connection, const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result != nullptr, "query returned null result");
    Require(!result->HasError(), result->GetError());
    return MaterializedResultChecksum(result->Cast<duckdb::MaterializedQueryResult>());
}

TimedSqlResult BenchmarkSql(duckdb::Connection &connection,
                            const std::string &sql,
                            int warmup_iterations,
                            int measured_iterations) {
    TimedSqlResult result;
    result.checksum = ExecuteSqlChecksum(connection, sql);
    result.timing = BenchmarkOperation(warmup_iterations, measured_iterations, [&]() {
        auto checksum = ExecuteSqlChecksum(connection, sql);
        Require(checksum != std::numeric_limits<uint64_t>::max(), "unreachable sql checksum guard");
    });
    return result;
}

uint64_t MixChecksum(uint64_t checksum, uint64_t value) {
    checksum ^= value + 0x9e3779b97f4a7c15ULL + (checksum << 6U) + (checksum >> 2U);
    return checksum;
}

uint64_t MixStringChecksum(uint64_t checksum, const std::string &value) {
    for (const auto ch : value) {
        checksum = MixChecksum(checksum, static_cast<uint8_t>(ch));
    }
    return checksum;
}

uint64_t ChecksumQ1AllColumns(duckdb::DataChunk &chunk) {
    uint64_t checksum = 0;

    duckdb::UnifiedVectorFormat shipdate_format;
    duckdb::UnifiedVectorFormat quantity_format;
    duckdb::UnifiedVectorFormat extendedprice_format;
    duckdb::UnifiedVectorFormat discount_format;
    duckdb::UnifiedVectorFormat tax_format;
    duckdb::UnifiedVectorFormat returnflag_format;
    duckdb::UnifiedVectorFormat linestatus_format;
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(chunk.size(), shipdate_format);
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(chunk.size(), quantity_format);
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(chunk.size(),
                                                                                                 extendedprice_format);
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(chunk.size(),
                                                                                            discount_format);
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::TAX_INDEX].ToUnifiedFormat(chunk.size(), tax_format);
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::RETURNFLAG_INDEX].ToUnifiedFormat(chunk.size(),
                                                                                              returnflag_format);
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::LINESTATUS_INDEX].ToUnifiedFormat(chunk.size(),
                                                                                              linestatus_format);

    auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
    auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
    auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
    auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
    auto taxes = duckdb::UnifiedVectorFormat::GetData<int64_t>(tax_format);
    auto returnflags = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(returnflag_format);
    auto linestatuses = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(linestatus_format);

    for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
        const auto shipdate_index = shipdate_format.sel->get_index(row);
        const auto quantity_index = quantity_format.sel->get_index(row);
        const auto extendedprice_index = extendedprice_format.sel->get_index(row);
        const auto discount_index = discount_format.sel->get_index(row);
        const auto tax_index = tax_format.sel->get_index(row);
        const auto returnflag_index = returnflag_format.sel->get_index(row);
        const auto linestatus_index = linestatus_format.sel->get_index(row);

        checksum = MixChecksum(checksum, static_cast<uint32_t>(shipdates[shipdate_index].days));
        checksum = MixChecksum(checksum, static_cast<uint64_t>(quantities[quantity_index]));
        checksum = MixChecksum(checksum, static_cast<uint64_t>(extendedprices[extendedprice_index]));
        checksum = MixChecksum(checksum, static_cast<uint64_t>(discounts[discount_index]));
        checksum = MixChecksum(checksum, static_cast<uint64_t>(taxes[tax_index]));
        checksum = MixStringChecksum(checksum, returnflags[returnflag_index].GetString());
        checksum = MixStringChecksum(checksum, linestatuses[linestatus_index].GetString());
    }

    return checksum;
}

uint64_t ChecksumQ6AllColumns(duckdb::DataChunk &chunk) {
    uint64_t checksum = 0;

    duckdb::UnifiedVectorFormat shipdate_format;
    duckdb::UnifiedVectorFormat discount_format;
    duckdb::UnifiedVectorFormat quantity_format;
    duckdb::UnifiedVectorFormat extendedprice_format;
    chunk.data[accelerator_codegen::Q6LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(chunk.size(), shipdate_format);
    chunk.data[accelerator_codegen::Q6LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(chunk.size(), discount_format);
    chunk.data[accelerator_codegen::Q6LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(chunk.size(), quantity_format);
    chunk.data[accelerator_codegen::Q6LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(chunk.size(),
                                                                                                 extendedprice_format);

    auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
    auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
    auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
    auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);

    for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
        const auto shipdate_index = shipdate_format.sel->get_index(row);
        const auto discount_index = discount_format.sel->get_index(row);
        const auto quantity_index = quantity_format.sel->get_index(row);
        const auto extendedprice_index = extendedprice_format.sel->get_index(row);

        checksum = MixChecksum(checksum, static_cast<uint32_t>(shipdates[shipdate_index].days));
        checksum = MixChecksum(checksum, static_cast<uint64_t>(discounts[discount_index]));
        checksum = MixChecksum(checksum, static_cast<uint64_t>(quantities[quantity_index]));
        checksum = MixChecksum(checksum, static_cast<uint64_t>(extendedprices[extendedprice_index]));
    }

    return checksum;
}

duckdb::idx_t CountQ1QualifyingRows(duckdb::DataChunk &chunk, const accelerator_codegen::Q1Parameters &parameters) {
    duckdb::UnifiedVectorFormat shipdate_format;
    chunk.data[accelerator_codegen::Q1LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(chunk.size(), shipdate_format);
    auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);

    duckdb::idx_t qualifying_rows = 0;
    for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
        const auto shipdate_index = shipdate_format.sel->get_index(row);
        if (shipdates[shipdate_index] <= parameters.cutoff_date) {
            qualifying_rows++;
        }
    }
    return qualifying_rows;
}

duckdb::idx_t CountQ6QualifyingRows(duckdb::DataChunk &chunk, const accelerator_codegen::Q6Parameters &parameters) {
    duckdb::UnifiedVectorFormat shipdate_format;
    duckdb::UnifiedVectorFormat discount_format;
    duckdb::UnifiedVectorFormat quantity_format;
    chunk.data[accelerator_codegen::Q6LineitemChunkLayout::SHIPDATE_INDEX].ToUnifiedFormat(chunk.size(), shipdate_format);
    chunk.data[accelerator_codegen::Q6LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(chunk.size(), discount_format);
    chunk.data[accelerator_codegen::Q6LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(chunk.size(), quantity_format);

    auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
    auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
    auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);

    duckdb::idx_t qualifying_rows = 0;
    for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
        const auto shipdate_index = shipdate_format.sel->get_index(row);
        const auto discount_index = discount_format.sel->get_index(row);
        const auto quantity_index = quantity_format.sel->get_index(row);
        if (shipdates[shipdate_index] >= parameters.start_date && shipdates[shipdate_index] < parameters.end_date &&
            discounts[discount_index] >= parameters.discount_lower_raw &&
            discounts[discount_index] <= parameters.discount_upper_raw &&
            quantities[quantity_index] < parameters.quantity_upper_raw) {
            qualifying_rows++;
        }
    }
    return qualifying_rows;
}

std::string AcceleratorQ1Sql(const accelerator_codegen::test_support::Q1ManifestEntry &entry) {
    return "SELECT * FROM accel_tpch_q1(" + std::to_string(entry.day_offset) + ")";
}

std::string AcceleratorQ6Sql(const accelerator_codegen::test_support::Q6ManifestEntry &entry) {
    return std::string("SELECT revenue FROM accel_tpch_q6(DATE '") + entry.start_date + "', " + entry.discount + ", " +
           entry.quantity + ")";
}

int64_t ParseScaledDecimal2(const std::string &text) {
    bool seen_dot = false;
    int fractional_digits = 0;
    int64_t value = 0;
    for (const auto ch : text) {
        if (ch == '.') {
            Require(!seen_dot, "invalid decimal literal: " + text);
            seen_dot = true;
            continue;
        }
        Require(ch >= '0' && ch <= '9', "invalid decimal literal: " + text);
        value = value * 10 + static_cast<int64_t>(ch - '0');
        if (seen_dot) {
            fractional_digits++;
            Require(fractional_digits <= 2, "decimal literal exceeds scale 2: " + text);
        }
    }
    while (fractional_digits < 2) {
        value *= 10;
        fractional_digits++;
    }
    return value;
}

accelerator_codegen::Q6Parameters MakeQ6Parameters(const accelerator_codegen::test_support::Q6ManifestEntry &entry) {
    accelerator_codegen::Q6Parameters parameters;
    parameters.start_date = duckdb::Date::FromString(entry.start_date);
    duckdb::interval_t one_year;
    one_year.months = duckdb::Interval::MONTHS_PER_YEAR;
    parameters.end_date = duckdb::Interval::Add(parameters.start_date, one_year);
    const auto discount_raw = ParseScaledDecimal2(entry.discount);
    const auto quantity_raw = ParseScaledDecimal2(entry.quantity);
    parameters.discount_lower_raw = discount_raw - 1;
    parameters.discount_upper_raw = discount_raw + 1;
    parameters.quantity_upper_raw = quantity_raw;
    return parameters;
}

accelerator_codegen::NativeScanSpec Q1FullRequiredScanNoFilters(const accelerator_codegen::Q1Parameters &parameters) {
    auto scan_spec = accelerator_codegen::CreateQ1ScanSpec(parameters);
    scan_spec.filters.clear();
    return scan_spec;
}

accelerator_codegen::NativeScanSpec Q1PredicateOnlyScanSpec() {
    accelerator_codegen::NativeScanSpec scan_spec;
    scan_spec.table_name = "lineitem";
    scan_spec.projected_columns = {
        {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
    };
    return scan_spec;
}

accelerator_codegen::NativeScanSpec Q6FullRequiredScanNoFilters(const accelerator_codegen::Q6Parameters &parameters) {
    (void)parameters;
    accelerator_codegen::NativeScanSpec scan_spec;
    scan_spec.table_name = "lineitem";
    scan_spec.projected_columns = {
        {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
        {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
    };
    scan_spec.filters.clear();
    return scan_spec;
}

accelerator_codegen::NativeScanSpec Q6PredicateOnlyScanSpec() {
    accelerator_codegen::NativeScanSpec scan_spec;
    scan_spec.table_name = "lineitem";
    scan_spec.projected_columns = {
        {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
        {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
    };
    return scan_spec;
}

accelerator_codegen::NativeScanSpec Q6FilteredScanSpec(const accelerator_codegen::Q6Parameters &parameters) {
    auto scan_spec = Q6FullRequiredScanNoFilters(parameters);
    scan_spec.filters = {
        {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::DATE(parameters.start_date)},
        {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
        {"l_discount", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
         duckdb::Value::DECIMAL(parameters.discount_lower_raw, 15, 2)},
        {"l_discount", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
         duckdb::Value::DECIMAL(parameters.discount_upper_raw, 15, 2)},
        {"l_quantity", duckdb::ExpressionType::COMPARE_LESSTHAN,
         duckdb::Value::DECIMAL(parameters.quantity_upper_raw, 15, 2)},
    };
    return scan_spec;
}

accelerator_codegen::NativeScanSpec Q1FilteredPredicateOnlyScanSpec(const accelerator_codegen::Q1Parameters &parameters) {
    auto scan_spec = Q1PredicateOnlyScanSpec();
    scan_spec.filters = {
        {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::DATE(parameters.cutoff_date)},
    };
    return scan_spec;
}

accelerator_codegen::NativeScanSpec Q6FilteredPredicateOnlyScanSpec(const accelerator_codegen::Q6Parameters &parameters) {
    auto scan_spec = Q6PredicateOnlyScanSpec();
    scan_spec.filters = {
        {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::DATE(parameters.start_date)},
        {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
        {"l_discount", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
         duckdb::Value::DECIMAL(parameters.discount_lower_raw, 15, 2)},
        {"l_discount", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
         duckdb::Value::DECIMAL(parameters.discount_upper_raw, 15, 2)},
        {"l_quantity", duckdb::ExpressionType::COMPARE_LESSTHAN,
         duckdb::Value::DECIMAL(parameters.quantity_upper_raw, 15, 2)},
    };
    return scan_spec;
}

StageRunResult RunQ1ScanDrain(duckdb::Connection &connection, const accelerator_codegen::Q1Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ1ScanSpec(parameters));
        auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        while (true) {
            const auto scan_start = Clock::now();
            const auto has_next = source->Next();
            result.scan_us += ElapsedUs(scan_start, Clock::now());
            if (!has_next) {
                break;
            }
            result.chunks++;
            result.rows += source->Chunk().size();
        }
        result.checksum = static_cast<uint64_t>(result.rows);
    });
    return result;
}

StageRunResult RunQ1PredicateOnly(duckdb::Connection &connection, const accelerator_codegen::Q1Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, Q1FilteredPredicateOnlyScanSpec(parameters));
        auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        while (true) {
            const auto scan_start = Clock::now();
            const auto has_next = source->Next();
            result.scan_us += ElapsedUs(scan_start, Clock::now());
            if (!has_next) {
                break;
            }
            auto &chunk = source->Chunk();
            result.chunks++;
            result.rows += chunk.size();
            const auto predicate_start = Clock::now();
            result.qualifying_rows += CountQ1QualifyingRows(chunk, parameters);
            result.predicate_us += ElapsedUs(predicate_start, Clock::now());
        }
        result.checksum = static_cast<uint64_t>(result.qualifying_rows);
    });
    return result;
}

StageRunResult RunQ1ReadChecksum(duckdb::Connection &connection, const accelerator_codegen::Q1Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ1ScanSpec(parameters));
        auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        while (true) {
            const auto scan_start = Clock::now();
            const auto has_next = source->Next();
            result.scan_us += ElapsedUs(scan_start, Clock::now());
            if (!has_next) {
                break;
            }
            auto &chunk = source->Chunk();
            result.chunks++;
            result.rows += chunk.size();
            const auto checksum_start = Clock::now();
            result.checksum = MixChecksum(result.checksum, ChecksumQ1AllColumns(chunk));
            result.checksum_us += ElapsedUs(checksum_start, Clock::now());
        }
    });
    return result;
}

StageRunResult RunQ1FusedNative(duckdb::Connection &connection, const accelerator_codegen::Q1Parameters &parameters);

StageRunResult RunQ1FullNative(duckdb::Connection &connection, const accelerator_codegen::Q1Parameters &parameters) {
    // Production Q1 path is fused ScanFilteredConsumer (chunk ConsumeInput kernel removed).
    return RunQ1FusedNative(connection, parameters);
}

StageRunResult RunQ1FusedNative(duckdb::Connection &connection, const accelerator_codegen::Q1Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ1ScanSpec(parameters));
        auto fused = duckdb::make_uniq<accelerator_codegen::Q1FusedKernel>(parameters);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        const auto scan_start = Clock::now();
        fused->RunFusedScan(context, *scan);
        const auto &diag = fused->Diagnostics();
        // Map fused timers into existing CSV columns:
        // scan_us ~= payload Select, kernel_us ~= aggregate,
        // predicate_us ~= MVCC+filter wall outside select/agg,
        // checksum_us carries residual_checks (must be 0), chunks carries chunks_materialized (must be 0).
        result.scan_us = static_cast<double>(diag.payload_decode_ns) / 1000.0;
        result.kernel_us = static_cast<double>(diag.aggregate_ns) / 1000.0;
        result.predicate_us = ElapsedUs(scan_start, Clock::now()) - result.scan_us - result.kernel_us;
        result.chunks = diag.chunks_materialized;
        result.rows = diag.qualifying_rows;
        result.qualifying_rows = diag.qualifying_rows;
        result.checksum_us = static_cast<double>(diag.residual_checks);

        Require(diag.residual_checks == 0, "fused path must not perform residual shipdate checks");
        Require(diag.chunks_materialized == 0, "fused path must not materialize aggregation DataChunks");
        Require(diag.used_fused_path, "fused path diagnostics not marked used");

        // Stash path mix into unused-looking fields for CSV readers: output_rows already used;
        // print path mix on stderr once when ACCEL_Q1_FUSED_LAYOUT_LOG=1.
        if (std::getenv("ACCEL_Q1_FUSED_LAYOUT_LOG") && *std::getenv("ACCEL_Q1_FUSED_LAYOUT_LOG") == '1') {
            const auto &L = diag.layouts;
            std::cerr << "fused_layout vectors=" << L.vectors_inspected
                      << " qty_dict=" << L.qty_dictionary << " qty_flat=" << L.qty_flat
                      << " rf_dict=" << L.rf_dictionary << " ls_dict=" << L.ls_dictionary
                      << " shared_sel_ptr=" << L.numeric_shared_sel_ptr
                      << " shared_sel_vals=" << L.numeric_shared_sel_values
                      << " divergent_sel=" << L.numeric_divergent_sel
                      << " dominant_eligible=" << L.dominant_eligible << " flat_eligible=" << L.flat_eligible
                      << " fixed_slot_vecs=" << diag.fixed_slot_vectors
                      << " flat_vecs=" << diag.flat_fast_vectors
                      << " scalar_vecs=" << diag.scalar_fallback_vectors
                      << " aligned=" << (diag.used_aligned_payload ? 1 : 0)
                      << " aligned_vecs=" << diag.aligned_payload_vectors
                      << " aligned_fixed_vecs=" << diag.aligned_fixed_slot_vectors
                      << " identity_vecs=" << diag.aligned_identity_filter_vectors
                      << " nested_vecs=" << diag.aligned_nested_index_vectors
                      << " compose_vecs=" << diag.aligned_compose_sel_vectors
                      << " payload_decode_us=" << (diag.payload_decode_ns / 1000.0)
                      << " aggregate_us=" << (diag.aggregate_ns / 1000.0)
                      << " sel_compose_us=" << (diag.sel_compose_ns / 1000.0)
                      << " sel_index_us=" << (diag.sel_index_ns / 1000.0)
                      << " agg_update_us=" << (diag.agg_update_ns / 1000.0) << "\n";
        }

        const auto merge_start = Clock::now();
        fused->Finalize();
        result.merge_us += ElapsedUs(merge_start, Clock::now());

        duckdb::DataChunk output;
        output.Initialize(context,
                          {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DECIMAL(38, 2),
                           duckdb::LogicalType::DECIMAL(38, 2), duckdb::LogicalType::DECIMAL(38, 4),
                           duckdb::LogicalType::DECIMAL(38, 6), duckdb::LogicalType::DOUBLE,
                           duckdb::LogicalType::DOUBLE, duckdb::LogicalType::DOUBLE, duckdb::LogicalType::BIGINT});
        const auto output_start = Clock::now();
        while (fused->ProduceOutput(output)) {
            result.output_rows += output.size();
            for (duckdb::idx_t row = 0; row < output.size(); row++) {
                for (duckdb::idx_t col = 0; col < output.ColumnCount(); col++) {
                    result.checksum = MixStringChecksum(result.checksum, output.GetValue(col, row).ToString());
                }
            }
            output.Reset();
        }
        result.output_us += ElapsedUs(output_start, Clock::now());
    });
    return result;
}

StageRunResult RunQ6ScanDrain(duckdb::Connection &connection, const accelerator_codegen::Q6Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, Q6FilteredScanSpec(parameters));
        auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        while (true) {
            const auto scan_start = Clock::now();
            const auto has_next = source->Next();
            result.scan_us += ElapsedUs(scan_start, Clock::now());
            if (!has_next) {
                break;
            }
            result.chunks++;
            result.rows += source->Chunk().size();
        }
        result.checksum = static_cast<uint64_t>(result.rows);
    });
    return result;
}

StageRunResult RunQ6PredicateOnly(duckdb::Connection &connection, const accelerator_codegen::Q6Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, Q6FilteredPredicateOnlyScanSpec(parameters));
        auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        while (true) {
            const auto scan_start = Clock::now();
            const auto has_next = source->Next();
            result.scan_us += ElapsedUs(scan_start, Clock::now());
            if (!has_next) {
                break;
            }
            auto &chunk = source->Chunk();
            result.chunks++;
            result.rows += chunk.size();
            const auto predicate_start = Clock::now();
            result.qualifying_rows += CountQ6QualifyingRows(chunk, parameters);
            result.predicate_us += ElapsedUs(predicate_start, Clock::now());
        }
        result.checksum = static_cast<uint64_t>(result.qualifying_rows);
    });
    return result;
}

StageRunResult RunQ6ReadChecksum(duckdb::Connection &connection, const accelerator_codegen::Q6Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, Q6FilteredScanSpec(parameters));
        auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        while (true) {
            const auto scan_start = Clock::now();
            const auto has_next = source->Next();
            result.scan_us += ElapsedUs(scan_start, Clock::now());
            if (!has_next) {
                break;
            }
            auto &chunk = source->Chunk();
            result.chunks++;
            result.rows += chunk.size();
            const auto checksum_start = Clock::now();
            result.checksum = MixChecksum(result.checksum, ChecksumQ6AllColumns(chunk));
            result.checksum_us += ElapsedUs(checksum_start, Clock::now());
        }
    });
    return result;
}

StageRunResult RunQ6FullNative(duckdb::Connection &connection, const accelerator_codegen::Q6Parameters &parameters) {
    StageRunResult result;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        const auto setup_start = Clock::now();
        auto scan = accelerator_codegen::BindNativeScan(context, Q6FilteredScanSpec(parameters));
        auto source = accelerator_codegen::CreateNativeChunkSource(context, *scan);
        auto kernel = accelerator_codegen::CreateQ6Kernel(parameters);
        result.setup_us = ElapsedUs(setup_start, Clock::now());

        while (true) {
            const auto scan_start = Clock::now();
            const auto has_next = source->Next();
            result.scan_us += ElapsedUs(scan_start, Clock::now());
            if (!has_next) {
                break;
            }
            auto &chunk = source->Chunk();
            result.chunks++;
            result.rows += chunk.size();
            result.qualifying_rows += chunk.size();

            const auto kernel_start = Clock::now();
            kernel->ConsumeInput(0, chunk);
            result.kernel_us += ElapsedUs(kernel_start, Clock::now());
        }

        const auto merge_start = Clock::now();
        kernel->Finalize();
        result.merge_us += ElapsedUs(merge_start, Clock::now());

        duckdb::DataChunk output;
        output.Initialize(context, {duckdb::LogicalType::DECIMAL(38, 4)});
        const auto output_start = Clock::now();
        while (kernel->ProduceOutput(output)) {
            result.output_rows += output.size();
            for (duckdb::idx_t row = 0; row < output.size(); row++) {
                result.checksum = MixStringChecksum(result.checksum, output.GetValue(0, row).ToString());
            }
            output.Reset();
        }
        result.output_us += ElapsedUs(output_start, Clock::now());
    });
    return result;
}

void PrintStageRow(const std::string &query_name,
                   const std::string &label,
                   const std::string &stage_name,
                   const StageStats &stats) {
    std::cout << query_name << ',' << label << ',' << stage_name << ',' << std::fixed << std::setprecision(2)
              << stats.total.average_us << ',' << stats.total.median_us << ','
              << stats.sample.checksum << ','
              << stats.sample.chunks << ','
              << stats.sample.rows << ','
              << stats.sample.qualifying_rows << ','
              << stats.sample.output_rows << ','
              << stats.setup.average_us << ','
              << stats.scan.average_us << ','
              << stats.predicate.average_us << ','
              << stats.checksum.average_us << ','
              << stats.kernel.average_us << ','
              << stats.merge.average_us << ','
              << stats.output.average_us << "\n";
}

void PrintSqlRow(const std::string &query_name,
                 const std::string &label,
                 const std::string &stage_name,
                 const TimedSqlResult &stats) {
    std::cout << query_name << ',' << label << ',' << stage_name << ',' << std::fixed << std::setprecision(2)
              << stats.timing.average_us << ',' << stats.timing.median_us << ','
              << stats.checksum << ",0,0,0,0,0,0,0,0,0,0,0\n";
}

} // namespace

int main() {
    try {
        const int warmup_iterations = GetEnvInt("ACCEL_BENCH_WARMUP", 3);
        const int measured_iterations = GetEnvInt("ACCEL_BENCH_ITERS", 10);
        const auto filters = SplitCommaSeparated(GetEnvString("ACCEL_BENCH_FILTER"));

        duckdb::DuckDB database(GetTpchDbPath().c_str());
        duckdb::Connection connection(database);
        accelerator_codegen::RegisterAcceleratorFunctions(connection);
        auto pragma_result = connection.Query("PRAGMA threads=1");
        Require(pragma_result != nullptr && !pragma_result->HasError(), "failed to set PRAGMA threads=1");

        std::cout << "query,label,stage,average_us,median_us,checksum,chunks,rows,qualifying_rows,output_rows,"
                     "setup_us,scan_us,predicate_us,checksum_us,kernel_us,merge_us,output_us\n";

        for (const auto &entry : accelerator_codegen::test_support::kQ1Manifest) {
            if (!MatchesFilter(entry.label, filters)) {
                continue;
            }

            const auto parameters = accelerator_codegen::MakeQ1Parameters(entry.day_offset);
            PrintSqlRow("q1", entry.label, "duckdb_reference_sql",
                        BenchmarkSql(connection, ReadFile(entry.sql_path), warmup_iterations, measured_iterations));
            PrintSqlRow("q1", entry.label, "accelerator_sql",
                        BenchmarkSql(connection, AcceleratorQ1Sql(entry), warmup_iterations, measured_iterations));
            PrintStageRow("q1", entry.label, "scan_drain_required_cols",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ1ScanDrain(connection, parameters); }));
            PrintStageRow("q1", entry.label, "predicate_only_scan",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ1PredicateOnly(connection, parameters); }));
            PrintStageRow("q1", entry.label, "read_checksum_required_cols",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ1ReadChecksum(connection, parameters); }));
            PrintStageRow("q1", entry.label, "full_accelerator_native",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ1FullNative(connection, parameters); }));
            // Direct fused native stages (bypass SQL UDF). Ablation of scalar vs fixed-slot vs
            // aligned requires -DACCEL_EXPERIMENTAL_Q1_OPTS; in production builds all three call
            // the same hardcoded fused+aligned+direct-index path — keep one representative label.
            PrintStageRow("q1", entry.label, "full_accelerator_fused_aligned_direct",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ1FusedNative(connection, parameters); }));
            // accelerator_sql already exercises the production fused SQL path (hardcoded on).
            // Removed obsolete accelerator_sql_fused_* rows that setenv'd ACCEL_Q1_* toggles —
            // those env vars are ignored unless built with ACCEL_EXPERIMENTAL_Q1_OPTS.
        }

        for (const auto &entry : accelerator_codegen::test_support::kQ6Manifest) {
            if (!MatchesFilter(entry.label, filters)) {
                continue;
            }

            const auto parameters = MakeQ6Parameters(entry);
            PrintSqlRow("q6", entry.label, "duckdb_reference_sql",
                        BenchmarkSql(connection, ReadFile(entry.sql_path), warmup_iterations, measured_iterations));
            PrintSqlRow("q6", entry.label, "accelerator_sql",
                        BenchmarkSql(connection, AcceleratorQ6Sql(entry), warmup_iterations, measured_iterations));
            PrintStageRow("q6", entry.label, "scan_drain_required_cols",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ6ScanDrain(connection, parameters); }));
            PrintStageRow("q6", entry.label, "predicate_only_scan",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ6PredicateOnly(connection, parameters); }));
            PrintStageRow("q6", entry.label, "read_checksum_required_cols",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ6ReadChecksum(connection, parameters); }));
            PrintStageRow("q6", entry.label, "full_accelerator_native",
                          BenchmarkStage(warmup_iterations, measured_iterations,
                                         [&]() { return RunQ6FullNative(connection, parameters); }));
        }

        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "benchmark_tpch_breakdown failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
