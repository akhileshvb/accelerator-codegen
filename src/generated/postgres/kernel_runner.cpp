// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/kernel_runner.hpp"

#include <cmath>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "accelerator_codegen/runtime/postgres/scan_adapter.hpp"
#include "tpch/q1/base/query.hpp"
#include "tpch/q1/aux/finalize.hpp"
#include "tpch/q6/base/query.hpp"
#include "tpch/q10/base/query.hpp"
#include "tpch/q12/base/query.hpp"
#include "tpch/q14/base/query.hpp"
#include "tpch/q3/base/query.hpp"
#include "sec_edgar/q1/base/query.hpp"
#include "sec_edgar/q2/base/query.hpp"
#include "sec_edgar/q24/base/query.hpp"
#include "sec_edgar/q3/base/query.hpp"
#include "sec_edgar/q4/base/query.hpp"
#include "sec_edgar/q6/base/query.hpp"
#include "accelerator_codegen/runtime/postgres/kernel_dispatch.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

constexpr uint8_t kLineitemDecimalWidth = 15;
constexpr uint8_t kLineitemDecimalScale = 2;

::duckdb::date_t ComputeExclusiveEndDate(::duckdb::date_t start_date) {
  ::duckdb::interval_t one_year;
  one_year.months = ::duckdb::Interval::MONTHS_PER_YEAR;
  one_year.days = 0;
  one_year.micros = 0;
  return ::duckdb::Interval::Add(start_date, one_year);
}

char ExtractSingleChar(::duckdb::Vector &vector, ::duckdb::idx_t row,
                       ::duckdb::idx_t count) {
  ::duckdb::UnifiedVectorFormat format;
  vector.ToUnifiedFormat(count, format);
  const auto index = format.sel->get_index(row);
  if (!format.validity.AllValid() && !format.validity.RowIsValidUnsafe(index)) {
    return '\0';
  }
  const auto value =
      ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(format)[index];
  if (value.GetSize() != 1) {
    throw std::runtime_error("expected single-character varchar value");
  }
  return value.GetData()[0];
}

void AccumulateQ1Chunk(::duckdb::DataChunk &chunk,
                       std::unordered_map<Q1GroupKey, Q1DayGroupStats> &groups) {
  for (::duckdb::idx_t row = 0; row < chunk.size(); ++row) {
    const auto quantity = ::duckdb::FlatVector::GetData<int64_t>(
        chunk.data[Q1LineitemChunkLayout::QUANTITY_INDEX])[row];
    const auto extendedprice = ::duckdb::FlatVector::GetData<int64_t>(
        chunk.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX])[row];
    const auto discount = ::duckdb::FlatVector::GetData<int64_t>(
        chunk.data[Q1LineitemChunkLayout::DISCOUNT_INDEX])[row];
    const auto tax = ::duckdb::FlatVector::GetData<int64_t>(
        chunk.data[Q1LineitemChunkLayout::TAX_INDEX])[row];
    const auto returnflag = ExtractSingleChar(
        chunk.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX], row, chunk.size());
    const auto linestatus = ExtractSingleChar(
        chunk.data[Q1LineitemChunkLayout::LINESTATUS_INDEX], row, chunk.size());
    const auto group_key = PackQ1GroupKey(returnflag, linestatus);
    auto &stats = groups[group_key];
    ApplyQ1RowContribution(stats, quantity, extendedprice, discount, tax, 1);
  }
}

void MakeQ1OutputChunk(const std::vector<Q1OutputRow> &rows, ::duckdb::DataChunk &output) {
  output.Initialize(::duckdb::Allocator::DefaultAllocator(),
                    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
                     ::duckdb::LogicalType::DECIMAL(38, 2),
                     ::duckdb::LogicalType::DECIMAL(38, 2),
                     ::duckdb::LogicalType::DECIMAL(38, 4),
                     ::duckdb::LogicalType::DECIMAL(38, 6),
                     ::duckdb::LogicalType::DOUBLE, ::duckdb::LogicalType::DOUBLE,
                     ::duckdb::LogicalType::DOUBLE, ::duckdb::LogicalType::BIGINT});
  output.SetCardinality(rows.size());
  for (::duckdb::idx_t row = 0; row < rows.size(); ++row) {
    const auto &result = rows[row];
    output.SetValue(0, row, ::duckdb::Value(UnpackQ1Returnflag(result.group_key)));
    output.SetValue(1, row, ::duckdb::Value(UnpackQ1Linestatus(result.group_key)));
    output.SetValue(2, row, ::duckdb::Value::DECIMAL(result.sum_qty, 38, 2));
    output.SetValue(3, row, ::duckdb::Value::DECIMAL(result.sum_base_price, 38, 2));
    output.SetValue(4, row, ::duckdb::Value::DECIMAL(result.sum_disc_price, 38, 4));
    output.SetValue(5, row, ::duckdb::Value::DECIMAL(result.sum_charge, 38, 6));
    output.SetValue(6, row, ::duckdb::Value::DOUBLE(result.avg_qty));
    output.SetValue(7, row, ::duckdb::Value::DOUBLE(result.avg_price));
    output.SetValue(8, row, ::duckdb::Value::DOUBLE(result.avg_disc));
    output.SetValue(9, row, ::duckdb::Value::BIGINT(result.count_order));
  }
}

void WriteRowsFromChunk(const ::duckdb::DataChunk &output, const OutputRowWriter &write_output) {
  std::vector<std::vector<std::string>> rows;
  rows.reserve(output.size());
  for (::duckdb::idx_t row = 0; row < output.size(); ++row) {
    std::vector<std::string> values;
    values.reserve(output.ColumnCount());
    for (::duckdb::idx_t column = 0; column < output.ColumnCount(); ++column) {
      const auto value = output.GetValue(column, row);
      values.push_back(value.IsNull() ? "NULL" : value.ToString());
    }
    rows.push_back(std::move(values));
  }
  write_output(rows);
}

void ProduceKernelOutput(KernelInstance &kernel,
                         const std::vector<::duckdb::LogicalType> &output_types,
                         const OutputRowWriter &write_output) {
  kernel.Finalize();
  ::duckdb::DataChunk output;
  ::duckdb::vector<::duckdb::LogicalType> duckdb_types(output_types.begin(),
                                                        output_types.end());
  output.Initialize(::duckdb::Allocator::DefaultAllocator(), duckdb_types);
  while (kernel.ProduceOutput(output)) {
    WriteRowsFromChunk(output, write_output);
  }
}

} // namespace

const std::vector<::duckdb::LogicalType> &TpchQ3OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::DECIMAL(38, 4),
      ::duckdb::LogicalType::DATE, ::duckdb::LogicalType::INTEGER};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ6OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::DECIMAL(38, 4)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ10OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::DECIMAL(38, 4), ::duckdb::LogicalType::DECIMAL(15, 2),
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ12OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::HUGEINT,
      ::duckdb::LogicalType::HUGEINT};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ14OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::DOUBLE};
  return types;
}

const std::vector<::duckdb::LogicalType> &SecEdgarQ1OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::BIGINT,
      ::duckdb::LogicalType::DOUBLE};
  return types;
}

const std::vector<::duckdb::LogicalType> &SecEdgarQ2OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::DOUBLE};
  return types;
}

const std::vector<::duckdb::LogicalType> &SecEdgarQ3OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::INTEGER,
      ::duckdb::LogicalType::DOUBLE};
  return types;
}

const std::vector<::duckdb::LogicalType> &SecEdgarQ4OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT,
      ::duckdb::LogicalType::DOUBLE, ::duckdb::LogicalType::DOUBLE};
  return types;
}

const std::vector<::duckdb::LogicalType> &SecEdgarQ6OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::DOUBLE, ::duckdb::LogicalType::BIGINT};
  return types;
}

const std::vector<::duckdb::LogicalType> &SecEdgarQ24OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::DOUBLE};
  return types;
}

void RunChunkKernel(std::unique_ptr<KernelInstance> kernel,
                    const std::vector<NativeScanSpec> &scans,
                    const std::vector<::duckdb::LogicalType> &output_types,
                    const OutputRowWriter &write_output) {
  for (::duckdb::idx_t input_id = 0; input_id < scans.size(); ++input_id) {
    ScanNativeSpec(scans[input_id], [&](::duckdb::DataChunk &chunk) {
      kernel->ConsumeInput(input_id, chunk);
    });
  }
  ProduceKernelOutput(*kernel, output_types, write_output);
}

void RunPostgresPrescanKernel(
    KernelInstance &kernel,
    const std::function<void(const PostgresChunkScanner &)> &prescan,
    const std::vector<::duckdb::LogicalType> &output_types,
    const OutputRowWriter &write_output) {
  prescan([&](const NativeScanSpec &scan_spec,
              const std::function<void(::duckdb::DataChunk &)> &consumer) {
    ScanNativeSpec(scan_spec, consumer);
  });
  ProduceKernelOutput(kernel, output_types, write_output);
}

void RunTpchQ1Kernel(int32_t day_offset, const OutputRowWriter &write_output) {
  const auto parameters = MakeQ1Parameters(day_offset);
  std::unordered_map<Q1GroupKey, Q1DayGroupStats> day_groups;
  ScanNativeSpec(CreateQ1ScanSpec(parameters),
                 [&](::duckdb::DataChunk &chunk) { AccumulateQ1Chunk(chunk, day_groups); });
  std::unordered_map<Q1GroupKey, Q1MergedGroupStats> merged_groups;
  for (const auto &entry : day_groups) {
    MergeQ1DayGroupStats(merged_groups[entry.first], entry.second);
  }
  ::duckdb::DataChunk output_chunk;
  MakeQ1OutputChunk(FinalizeQ1MergedGroups(merged_groups), output_chunk);
  WriteRowsFromChunk(output_chunk, write_output);
}

Q6Parameters MakeQ6Parameters(::duckdb::date_t start_date, double discount,
                              double quantity) {
  Q6Parameters parameters;
  parameters.start_date = start_date;
  parameters.end_date = ComputeExclusiveEndDate(start_date);
  const auto discount_raw =
      static_cast<int64_t>(std::llround(discount * 100.0));
  const auto quantity_raw =
      static_cast<int64_t>(std::llround(quantity * 100.0));
  parameters.discount_lower_raw = discount_raw - 1;
  parameters.discount_upper_raw = discount_raw + 1;
  parameters.quantity_upper_raw = quantity_raw;
  return parameters;
}

NativeScanSpec BuildQ6NativeScanSpec(const Q6Parameters &parameters) {
  NativeScanSpec scan_spec;
  scan_spec.table_name = "lineitem";
  scan_spec.projected_columns = {
      {"l_shipdate", "shipdate", ::duckdb::LogicalType::DATE,
       ::duckdb::PhysicalType::INT32},
      {"l_discount", "discount", ::duckdb::LogicalType::DECIMAL(15, 2),
       ::duckdb::PhysicalType::INT64},
      {"l_quantity", "quantity", ::duckdb::LogicalType::DECIMAL(15, 2),
       ::duckdb::PhysicalType::INT64},
      {"l_extendedprice", "extendedprice", ::duckdb::LogicalType::DECIMAL(15, 2),
       ::duckdb::PhysicalType::INT64},
  };
  scan_spec.filters = {
      {"l_shipdate", ::duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
       ::duckdb::Value::DATE(parameters.start_date)},
      {"l_shipdate", ::duckdb::ExpressionType::COMPARE_LESSTHAN,
       ::duckdb::Value::DATE(parameters.end_date)},
      {"l_discount", ::duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
       ::duckdb::Value::DECIMAL(parameters.discount_lower_raw, kLineitemDecimalWidth,
                                kLineitemDecimalScale)},
      {"l_discount", ::duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
       ::duckdb::Value::DECIMAL(parameters.discount_upper_raw, kLineitemDecimalWidth,
                                kLineitemDecimalScale)},
      {"l_quantity", ::duckdb::ExpressionType::COMPARE_LESSTHAN,
       ::duckdb::Value::DECIMAL(parameters.quantity_upper_raw, kLineitemDecimalWidth,
                                kLineitemDecimalScale)},
  };
  return scan_spec;
}

constexpr int32_t kDispatchPostgresDateEpochOffset = 10957;

::duckdb::date_t PgDate(int32_t days) {
  ::duckdb::date_t date;
  date.days = days + kDispatchPostgresDateEpochOffset;
  return date;
}

void RunPostgresBaseTpchQ1(int32_t day_offset, const PostgresRowWriter &writer) {
  RunTpchQ1Kernel(day_offset, writer);
}

void RunPostgresBaseTpchQ3(const std::string &market_segment, int32_t cutoff_date_days,
                           const PostgresRowWriter &writer) {
  Q3Parameters parameters;
  parameters.market_segment = market_segment;
  parameters.cutoff_date = PgDate(cutoff_date_days);
  RunChunkKernel(CreateQ3Kernel(parameters),
                 {CreateQ3CustomerScanSpec(parameters), CreateQ3OrdersScanSpec(parameters),
                  CreateQ3LineitemScanSpec(parameters)},
                 TpchQ3OutputTypes(), writer);
}

void RunPostgresBaseTpchQ6(int32_t start_date_days, double discount, double quantity,
                           const PostgresRowWriter &writer) {
  const auto parameters = MakeQ6Parameters(PgDate(start_date_days), discount, quantity);
  RunChunkKernel(CreateQ6Kernel(parameters), {BuildQ6NativeScanSpec(parameters)},
                 TpchQ6OutputTypes(), writer);
}

void RunPostgresBaseTpchQ10(int32_t start_date_days, const PostgresRowWriter &writer) {
  const auto parameters = MakeQ10Parameters(PgDate(start_date_days));
  RunChunkKernel(CreateQ10Kernel(parameters),
                 {CreateQ10NationScanSpec(parameters), CreateQ10OrdersScanSpec(parameters),
                  CreateQ10LineitemScanSpec(parameters), CreateQ10CustomerScanSpec(parameters)},
                 TpchQ10OutputTypes(), writer);
}

void RunPostgresBaseTpchQ12(const std::string &shipmode1, const std::string &shipmode2,
                            int32_t start_date_days, const PostgresRowWriter &writer) {
  const auto parameters =
      MakeQ12Parameters(shipmode1, shipmode2, PgDate(start_date_days));
  RunChunkKernel(CreateQ12Kernel(parameters),
                 {CreateQ12OrdersScanSpec(parameters), CreateQ12LineitemScanSpec(parameters)},
                 TpchQ12OutputTypes(), writer);
}

void RunPostgresBaseTpchQ14(int32_t start_date_days, const PostgresRowWriter &writer) {
  const auto parameters = MakeQ14Parameters(PgDate(start_date_days));
  RunChunkKernel(CreateQ14Kernel(parameters),
                 {CreateQ14PartScanSpec(parameters), CreateQ14LineitemScanSpec(parameters)},
                 TpchQ14OutputTypes(), writer);
}

void RunPostgresBaseSecEdgarQ1(const PostgresRowWriter &writer) {
  SecEdgarQ1Parameters parameters;
  RunChunkKernel(CreateSecEdgarQ1Kernel(parameters), {CreateSecEdgarQ1ScanSpec()},
                 SecEdgarQ1OutputTypes(), writer);
}

void RunPostgresBaseSecEdgarQ2(const PostgresRowWriter &writer) {
  SecEdgarQ2Parameters parameters;
  RunChunkKernel(CreateSecEdgarQ2Kernel(parameters),
                 {CreateSecEdgarQ2SubScanSpec(), CreateSecEdgarQ2NumScanSpec()},
                 SecEdgarQ2OutputTypes(), writer);
}

void RunPostgresBaseSecEdgarQ3(const PostgresRowWriter &writer) {
  SecEdgarQ3Parameters parameters;
  RunChunkKernel(CreateSecEdgarQ3Kernel(parameters),
                 {CreateSecEdgarQ3SubScanSpec(), CreateSecEdgarQ3NumScanSpec()},
                 SecEdgarQ3OutputTypes(), writer);
}

void RunPostgresBaseSecEdgarQ4(const PostgresRowWriter &writer) {
  SecEdgarQ4Parameters parameters;
  auto kernel = CreateSecEdgarQ4Kernel(parameters);
  auto *q4_kernel = dynamic_cast<SecEdgarQ4Kernel *>(kernel.get());
  if (q4_kernel == nullptr) {
    throw std::runtime_error("failed to create SEC EDGAR Q4 base kernel");
  }
  RunPostgresPrescanKernel(
      *q4_kernel,
      [&](const PostgresChunkScanner &scan) { q4_kernel->RunPostgresChunkPrescan(scan); },
      SecEdgarQ4OutputTypes(), writer);
}

void RunPostgresBaseSecEdgarQ6(const PostgresRowWriter &writer) {
  SecEdgarQ6Parameters parameters;
  auto kernel = CreateSecEdgarQ6Kernel(parameters);
  auto *q6_kernel = dynamic_cast<SecEdgarQ6Kernel *>(kernel.get());
  if (q6_kernel == nullptr) {
    throw std::runtime_error("failed to create SEC EDGAR Q6 base kernel");
  }
  RunPostgresPrescanKernel(
      *q6_kernel,
      [&](const PostgresChunkScanner &scan) { q6_kernel->RunPostgresChunkPrescan(scan); },
      SecEdgarQ6OutputTypes(), writer);
}

void RunPostgresBaseSecEdgarQ24(const PostgresRowWriter &writer) {
  SecEdgarQ24Parameters parameters;
  auto kernel = CreateSecEdgarQ24Kernel(parameters);
  auto *q24_kernel = dynamic_cast<SecEdgarQ24Kernel *>(kernel.get());
  if (q24_kernel == nullptr) {
    throw std::runtime_error("failed to create SEC EDGAR Q24 base kernel");
  }
  RunPostgresPrescanKernel(
      *q24_kernel,
      [&](const PostgresChunkScanner &scan) { q24_kernel->RunPostgresChunkPrescan(scan); },
      SecEdgarQ24OutputTypes(), writer);
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
