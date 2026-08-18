// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "postgres/kernel_runner.hpp"

#include "accelerator_codegen/runtime/postgres/bind_helper.hpp"
#include "accelerator_codegen/runtime/postgres/kernel_dispatch.hpp"
#include "tpch/q11/base/query.hpp"
#include "tpch/q13/base/query.hpp"
#include "tpch/q15/base/query.hpp"
#include "tpch/q16/base/query.hpp"
#include "tpch/q17/base/query.hpp"
#include "tpch/q18/base/query.hpp"
#include "tpch/q19/base/query.hpp"
#include "tpch/q2/base/query.hpp"
#include "tpch/q20/base/query.hpp"
#include "tpch/q21/base/query.hpp"
#include "tpch/q22/base/query.hpp"
#include "tpch/q4/base/query.hpp"
#include "tpch/q5/base/query.hpp"
#include "tpch/q7/base/query.hpp"
#include "tpch/q8/base/query.hpp"
#include "tpch/q9/base/query.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

constexpr int32_t kDispatchPostgresDateEpochOffset = 10957;

::duckdb::date_t PgDate(int32_t days) {
  ::duckdb::date_t date;
  date.days = days + kDispatchPostgresDateEpochOffset;
  return date;
}

const std::vector<::duckdb::LogicalType> &TpchQ2OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::DECIMAL(15, 2), ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT,
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ4OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::VARCHAR,
                                                            ::duckdb::LogicalType::BIGINT};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ5OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::VARCHAR,
                                                            ::duckdb::LogicalType::DECIMAL(38, 4)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ7OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::DECIMAL(38, 4)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ8OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::BIGINT,
                                                            ::duckdb::LogicalType::DOUBLE};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ9OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT,
      ::duckdb::LogicalType::DECIMAL(38, 4)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ11OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::BIGINT,
                                                            ::duckdb::LogicalType::DECIMAL(38, 2)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ13OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::BIGINT,
                                                            ::duckdb::LogicalType::BIGINT};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ15OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::DECIMAL(38, 4)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ16OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR,
      ::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::BIGINT};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ17OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::DOUBLE};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ18OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT,
      ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::DATE,
      ::duckdb::LogicalType::DECIMAL(15, 2), ::duckdb::LogicalType::DECIMAL(38, 2)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ19OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::DECIMAL(38, 4)};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ20OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::VARCHAR,
                                                            ::duckdb::LogicalType::VARCHAR};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ21OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {::duckdb::LogicalType::VARCHAR,
                                                            ::duckdb::LogicalType::BIGINT};
  return types;
}

const std::vector<::duckdb::LogicalType> &TpchQ22OutputTypes() {
  static const std::vector<::duckdb::LogicalType> types = {
      ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT,
      ::duckdb::LogicalType::DECIMAL(38, 2)};
  return types;
}

} // namespace

void RunPostgresBaseTpchQ2(int32_t part_size, const std::string &part_type_pattern,
                           const std::string &region_name, const PostgresRowWriter &writer) {
  auto parameters = MakeQ2Parameters(part_size, part_type_pattern, region_name);
  FillRegionNationsAndNames(region_name, parameters.region_nations, parameters.nation_names);
  RunChunkKernel(CreateQ2Kernel(parameters),
                 {CreateQ2PartScanSpec(parameters), CreateQ2SupplierNumericScanSpec(parameters),
                  CreateQ2PartsuppScanSpec(parameters), CreateQ2SupplierVarcharScanSpec(parameters)},
                 TpchQ2OutputTypes(), writer);
}

void RunPostgresBaseTpchQ4(int32_t start_date_days, const PostgresRowWriter &writer) {
  const auto parameters = MakeQ4Parameters(PgDate(start_date_days));
  RunChunkKernel(CreateQ4Kernel(parameters),
                 {CreateQ4LineitemScanSpec(parameters), CreateQ4OrdersScanSpec(parameters)},
                 TpchQ4OutputTypes(), writer);
}

void RunPostgresBaseTpchQ5(const std::string &region_name, int32_t start_date_days,
                           const PostgresRowWriter &writer) {
  const auto parameters = MakeQ5Parameters(region_name, PgDate(start_date_days));
  RunChunkKernel(CreateQ5Kernel(parameters),
                 {CreateQ5RegionScanSpec(parameters), CreateQ5NationScanSpec(parameters),
                  CreateQ5CustomerScanSpec(parameters), CreateQ5SupplierScanSpec(parameters),
                  CreateQ5OrdersScanSpec(parameters), CreateQ5LineitemScanSpec(parameters)},
                 TpchQ5OutputTypes(), writer);
}

void RunPostgresBaseTpchQ7(const std::string &nation1, const std::string &nation2,
                           const PostgresRowWriter &writer) {
  const auto parameters = MakeQ7Parameters(nation1, nation2);
  RunChunkKernel(CreateQ7Kernel(parameters),
                 {CreateQ7NationScanSpec(parameters), CreateQ7CustomerScanSpec(parameters),
                  CreateQ7SupplierScanSpec(parameters), CreateQ7OrdersScanSpec(parameters),
                  CreateQ7LineitemScanSpec(parameters)},
                 TpchQ7OutputTypes(), writer);
}

void RunPostgresBaseTpchQ8(const std::string &nation_name, const std::string &region_name,
                           const std::string &part_type, const PostgresRowWriter &writer) {
  auto parameters = MakeQ8Parameters(nation_name, region_name, part_type);
  parameters.target_nationkey = ResolveNationKey(parameters.nation_name);
  FillRegionNations(parameters.region_name, parameters.region_nations);
  ResolvePartKeys(parameters.part_type, parameters.bind_part_keys);
  RunChunkKernel(CreateQ8Kernel(parameters),
                 {CreateQ8PartScanSpec(parameters), CreateQ8SupplierScanSpec(parameters),
                  CreateQ8CustomerScanSpec(parameters), CreateQ8OrdersScanSpec(parameters),
                  CreateQ8LineitemScanSpec(parameters)},
                 TpchQ8OutputTypes(), writer);
}

void RunPostgresBaseTpchQ9(const std::string &color, const PostgresRowWriter &writer) {
  Q9Parameters parameters;
  parameters.color = color;
  RunChunkKernel(CreateQ9Kernel(parameters),
                 {CreateQ9PartScanSpec(parameters), CreateQ9SupplierScanSpec(parameters),
                  CreateQ9NationScanSpec(parameters), CreateQ9OrdersScanSpec(parameters),
                  CreateQ9PartsuppScanSpec(parameters), CreateQ9LineitemScanSpec(parameters)},
                 TpchQ9OutputTypes(), writer);
}

void RunPostgresBaseTpchQ11(const std::string &nation_name, double fraction,
                            const PostgresRowWriter &writer) {
  auto parameters = MakeQ11Parameters(nation_name, fraction);
  parameters.target_nationkey = ResolveNationKey(parameters.nation_name);
  RunChunkKernel(CreateQ11Kernel(parameters),
                 {CreateQ11SupplierScanSpec(parameters), CreateQ11PartsuppScanSpec(parameters)},
                 TpchQ11OutputTypes(), writer);
}

void RunPostgresBaseTpchQ13(const std::string &comment_pattern, const PostgresRowWriter &writer) {
  const auto parameters = MakeQ13Parameters(comment_pattern);
  RunChunkKernel(CreateQ13Kernel(parameters),
                 {CreateQ13CustomerScanSpec(parameters), CreateQ13OrdersScanSpec(parameters)},
                 TpchQ13OutputTypes(), writer);
}

void RunPostgresBaseTpchQ15(int32_t start_date_days, const PostgresRowWriter &writer) {
  const auto parameters = MakeQ15Parameters(PgDate(start_date_days));
  RunChunkKernel(CreateQ15Kernel(parameters),
                 {CreateQ15LineitemScanSpec(parameters), CreateQ15SupplierScanSpec(parameters)},
                 TpchQ15OutputTypes(), writer);
}

void RunPostgresBaseTpchQ16(const std::string &brand, const std::string &type_pattern,
                            int32_t size1, int32_t size2, int32_t size3, int32_t size4,
                            int32_t size5, int32_t size6, int32_t size7, int32_t size8,
                            const PostgresRowWriter &writer) {
  const std::array<int32_t, 8> sizes = {size1, size2, size3, size4, size5, size6, size7, size8};
  const auto parameters = MakeQ16Parameters(brand, type_pattern, sizes);
  RunChunkKernel(CreateQ16Kernel(parameters),
                 {CreateQ16SupplierScanSpec(parameters), CreateQ16PartScanSpec(parameters),
                  CreateQ16PartsuppScanSpec(parameters)},
                 TpchQ16OutputTypes(), writer);
}

void RunPostgresBaseTpchQ17(const std::string &brand, const std::string &container,
                            const PostgresRowWriter &writer) {
  const auto parameters = MakeQ17Parameters(brand, container);
  RunChunkKernel(CreateQ17Kernel(parameters),
                 {CreateQ17LineitemAggScanSpec(parameters), CreateQ17PartScanSpec(parameters),
                  CreateQ17LineitemProbeScanSpec(parameters)},
                 TpchQ17OutputTypes(), writer);
}

void RunPostgresBaseTpchQ18(int64_t quantity_threshold, const PostgresRowWriter &writer) {
  const auto parameters = MakeQ18Parameters(quantity_threshold);
  RunChunkKernel(CreateQ18Kernel(parameters),
                 {CreateQ18LineitemAggScanSpec(parameters), CreateQ18OrdersScanSpec(parameters),
                  CreateQ18CustomerScanSpec(parameters)},
                 TpchQ18OutputTypes(), writer);
}

void RunPostgresBaseTpchQ19(const std::string &brand1, int64_t qty_low1, const std::string &brand2,
                            int64_t qty_low2, const std::string &brand3, int64_t qty_low3,
                            const PostgresRowWriter &writer) {
  const auto parameters = MakeQ19Parameters(brand1, qty_low1, brand2, qty_low2, brand3, qty_low3);
  RunChunkKernel(CreateQ19Kernel(parameters),
                 {CreateQ19PartScanSpec(parameters), CreateQ19LineitemScanSpec(parameters)},
                 TpchQ19OutputTypes(), writer);
}

void RunPostgresBaseTpchQ20(int32_t start_date_days, const std::string &part_name_pattern,
                            const std::string &nation_name, const PostgresRowWriter &writer) {
  auto parameters = MakeQ20Parameters(PgDate(start_date_days), part_name_pattern, nation_name);
  parameters.target_nationkey = ResolveNationKey(parameters.nation_name);
  RunChunkKernel(CreateQ20Kernel(parameters),
                 {CreateQ20PartScanSpec(parameters), CreateQ20LineitemScanSpec(parameters),
                  CreateQ20PartsuppScanSpec(parameters), CreateQ20SupplierScanSpec(parameters)},
                 TpchQ20OutputTypes(), writer);
}

void RunPostgresBaseTpchQ21(const std::string &nation_name, const PostgresRowWriter &writer) {
  auto parameters = MakeQ21Parameters(nation_name);
  parameters.target_nationkey = ResolveNationKey(parameters.nation_name);
  RunChunkKernel(CreateQ21Kernel(parameters),
                 {CreateQ21SupplierScanSpec(parameters), CreateQ21OrdersScanSpec(parameters),
                  CreateQ21LineitemScanSpec(parameters)},
                 TpchQ21OutputTypes(), writer);
}

void RunPostgresBaseTpchQ22(const std::string &country_code1, const std::string &country_code2,
                            const std::string &country_code3, const std::string &country_code4,
                            const std::string &country_code5, const std::string &country_code6,
                            const std::string &country_code7, const PostgresRowWriter &writer) {
  const std::array<std::string, 7> country_codes = {
      country_code1, country_code2, country_code3, country_code4,
      country_code5, country_code6, country_code7};
  const auto parameters = MakeQ22Parameters(country_codes);
  RunChunkKernel(CreateQ22Kernel(parameters),
                 {CreateQ22OrdersScanSpec(parameters), CreateQ22CustomerScanSpec(parameters)},
                 TpchQ22OutputTypes(), writer);
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
