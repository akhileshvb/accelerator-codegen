#include "accelerator_codegen/tpch_q12_finalize.hpp"

#include <algorithm>
#include <cstring>

namespace accelerator_codegen {
namespace {

bool StringEquals(const duckdb::string_t &value, const char *expected, size_t expected_len) {
	return value.GetSize() == expected_len && std::memcmp(value.GetData(), expected, expected_len) == 0;
}

bool IsHighPriorityString(const duckdb::string_t &value) {
	static constexpr const char kUrgent[] = "1-URGENT";
	static constexpr const char kHigh[] = "2-HIGH";
	return StringEquals(value, kUrgent, sizeof(kUrgent) - 1) || StringEquals(value, kHigh, sizeof(kHigh) - 1);
}

} // namespace

Q12PriorityClass ClassifyQ12Priority(const std::string &priority) {
	static constexpr const char kUrgent[] = "1-URGENT";
	static constexpr const char kHigh[] = "2-HIGH";
	if (priority == kUrgent || priority == kHigh) {
		return Q12PriorityClass::High;
	}
	return Q12PriorityClass::Low;
}

Q12PriorityClass ClassifyQ12Priority(const duckdb::string_t &priority, bool is_null) {
	if (is_null) {
		return Q12PriorityClass::Neither;
	}
	return IsHighPriorityString(priority) ? Q12PriorityClass::High : Q12PriorityClass::Low;
}

bool Q12LineitemQualifies(duckdb::date_t commitdate, duckdb::date_t receiptdate, duckdb::date_t shipdate) {
	return commitdate < receiptdate && shipdate < commitdate;
}

void ApplyQ12LineitemContribution(Q12DayShipmodeStats &stats, Q12PriorityClass priority, int8_t sign) {
	switch (priority) {
	case Q12PriorityClass::High:
		stats.high_line_count += sign;
		break;
	case Q12PriorityClass::Low:
		stats.low_line_count += sign;
		break;
	default:
		break;
	}
}

void MergeQ12DayShipmodeStats(Q12MergedShipmodeStats &merged, const Q12DayShipmodeStats &day_stats) {
	merged.high_line_count += day_stats.high_line_count;
	merged.low_line_count += day_stats.low_line_count;
}

std::vector<Q12OutputRow> ComposeQ12MergedOutput(const std::unordered_map<std::string, Q12MergedShipmodeStats> &merged,
                                                 const std::string &shipmode1, const std::string &shipmode2) {
	std::vector<Q12OutputRow> rows;
	const bool modes_equal = shipmode1 == shipmode2;
	auto maybe_emit = [&](const std::string &shipmode) {
		const auto it = merged.find(shipmode);
		if (it == merged.end()) {
			return;
		}
		if (it->second.high_line_count == 0 && it->second.low_line_count == 0) {
			return;
		}
		Q12OutputRow row;
		row.shipmode = shipmode;
		row.high_line_count = it->second.high_line_count;
		row.low_line_count = it->second.low_line_count;
		rows.push_back(std::move(row));
	};
	maybe_emit(shipmode1);
	if (!modes_equal) {
		maybe_emit(shipmode2);
	}
	std::sort(rows.begin(), rows.end(),
	          [](const Q12OutputRow &lhs, const Q12OutputRow &rhs) { return lhs.shipmode < rhs.shipmode; });
	return rows;
}

} // namespace accelerator_codegen
