#include "sec_edgar/q4/aux/finalize.hpp"

#include <algorithm>

namespace accelerator_codegen {
namespace {

constexpr int32_t kMinDistinctCik = 2;
constexpr size_t kResultLimit = 500;

void EnsureCikBitCapacity(SecEdgarQ4GroupAggregate &group, uint32_t cik_bit_word_count) {
	if (group.cik_bits.size() != cik_bit_word_count) {
		group.cik_bits.assign(cik_bit_word_count, 0);
	}
}

void AccumulateGroup(SecEdgarQ4GroupAggregate &group, uint32_t cik_dense_id, uint32_t cik_bit_word_count,
                     uint32_t cik_dense_count, double value, uint32_t multiplicity) {
	if (!group.active) {
		group.active = true;
		EnsureCikBitCapacity(group, cik_bit_word_count);
	}
	const auto delta_sum = value * static_cast<double>(multiplicity);
	group.sum += delta_sum;
	group.count += static_cast<int64_t>(multiplicity);
	if (cik_bit_word_count > 0 && cik_dense_id < cik_dense_count) {
		const auto word = cik_dense_id >> 6;
		const auto bit = cik_dense_id & 63U;
		const auto mask = uint64_t(1) << bit;
		if ((group.cik_bits[word] & mask) == 0) {
			group.cik_bits[word] |= mask;
			group.distinct_cik_count++;
		}
	}
}

} // namespace

std::vector<SecEdgarQ4PartitionKey> SecEdgarQ4FixedPartitionKeys() {
	return {kSecEdgarQ4SicCohortSlot, kSecEdgarQ4PreEqBridgeSlot, kSecEdgarQ4TagTlabelSlot, kSecEdgarQ4GroupAggregatesSlot,
	        kSecEdgarQ4OutputSlot};
}

void RefreshSecEdgarQ4Output(const SecEdgarQ4GroupAggregatesPartition &aggregates,
                             const SecEdgarQ4TagTlabelPartition &tag_tlabel, SecEdgarQ4OutputPartition &output) {
	output.output_rows.clear();
	output.output_rows.reserve(aggregates.groups.size());
	for (const auto &group : aggregates.groups) {
		if (!group.active || static_cast<int64_t>(group.distinct_cik_count) < kMinDistinctCik || group.count == 0) {
			continue;
		}
		SecEdgarQ4OutputRow row;
		row.sic = group.sic;
		row.tlabel = group.tlabel_id < tag_tlabel.tlabels.size() ? tag_tlabel.tlabels[group.tlabel_id] : std::string();
		row.stmt = "EQ";
		row.num_companies = static_cast<int64_t>(group.distinct_cik_count);
		row.total_value = group.sum;
		row.avg_value = group.sum / static_cast<double>(group.count);
		output.output_rows.push_back(std::move(row));
	}
	std::sort(output.output_rows.begin(), output.output_rows.end(),
	          [](const SecEdgarQ4OutputRow &lhs, const SecEdgarQ4OutputRow &rhs) {
		          if (lhs.total_value != rhs.total_value) {
			          return lhs.total_value > rhs.total_value;
		          }
		          if (lhs.sic != rhs.sic) {
			          return lhs.sic < rhs.sic;
		          }
		          return lhs.tlabel < rhs.tlabel;
	          });
	if (output.output_rows.size() > kResultLimit) {
		output.output_rows.resize(kResultLimit);
	}
}

bool ApplySecEdgarQ4UsdNumDelta(const SecEdgarQ4SicCohortPartition &cohort,
                                const SecEdgarQ4PreEqBridgePartition &pre_bridge,
                                const SecEdgarQ4TagTlabelPartition &tag_tlabel, SecEdgarQ4GroupAggregatesPartition &aggregates,
                                SecEdgarQ4OutputPartition &output, const SecEdgarQ4AdshKey &adsh_key,
                                const SecEdgarQ4TagVersionKey &tag_version_key, double value, int8_t sign) {
	const auto sub_it = cohort.sub_lookup.find(adsh_key);
	if (sub_it == cohort.sub_lookup.end()) {
		return true;
	}
	const auto tag_it = tag_tlabel.tag_version_lookup.find(tag_version_key);
	if (tag_it == tag_tlabel.tag_version_lookup.end()) {
		return true;
	}
	const auto pre_adsh_it = pre_bridge.pre_by_adsh.find(adsh_key);
	if (pre_adsh_it == pre_bridge.pre_by_adsh.end()) {
		return true;
	}
	const auto pre_entry_it = pre_adsh_it->second.find(tag_it->second);
	if (pre_entry_it == pre_adsh_it->second.end() || pre_entry_it->second.multiplicity == 0) {
		return true;
	}
	const auto &resolved = pre_entry_it->second;
	if (resolved.group_id >= aggregates.groups.size()) {
		return false;
	}
	auto &group = aggregates.groups[resolved.group_id];
	const auto scaled_value = static_cast<double>(sign) * value;
	AccumulateGroup(group, resolved.cik_dense_id, cohort.cik_bit_word_count, cohort.cik_dense_count, scaled_value,
	                resolved.multiplicity);
	RefreshSecEdgarQ4Output(aggregates, tag_tlabel, output);
	return true;
}

} // namespace accelerator_codegen
