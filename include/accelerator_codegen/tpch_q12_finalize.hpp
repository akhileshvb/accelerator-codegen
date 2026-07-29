#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

enum class Q12PriorityClass : uint8_t {
	Absent = 0,
	High = 1,
	Low = 2,
	Neither = 3,
};

struct Q12DayShipmodeStats {
	int64_t high_line_count = 0;
	int64_t low_line_count = 0;
};

struct Q12DayPartition {
	std::unordered_map<std::string, Q12DayShipmodeStats> shipmodes;
};

struct Q12MergedShipmodeStats {
	int64_t high_line_count = 0;
	int64_t low_line_count = 0;
};

struct Q12OutputRow {
	std::string shipmode;
	int64_t high_line_count = 0;
	int64_t low_line_count = 0;
};

Q12PriorityClass ClassifyQ12Priority(const std::string &priority);
Q12PriorityClass ClassifyQ12Priority(const duckdb::string_t &priority, bool is_null);

bool Q12LineitemQualifies(duckdb::date_t commitdate, duckdb::date_t receiptdate, duckdb::date_t shipdate);

void ApplyQ12LineitemContribution(Q12DayShipmodeStats &stats, Q12PriorityClass priority, int8_t sign);

void MergeQ12DayShipmodeStats(Q12MergedShipmodeStats &merged, const Q12DayShipmodeStats &day_stats);

std::vector<Q12OutputRow> ComposeQ12MergedOutput(const std::unordered_map<std::string, Q12MergedShipmodeStats> &merged,
                                                 const std::string &shipmode1, const std::string &shipmode2);

} // namespace accelerator_codegen
