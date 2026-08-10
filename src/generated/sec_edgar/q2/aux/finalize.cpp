#include "sec_edgar/q2/aux/finalize.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace accelerator_codegen {

std::vector<SecEdgarQ2PartitionKey> SecEdgarQ2FixedPartitionKeys() {
	return {kSecEdgarQ2MaxPureMapSlot, kSecEdgarQ2Fy2022CohortSlot, kSecEdgarQ2MaxCandidatesSlot};
}

std::vector<SecEdgarQ2OutputRow> FinalizeSecEdgarQ2Candidates(std::vector<SecEdgarQ2CandidateRow> candidates) {
	std::sort(candidates.begin(), candidates.end(), [](const SecEdgarQ2CandidateRow &lhs, const SecEdgarQ2CandidateRow &rhs) {
		if (lhs.value != rhs.value) {
			return lhs.value > rhs.value;
		}
		if (lhs.name != rhs.name) {
			return lhs.name < rhs.name;
		}
		return lhs.tag < rhs.tag;
	});
	if (candidates.size() > 100) {
		candidates.resize(100);
	}
	std::vector<SecEdgarQ2OutputRow> rows;
	rows.reserve(candidates.size());
	for (const auto &candidate : candidates) {
		rows.push_back({candidate.name, candidate.tag, candidate.value});
	}
	return rows;
}

void RefreshSecEdgarQ2CandidateOutput(SecEdgarQ2CandidatePartition &candidates) {
	candidates.output_rows = FinalizeSecEdgarQ2Candidates(candidates.rows_at_max);
}

bool ApplySecEdgarQ2PureNumInsert(SecEdgarQ2MaxPureMapPartition &max_map, SecEdgarQ2Fy2022CohortPartition &cohort,
                                  SecEdgarQ2CandidatePartition &candidates, const SecEdgarQ2AdshKey &adsh_key,
                                  const std::string &tag, double value) {
	const auto group_key = SecEdgarQ2GroupKey {adsh_key, SecEdgarQ2AdshKey::FromStored(tag)};
	const auto max_it = max_map.max_by_adsh_tag.find(group_key);
	const double old_max = max_it == max_map.max_by_adsh_tag.end() ? -std::numeric_limits<double>::infinity()
	                                                               : max_it->second;

	if (value > old_max) {
		if (max_it == max_map.max_by_adsh_tag.end()) {
			max_map.max_by_adsh_tag.emplace(group_key, value);
		} else {
			max_it->second = value;
		}
	} else if (value < old_max) {
		return true;
	}

	const auto cohort_it = cohort.adsh_to_name.find(adsh_key);
	if (cohort_it == cohort.adsh_to_name.end()) {
		return true;
	}
	const auto &name = cohort_it->second;
	if (value < old_max) {
		return true;
	}

	if (value > old_max) {
		auto &rows = candidates.rows_at_max;
		rows.erase(std::remove_if(rows.begin(), rows.end(),
		                          [&](const SecEdgarQ2CandidateRow &row) {
			                          return row.name == name && row.tag == tag;
		                          }),
		           rows.end());
		rows.push_back({name, tag, value});
	} else {
		candidates.rows_at_max.push_back({name, tag, value});
	}
	RefreshSecEdgarQ2CandidateOutput(candidates);
	return true;
}

} // namespace accelerator_codegen
