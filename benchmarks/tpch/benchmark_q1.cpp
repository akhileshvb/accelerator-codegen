#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/register.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "test_support/q1_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb.hpp"
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

void ExecuteAndConsume(duckdb::Connection &connection, const std::string &sql) {
    auto result = connection.Query(sql);
    Require(result != nullptr, "query returned null result");
    Require(!result->HasError(), result->GetError());
    auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();

    std::size_t bytes_seen = 0;
    for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
        for (duckdb::idx_t col = 0; col < materialized.ColumnCount(); col++) {
            bytes_seen += materialized.GetValue(col, row).ToString().size();
        }
    }
    Require(bytes_seen != static_cast<std::size_t>(-1), "unreachable bytes_seen guard");
}

struct TimingStats {
    double average_us = 0.0;
    double median_us = 0.0;
};

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
        samples_us.push_back(
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
    }

    TimingStats stats;
    for (const auto sample : samples_us) {
        stats.average_us += sample;
    }
    stats.average_us /= static_cast<double>(samples_us.size());

    std::sort(samples_us.begin(), samples_us.end());
    const auto middle = samples_us.size() / 2;
    stats.median_us = samples_us.size() % 2 == 0 ? (samples_us[middle - 1] + samples_us[middle]) / 2.0
                                                  : samples_us[middle];
    return stats;
}

TimingStats BenchmarkQuery(duckdb::Connection &connection,
                           const std::string &sql,
                           int warmup_iterations,
                           int measured_iterations) {
    return BenchmarkOperation(warmup_iterations, measured_iterations,
                              [&]() { ExecuteAndConsume(connection, sql); });
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

std::string AcceleratorQ1Sql(const accelerator_codegen::test_support::Q1ManifestEntry &entry) {
    return "SELECT * FROM accel_tpch_q1(" + std::to_string(entry.day_offset) + ")";
}

struct KernelStageSample {
    accelerator_codegen::Q1KernelBenchmarkResult result;
};

KernelStageSample ExecuteKernelStageSample(duckdb::Connection &connection,
                                           const accelerator_codegen::Q1Parameters &parameters,
                                           accelerator_codegen::Q1KernelBenchmarkMode mode,
                                           bool use_chunk_local_partials) {
    KernelStageSample sample;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        auto bound_scan = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ1ScanSpec(parameters));
        auto chunk_source = accelerator_codegen::CreateNativeChunkSource(context, *bound_scan);
        while (chunk_source->Next()) {
            auto &chunk = chunk_source->Chunk();
            auto chunk_result =
                accelerator_codegen::BenchmarkQ1KernelChunk(chunk, parameters, mode, use_chunk_local_partials, true);
            sample.result.checksum += chunk_result.checksum;
            sample.result.counters.total_chunks += chunk_result.counters.total_chunks;
            sample.result.counters.dominant_fast_path_chunks += chunk_result.counters.dominant_fast_path_chunks;
            sample.result.counters.total_rows += chunk_result.counters.total_rows;
            sample.result.counters.dominant_fast_path_rows += chunk_result.counters.dominant_fast_path_rows;
            sample.result.counters.qualifying_rows += chunk_result.counters.qualifying_rows;
        }
    });
    return sample;
}

uint64_t ExecuteKernelStageTimed(duckdb::Connection &connection,
                                 const accelerator_codegen::Q1Parameters &parameters,
                                 accelerator_codegen::Q1KernelBenchmarkMode mode,
                                 bool use_chunk_local_partials) {
    uint64_t checksum = 0;
    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        auto bound_scan = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ1ScanSpec(parameters));
        auto chunk_source = accelerator_codegen::CreateNativeChunkSource(context, *bound_scan);
        while (chunk_source->Next()) {
            auto &chunk = chunk_source->Chunk();
            auto chunk_result =
                accelerator_codegen::BenchmarkQ1KernelChunk(chunk, parameters, mode, use_chunk_local_partials, false);
            checksum += chunk_result.checksum;
        }
    });
    return checksum;
}

struct KernelStageBenchmarkResult {
    TimingStats timing;
    KernelStageSample sample;
};

KernelStageBenchmarkResult BenchmarkKernelStage(duckdb::Connection &connection,
                                                const accelerator_codegen::Q1Parameters &parameters,
                                                accelerator_codegen::Q1KernelBenchmarkMode mode,
                                                bool use_chunk_local_partials,
                                                int warmup_iterations,
                                                int measured_iterations) {
    for (int i = 0; i < warmup_iterations; i++) {
        auto warmup = ExecuteKernelStageTimed(connection, parameters, mode, use_chunk_local_partials);
        Require(warmup != std::numeric_limits<uint64_t>::max(), "unreachable warmup guard");
    }

    KernelStageBenchmarkResult result;
    result.sample = ExecuteKernelStageSample(connection, parameters, mode, use_chunk_local_partials);
    result.timing = BenchmarkOperation(warmup_iterations, measured_iterations, [&]() {
        auto measured = ExecuteKernelStageTimed(connection, parameters, mode, use_chunk_local_partials);
        Require(measured != std::numeric_limits<uint64_t>::max(), "unreachable measured guard");
    });
    return result;
}

TimingStats BenchmarkSinkOnly(uint64_t seed_value, int warmup_iterations, int measured_iterations) {
    volatile uint64_t sink = 0;
    return BenchmarkOperation(warmup_iterations, measured_iterations, [&]() {
        sink += seed_value;
    });
}

struct PathAggregate {
    std::vector<double> average_us;
    std::vector<double> median_us;
};

double MedianOfVector(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

std::string StageName(accelerator_codegen::Q1KernelBenchmarkMode mode) {
    switch (mode) {
    case accelerator_codegen::Q1KernelBenchmarkMode::ResidualOnly:
        return "kernel_scan_residual";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantAdmissionOnly:
        return "dominant_admission_only";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantIndicesOnly:
        return "dominant_indices_only";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantIndicesAndValidity:
        return "dominant_indices_validity";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantIndicesValidityLength:
        return "dominant_indices_validity_length";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantReadChars:
        return "dominant_read_chars";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantPackKey:
        return "dominant_pack_key";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantByteCacheBuildOnly:
        return "dominant_byte_cache_build_only";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantByteCache:
        return "dominant_byte_cache";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantPackedKeyCacheBuildOnly:
        return "dominant_packed_key_cache_build_only";
    case accelerator_codegen::Q1KernelBenchmarkMode::DominantPackedKeyCache:
        return "dominant_packed_key_cache";
    case accelerator_codegen::Q1KernelBenchmarkMode::ReuseChunkLocalsVector:
        return "reuse_chunk_locals_vector";
    case accelerator_codegen::Q1KernelBenchmarkMode::ReuseChunkLocalsFixed:
        return "reuse_chunk_locals_fixed";
    case accelerator_codegen::Q1KernelBenchmarkMode::DecodeAndPackOnly:
        return "kernel_decode_pack";
    case accelerator_codegen::Q1KernelBenchmarkMode::SimpleAggregates:
        return "kernel_simple_aggregates";
    case accelerator_codegen::Q1KernelBenchmarkMode::DerivedAggregates:
        return "kernel_derived_aggregates";
    case accelerator_codegen::Q1KernelBenchmarkMode::Full:
        return "kernel_full";
    }
    throw std::runtime_error("unhandled Q1 benchmark mode");
}

struct DominantStringDictionaryStats {
    uint64_t dominant_chunks = 0;
    uint64_t returnflag_size_fallback_chunks = 0;
    uint64_t linestatus_size_fallback_chunks = 0;
    uint64_t returnflag_child_cardinality_total = 0;
    uint64_t linestatus_child_cardinality_total = 0;
    uint64_t returnflag_distinct_indexes_total = 0;
    uint64_t linestatus_distinct_indexes_total = 0;
    duckdb::idx_t returnflag_child_cardinality_min = std::numeric_limits<duckdb::idx_t>::max();
    duckdb::idx_t returnflag_child_cardinality_max = 0;
    duckdb::idx_t linestatus_child_cardinality_min = std::numeric_limits<duckdb::idx_t>::max();
    duckdb::idx_t linestatus_child_cardinality_max = 0;
    duckdb::idx_t returnflag_distinct_indexes_min = std::numeric_limits<duckdb::idx_t>::max();
    duckdb::idx_t returnflag_distinct_indexes_max = 0;
    duckdb::idx_t linestatus_distinct_indexes_min = std::numeric_limits<duckdb::idx_t>::max();
    duckdb::idx_t linestatus_distinct_indexes_max = 0;
    uint64_t selected_null_rows = 0;
    uint64_t selected_non_single_char_rows = 0;
    uint64_t returnflag_reused_chunks = 0;
    uint64_t linestatus_reused_chunks = 0;
    uint64_t returnflag_same_size_chunks = 0;
    uint64_t linestatus_same_size_chunks = 0;
    uint64_t both_identity_reused_chunks = 0;
    uint64_t both_identity_and_size_reused_chunks = 0;
    bool saw_returnflag_dictionary_id = false;
    bool saw_linestatus_dictionary_id = false;
};

void UpdateMinMax(duckdb::idx_t value, duckdb::idx_t &min_value, duckdb::idx_t &max_value) {
    min_value = std::min(min_value, value);
    max_value = std::max(max_value, value);
}

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
    if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
        return false;
    }
    return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

duckdb::idx_t DictionarySizeOrReferencedSpan(const duckdb::Vector &vector,
                                             const duckdb::SelectionVector &sel,
                                             duckdb::idx_t count,
                                             bool &used_fallback) {
    const auto dictionary_size = duckdb::DictionaryVector::DictionarySize(vector);
    if (dictionary_size.IsValid()) {
        used_fallback = false;
        return dictionary_size.GetIndex();
    }
    used_fallback = true;
    duckdb::idx_t max_index = 0;
    for (duckdb::idx_t row = 0; row < count; row++) {
        max_index = std::max(max_index, static_cast<duckdb::idx_t>(sel.get_index(row)));
    }
    return count == 0 ? 0 : max_index + 1;
}

DominantStringDictionaryStats InspectDominantStringDictionaryStructure(
    duckdb::Connection &connection, const accelerator_codegen::Q1Parameters &parameters) {
    DominantStringDictionaryStats stats;
    std::string previous_returnflag_dictionary_id;
    std::string previous_linestatus_dictionary_id;
    const duckdb::string_t *previous_returnflag_child = nullptr;
    const duckdb::string_t *previous_linestatus_child = nullptr;
    bool saw_previous_returnflag_child = false;
    bool saw_previous_linestatus_child = false;
    duckdb::idx_t previous_returnflag_dictionary_size = 0;
    duckdb::idx_t previous_linestatus_dictionary_size = 0;
    bool saw_previous_returnflag_size = false;
    bool saw_previous_linestatus_size = false;

    WithTransaction(connection, [&](duckdb::ClientContext &context) {
        auto bound_scan = accelerator_codegen::BindNativeScan(context, accelerator_codegen::CreateQ1ScanSpec(parameters));
        auto chunk_source = accelerator_codegen::CreateNativeChunkSource(context, *bound_scan);
        while (chunk_source->Next()) {
            auto &chunk = chunk_source->Chunk();
            auto &returnflag_vector = chunk.data[accelerator_codegen::Q1LineitemChunkLayout::RETURNFLAG_INDEX];
            auto &linestatus_vector = chunk.data[accelerator_codegen::Q1LineitemChunkLayout::LINESTATUS_INDEX];
            if (!IsDictionaryOverFlat(returnflag_vector) || !IsDictionaryOverFlat(linestatus_vector)) {
                continue;
            }

            const auto &returnflag_sel = duckdb::DictionaryVector::SelVector(returnflag_vector);
            const auto &linestatus_sel = duckdb::DictionaryVector::SelVector(linestatus_vector);
            bool returnflag_used_fallback = false;
            bool linestatus_used_fallback = false;
            const auto returnflag_dictionary_size =
                DictionarySizeOrReferencedSpan(returnflag_vector, returnflag_sel, chunk.size(), returnflag_used_fallback);
            const auto linestatus_dictionary_size =
                DictionarySizeOrReferencedSpan(linestatus_vector, linestatus_sel, chunk.size(), linestatus_used_fallback);
            stats.returnflag_size_fallback_chunks += returnflag_used_fallback ? 1 : 0;
            stats.linestatus_size_fallback_chunks += linestatus_used_fallback ? 1 : 0;
            stats.dominant_chunks += 1;
            stats.returnflag_child_cardinality_total += returnflag_dictionary_size;
            stats.linestatus_child_cardinality_total += linestatus_dictionary_size;
            UpdateMinMax(returnflag_dictionary_size, stats.returnflag_child_cardinality_min,
                         stats.returnflag_child_cardinality_max);
            UpdateMinMax(linestatus_dictionary_size, stats.linestatus_child_cardinality_min,
                         stats.linestatus_child_cardinality_max);

            auto &returnflag_child = duckdb::DictionaryVector::Child(returnflag_vector);
            auto &linestatus_child = duckdb::DictionaryVector::Child(linestatus_vector);
            auto returnflags = duckdb::FlatVector::GetData<duckdb::string_t>(returnflag_child);
            auto linestatuses = duckdb::FlatVector::GetData<duckdb::string_t>(linestatus_child);
            auto &returnflag_validity = duckdb::FlatVector::Validity(returnflag_child);
            auto &linestatus_validity = duckdb::FlatVector::Validity(linestatus_child);

            const auto &returnflag_dictionary_id = duckdb::DictionaryVector::DictionaryId(returnflag_vector);
            const auto &linestatus_dictionary_id = duckdb::DictionaryVector::DictionaryId(linestatus_vector);
            bool returnflag_identity_reused = false;
            bool linestatus_identity_reused = false;
            if (!returnflag_dictionary_id.empty()) {
                stats.saw_returnflag_dictionary_id = true;
                if (!previous_returnflag_dictionary_id.empty() &&
                    previous_returnflag_dictionary_id == returnflag_dictionary_id) {
                    stats.returnflag_reused_chunks += 1;
                    returnflag_identity_reused = true;
                }
                previous_returnflag_dictionary_id = returnflag_dictionary_id;
            } else if (saw_previous_returnflag_child && previous_returnflag_child == returnflags) {
                stats.returnflag_reused_chunks += 1;
                returnflag_identity_reused = true;
            }
            if (!linestatus_dictionary_id.empty()) {
                stats.saw_linestatus_dictionary_id = true;
                if (!previous_linestatus_dictionary_id.empty() &&
                    previous_linestatus_dictionary_id == linestatus_dictionary_id) {
                    stats.linestatus_reused_chunks += 1;
                    linestatus_identity_reused = true;
                }
                previous_linestatus_dictionary_id = linestatus_dictionary_id;
            } else if (saw_previous_linestatus_child && previous_linestatus_child == linestatuses) {
                stats.linestatus_reused_chunks += 1;
                linestatus_identity_reused = true;
            }
            if (returnflag_identity_reused && linestatus_identity_reused) {
                stats.both_identity_reused_chunks += 1;
            }
            if (saw_previous_returnflag_size && previous_returnflag_dictionary_size == returnflag_dictionary_size) {
                stats.returnflag_same_size_chunks += 1;
            }
            if (saw_previous_linestatus_size && previous_linestatus_dictionary_size == linestatus_dictionary_size) {
                stats.linestatus_same_size_chunks += 1;
            }
            if (returnflag_identity_reused && linestatus_identity_reused &&
                saw_previous_returnflag_size && previous_returnflag_dictionary_size == returnflag_dictionary_size &&
                saw_previous_linestatus_size && previous_linestatus_dictionary_size == linestatus_dictionary_size) {
                stats.both_identity_and_size_reused_chunks += 1;
            }
            previous_returnflag_child = returnflags;
            previous_linestatus_child = linestatuses;
            saw_previous_returnflag_child = true;
            saw_previous_linestatus_child = true;
            previous_returnflag_dictionary_size = returnflag_dictionary_size;
            previous_linestatus_dictionary_size = linestatus_dictionary_size;
            saw_previous_returnflag_size = true;
            saw_previous_linestatus_size = true;

            std::vector<uint8_t> returnflag_seen(returnflag_dictionary_size, 0);
            std::vector<uint8_t> linestatus_seen(linestatus_dictionary_size, 0);
            duckdb::idx_t returnflag_distinct = 0;
            duckdb::idx_t linestatus_distinct = 0;
            for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
                const auto returnflag_index = returnflag_sel.get_index(row);
                const auto linestatus_index = linestatus_sel.get_index(row);
                if (!returnflag_seen[returnflag_index]) {
                    returnflag_seen[returnflag_index] = 1;
                    returnflag_distinct += 1;
                    if (!returnflag_validity.AllValid() && !returnflag_validity.RowIsValidUnsafe(returnflag_index)) {
                        stats.selected_null_rows += 1;
                    } else if (returnflags[returnflag_index].GetSize() != 1) {
                        stats.selected_non_single_char_rows += 1;
                    }
                }
                if (!linestatus_seen[linestatus_index]) {
                    linestatus_seen[linestatus_index] = 1;
                    linestatus_distinct += 1;
                    if (!linestatus_validity.AllValid() && !linestatus_validity.RowIsValidUnsafe(linestatus_index)) {
                        stats.selected_null_rows += 1;
                    } else if (linestatuses[linestatus_index].GetSize() != 1) {
                        stats.selected_non_single_char_rows += 1;
                    }
                }
            }
            stats.returnflag_distinct_indexes_total += returnflag_distinct;
            stats.linestatus_distinct_indexes_total += linestatus_distinct;
            UpdateMinMax(returnflag_distinct, stats.returnflag_distinct_indexes_min,
                         stats.returnflag_distinct_indexes_max);
            UpdateMinMax(linestatus_distinct, stats.linestatus_distinct_indexes_min,
                         stats.linestatus_distinct_indexes_max);
        }
    });

    return stats;
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
        connection.Query("PRAGMA threads=1");

        std::unordered_map<std::string, PathAggregate> aggregates;

        std::cout << "label,path,average_us,median_us,checksum,qualifying_rows,total_chunks,dominant_fast_path_chunks,total_rows,dominant_fast_path_rows\n";
        for (const auto &entry : accelerator_codegen::test_support::kQ1Manifest) {
            const std::string label = entry.label;
            if (!MatchesFilter(label, filters)) {
                continue;
            }

            const auto parameters = accelerator_codegen::MakeQ1Parameters(entry.day_offset);
            const auto reference_sql = ReadFile(entry.sql_path);
            const auto accel_sql = AcceleratorQ1Sql(entry);
            const auto structure = InspectDominantStringDictionaryStructure(connection, parameters);

            const auto reference = BenchmarkQuery(connection, reference_sql, warmup_iterations, measured_iterations);
            const auto accel_full = BenchmarkQuery(connection, accel_sql, warmup_iterations, measured_iterations);
            const auto sink_only = BenchmarkSinkOnly(static_cast<uint64_t>(entry.day_offset) + 1, warmup_iterations,
                                                     measured_iterations);

            auto record_query = [&](const std::string &path, const TimingStats &timing) {
                std::cout << label << ',' << path << ',' << std::fixed << std::setprecision(2) << timing.average_us << ','
                          << timing.median_us << ",,,,,,\n";
                aggregates[path].average_us.push_back(timing.average_us);
                aggregates[path].median_us.push_back(timing.median_us);
            };

            record_query("duckdb_reference_q1", reference);
            record_query("accel_full_q1", accel_full);
            record_query("benchmark_sink_only", sink_only);
            if (structure.dominant_chunks > 0) {
                std::cout << label << ",dominant_string_structure,,,,"
                          << "null_rows=" << structure.selected_null_rows << ";"
                          << "non_single_char_rows=" << structure.selected_non_single_char_rows << ";"
                          << "returnflag_child_avg="
                          << (static_cast<double>(structure.returnflag_child_cardinality_total) /
                              static_cast<double>(structure.dominant_chunks))
                          << ";returnflag_child_min=" << structure.returnflag_child_cardinality_min
                          << ";returnflag_child_max=" << structure.returnflag_child_cardinality_max
                          << ";linestatus_child_avg="
                          << (static_cast<double>(structure.linestatus_child_cardinality_total) /
                              static_cast<double>(structure.dominant_chunks))
                          << ";linestatus_child_min=" << structure.linestatus_child_cardinality_min
                          << ";linestatus_child_max=" << structure.linestatus_child_cardinality_max
                          << ";returnflag_distinct_avg="
                          << (static_cast<double>(structure.returnflag_distinct_indexes_total) /
                              static_cast<double>(structure.dominant_chunks))
                          << ";returnflag_distinct_min=" << structure.returnflag_distinct_indexes_min
                          << ";returnflag_distinct_max=" << structure.returnflag_distinct_indexes_max
                          << ";linestatus_distinct_avg="
                          << (static_cast<double>(structure.linestatus_distinct_indexes_total) /
                              static_cast<double>(structure.dominant_chunks))
                          << ";linestatus_distinct_min=" << structure.linestatus_distinct_indexes_min
                          << ";linestatus_distinct_max=" << structure.linestatus_distinct_indexes_max
                          << ";returnflag_size_fallback_chunks=" << structure.returnflag_size_fallback_chunks
                          << ";linestatus_size_fallback_chunks=" << structure.linestatus_size_fallback_chunks
                          << ";returnflag_reused_chunks=" << structure.returnflag_reused_chunks
                          << ";linestatus_reused_chunks=" << structure.linestatus_reused_chunks
                          << ";returnflag_same_size_chunks=" << structure.returnflag_same_size_chunks
                          << ";linestatus_same_size_chunks=" << structure.linestatus_same_size_chunks
                          << ";both_identity_reused_chunks=" << structure.both_identity_reused_chunks
                          << ";both_identity_and_size_reused_chunks=" << structure.both_identity_and_size_reused_chunks
                          << "\n";
            }

            const std::array stages = {
                accelerator_codegen::Q1KernelBenchmarkMode::DominantAdmissionOnly,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantIndicesOnly,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantIndicesAndValidity,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantIndicesValidityLength,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantReadChars,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantPackKey,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantByteCacheBuildOnly,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantByteCache,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantPackedKeyCacheBuildOnly,
                accelerator_codegen::Q1KernelBenchmarkMode::DominantPackedKeyCache,
                accelerator_codegen::Q1KernelBenchmarkMode::ReuseChunkLocalsVector,
                accelerator_codegen::Q1KernelBenchmarkMode::ReuseChunkLocalsFixed,
                accelerator_codegen::Q1KernelBenchmarkMode::ResidualOnly,
                accelerator_codegen::Q1KernelBenchmarkMode::DecodeAndPackOnly,
                accelerator_codegen::Q1KernelBenchmarkMode::SimpleAggregates,
                accelerator_codegen::Q1KernelBenchmarkMode::DerivedAggregates,
                accelerator_codegen::Q1KernelBenchmarkMode::Full,
            };

            for (const auto stage : stages) {
                const auto benchmark =
                    BenchmarkKernelStage(connection, parameters, stage, false, warmup_iterations, measured_iterations);
                const auto path = StageName(stage);
                std::cout << label << ',' << path << ',' << std::fixed << std::setprecision(2)
                          << benchmark.timing.average_us << ',' << benchmark.timing.median_us << ','
                          << benchmark.sample.result.checksum << ','
                          << benchmark.sample.result.counters.qualifying_rows << ','
                          << benchmark.sample.result.counters.total_chunks << ','
                          << benchmark.sample.result.counters.dominant_fast_path_chunks << ','
                          << benchmark.sample.result.counters.total_rows << ','
                          << benchmark.sample.result.counters.dominant_fast_path_rows << "\n";
                aggregates[path].average_us.push_back(benchmark.timing.average_us);
                aggregates[path].median_us.push_back(benchmark.timing.median_us);
            }

            const auto full_chunk_locals =
                BenchmarkKernelStage(connection, parameters, accelerator_codegen::Q1KernelBenchmarkMode::Full, true,
                                     warmup_iterations, measured_iterations);
            std::cout << label << ",kernel_full_chunk_locals," << std::fixed << std::setprecision(2)
                      << full_chunk_locals.timing.average_us << ',' << full_chunk_locals.timing.median_us << ','
                      << full_chunk_locals.sample.result.checksum << ','
                      << full_chunk_locals.sample.result.counters.qualifying_rows << ','
                      << full_chunk_locals.sample.result.counters.total_chunks << ','
                      << full_chunk_locals.sample.result.counters.dominant_fast_path_chunks << ','
                      << full_chunk_locals.sample.result.counters.total_rows << ','
                      << full_chunk_locals.sample.result.counters.dominant_fast_path_rows << "\n";
            aggregates["kernel_full_chunk_locals"].average_us.push_back(full_chunk_locals.timing.average_us);
            aggregates["kernel_full_chunk_locals"].median_us.push_back(full_chunk_locals.timing.median_us);
        }

        Require(!aggregates.empty(), "benchmark filter selected no Q1 instances");
        for (const auto &entry : aggregates) {
            double average_of_averages = 0.0;
            for (const auto value : entry.second.average_us) {
                average_of_averages += value;
            }
            average_of_averages /= static_cast<double>(entry.second.average_us.size());
            std::cout << "summary_avg," << entry.first << ',' << std::fixed << std::setprecision(2)
                      << average_of_averages << ",,,,,,\n";
            std::cout << "summary_median," << entry.first << ',' << MedianOfVector(entry.second.average_us)
                      << ",,,,,,\n";
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &ex) {
        std::cerr << "benchmark_q1 failed: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }
}
