#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/state/partition_key.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

inline constexpr aux_state::PartitionKey kSecEdgarQ24PreMembershipSlot = 0;
inline constexpr aux_state::PartitionKey kSecEdgarQ24TagVersionSlot = 1;
inline constexpr aux_state::PartitionKey kSecEdgarQ24GroupAggregatesSlot = 2;
inline constexpr aux_state::PartitionKey kSecEdgarQ24OutputSlot = 3;

using SecEdgarQ24PartitionKey = aux_state::PartitionKey;

struct SecEdgarQ24AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;

	static SecEdgarQ24AdshKey From(const duckdb::string_t &value) {
		SecEdgarQ24AdshKey key;
		key.size = static_cast<uint32_t>(value.GetSize());
		const auto *data = value.GetData();
		if (data == nullptr || key.size == 0) {
			return key;
		}
		std::memcpy(&key.w0, data, std::min<duckdb::idx_t>(key.size, 8));
		if (key.size > 8) {
			std::memcpy(&key.w1, data + 8, std::min<duckdb::idx_t>(key.size - 8, 8));
		}
		if (key.size > 16) {
			std::memcpy(&key.w2, data + 16, std::min<duckdb::idx_t>(key.size - 16, 8));
		}
		return key;
	}

	static SecEdgarQ24AdshKey FromStored(const std::string &value) {
		return From(duckdb::string_t(value));
	}

	bool operator==(const SecEdgarQ24AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2;
	}
};

struct SecEdgarQ24AdshKeyHash {
	size_t operator()(const SecEdgarQ24AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ key.size;
	}
};

struct SecEdgarQ24TagVersionKey {
	std::string tag;
	std::string version;

	static SecEdgarQ24TagVersionKey From(const duckdb::string_t &tag, const duckdb::string_t &version) {
		return {std::string(tag.GetData(), tag.GetSize()), std::string(version.GetData(), version.GetSize())};
	}

	static SecEdgarQ24TagVersionKey FromStored(const std::string &tag, const std::string &version) {
		return {tag, version};
	}

	bool operator==(const SecEdgarQ24TagVersionKey &other) const {
		return tag == other.tag && version == other.version;
	}
};

struct SecEdgarQ24TagVersionKeyHash {
	size_t operator()(const SecEdgarQ24TagVersionKey &key) const noexcept {
		return std::hash<std::string> {}(key.tag) ^ (std::hash<std::string> {}(key.version) << 1);
	}
};

struct SecEdgarQ24PreMembershipPartition {
	std::unordered_map<SecEdgarQ24AdshKey, std::vector<uint32_t>, SecEdgarQ24AdshKeyHash> pre_by_adsh;
};

struct SecEdgarQ24TagVersionPartition {
	std::unordered_map<SecEdgarQ24TagVersionKey, uint32_t, SecEdgarQ24TagVersionKeyHash> tag_version_lookup;
	std::vector<std::pair<std::string, std::string>> tag_versions;
};

struct SecEdgarQ24GroupAggregate {
	int64_t cnt = 0;
	double total = 0.0;
};

struct SecEdgarQ24GroupAggregatesPartition {
	std::vector<SecEdgarQ24GroupAggregate> groups;
};

struct SecEdgarQ24OutputRow {
	std::string tag;
	std::string version;
	int64_t cnt = 0;
	double total = 0.0;
};

struct SecEdgarQ24OutputPartition {
	std::vector<SecEdgarQ24OutputRow> output_rows;
};

std::vector<SecEdgarQ24PartitionKey> SecEdgarQ24FixedPartitionKeys();
void RefreshSecEdgarQ24Output(const SecEdgarQ24TagVersionPartition &tag_versions,
                                const SecEdgarQ24GroupAggregatesPartition &aggregates, SecEdgarQ24OutputPartition &output);
bool ApplySecEdgarQ24UsdNumDelta(const SecEdgarQ24PreMembershipPartition &pre_membership,
                                 const SecEdgarQ24TagVersionPartition &tag_versions, SecEdgarQ24GroupAggregatesPartition &aggregates,
                                 SecEdgarQ24OutputPartition &output, const SecEdgarQ24AdshKey &adsh_key,
                                 const SecEdgarQ24TagVersionKey &tag_version_key, double value, int8_t sign);

} // namespace accelerator_codegen
