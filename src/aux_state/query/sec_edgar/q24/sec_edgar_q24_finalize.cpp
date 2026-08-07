#include "accelerator_codegen/sec_edgar_q24_finalize.hpp"

#include <algorithm>

namespace accelerator_codegen {
namespace {

constexpr size_t kResultLimit = 100;
constexpr int64_t kMinGroupCount = 11;

bool PreContains(const SecEdgarQ24PreMembershipPartition &pre_membership, const SecEdgarQ24AdshKey &adsh_key,
                 uint32_t tv_id) {
	const auto it = pre_membership.pre_by_adsh.find(adsh_key);
	if (it == pre_membership.pre_by_adsh.end()) {
		return false;
	}
	const auto &tvs = it->second;
	if (tvs.size() <= 4) {
		for (const auto candidate : tvs) {
			if (candidate == tv_id) {
				return true;
			}
		}
		return false;
	}
	return std::binary_search(tvs.begin(), tvs.end(), tv_id);
}

} // namespace

std::vector<SecEdgarQ24PartitionKey> SecEdgarQ24FixedPartitionKeys() {
	return {kSecEdgarQ24PreMembershipSlot, kSecEdgarQ24TagVersionSlot, kSecEdgarQ24GroupAggregatesSlot,
	        kSecEdgarQ24OutputSlot};
}

void RefreshSecEdgarQ24Output(const SecEdgarQ24TagVersionPartition &tag_versions,
                              const SecEdgarQ24GroupAggregatesPartition &aggregates, SecEdgarQ24OutputPartition &output) {
	output.output_rows.clear();
	std::vector<uint32_t> active_tv_ids;
	active_tv_ids.reserve(aggregates.groups.size());
	for (uint32_t tv_id = 0; tv_id < aggregates.groups.size(); tv_id++) {
		const auto &group = aggregates.groups[tv_id];
		if (group.cnt >= kMinGroupCount) {
			active_tv_ids.push_back(tv_id);
		}
	}
	const auto compare_tv = [&](uint32_t lhs_id, uint32_t rhs_id) {
		const auto &lhs = aggregates.groups[lhs_id];
		const auto &rhs = aggregates.groups[rhs_id];
		if (lhs.cnt != rhs.cnt) {
			return lhs.cnt > rhs.cnt;
		}
		if (lhs_id != rhs_id) {
			return lhs_id < rhs_id;
		}
		return lhs.total > rhs.total;
	};
	const auto output_count = std::min(active_tv_ids.size(), kResultLimit);
	if (active_tv_ids.size() > kResultLimit) {
		std::partial_sort(active_tv_ids.begin(), active_tv_ids.begin() + static_cast<ptrdiff_t>(output_count),
		                  active_tv_ids.end(), compare_tv);
		active_tv_ids.resize(output_count);
	} else {
		std::sort(active_tv_ids.begin(), active_tv_ids.end(), compare_tv);
	}
	output.output_rows.reserve(output_count);
	for (const auto tv_id : active_tv_ids) {
		const auto &group = aggregates.groups[tv_id];
		const auto &stored = tag_versions.tag_versions[tv_id];
		output.output_rows.push_back({stored.first, stored.second, group.cnt, group.total});
	}
}

bool ApplySecEdgarQ24UsdNumDelta(const SecEdgarQ24PreMembershipPartition &pre_membership,
                                 const SecEdgarQ24TagVersionPartition &tag_versions, SecEdgarQ24GroupAggregatesPartition &aggregates,
                                 SecEdgarQ24OutputPartition &output, const SecEdgarQ24AdshKey &adsh_key,
                                 const SecEdgarQ24TagVersionKey &tag_version_key, double value, int8_t sign) {
	const auto tv_it = tag_versions.tag_version_lookup.find(tag_version_key);
	if (tv_it == tag_versions.tag_version_lookup.end()) {
		return true;
	}
	const auto tv_id = tv_it->second;
	if (PreContains(pre_membership, adsh_key, tv_id)) {
		return true;
	}
	if (aggregates.groups.size() <= tv_id) {
		aggregates.groups.resize(tv_id + 1);
	}
	auto &group = aggregates.groups[tv_id];
	group.cnt += static_cast<int64_t>(sign);
	group.total += static_cast<double>(sign) * value;
	RefreshSecEdgarQ24Output(tag_versions, aggregates, output);
	return true;
}

} // namespace accelerator_codegen
