#pragma once

#include <cstdint>
#include <memory>

#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace accelerator_codegen {

struct Q1Parameters final : public QueryParameters {
    int32_t day_offset = 0;
    duckdb::date_t cutoff_date;

    std::unique_ptr<QueryParameters> Copy() const override {
        return std::make_unique<Q1Parameters>(*this);
    }

    bool Equals(const QueryParameters &other) const override {
        auto &rhs = dynamic_cast<const Q1Parameters &>(other);
        return day_offset == rhs.day_offset && cutoff_date == rhs.cutoff_date;
    }
};

struct Q1LineitemChunkLayout {
    static constexpr duckdb::idx_t SHIPDATE_INDEX = 0;
    static constexpr duckdb::idx_t QUANTITY_INDEX = 1;
    static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 2;
    static constexpr duckdb::idx_t DISCOUNT_INDEX = 3;
    static constexpr duckdb::idx_t TAX_INDEX = 4;
    static constexpr duckdb::idx_t RETURNFLAG_INDEX = 5;
    static constexpr duckdb::idx_t LINESTATUS_INDEX = 6;
};

Q1Parameters MakeQ1Parameters(int32_t day_offset);
//! push_shipdate_filter=false drops the l_shipdate <= cutoff NativeFilterSpec: the single-threaded kernel then
//! applies the predicate itself with SIMD (much cheaper than DuckDB's filtered scan, since ~98% of rows qualify);
//! the parallel kernel uses the pushed filter (RunParallelFilteredScan).
NativeScanSpec CreateQ1ScanSpec(const Q1Parameters &parameters, bool push_shipdate_filter = true);
duckdb::unique_ptr<KernelInstance> CreateQ1Kernel(const Q1Parameters &parameters);
duckdb::unique_ptr<KernelInstance> CreateQ1ParallelKernel(const Q1Parameters &parameters);

} // namespace accelerator_codegen
