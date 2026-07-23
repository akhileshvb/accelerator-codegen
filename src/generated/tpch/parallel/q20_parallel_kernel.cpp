#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/parallel/parallel_runtime.hpp"
#include "accelerator_codegen/tpch_q20.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

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

struct PrefixLikeMatcher {
	std::string prefix;
	bool enabled = false;

	explicit PrefixLikeMatcher(const std::string &pattern) {
		if (pattern.empty() || pattern.back() != '%') {
			return;
		}
		if (pattern.find('%', 0) != pattern.size() - 1) {
			return;
		}
		prefix = pattern.substr(0, pattern.size() - 1);
		enabled = !prefix.empty();
	}

	bool Matches(const duckdb::string_t &text) const {
		if (!enabled) {
			return false;
		}
		if (text.GetSize() < prefix.size()) {
			return false;
		}
		return std::memcmp(text.GetData(), prefix.data(), prefix.size()) == 0;
	}
};

inline uint64_t MakeAggKey(int64_t partkey, int64_t suppkey) {
	return (static_cast<uint64_t>(partkey) << 20) | static_cast<uint64_t>(suppkey);
}

struct Q20OutputRow {
	std::string name;
	std::string address;
};

struct SupplierRow {
	std::string name;
	std::string address;
};

struct NationSupplierIndex {
	std::vector<int64_t> compact_to_suppkey;
	std::unordered_map<int64_t, int32_t> suppkey_to_compact;
	std::vector<SupplierRow> supplier_rows;

	int32_t compact_count() const {
		return static_cast<int32_t>(compact_to_suppkey.size());
	}

	bool IsNationSupplier(int64_t suppkey) const {
		return suppkey_to_compact.find(suppkey) != suppkey_to_compact.end();
	}

	int32_t ToCompact(int64_t suppkey) const {
		const auto it = suppkey_to_compact.find(suppkey);
		return it == suppkey_to_compact.end() ? -1 : it->second;
	}

	void Insert(int64_t suppkey, SupplierRow row) {
		if (suppkey_to_compact.emplace(suppkey, compact_count()).second) {
			compact_to_suppkey.push_back(suppkey);
			supplier_rows.push_back(std::move(row));
		}
	}
};

struct PartsuppWorkerStats {
	size_t rows_scanned = 0;
	size_t after_part_filter = 0;
	size_t after_nation_filter = 0;
	size_t qty_probes = 0;
	size_t qty_hits = 0;
	size_t after_threshold = 0;
	size_t flat_chunks = 0;
	int64_t process_ns = 0;
};

struct Q20ParallelProfile {
	int64_t part_ns = 0;
	int64_t nation_supplier_ns = 0;
	int64_t lineitem_ns = 0;
	int64_t merge_ns = 0;
	int64_t partsupp_scan_ns = 0;
	int64_t partsupp_merge_ns = 0;
	int64_t output_ns = 0;
	size_t worker_count = 0;
	size_t accepted_lineitems = 0;
	size_t distinct_qty_keys = 0;
	size_t worker_local_map_entries = 0;
	size_t quantity_map_buckets = 0;
	size_t nation_supplier_count = 0;
	size_t qualifying_supplier_count = 0;
	size_t partsupp_rows_scanned = 0;
	size_t partsupp_after_part_filter = 0;
	size_t partsupp_after_nation_filter = 0;
	size_t partsupp_qty_probes = 0;
	size_t partsupp_qty_hits = 0;
	size_t partsupp_after_threshold = 0;
	size_t partsupp_flat_chunks = 0;
	size_t min_worker_partsupp_ns = 0;
	size_t max_worker_partsupp_ns = 0;
	size_t median_worker_partsupp_ns = 0;
};

bool BetterOutputRow(const Q20OutputRow &lhs, const Q20OutputRow &rhs) {
	return lhs.name < rhs.name;
}

bool AvailQtyGreaterThanHalfSum(int64_t availqty, int64_t sum_qty_raw) {
#if defined(__SIZEOF_INT128__)
	return static_cast<__int128_t>(availqty) * 200 > static_cast<__int128_t>(sum_qty_raw);
#else
	return availqty > sum_qty_raw / 200;
#endif
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

constexpr int64_t kDefaultPartCapacity = 200001;

class QualifyingPartBitmap {
public:
	QualifyingPartBitmap() {
		const auto words = static_cast<size_t>((kDefaultPartCapacity + 63) / 64);
		bits.assign(words, 0);
		bit_capacity = static_cast<int64_t>(words) * 64;
	}

	void Insert(int64_t partkey) {
		++count;
		if (partkey < 0 || partkey > 1'000'000'000) {
			overflow.insert(partkey);
			return;
		}
		if (partkey >= bit_capacity) {
			int64_t target = partkey + 1;
			if (bit_capacity > 0) {
				const int64_t doubled = bit_capacity > (std::numeric_limits<int64_t>::max() / 2)
				                            ? std::numeric_limits<int64_t>::max()
				                            : bit_capacity * 2;
				if (doubled > target) {
					target = doubled;
				}
			}
			const auto words = static_cast<size_t>((target + 63) / 64);
			bits.resize(words, 0);
			bit_capacity = static_cast<int64_t>(words) * 64;
		}
		bits[static_cast<size_t>(partkey) >> 6] |= (uint64_t {1} << (partkey & 63));
	}

	void MergeFrom(const QualifyingPartBitmap &other) {
		if (other.bit_capacity > bit_capacity) {
			const auto words = static_cast<size_t>((other.bit_capacity + 63) / 64);
			bits.resize(words, 0);
			bit_capacity = static_cast<int64_t>(words) * 64;
		}
		const auto word_count = std::min(bits.size(), other.bits.size());
		for (size_t i = 0; i < word_count; i++) {
			bits[i] |= other.bits[i];
		}
		overflow.insert(other.overflow.begin(), other.overflow.end());
		count += other.count;
	}

	bool Contains(int64_t partkey) const {
		if (partkey >= 0 && partkey < bit_capacity) {
			return (bits[static_cast<size_t>(partkey) >> 6] >> (partkey & 63)) & 1ULL;
		}
		return overflow.find(partkey) != overflow.end();
	}

	bool Empty() const {
		return count == 0;
	}

private:
	int64_t bit_capacity = 0;
	duckdb::idx_t count = 0;
	std::vector<uint64_t> bits;
	std::unordered_set<int64_t> overflow;
};

struct Q20FrozenState {
	PrefixLikeMatcher part_name_matcher;
	std::string fallback_part_name_pattern;
	QualifyingPartBitmap qualifying_parts;
	std::unordered_map<uint64_t, int64_t> quantity_by_part_supplier;

	explicit Q20FrozenState(const Q20Parameters &params)
	    : part_name_matcher(params.part_name_pattern), fallback_part_name_pattern(params.part_name_pattern) {
	}

	bool PartNameMatches(const duckdb::string_t &name) const {
		if (part_name_matcher.enabled) {
			return part_name_matcher.Matches(name);
		}
		return SqlLike(name, fallback_part_name_pattern);
	}
};

void ConsumePartChunk(duckdb::DataChunk &chunk, const Q20FrozenState &state, QualifyingPartBitmap &qualifying_parts) {
	auto &partkey_vector = chunk.data[Q20PartChunkLayout::PARTKEY_INDEX];
	auto &name_vector = chunk.data[Q20PartChunkLayout::NAME_INDEX];
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(name_vector)) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (!state.PartNameMatches(names[row])) {
				continue;
			}
			qualifying_parts.Insert(partkeys[row]);
		}
		return;
	}

	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat name_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	name_vector.ToUnifiedFormat(chunk.size(), name_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto name_index = name_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
			continue;
		}
		const auto name = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
		if (!state.PartNameMatches(name)) {
			continue;
		}
		qualifying_parts.Insert(partkeys[partkey_index]);
	}
}

void BuildPartState(duckdb::ClientContext &context, const duckdb::FunctionData &part_bind, Q20FrozenState &state,
                    duckdb::idx_t worker_count) {
	std::vector<CacheLinePadded<QualifyingPartBitmap>> part_workers(worker_count);
	RunParallelChunkScan(context, part_bind, worker_count, [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
		ConsumePartChunk(chunk, state, part_workers[worker_id].value);
	});
	for (const auto &worker : part_workers) {
		state.qualifying_parts.MergeFrom(worker.value);
	}
}

void BuildNationSupplierIndex(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                              NationSupplierIndex &index) {
	auto source = CreateNativeChunkSource(context, supplier_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat address_format;
		chunk.data[Q20SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(chunk.size(), suppkey_format);
		chunk.data[Q20SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		chunk.data[Q20SupplierChunkLayout::ADDRESS_INDEX].ToUnifiedFormat(chunk.size(), address_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto address_index = address_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index))) {
				continue;
			}
			SupplierRow supplier;
			supplier.name = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]);
			supplier.address = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index]);
			index.Insert(suppkeys[suppkey_index], std::move(supplier));
		}
	}
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const Q20FrozenState &state,
                             std::unordered_map<uint64_t, int64_t> &quantity_by_part_supplier, size_t &accepted_rows) {
	if (state.qualifying_parts.Empty()) {
		return;
	}
	auto &partkey_vector = chunk.data[Q20LineitemChunkLayout::PARTKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q20LineitemChunkLayout::SUPPKEY_INDEX];
	auto &quantity_vector = chunk.data[Q20LineitemChunkLayout::QUANTITY_INDEX];
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(quantity_vector)) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey = partkeys[row];
			if (!state.qualifying_parts.Contains(partkey)) {
				continue;
			}
			quantity_by_part_supplier[MakeAggKey(partkey, suppkeys[row])] += quantities[row];
			accepted_rows++;
		}
		return;
	}

	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	quantity_vector.ToUnifiedFormat(chunk.size(), quantity_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto quantity_index = quantity_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
			continue;
		}
		const auto partkey = partkeys[partkey_index];
		if (!state.qualifying_parts.Contains(partkey)) {
			continue;
		}
		const auto suppkey = suppkeys[suppkey_index];
		quantity_by_part_supplier[MakeAggKey(partkey, suppkey)] += quantities[quantity_index];
		accepted_rows++;
	}
}

void MergeQuantityMaps(std::unordered_map<uint64_t, int64_t> &global,
                       const std::unordered_map<uint64_t, int64_t> &local) {
	for (const auto &entry : local) {
		global[entry.first] += entry.second;
	}
}

void ProcessPartsuppChunkFlat(duckdb::DataChunk &chunk, const Q20FrozenState &state,
                              const NationSupplierIndex &nation_suppliers, std::vector<uint8_t> &qualifying_flags,
                              PartsuppWorkerStats &stats) {
	auto partkeys = duckdb::FlatVector::GetData<int64_t>(chunk.data[Q20PartsuppChunkLayout::PARTKEY_INDEX]);
	auto suppkeys = duckdb::FlatVector::GetData<int64_t>(chunk.data[Q20PartsuppChunkLayout::SUPPKEY_INDEX]);
	auto availqtys = duckdb::FlatVector::GetData<int64_t>(chunk.data[Q20PartsuppChunkLayout::AVAILQTY_INDEX]);
	stats.flat_chunks++;

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		stats.rows_scanned++;
		const auto partkey = partkeys[row];
		if (!state.qualifying_parts.Contains(partkey)) {
			continue;
		}
		stats.after_part_filter++;

		const auto suppkey = suppkeys[row];
		const auto compact = nation_suppliers.ToCompact(suppkey);
		if (compact < 0) {
			continue;
		}
		stats.after_nation_filter++;

		stats.qty_probes++;
		const auto agg_it = state.quantity_by_part_supplier.find(MakeAggKey(partkey, suppkey));
		if (agg_it == state.quantity_by_part_supplier.end()) {
			continue;
		}
		stats.qty_hits++;

		if (!AvailQtyGreaterThanHalfSum(availqtys[row], agg_it->second)) {
			continue;
		}
		stats.after_threshold++;
		qualifying_flags[static_cast<size_t>(compact)] = 1;
	}
}

void ProcessPartsuppChunkGeneric(duckdb::DataChunk &chunk, const Q20FrozenState &state,
                                 const NationSupplierIndex &nation_suppliers, std::vector<uint8_t> &qualifying_flags,
                                 PartsuppWorkerStats &stats) {
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat availqty_format;
	chunk.data[Q20PartsuppChunkLayout::PARTKEY_INDEX].ToUnifiedFormat(chunk.size(), partkey_format);
	chunk.data[Q20PartsuppChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(chunk.size(), suppkey_format);
	chunk.data[Q20PartsuppChunkLayout::AVAILQTY_INDEX].ToUnifiedFormat(chunk.size(), availqty_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto availqtys = duckdb::UnifiedVectorFormat::GetData<int64_t>(availqty_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto availqty_index = availqty_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!availqty_format.validity.AllValid() && !availqty_format.validity.RowIsValidUnsafe(availqty_index))) {
			continue;
		}
		stats.rows_scanned++;
		const auto partkey = partkeys[partkey_index];
		if (!state.qualifying_parts.Contains(partkey)) {
			continue;
		}
		stats.after_part_filter++;

		const auto suppkey = suppkeys[suppkey_index];
		const auto compact = nation_suppliers.ToCompact(suppkey);
		if (compact < 0) {
			continue;
		}
		stats.after_nation_filter++;

		stats.qty_probes++;
		const auto agg_it = state.quantity_by_part_supplier.find(MakeAggKey(partkey, suppkey));
		if (agg_it == state.quantity_by_part_supplier.end()) {
			continue;
		}
		stats.qty_hits++;

		if (!AvailQtyGreaterThanHalfSum(availqtys[availqty_index], agg_it->second)) {
			continue;
		}
		stats.after_threshold++;
		qualifying_flags[static_cast<size_t>(compact)] = 1;
	}
}

void ProcessPartsuppChunk(duckdb::DataChunk &chunk, const Q20FrozenState &state,
                          const NationSupplierIndex &nation_suppliers, std::vector<uint8_t> &qualifying_flags,
                          PartsuppWorkerStats &stats) {
	auto &partkey_vector = chunk.data[Q20PartsuppChunkLayout::PARTKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q20PartsuppChunkLayout::SUPPKEY_INDEX];
	auto &availqty_vector = chunk.data[Q20PartsuppChunkLayout::AVAILQTY_INDEX];
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(availqty_vector)) {
		ProcessPartsuppChunkFlat(chunk, state, nation_suppliers, qualifying_flags, stats);
		return;
	}
	ProcessPartsuppChunkGeneric(chunk, state, nation_suppliers, qualifying_flags, stats);
}

void BuildQualifyingSuppliersParallel(duckdb::ClientContext &context, const duckdb::FunctionData &partsupp_bind,
                                      const Q20FrozenState &state, const NationSupplierIndex &nation_suppliers,
                                      std::vector<uint8_t> &qualifying_flags, Q20ParallelProfile &profile) {
	using clock = std::chrono::steady_clock;
	const auto worker_count = ParallelWorkerCount(context);
	const auto compact_count = static_cast<size_t>(nation_suppliers.compact_count());
	if (compact_count == 0) {
		return;
	}

	struct WorkerState {
		std::vector<uint8_t> qualifying_flags;
		PartsuppWorkerStats stats;
	};

	std::vector<CacheLinePadded<WorkerState>> workers(static_cast<size_t>(worker_count));
	for (auto &worker : workers) {
		worker.value.qualifying_flags.assign(compact_count, 0);
	}

	const auto scan_t0 = clock::now();
	RunParallelChunkScan(context, partsupp_bind, worker_count, [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
		const auto chunk_t0 = clock::now();
		auto &worker = workers[static_cast<size_t>(worker_id)].value;
		ProcessPartsuppChunk(chunk, state, nation_suppliers, worker.qualifying_flags, worker.stats);
		worker.stats.process_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0).count();
	});
	profile.partsupp_scan_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - scan_t0).count();

	const auto merge_t0 = clock::now();
	qualifying_flags.assign(compact_count, 0);
	std::vector<int64_t> worker_ns(worker_count, 0);
	for (size_t worker_id = 0; worker_id < static_cast<size_t>(worker_count); worker_id++) {
		const auto &worker = workers[worker_id].value;
		worker_ns[worker_id] = worker.stats.process_ns;
		profile.partsupp_rows_scanned += worker.stats.rows_scanned;
		profile.partsupp_after_part_filter += worker.stats.after_part_filter;
		profile.partsupp_after_nation_filter += worker.stats.after_nation_filter;
		profile.partsupp_qty_probes += worker.stats.qty_probes;
		profile.partsupp_qty_hits += worker.stats.qty_hits;
		profile.partsupp_after_threshold += worker.stats.after_threshold;
		profile.partsupp_flat_chunks += worker.stats.flat_chunks;
		for (size_t compact = 0; compact < compact_count; compact++) {
			qualifying_flags[compact] |= worker.qualifying_flags[compact];
		}
	}
	profile.partsupp_merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();

	if (!worker_ns.empty()) {
		profile.min_worker_partsupp_ns = static_cast<size_t>(*std::min_element(worker_ns.begin(), worker_ns.end()));
		profile.max_worker_partsupp_ns = static_cast<size_t>(*std::max_element(worker_ns.begin(), worker_ns.end()));
		auto sorted_ns = worker_ns;
		std::sort(sorted_ns.begin(), sorted_ns.end());
		profile.median_worker_partsupp_ns = static_cast<size_t>(sorted_ns[sorted_ns.size() / 2]);
	}
}

void PrintQ20ParallelProfile(const Q20ParallelProfile &profile) {
	const auto qty_bytes = profile.distinct_qty_keys * (sizeof(uint64_t) + sizeof(int64_t) + sizeof(void *));
	std::cerr << "q20_parallel_profile"
	          << " part_ns=" << profile.part_ns << " nation_supplier_ns=" << profile.nation_supplier_ns
	          << " lineitem_ns=" << profile.lineitem_ns << " merge_ns=" << profile.merge_ns
	          << " partsupp_scan_ns=" << profile.partsupp_scan_ns << " partsupp_merge_ns=" << profile.partsupp_merge_ns
	          << " output_ns=" << profile.output_ns << " worker_count=" << profile.worker_count
	          << " accepted_lineitems=" << profile.accepted_lineitems
	          << " distinct_qty_keys=" << profile.distinct_qty_keys
	          << " worker_local_map_entries=" << profile.worker_local_map_entries
	          << " quantity_map_buckets=" << profile.quantity_map_buckets
	          << " quantity_map_est_bytes=" << qty_bytes
	          << " nation_supplier_count=" << profile.nation_supplier_count
	          << " qualifying_supplier_count=" << profile.qualifying_supplier_count
	          << " partsupp_rows_scanned=" << profile.partsupp_rows_scanned
	          << " partsupp_after_part_filter=" << profile.partsupp_after_part_filter
	          << " partsupp_after_nation_filter=" << profile.partsupp_after_nation_filter
	          << " partsupp_qty_probes=" << profile.partsupp_qty_probes
	          << " partsupp_qty_hits=" << profile.partsupp_qty_hits
	          << " partsupp_after_threshold=" << profile.partsupp_after_threshold
	          << " partsupp_flat_chunks=" << profile.partsupp_flat_chunks
	          << " min_worker_partsupp_ns=" << profile.min_worker_partsupp_ns
	          << " max_worker_partsupp_ns=" << profile.max_worker_partsupp_ns
	          << " median_worker_partsupp_ns=" << profile.median_worker_partsupp_ns << "\n";
}

class Q20ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q20ParallelKernel(Q20Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 4 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2] ||
		    !bound_inputs[3]) {
			throw duckdb::InternalException("Q20 parallel expects bound part, lineitem, partsupp, supplier scans");
		}

		using clock = std::chrono::steady_clock;
		Q20ParallelProfile profile;
		Q20FrozenState frozen(params);
		NationSupplierIndex nation_suppliers;

		const auto worker_count = ParallelWorkerCount(context);

		const auto part_t0 = clock::now();
		BuildPartState(context, *bound_inputs[0], frozen, worker_count);
		profile.part_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - part_t0).count();

		const auto nation_t0 = clock::now();
		BuildNationSupplierIndex(context, *bound_inputs[3], nation_suppliers);
		profile.nation_supplier_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - nation_t0).count();
		profile.nation_supplier_count = static_cast<size_t>(nation_suppliers.compact_count());
		profile.worker_count = static_cast<size_t>(worker_count);
		std::vector<CacheLinePadded<std::unordered_map<uint64_t, int64_t>>> workers(worker_count);

		const auto lineitem_t0 = clock::now();
		std::atomic<size_t> accepted_lineitems {0};
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     size_t local_accepted = 0;
			                     AccumulateLineitemChunk(chunk, frozen, workers[worker_id].value, local_accepted);
			                     accepted_lineitems.fetch_add(local_accepted, std::memory_order_relaxed);
		                     });
		profile.lineitem_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();
		profile.accepted_lineitems = accepted_lineitems.load();

		const auto merge_t0 = clock::now();
		for (const auto &worker : workers) {
			profile.worker_local_map_entries += worker.value.size();
			MergeQuantityMaps(frozen.quantity_by_part_supplier, worker.value);
		}
		profile.merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		profile.distinct_qty_keys = frozen.quantity_by_part_supplier.size();
		profile.quantity_map_buckets = frozen.quantity_by_part_supplier.bucket_count();

		std::vector<uint8_t> qualifying_flags;
		BuildQualifyingSuppliersParallel(context, *bound_inputs[2], frozen, nation_suppliers, qualifying_flags,
		                                 profile);

		const auto output_t0 = clock::now();
		output_rows.clear();
		for (int32_t compact = 0; compact < nation_suppliers.compact_count(); compact++) {
			if (!qualifying_flags[static_cast<size_t>(compact)]) {
				continue;
			}
			const auto &supplier = nation_suppliers.supplier_rows[static_cast<size_t>(compact)];
			Q20OutputRow row;
			row.name = supplier.name;
			row.address = supplier.address;
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		profile.output_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_t0).count();
		profile.qualifying_supplier_count = output_rows.size();

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			PrintQ20ParallelProfile(profile);
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
			output.SetValue(0, row, duckdb::Value(entry.name));
			output.SetValue(1, row, duckdb::Value(entry.address));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q20Parameters params;
	std::vector<Q20OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ20ParallelKernel(const Q20Parameters &params) {
	return duckdb::make_uniq<Q20ParallelKernel>(params);
}

} // namespace accelerator_codegen
