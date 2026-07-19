#pragma once

#include <cstdint>
#include <memory>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
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
NativeScanSpec CreateQ1ScanSpec(const Q1Parameters &parameters);

} // namespace accelerator_codegen
