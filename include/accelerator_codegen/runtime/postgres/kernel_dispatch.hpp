// GENERATED BACKEND
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <cstdint>
#include <string>

#include "accelerator_codegen/runtime/postgres/row_writer.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

void RunPostgresBaseTpchQ1(int32_t day_offset, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ2(int32_t part_size, const std::string &part_type_pattern,
                           const std::string &region_name, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ3(const std::string &market_segment, int32_t cutoff_date_days,
                           const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ4(int32_t start_date_days, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ5(const std::string &region_name, int32_t start_date_days,
                           const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ6(int32_t start_date_days, double discount, double quantity,
                           const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ7(const std::string &nation1, const std::string &nation2,
                           const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ8(const std::string &nation_name, const std::string &region_name,
                           const std::string &part_type, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ9(const std::string &color, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ10(int32_t start_date_days, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ11(const std::string &nation_name, double fraction,
                            const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ12(const std::string &shipmode1, const std::string &shipmode2,
                            int32_t start_date_days, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ13(const std::string &comment_pattern, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ14(int32_t start_date_days, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ15(int32_t start_date_days, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ16(const std::string &brand, const std::string &type_pattern,
                            int32_t size1, int32_t size2, int32_t size3, int32_t size4,
                            int32_t size5, int32_t size6, int32_t size7, int32_t size8,
                            const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ17(const std::string &brand, const std::string &container,
                            const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ18(int64_t quantity_threshold, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ19(const std::string &brand1, int64_t qty_low1, const std::string &brand2,
                            int64_t qty_low2, const std::string &brand3, int64_t qty_low3,
                            const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ20(int32_t start_date_days, const std::string &part_name_pattern,
                            const std::string &nation_name, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ21(const std::string &nation_name, const PostgresRowWriter &writer);
void RunPostgresBaseTpchQ22(const std::string &country_code1, const std::string &country_code2,
                            const std::string &country_code3, const std::string &country_code4,
                            const std::string &country_code5, const std::string &country_code6,
                            const std::string &country_code7, const PostgresRowWriter &writer);
void RunPostgresBaseSecEdgarQ1(const PostgresRowWriter &writer);
void RunPostgresBaseSecEdgarQ2(const PostgresRowWriter &writer);
void RunPostgresBaseSecEdgarQ3(const PostgresRowWriter &writer);
void RunPostgresBaseSecEdgarQ4(const PostgresRowWriter &writer);
void RunPostgresBaseSecEdgarQ6(const PostgresRowWriter &writer);
void RunPostgresBaseSecEdgarQ24(const PostgresRowWriter &writer);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
