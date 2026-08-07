#include "accelerator_codegen/sec_edgar_q3_finalize.hpp"

#include <algorithm>

namespace accelerator_codegen {

std::vector<SecEdgarQ3PartitionKey> SecEdgarQ3FixedPartitionKeys() {
	return {kSecEdgarQ3Fy2022CohortSlot, kSecEdgarQ3CikTotalsSlot, kSecEdgarQ3OutputSlot};
}

std::vector<SecEdgarQ3OutputRow> FinalizeSecEdgarQ3Groups(const std::unordered_map<int32_t, double> &cik_totals,
                                                            const std::vector<std::string> &group_names,
                                                            const std::vector<int32_t> &group_ciks,
                                                            const std::vector<double> &group_sums) {
	double cik_sum_total = 0.0;
	for (const auto &entry : cik_totals) {
		cik_sum_total += entry.second;
	}
	const double avg_threshold =
	    cik_totals.empty() ? 0.0 : cik_sum_total / static_cast<double>(cik_totals.size());

	std::vector<SecEdgarQ3OutputRow> rows;
	rows.reserve(group_names.size());
	for (size_t group_id = 0; group_id < group_names.size(); group_id++) {
		const auto total = group_sums[group_id];
		if (total <= avg_threshold) {
			continue;
		}
		rows.push_back({group_names[group_id], group_ciks[group_id], total});
	}

	std::sort(rows.begin(), rows.end(), [](const SecEdgarQ3OutputRow &lhs, const SecEdgarQ3OutputRow &rhs) {
		if (lhs.total_value != rhs.total_value) {
			return lhs.total_value > rhs.total_value;
		}
		if (lhs.name != rhs.name) {
			return lhs.name < rhs.name;
		}
		return lhs.cik < rhs.cik;
	});
	if (rows.size() > 100) {
		rows.resize(100);
	}
	return rows;
}

void RefreshSecEdgarQ3Output(SecEdgarQ3CikTotalsPartition &aggregates, SecEdgarQ3OutputPartition &output) {
	output.output_rows =
	    FinalizeSecEdgarQ3Groups(aggregates.cik_totals, aggregates.group_names, aggregates.group_ciks, aggregates.group_sums);
}

bool ApplySecEdgarQ3UsdNumDelta(SecEdgarQ3Fy2022CohortPartition &cohort, SecEdgarQ3CikTotalsPartition &aggregates,
                                SecEdgarQ3OutputPartition &output, const SecEdgarQ3AdshKey &adsh_key, double value,
                                int8_t sign) {
	const auto filing_it = cohort.adsh_to_filing.find(adsh_key);
	if (filing_it == cohort.adsh_to_filing.end()) {
		return true;
	}
	const auto &filing = filing_it->second;
	if (filing.group_id >= aggregates.group_sums.size()) {
		return false;
	}
	const auto delta = static_cast<double>(sign) * value;
	aggregates.cik_totals[filing.cik] += delta;
	aggregates.group_sums[filing.group_id] += delta;
	RefreshSecEdgarQ3Output(aggregates, output);
	return true;
}

} // namespace accelerator_codegen
