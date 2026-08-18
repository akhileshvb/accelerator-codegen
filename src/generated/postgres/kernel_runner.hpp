// GENERATED BACKEND
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "accelerator_codegen/runtime/postgres/row_writer.hpp"
#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q6/base/query.hpp"
#include "duckdb/common/types.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

using PostgresChunkScanner = std::function<void(
    const NativeScanSpec &scan_spec,
    const std::function<void(::duckdb::DataChunk &chunk)> &consumer)>;

using OutputRowWriter = PostgresRowWriter;

const std::vector<::duckdb::LogicalType> &TpchQ3OutputTypes();
const std::vector<::duckdb::LogicalType> &TpchQ6OutputTypes();
const std::vector<::duckdb::LogicalType> &TpchQ10OutputTypes();
const std::vector<::duckdb::LogicalType> &TpchQ12OutputTypes();
const std::vector<::duckdb::LogicalType> &TpchQ14OutputTypes();
const std::vector<::duckdb::LogicalType> &SecEdgarQ1OutputTypes();
const std::vector<::duckdb::LogicalType> &SecEdgarQ2OutputTypes();
const std::vector<::duckdb::LogicalType> &SecEdgarQ3OutputTypes();
const std::vector<::duckdb::LogicalType> &SecEdgarQ4OutputTypes();
const std::vector<::duckdb::LogicalType> &SecEdgarQ6OutputTypes();
const std::vector<::duckdb::LogicalType> &SecEdgarQ24OutputTypes();

void RunChunkKernel(std::unique_ptr<KernelInstance> kernel,
                    const std::vector<NativeScanSpec> &scans,
                    const std::vector<::duckdb::LogicalType> &output_types,
                    const OutputRowWriter &write_output);

void RunPostgresPrescanKernel(
    KernelInstance &kernel,
    const std::function<void(const PostgresChunkScanner &)> &prescan,
    const std::vector<::duckdb::LogicalType> &output_types,
    const OutputRowWriter &write_output);

void RunTpchQ1Kernel(int32_t day_offset, const OutputRowWriter &write_output);

Q6Parameters MakeQ6Parameters(::duckdb::date_t start_date, double discount, double quantity);
NativeScanSpec BuildQ6NativeScanSpec(const Q6Parameters &parameters);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
