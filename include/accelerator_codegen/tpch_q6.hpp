#pragma once

#include <cstdint>

#include "accelerator_codegen/chunk_source.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace accelerator_codegen {

struct Q6Parameters {
    duckdb::date_t start_date;
    duckdb::date_t end_date;
    int64_t discount_lower_raw;
    int64_t discount_upper_raw;
    int64_t quantity_upper_raw;
};

struct DecimalSumResult {
    duckdb::hugeint_t value;
    bool is_null;
};

struct Q6LineitemChunkLayout {
    static constexpr duckdb::idx_t SHIPDATE_INDEX = 0;
    static constexpr duckdb::idx_t DISCOUNT_INDEX = 1;
    static constexpr duckdb::idx_t QUANTITY_INDEX = 2;
    static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 3;
};

DecimalSumResult ExecuteQ6(ChunkSource &lineitem_scan, const Q6Parameters &params);

} // namespace accelerator_codegen
