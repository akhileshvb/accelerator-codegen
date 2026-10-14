#pragma once
// Private state of q1::Builder, shared by the engine-neutral build paths
// (state.cpp) and the engine scan adaptors (e.g. duckdb/aux/chunk_builder.cpp).

#include "sec_edgar/q1/common/module.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace accelerator_codegen::aux_state::query {
namespace q1 {

struct Builder::Impl {
	struct G {
		std::string stmt;
		std::string rfile;
		bool rfile_null = false;
		int64_t cnt = 0;
		Int128 line_sum = 0;
		int64_t line_cnt = 0;
		std::vector<uint32_t> counts; // by builder-local adsh id
	};

	static constexpr uint64_t kEmpty = ~uint64_t(0);
	static constexpr uint64_t kMaxLocalIds = 0xFFFFFFFEull;

	std::vector<G> groups;
	std::unordered_map<std::string, int> group_index;
	std::vector<uint64_t> table_keys;
	std::vector<uint32_t> table_ids;
	std::vector<uint64_t> id_keys;
	core::StringDict others;
	std::size_t mask = 0;

	// ConsumeRowView memo: the previous row's group and accession.
	static constexpr std::size_t kMemoAdshMax = 32;
	int memo_group = -1;
	std::size_t memo_adsh_len = ~std::size_t(0);
	char memo_adsh[kMemoAdshMax];
	uint32_t memo_id = 0;

	// chunk scratch
	struct Run {
		uint32_t start, len, si, ri, ai;
	};
	std::vector<Run> runs;
	std::vector<uint32_t> adsh_sel;
	std::vector<uint64_t> adsh_keys;

	Impl() {
		Rehash(1u << 12);
	}

	void Rehash(std::size_t capacity) {
		std::vector<uint64_t> old_keys = std::move(table_keys);
		std::vector<uint32_t> old_ids = std::move(table_ids);
		table_keys.assign(capacity, kEmpty);
		table_ids.assign(capacity, 0);
		mask = capacity - 1;
		for (std::size_t i = 0; i < old_keys.size(); ++i) {
			if (old_keys[i] != kEmpty) {
				Insert(old_keys[i], old_ids[i]);
			}
		}
	}

	static inline std::size_t Hash(uint64_t key) {
		return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> 20);
	}

	inline void Insert(uint64_t key, uint32_t id) {
		std::size_t pos = Hash(key) & mask;
		while (table_keys[pos] != kEmpty) {
			pos = (pos + 1) & mask;
		}
		table_keys[pos] = key;
		table_ids[pos] = id;
	}

	inline uint32_t IdFor(uint64_t key) {
		std::size_t pos = Hash(key) & mask;
		while (true) {
			const uint64_t k = table_keys[pos];
			if (k == key) {
				return table_ids[pos];
			}
			if (k == kEmpty) {
				break;
			}
			pos = (pos + 1) & mask;
		}
		if (id_keys.size() >= kMaxLocalIds) {
			throw std::overflow_error("sec_edgar q1: more than 2^32-2 distinct accession values in one build");
		}
		const uint32_t id = static_cast<uint32_t>(id_keys.size());
		id_keys.push_back(key);
		table_keys[pos] = key;
		table_ids[pos] = id;
		if (id_keys.size() * 2 > table_keys.size()) {
			Rehash(table_keys.size() * 2);
		}
		return id;
	}

	uint64_t OtherKey(const char *data, std::size_t size) {
		return kOtherKeyBit | others.Intern(std::string_view(data, size));
	}

	static std::string GroupName(const char *stmt, std::size_t stmt_len, const char *rfile, std::size_t rfile_len,
	                             bool rfile_null) {
		std::string name = std::to_string(stmt_len);
		name.push_back(':');
		name.append(stmt, stmt_len);
		name.push_back(rfile_null ? '\1' : '\2');
		if (!rfile_null) {
			name.append(rfile, rfile_len);
		}
		return name;
	}

	int GroupFor(const char *stmt, std::size_t stmt_len, const char *rfile, std::size_t rfile_len, bool rfile_null) {
		auto name = GroupName(stmt, stmt_len, rfile, rfile_len, rfile_null);
		const auto it = group_index.find(name);
		if (it != group_index.end()) {
			return it->second;
		}
		G g;
		g.stmt.assign(stmt, stmt_len);
		if (!rfile_null) {
			g.rfile.assign(rfile, rfile_len);
		}
		g.rfile_null = rfile_null;
		groups.push_back(std::move(g));
		const int index = static_cast<int>(groups.size() - 1);
		group_index.emplace(std::move(name), index);
		return index;
	}

	inline void Bump(G &g, uint32_t id, uint64_t by = 1) {
		if (id >= g.counts.size()) {
			g.counts.resize(std::max<std::size_t>(id + 1, g.counts.size() + g.counts.size() / 2 + 1024), 0);
		}
		const uint64_t sum = uint64_t(g.counts[id]) + by;
		if (sum > 0xFFFFFFFFull) {
			throw std::overflow_error("sec_edgar q1: more than 2^32-1 rows for one (group, accession) in one build");
		}
		g.counts[id] = static_cast<uint32_t>(sum);
	}
};


} // namespace q1
} // namespace accelerator_codegen::aux_state::query
