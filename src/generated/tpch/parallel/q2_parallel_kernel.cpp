#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/tpch_q2.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr duckdb::idx_t kOutputLimit = 100;
constexpr int64_t kDefaultSupplierCapacity = 10001;

bool Q2ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

struct Q2PartScanProfile {
	size_t parts_scanned = 0;
	size_t parts_size_survivors = 0;
	size_t parts_type_survivors = 0;
	size_t parts_both_survivors = 0;
	size_t mfgr_strings_allocated = 0;
	size_t flat_rows = 0;
	size_t fallback_rows = 0;
};

struct Q2SupplierScanProfile {
	size_t suppliers_scanned = 0;
	size_t suppliers_region_survivors = 0;
	size_t flat_rows = 0;
	size_t fallback_rows = 0;
};

struct Q2PartsuppScanProfile {
	size_t partsupp_rows_scanned = 0;
	size_t rows_qualifying_part = 0;
	size_t rows_qualifying_supplier = 0;
	size_t rows_qualifying_both = 0;
	size_t min_consider_calls = 0;
	size_t flat_rows = 0;
	size_t fallback_rows = 0;
};

struct Q2WorkerProfile {
	int64_t elapsed_ns = 0;
	size_t partsupp_rows = 0;
	size_t accepted_rows = 0;
};

struct Q2ParallelProfile {
	int64_t total_ns = 0;
	int64_t part_scan_ns = 0;
	int64_t supplier_scan_ns = 0;
	int64_t partsupp_scan_ns = 0;
	int64_t merge_ns = 0;
	int64_t supplier_string_fetch_ns = 0;
	int64_t output_materialize_ns = 0;
	int64_t sort_ns = 0;
	size_t worker_count = 0;
	size_t nations_in_region = 0;
	size_t qualifying_parts = 0;
	size_t regional_suppliers = 0;
	size_t partsupp_rows_scanned = 0;
	size_t partsupp_rows_both = 0;
	size_t distinct_parts_with_candidates = 0;
	size_t total_tie_candidates = 0;
	size_t max_ties_per_part = 0;
	size_t result_candidates_before_topk = 0;
	size_t output_rows_materialized = 0;
	size_t supplier_strings_copied = 0;
	size_t part_mfgr_strings_retained = 0;
	size_t supplier_dense_bytes = 0;
	size_t qualifying_part_flag_bytes = 0;
	size_t worker_aggregator_part_entries = 0;
	int64_t max_worker_ns = 0;
	int64_t avg_worker_ns = 0;
	Q2PartScanProfile part_scan;
	Q2SupplierScanProfile supplier_scan;
	Q2PartsuppScanProfile partsupp_scan;
};

void PrintQ2ParallelProfile(const Q2ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	std::cerr << "q2_parallel_profile total_ns=" << profile.total_ns << " part_scan_ns=" << profile.part_scan_ns
	          << " (" << pct(profile.part_scan_ns) << "%)"
	          << " supplier_scan_ns=" << profile.supplier_scan_ns << " (" << pct(profile.supplier_scan_ns) << "%)"
	          << " partsupp_scan_ns=" << profile.partsupp_scan_ns << " (" << pct(profile.partsupp_scan_ns) << "%)"
	          << " merge_ns=" << profile.merge_ns << " (" << pct(profile.merge_ns) << "%)"
	          << " supplier_string_fetch_ns=" << profile.supplier_string_fetch_ns
	          << " (" << pct(profile.supplier_string_fetch_ns) << "%)"
	          << " output_materialize_ns=" << profile.output_materialize_ns
	          << " (" << pct(profile.output_materialize_ns) << "%)"
	          << " sort_ns=" << profile.sort_ns << " (" << pct(profile.sort_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " nations_in_region=" << profile.nations_in_region
	          << " qualifying_parts=" << profile.qualifying_parts << " regional_suppliers=" << profile.regional_suppliers
	          << " parts_scanned=" << profile.part_scan.parts_scanned
	          << " parts_size_survivors=" << profile.part_scan.parts_size_survivors
	          << " parts_type_survivors=" << profile.part_scan.parts_type_survivors
	          << " parts_both_survivors=" << profile.part_scan.parts_both_survivors
	          << " mfgr_strings_allocated=" << profile.part_scan.mfgr_strings_allocated
	          << " part_flat_rows=" << profile.part_scan.flat_rows
	          << " part_fallback_rows=" << profile.part_scan.fallback_rows
	          << " suppliers_scanned=" << profile.supplier_scan.suppliers_scanned
	          << " suppliers_region_survivors=" << profile.supplier_scan.suppliers_region_survivors
	          << " supplier_flat_rows=" << profile.supplier_scan.flat_rows
	          << " supplier_fallback_rows=" << profile.supplier_scan.fallback_rows
	          << " partsupp_rows_scanned=" << profile.partsupp_rows_scanned
	          << " partsupp_rows_qualifying_part=" << profile.partsupp_scan.rows_qualifying_part
	          << " partsupp_rows_qualifying_supplier=" << profile.partsupp_scan.rows_qualifying_supplier
	          << " partsupp_rows_both=" << profile.partsupp_rows_both
	          << " min_consider_calls=" << profile.partsupp_scan.min_consider_calls
	          << " partsupp_flat_rows=" << profile.partsupp_scan.flat_rows
	          << " partsupp_fallback_rows=" << profile.partsupp_scan.fallback_rows
	          << " distinct_parts_with_candidates=" << profile.distinct_parts_with_candidates
	          << " total_tie_candidates=" << profile.total_tie_candidates
	          << " max_ties_per_part=" << profile.max_ties_per_part
	          << " result_candidates_before_topk=" << profile.result_candidates_before_topk
	          << " output_rows_materialized=" << profile.output_rows_materialized
	          << " supplier_strings_copied=" << profile.supplier_strings_copied
	          << " part_mfgr_strings_retained=" << profile.part_mfgr_strings_retained
	          << " supplier_dense_bytes=" << profile.supplier_dense_bytes
	          << " qualifying_part_flag_bytes=" << profile.qualifying_part_flag_bytes
	          << " worker_aggregator_part_entries=" << profile.worker_aggregator_part_entries
	          << " max_worker_ns=" << profile.max_worker_ns << " avg_worker_ns=" << profile.avg_worker_ns << "\n";
}

bool SqlLikeAt(const char *text, duckdb::idx_t text_len, const char *pattern, duckdb::idx_t pattern_len,
               duckdb::idx_t text_pos, duckdb::idx_t pattern_pos) {
	while (pattern_pos < pattern_len) {
		const char pattern_ch = pattern[pattern_pos];
		if (pattern_ch == '%') {
			if (pattern_pos + 1 == pattern_len) {
				return true;
			}
			for (duckdb::idx_t start = text_pos; start <= text_len; start++) {
				if (SqlLikeAt(text, text_len, pattern, pattern_len, start, pattern_pos + 1)) {
					return true;
				}
			}
			return false;
		}
		if (pattern_ch == '_') {
			if (text_pos >= text_len) {
				return false;
			}
			text_pos++;
			pattern_pos++;
			continue;
		}
		if (text_pos >= text_len || text[text_pos] != pattern_ch) {
			return false;
		}
		text_pos++;
		pattern_pos++;
	}
	return text_pos == text_len;
}

bool SqlLike(const duckdb::string_t &text, const std::string &pattern) {
	return SqlLikeAt(text.GetData(), text.GetSize(), pattern.data(), pattern.size(), 0, 0);
}

struct TwoPartLikeMatcher {
	std::string part1;
	std::string part2;
	bool enabled = false;

	explicit TwoPartLikeMatcher(const std::string &pattern) {
		if (pattern.size() < 4 || pattern.front() != '%' || pattern.back() != '%') {
			return;
		}
		const auto first_end = pattern.find('%', 1);
		if (first_end == std::string::npos || first_end == 1) {
			return;
		}
		const auto second_start = first_end + 1;
		const auto second_end = pattern.find('%', second_start);
		if (second_end == std::string::npos || second_end == second_start) {
			return;
		}
		if (pattern.find('%', second_end + 1) != std::string::npos) {
			return;
		}
		part1 = pattern.substr(1, first_end - 1);
		part2 = pattern.substr(second_start, second_end - second_start);
		enabled = !part1.empty() && !part2.empty();
	}

	bool Matches(const duckdb::string_t &text) const {
		if (!enabled) {
			return false;
		}
		const std::string_view haystack(text.GetData(), text.GetSize());
		size_t pos = 0;
		while (pos + part1.size() <= haystack.size()) {
			const auto found = haystack.find(part1, pos);
			if (found == std::string_view::npos) {
				return false;
			}
			if (haystack.find(part2, found + part1.size()) != std::string_view::npos) {
				return true;
			}
			pos = found + 1;
		}
		return false;
	}
};

struct SupplierCandidate {
	int64_t suppkey = 0;
	int64_t supplycost_raw = 0;
};

struct Q2OutputRow {
	int64_t acctbal_raw = 0;
	std::string supplier_name;
	std::string nation_name;
	int64_t partkey = 0;
	std::string mfgr;
	std::string address;
	std::string phone;
	std::string comment;
};

bool BetterOutputRow(const Q2OutputRow &lhs, const Q2OutputRow &rhs) {
	if (lhs.acctbal_raw != rhs.acctbal_raw) {
		return lhs.acctbal_raw > rhs.acctbal_raw;
	}
	if (lhs.nation_name != rhs.nation_name) {
		return lhs.nation_name < rhs.nation_name;
	}
	if (lhs.supplier_name != rhs.supplier_name) {
		return lhs.supplier_name < rhs.supplier_name;
	}
	return lhs.partkey < rhs.partkey;
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

class QualifyingPartSet {
public:
	bool Contains(int64_t partkey) const {
		if (partkey < 0) {
			return false;
		}
		const auto index = static_cast<size_t>(partkey);
		return index < flags.size() && flags[index] != 0;
	}

	void Insert(int64_t partkey, std::string mfgr) {
		if (partkey < 0) {
			return;
		}
		const auto index = static_cast<size_t>(partkey);
		if (index >= flags.size()) {
			EnsureCapacity(partkey);
		}
		if (flags[index] == 0) {
			flags[index] = 1;
			mfgr_by_part.emplace(partkey, std::move(mfgr));
		}
	}

	void MergeFrom(const QualifyingPartSet &other) {
		for (const auto &entry : other.mfgr_by_part) {
			if (!Contains(entry.first)) {
				Insert(entry.first, entry.second);
			}
		}
	}

	const std::string *Mfgr(int64_t partkey) const {
		const auto it = mfgr_by_part.find(partkey);
		return it == mfgr_by_part.end() ? nullptr : &it->second;
	}

	bool Empty() const {
		return mfgr_by_part.empty();
	}

	size_t QualifyingCount() const {
		return mfgr_by_part.size();
	}

	size_t FlagBytes() const {
		return flags.size();
	}

private:
	void EnsureCapacity(int64_t partkey) {
		const auto required = static_cast<size_t>(partkey) + 1;
		if (required <= flags.size()) {
			return;
		}
		size_t target = required;
		if (!flags.empty()) {
			const size_t doubled = flags.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		flags.resize(target, 0);
	}

	std::vector<uint8_t> flags;
	std::unordered_map<int64_t, std::string> mfgr_by_part;
};

class SupplierDenseStore {
public:
	SupplierDenseStore() {
		EnsureCapacity(kDefaultSupplierCapacity);
	}

	void Store(int64_t suppkey, int32_t nationkey, int64_t acctbal_raw) {
		if (suppkey < 0) {
			return;
		}
		if (suppkey >= capacity) {
			EnsureCapacity(suppkey + 1);
		}
		const auto index = static_cast<size_t>(suppkey);
		nationkeys[index] = nationkey;
		acctbals[index] = acctbal_raw;
		has_data[index] = 1;
	}

	bool HasData(int64_t suppkey) const {
		return suppkey >= 0 && suppkey < capacity && has_data[static_cast<size_t>(suppkey)] != 0;
	}

	int32_t NationKey(int64_t suppkey) const {
		return nationkeys[static_cast<size_t>(suppkey)];
	}

	int64_t AcctBalRaw(int64_t suppkey) const {
		return acctbals[static_cast<size_t>(suppkey)];
	}

	size_t RegionalCount() const {
		size_t count = 0;
		for (const auto flag : has_data) {
			count += flag != 0;
		}
		return count;
	}

	size_t DenseBytes() const {
		return nationkeys.size() + acctbals.size() + has_data.size();
	}

	void MergeFrom(const SupplierDenseStore &other) {
		for (int64_t suppkey = 0; suppkey < other.capacity; suppkey++) {
			if (!other.HasData(suppkey)) {
				continue;
			}
			Store(suppkey, other.NationKey(suppkey), other.AcctBalRaw(suppkey));
		}
	}

private:
	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		const auto target_size = static_cast<size_t>(target);
		nationkeys.resize(target_size, 0);
		acctbals.resize(target_size, 0);
		has_data.resize(target_size, 0);
		capacity = target;
	}

	int64_t capacity = 0;
	std::vector<int32_t> nationkeys;
	std::vector<int64_t> acctbals;
	std::vector<uint8_t> has_data;
};

struct SupplierStrings {
	std::string name;
	std::string address;
	std::string phone;
	std::string comment;
};

class PartCandidateAggregator {
public:
	void Consider(int64_t partkey, int64_t suppkey, int64_t supplycost_raw) {
		const auto existing = min_supplycost_by_part.find(partkey);
		if (existing == min_supplycost_by_part.end()) {
			min_supplycost_by_part.emplace(partkey, supplycost_raw);
			part_candidates[partkey] = {{suppkey, supplycost_raw}};
			return;
		}
		if (supplycost_raw < existing->second) {
			existing->second = supplycost_raw;
			part_candidates[partkey] = {{suppkey, supplycost_raw}};
			return;
		}
		if (supplycost_raw == existing->second) {
			auto &candidates = part_candidates[partkey];
			for (const auto &candidate : candidates) {
				if (candidate.suppkey == suppkey) {
					return;
				}
			}
			candidates.push_back({suppkey, supplycost_raw});
		}
	}

	void MergeFrom(const PartCandidateAggregator &other) {
		for (const auto &entry : other.min_supplycost_by_part) {
			const auto partkey = entry.first;
			const auto cost = entry.second;
			const auto &local_candidates = other.part_candidates.at(partkey);
			const auto existing = min_supplycost_by_part.find(partkey);
			if (existing == min_supplycost_by_part.end()) {
				min_supplycost_by_part.emplace(partkey, cost);
				part_candidates[partkey] = local_candidates;
				continue;
			}
			if (cost < existing->second) {
				existing->second = cost;
				part_candidates[partkey] = local_candidates;
				continue;
			}
			if (cost == existing->second) {
				auto &dest = part_candidates[partkey];
				for (const auto &candidate : local_candidates) {
					bool found = false;
					for (const auto &existing_candidate : dest) {
						if (existing_candidate.suppkey == candidate.suppkey) {
							found = true;
							break;
						}
					}
					if (!found) {
						dest.push_back(candidate);
					}
				}
			}
		}
	}

	std::unordered_map<int64_t, int64_t> min_supplycost_by_part;
	std::unordered_map<int64_t, std::vector<SupplierCandidate>> part_candidates;
};

struct Q2FrozenState {
	const Q2Parameters &params;
	TwoPartLikeMatcher part_type_matcher;
	std::string fallback_part_type_pattern;
	QualifyingPartSet qualifying_parts;
	SupplierDenseStore supplier_store;

	explicit Q2FrozenState(const Q2Parameters &params_p)
	    : params(params_p), part_type_matcher(params_p.part_type_pattern),
	      fallback_part_type_pattern(params_p.part_type_pattern) {
	}

	bool IsRegionNation(int32_t nationkey) const {
		return nationkey >= 0 && static_cast<size_t>(nationkey) < params.region_nations.size() &&
		       params.region_nations[static_cast<size_t>(nationkey)];
	}

	bool PartTypeMatches(const duckdb::string_t &type_value) const {
		if (part_type_matcher.enabled) {
			return part_type_matcher.Matches(type_value);
		}
		return SqlLike(type_value, fallback_part_type_pattern);
	}
};

void ConsumePartChunk(duckdb::DataChunk &chunk, const Q2FrozenState &state, QualifyingPartSet &qualifying_parts,
                      Q2PartScanProfile *profile) {
	auto &partkey_vector = chunk.data[Q2PartChunkLayout::PARTKEY_INDEX];
	auto &mfgr_vector = chunk.data[Q2PartChunkLayout::MFGR_INDEX];
	auto &type_vector = chunk.data[Q2PartChunkLayout::TYPE_INDEX];
	const bool flat_fast_path = IsFlatAllValid(partkey_vector) && IsFlatAllValid(mfgr_vector) &&
	                            IsFlatAllValid(type_vector);
	if (profile) {
		profile->flat_rows += flat_fast_path ? chunk.size() : 0;
		profile->fallback_rows += flat_fast_path ? 0 : chunk.size();
	}
	if (flat_fast_path) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto mfgrs = duckdb::FlatVector::GetData<duckdb::string_t>(mfgr_vector);
		auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->parts_scanned++;
				profile->parts_size_survivors++;
			}
			if (!state.PartTypeMatches(types[row])) {
				continue;
			}
			if (profile) {
				profile->parts_type_survivors++;
				profile->parts_both_survivors++;
				profile->mfgr_strings_allocated++;
			}
			qualifying_parts.Insert(partkeys[row], ReadString(mfgrs[row]));
		}
		return;
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat mfgr_format;
	duckdb::UnifiedVectorFormat type_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	mfgr_vector.ToUnifiedFormat(chunk.size(), mfgr_format);
	type_vector.ToUnifiedFormat(chunk.size(), type_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto mfgr_index = mfgr_format.sel->get_index(row);
		const auto type_index = type_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!mfgr_format.validity.AllValid() && !mfgr_format.validity.RowIsValidUnsafe(mfgr_index)) ||
		    (!type_format.validity.AllValid() && !type_format.validity.RowIsValidUnsafe(type_index))) {
			continue;
		}
		if (profile) {
			profile->parts_scanned++;
			profile->parts_size_survivors++;
		}
		const auto type_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format)[type_index];
		if (!state.PartTypeMatches(type_value)) {
			continue;
		}
		if (profile) {
			profile->parts_type_survivors++;
			profile->parts_both_survivors++;
			profile->mfgr_strings_allocated++;
		}
		qualifying_parts.Insert(partkeys[partkey_index],
		                        ReadString(
		                            duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(mfgr_format)[mfgr_index]));
	}
}

void ConsumeSupplierNumericChunk(duckdb::DataChunk &chunk, const Q2FrozenState &state, SupplierDenseStore &supplier_store,
                                 Q2SupplierScanProfile *profile) {
	auto &suppkey_vector = chunk.data[Q2SupplierNumericChunkLayout::SUPPKEY_INDEX];
	auto &nationkey_vector = chunk.data[Q2SupplierNumericChunkLayout::NATIONKEY_INDEX];
	auto &acctbal_vector = chunk.data[Q2SupplierNumericChunkLayout::ACCTBAL_INDEX];
	const bool flat_fast_path = IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector) &&
	                            IsFlatAllValid(acctbal_vector);
	if (profile) {
		profile->flat_rows += flat_fast_path ? chunk.size() : 0;
		profile->fallback_rows += flat_fast_path ? 0 : chunk.size();
	}
	if (flat_fast_path) {
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->suppliers_scanned++;
			}
			if (!state.IsRegionNation(nationkeys[row])) {
				continue;
			}
			if (profile) {
				profile->suppliers_region_survivors++;
			}
			supplier_store.Store(suppkeys[row], nationkeys[row], acctbals[row]);
		}
		return;
	}
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	duckdb::UnifiedVectorFormat acctbal_format;
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	nationkey_vector.ToUnifiedFormat(chunk.size(), nationkey_format);
	acctbal_vector.ToUnifiedFormat(chunk.size(), acctbal_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(acctbal_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		const auto acctbal_index = acctbal_format.sel->get_index(row);
		if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
		    (!acctbal_format.validity.AllValid() && !acctbal_format.validity.RowIsValidUnsafe(acctbal_index))) {
			continue;
		}
		if (profile) {
			profile->suppliers_scanned++;
		}
		const int32_t nationkey = nationkeys[nationkey_index];
		if (!state.IsRegionNation(nationkey)) {
			continue;
		}
		if (profile) {
			profile->suppliers_region_survivors++;
		}
		supplier_store.Store(suppkeys[suppkey_index], nationkey, acctbals[acctbal_index]);
	}
}

void BuildPartState(duckdb::ClientContext &context, const duckdb::FunctionData &part_bind, Q2FrozenState &state,
                    duckdb::idx_t worker_count, Q2PartScanProfile *profile) {
	std::vector<CacheLinePadded<QualifyingPartSet>> part_workers(worker_count);
	RunParallelChunkScan(context, part_bind, worker_count, [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
		ConsumePartChunk(chunk, state, part_workers[worker_id].value, profile);
	});
	for (const auto &worker : part_workers) {
		state.qualifying_parts.MergeFrom(worker.value);
	}
}

void BuildSupplierNumericState(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                               Q2FrozenState &state, duckdb::idx_t worker_count, Q2SupplierScanProfile *profile) {
	std::vector<CacheLinePadded<SupplierDenseStore>> supplier_workers(worker_count);
	RunParallelChunkScan(context, supplier_bind, worker_count,
	                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
		                     ConsumeSupplierNumericChunk(chunk, state, supplier_workers[worker_id].value, profile);
	                     });
	for (const auto &worker : supplier_workers) {
		state.supplier_store.MergeFrom(worker.value);
	}
}

void AccumulatePartsuppChunk(duckdb::DataChunk &chunk, const Q2FrozenState &state, PartCandidateAggregator &agg,
                             Q2PartsuppScanProfile *profile) {
	if (state.qualifying_parts.Empty()) {
		return;
	}
	auto &partkey_vector = chunk.data[Q2PartsuppChunkLayout::PARTKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q2PartsuppChunkLayout::SUPPKEY_INDEX];
	auto &supplycost_vector = chunk.data[Q2PartsuppChunkLayout::SUPPLYCOST_INDEX];
	const bool flat_fast_path = IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) &&
	                            IsFlatAllValid(supplycost_vector);
	if (profile) {
		profile->flat_rows += flat_fast_path ? chunk.size() : 0;
		profile->fallback_rows += flat_fast_path ? 0 : chunk.size();
	}
	if (flat_fast_path) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto supplycosts = duckdb::FlatVector::GetData<int64_t>(supplycost_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->partsupp_rows_scanned++;
			}
			const auto partkey = partkeys[row];
			if (!state.qualifying_parts.Contains(partkey)) {
				continue;
			}
			if (profile) {
				profile->rows_qualifying_part++;
			}
			const auto suppkey = suppkeys[row];
			if (!state.supplier_store.HasData(suppkey)) {
				continue;
			}
			if (profile) {
				profile->rows_qualifying_supplier++;
				profile->rows_qualifying_both++;
				profile->min_consider_calls++;
			}
			agg.Consider(partkey, suppkey, supplycosts[row]);
		}
		return;
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat supplycost_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	supplycost_vector.ToUnifiedFormat(chunk.size(), supplycost_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto supplycost_index = supplycost_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!supplycost_format.validity.AllValid() && !supplycost_format.validity.RowIsValidUnsafe(supplycost_index))) {
			continue;
		}
		if (profile) {
			profile->partsupp_rows_scanned++;
		}
		const auto partkey = partkeys[partkey_index];
		if (!state.qualifying_parts.Contains(partkey)) {
			continue;
		}
		if (profile) {
			profile->rows_qualifying_part++;
		}
		const auto suppkey = suppkeys[suppkey_index];
		if (!state.supplier_store.HasData(suppkey)) {
			continue;
		}
		if (profile) {
			profile->rows_qualifying_supplier++;
			profile->rows_qualifying_both++;
			profile->min_consider_calls++;
		}
		agg.Consider(partkey, suppkey, supplycosts[supplycost_index]);
	}
}

size_t FetchSupplierStrings(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                            const std::vector<uint8_t> &needed_suppkeys,
                            std::unordered_map<int64_t, SupplierStrings> &supplier_strings) {
	if (needed_suppkeys.empty()) {
		return 0;
	}
	size_t strings_copied = 0;
	auto source = CreateNativeChunkSource(context, supplier_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat address_format;
		duckdb::UnifiedVectorFormat phone_format;
		duckdb::UnifiedVectorFormat comment_format;
		chunk.data[Q2SupplierVarcharChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(chunk.size(), suppkey_format);
		chunk.data[Q2SupplierVarcharChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		chunk.data[Q2SupplierVarcharChunkLayout::ADDRESS_INDEX].ToUnifiedFormat(chunk.size(), address_format);
		chunk.data[Q2SupplierVarcharChunkLayout::PHONE_INDEX].ToUnifiedFormat(chunk.size(), phone_format);
		chunk.data[Q2SupplierVarcharChunkLayout::COMMENT_INDEX].ToUnifiedFormat(chunk.size(), comment_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto address_index = address_format.sel->get_index(row);
			const auto phone_index = phone_format.sel->get_index(row);
			const auto comment_index = comment_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index)) ||
			    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index)) ||
			    (!comment_format.validity.AllValid() && !comment_format.validity.RowIsValidUnsafe(comment_index))) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			const auto index = static_cast<size_t>(suppkey);
			if (suppkey < 0 || index >= needed_suppkeys.size() || needed_suppkeys[index] == 0) {
				continue;
			}
			auto &strings = supplier_strings[suppkey];
			strings.name = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]);
			strings.address = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index]);
			strings.phone = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index]);
			strings.comment = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comment_format)[comment_index]);
			strings_copied += 4;
		}
	}
	return strings_copied;
}

class Q2ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q2ParallelKernel(Q2Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 4 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2] ||
		    !bound_inputs[3]) {
			throw duckdb::InternalException("Q2 parallel expects bound part, supplier numeric, partsupp, supplier varchar scans");
		}

		using clock = std::chrono::steady_clock;
		const bool profile_enabled = Q2ProfileEnabled();
		Q2ParallelProfile profile;
		const auto total_t0 = clock::now();

		Q2FrozenState frozen(params);
		if (profile_enabled) {
			for (const bool in_region : params.region_nations) {
				profile.nations_in_region += in_region ? 1 : 0;
			}
		}

		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);
		std::vector<CacheLinePadded<PartCandidateAggregator>> workers(worker_count);
		std::vector<Q2WorkerProfile> worker_profiles(profile_enabled ? static_cast<size_t>(worker_count) : 0);

		{
			const auto t0 = clock::now();
			BuildPartState(context, *bound_inputs[0], frozen, worker_count,
			               profile_enabled ? &profile.part_scan : nullptr);
			profile.part_scan_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}
		{
			const auto t0 = clock::now();
			BuildSupplierNumericState(context, *bound_inputs[1], frozen, worker_count,
			                          profile_enabled ? &profile.supplier_scan : nullptr);
			profile.supplier_scan_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}
		if (profile_enabled) {
			profile.qualifying_parts = frozen.qualifying_parts.QualifyingCount();
			profile.regional_suppliers = frozen.supplier_store.RegionalCount();
			profile.qualifying_part_flag_bytes = frozen.qualifying_parts.FlagBytes();
			profile.supplier_dense_bytes = frozen.supplier_store.DenseBytes();
			profile.part_mfgr_strings_retained = profile.qualifying_parts;
		}

		if (!frozen.qualifying_parts.Empty()) {
			const auto t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[2], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
				                     AccumulatePartsuppChunk(chunk, frozen, workers[worker_id].value,
				                                             profile_enabled ? &profile.partsupp_scan : nullptr);
				                     if (profile_enabled) {
					                     auto &worker_profile = worker_profiles[worker_id];
					                     worker_profile.partsupp_rows += chunk.size();
					                     worker_profile.elapsed_ns +=
					                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() -
					                                                                            worker_t0)
					                             .count();
				                     }
			                     });
			profile.partsupp_scan_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}
		if (profile_enabled) {
			profile.partsupp_rows_scanned = profile.partsupp_scan.partsupp_rows_scanned;
			profile.partsupp_rows_both = profile.partsupp_scan.rows_qualifying_both;
			int64_t worker_ns_sum = 0;
			for (const auto &worker_profile : worker_profiles) {
				worker_ns_sum += worker_profile.elapsed_ns;
				profile.max_worker_ns = std::max(profile.max_worker_ns, worker_profile.elapsed_ns);
			}
			profile.avg_worker_ns =
			    worker_profiles.empty() ? 0 : worker_ns_sum / static_cast<int64_t>(worker_profiles.size());
		}

		PartCandidateAggregator merged;
		{
			const auto t0 = clock::now();
			for (const auto &worker : workers) {
				merged.MergeFrom(worker.value);
			}
			profile.merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}
		if (profile_enabled) {
			profile.worker_aggregator_part_entries = merged.min_supplycost_by_part.size();
			profile.distinct_parts_with_candidates = merged.part_candidates.size();
			for (const auto &entry : merged.part_candidates) {
				profile.total_tie_candidates += entry.second.size();
				profile.max_ties_per_part = std::max(profile.max_ties_per_part, entry.second.size());
			}
		}

		std::vector<uint8_t> needed_suppkeys;
		for (const auto &entry : merged.part_candidates) {
			for (const auto &candidate : entry.second) {
				const auto suppkey = candidate.suppkey;
				if (suppkey < 0) {
					continue;
				}
				const auto index = static_cast<size_t>(suppkey);
				if (index >= needed_suppkeys.size()) {
					needed_suppkeys.resize(index + 1, 0);
				}
				needed_suppkeys[index] = 1;
			}
		}

		std::unordered_map<int64_t, SupplierStrings> supplier_strings;
		{
			const auto t0 = clock::now();
			profile.supplier_strings_copied =
			    FetchSupplierStrings(context, *bound_inputs[3], needed_suppkeys, supplier_strings);
			profile.supplier_string_fetch_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}

		output_rows.clear();
		output_rows.reserve(merged.part_candidates.size());
		{
			const auto t0 = clock::now();
			for (const auto &entry : merged.part_candidates) {
				const int64_t partkey = entry.first;
				const auto *mfgr = frozen.qualifying_parts.Mfgr(partkey);
				if (mfgr == nullptr) {
					continue;
				}
				for (const auto &candidate : entry.second) {
					if (!frozen.supplier_store.HasData(candidate.suppkey)) {
						continue;
					}
					const int32_t nationkey = frozen.supplier_store.NationKey(candidate.suppkey);
					const auto nation_it = params.nation_names.find(nationkey);
					if (nation_it == params.nation_names.end()) {
						continue;
					}
					const auto strings_it = supplier_strings.find(candidate.suppkey);
					if (strings_it == supplier_strings.end()) {
						continue;
					}
					Q2OutputRow output_row;
					output_row.acctbal_raw = frozen.supplier_store.AcctBalRaw(candidate.suppkey);
					output_row.supplier_name = strings_it->second.name;
					output_row.nation_name = nation_it->second;
					output_row.partkey = partkey;
					output_row.mfgr = *mfgr;
					output_row.address = strings_it->second.address;
					output_row.phone = strings_it->second.phone;
					output_row.comment = strings_it->second.comment;
					output_rows.push_back(std::move(output_row));
				}
			}
			profile.output_materialize_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}
		if (profile_enabled) {
			profile.result_candidates_before_topk = output_rows.size();
			profile.output_rows_materialized = output_rows.size();
		}
		{
			const auto t0 = clock::now();
			std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
			if (output_rows.size() > kOutputLimit) {
				output_rows.resize(kOutputLimit);
			}
			profile.sort_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}

		profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
		if (profile_enabled) {
			PrintQ2ParallelProfile(profile);
		}
	}

	void Finalize() override {
		output_offset = 0;
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= output_rows.size()) {
			return false;
		}
		const duckdb::idx_t remaining = output_rows.size() - output_offset;
		const duckdb::idx_t count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &entry = output_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value::DECIMAL(duckdb::hugeint_t(entry.acctbal_raw), 15, 2));
			output.SetValue(1, row, duckdb::Value(entry.supplier_name));
			output.SetValue(2, row, duckdb::Value(entry.nation_name));
			output.SetValue(3, row, duckdb::Value::BIGINT(entry.partkey));
			output.SetValue(4, row, duckdb::Value(entry.mfgr));
			output.SetValue(5, row, duckdb::Value(entry.address));
			output.SetValue(6, row, duckdb::Value(entry.phone));
			output.SetValue(7, row, duckdb::Value(entry.comment));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q2Parameters params;
	std::vector<Q2OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ2ParallelKernel(const Q2Parameters &params) {
	return duckdb::make_uniq<Q2ParallelKernel>(params);
}

} // namespace accelerator_codegen
