#include "accelerator_codegen/sec_edgar_q6_finalize.hpp"

#include <algorithm>

namespace accelerator_codegen {
namespace {

constexpr size_t kResultLimit = 200;

void ApplyResolved(SecEdgarQ6GroupAggregatesPartition &aggregates, const SecEdgarQ6ResolvedPre &resolved, double value,
                   int8_t sign) {
	const auto scaled = static_cast<double>(sign) * value;
	if (resolved.single) {
		if (resolved.single_group_id >= aggregates.groups.size()) {
			return;
		}
		auto &group = aggregates.groups[resolved.single_group_id];
		group.sum += scaled * static_cast<double>(resolved.single_multiplicity);
		group.count += static_cast<int64_t>(sign) * static_cast<int64_t>(resolved.single_multiplicity);
		group.active = true;
		return;
	}
	for (const auto &entry : resolved.entries) {
		if (entry.group_id >= aggregates.groups.size()) {
			continue;
		}
		auto &group = aggregates.groups[entry.group_id];
		const auto weight = static_cast<double>(entry.multiplicity);
		group.sum += scaled * weight;
		group.count += static_cast<int64_t>(sign) * static_cast<int64_t>(entry.multiplicity);
		group.active = true;
	}
}

} // namespace

std::vector<SecEdgarQ6PartitionKey> SecEdgarQ6FixedPartitionKeys() {
	return {kSecEdgarQ6Fy2023CohortSlot, kSecEdgarQ6PreIsBridgeSlot, kSecEdgarQ6TagVersionSlot,
	        kSecEdgarQ6GroupAggregatesSlot, kSecEdgarQ6OutputSlot};
}

void RefreshSecEdgarQ6Output(const SecEdgarQ6Fy2023CohortPartition &cohort,
                             const SecEdgarQ6GroupAggregatesPartition &aggregates, SecEdgarQ6OutputPartition &output) {
	output.output_rows.clear();
	std::vector<uint32_t> active_group_ids;
	active_group_ids.reserve(aggregates.groups.size());
	for (uint32_t group_id = 0; group_id < aggregates.groups.size(); group_id++) {
		if (aggregates.groups[group_id].count != 0) {
			active_group_ids.push_back(group_id);
		}
	}
	const auto compare_groups = [&](uint32_t lhs_id, uint32_t rhs_id) {
		const auto &lhs = aggregates.groups[lhs_id];
		const auto &rhs = aggregates.groups[rhs_id];
		if (lhs.sum != rhs.sum) {
			return lhs.sum > rhs.sum;
		}
		const auto &lhs_meta = aggregates.group_meta[lhs_id];
		const auto &rhs_meta = aggregates.group_meta[rhs_id];
		const auto &lhs_name = cohort.names[lhs_meta.name_id];
		const auto &rhs_name = cohort.names[rhs_meta.name_id];
		if (lhs_name != rhs_name) {
			return lhs_name < rhs_name;
		}
		const auto &lhs_tag = aggregates.tags[lhs_meta.tag_id];
		const auto &rhs_tag = aggregates.tags[rhs_meta.tag_id];
		if (lhs_tag != rhs_tag) {
			return lhs_tag < rhs_tag;
		}
		return aggregates.plabels[lhs_meta.plabel_id] < aggregates.plabels[rhs_meta.plabel_id];
	};
	const auto output_count = std::min(active_group_ids.size(), kResultLimit);
	if (active_group_ids.size() > kResultLimit) {
		std::partial_sort(active_group_ids.begin(), active_group_ids.begin() + static_cast<ptrdiff_t>(output_count),
		                  active_group_ids.end(), compare_groups);
		active_group_ids.resize(output_count);
	} else {
		std::sort(active_group_ids.begin(), active_group_ids.end(), compare_groups);
	}
	output.output_rows.reserve(output_count);
	for (const auto group_id : active_group_ids) {
		const auto &group = aggregates.groups[group_id];
		const auto &meta = aggregates.group_meta[group_id];
		SecEdgarQ6OutputRow row;
		row.name = cohort.names[meta.name_id];
		row.tag = aggregates.tags[meta.tag_id];
		row.plabel = aggregates.plabels[meta.plabel_id];
		row.stmt = "IS";
		row.total_value = group.sum;
		row.cnt = group.count;
		output.output_rows.push_back(std::move(row));
	}
}

bool ApplySecEdgarQ6UsdNumDelta(const SecEdgarQ6Fy2023CohortPartition &cohort,
                                const SecEdgarQ6PreIsBridgePartition &pre_bridge,
                                const SecEdgarQ6TagVersionPartition &tag_versions, SecEdgarQ6GroupAggregatesPartition &aggregates,
                                SecEdgarQ6OutputPartition &output, const SecEdgarQ6AdshKey &adsh_key,
                                const SecEdgarQ6TagVersionKey &tag_version_key, double value, int8_t sign) {
	(void)cohort;
	const auto pre_adsh_it = pre_bridge.pre_by_adsh.find(adsh_key);
	if (pre_adsh_it == pre_bridge.pre_by_adsh.end()) {
		return true;
	}
	const auto tv_it = tag_versions.tag_version_lookup.find(tag_version_key);
	if (tv_it == tag_versions.tag_version_lookup.end()) {
		return true;
	}
	const auto pre_tv_it = pre_adsh_it->second.find(tv_it->second);
	if (pre_tv_it == pre_adsh_it->second.end()) {
		return true;
	}
	ApplyResolved(aggregates, pre_tv_it->second, value, sign);
	RefreshSecEdgarQ6Output(cohort, aggregates, output);
	return true;
}

} // namespace accelerator_codegen
