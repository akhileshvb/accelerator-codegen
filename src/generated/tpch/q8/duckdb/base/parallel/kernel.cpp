#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q8/duckdb/base/query.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int32_t kAbsentNation = -1;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int32_t kMaxNationSlots = 25;
constexpr int64_t kMinYear = 1995;
constexpr int64_t kMaxYear = 1996;
constexpr int kYearSlotCount = static_cast<int>(kMaxYear - kMinYear + 1);
constexpr duckdb::idx_t kLineitemPartkeyScanIndex = 0;
constexpr duckdb::idx_t kLineitemOrderkeyScanIndex = 1;
constexpr duckdb::idx_t kLineitemSuppkeyScanIndex = 2;
constexpr duckdb::idx_t kLineitemExtendedpriceScanIndex = 3;
constexpr duckdb::idx_t kLineitemDiscountScanIndex = 4;
constexpr duckdb::idx_t kLineitemProjectedColumnCount = 5;
constexpr duckdb::idx_t kCustomerCustkeyScanIndex = 0;
constexpr duckdb::idx_t kCustomerNationkeyScanIndex = 1;
constexpr duckdb::idx_t kCustomerProjectedColumnCount = 2;
constexpr duckdb::idx_t kOrdersOrderkeyScanIndex = 0;
constexpr duckdb::idx_t kOrdersCustkeyScanIndex = 1;
constexpr duckdb::idx_t kOrdersOrderdateScanIndex = 2;
constexpr duckdb::idx_t kOrdersProjectedColumnCount = 3;

#if defined(__SIZEOF_INT128__)
using q8_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q8_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddVolume(q8_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q8_acc_t>(extendedprice_raw) * static_cast<q8_acc_t>(100 - discount_raw);
}
#else
using q8_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q8_acc_t &value) {
	return value;
}

void AddVolume(q8_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q8 volume");
	}
}
#endif

struct VolumeState {
	q8_acc_t nation_volume = 0;
	q8_acc_t total_volume = 0;
};

struct Q8OutputRow {
	int64_t year = 0;
	double mkt_share = 0.0;
};

bool BetterOutputRow(const Q8OutputRow &lhs, const Q8OutputRow &rhs) {
	return lhs.year < rhs.year;
}

double ComputeMarketShare(const VolumeState &state) {
	const auto nation_decimal = duckdb::Value::DECIMAL(FromAccumulator(state.nation_volume), 38, 4);
	const auto total_decimal = duckdb::Value::DECIMAL(FromAccumulator(state.total_volume), 38, 4);
	const auto nation_double = nation_decimal.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
	const auto total_double = total_decimal.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
	if (total_double == 0.0) {
		return 0.0;
	}
	return nation_double / total_double;
}

bool Q8ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
}

bool TryGetRowGroupNumericMinMax(const duckdb::RowGroup &row_group, const duckdb::StorageIndex &column_id,
                                 int64_t &out_min, int64_t &out_max) {
	const auto stats = row_group.GetStatistics(column_id);
	if (!stats || stats->GetStatsType() != duckdb::StatisticsType::NUMERIC_STATS) {
		return false;
	}
	out_min = duckdb::NumericStats::GetMin<int64_t>(*stats);
	out_max = duckdb::NumericStats::GetMax<int64_t>(*stats);
	return true;
}

bool SortedKeysOverlapRange(const std::vector<int64_t> &keys, int64_t range_min, int64_t range_max) {
	if (keys.empty()) {
		return false;
	}
	const auto it = std::lower_bound(keys.begin(), keys.end(), range_min);
	return it != keys.end() && *it <= range_max;
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

bool PartTypeMatchesSuffix(const duckdb::string_t &part_type, const std::string &suffix) {
	const auto type_size = part_type.GetSize();
	const auto suffix_size = suffix.size();
	if (type_size < suffix_size) {
		return false;
	}
	return std::memcmp(part_type.GetData() + type_size - suffix_size, suffix.data(), suffix_size) == 0;
}

struct Q8ParallelProfile {
	int64_t total_ns = 0;
	int64_t part_ns = 0;
	int64_t supplier_ns = 0;
	int64_t customer_ns = 0;
	int64_t orders_ns = 0;
	int64_t build_ns = 0;
	int64_t lineitem_ns = 0;
	int64_t merge_ns = 0;
	int64_t output_ns = 0;
	size_t worker_count = 0;
	size_t nations_in_region = 0;
	size_t parts_scanned = 0;
	size_t parts_matching_type = 0;
	size_t qualifying_parts = 0;
	size_t suppliers_scanned = 0;
	size_t suppliers_stored = 0;
	size_t customers_scanned = 0;
	size_t customers_in_region = 0;
	size_t orders_scanned = 0;
	size_t orders_customer_survivors = 0;
	size_t orders_date_survivors = 0;
	size_t lineitem_rows = 0;
	size_t lineitem_part_hits = 0;
	size_t lineitem_order_hits = 0;
	size_t lineitem_supplier_hits = 0;
	size_t lineitem_target_nation = 0;
	size_t lineitem_revenue_contribs = 0;
	size_t lineitem_flat_rows = 0;
	size_t lineitem_dict_rows = 0;
	size_t lineitem_fallback_rows = 0;
	size_t part_flag_bytes = 0;
	size_t supplier_dense_bytes = 0;
	size_t order_slot_bytes = 0;
	int64_t max_lineitem_worker_ns = 0;
	int64_t avg_lineitem_worker_ns = 0;
};

void PrintQ8ParallelProfile(const Q8ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	std::cerr << "q8_parallel_profile total_ns=" << profile.total_ns << " build_ns=" << profile.build_ns << " ("
	          << pct(profile.build_ns) << "%)"
	          << " part_ns=" << profile.part_ns << " (" << pct(profile.part_ns) << "%)"
	          << " supplier_ns=" << profile.supplier_ns << " (" << pct(profile.supplier_ns) << "%)"
	          << " customer_ns=" << profile.customer_ns << " (" << pct(profile.customer_ns) << "%)"
	          << " orders_ns=" << profile.orders_ns << " (" << pct(profile.orders_ns) << "%)"
	          << " lineitem_ns=" << profile.lineitem_ns << " (" << pct(profile.lineitem_ns) << "%)"
	          << " merge_ns=" << profile.merge_ns << " (" << pct(profile.merge_ns) << "%)"
	          << " output_ns=" << profile.output_ns << " (" << pct(profile.output_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " nations_in_region=" << profile.nations_in_region
	          << " parts_scanned=" << profile.parts_scanned << " parts_matching_type=" << profile.parts_matching_type
	          << " qualifying_parts=" << profile.qualifying_parts << " suppliers_scanned=" << profile.suppliers_scanned
	          << " suppliers_stored=" << profile.suppliers_stored << " customers_scanned=" << profile.customers_scanned
	          << " customers_in_region=" << profile.customers_in_region << " orders_scanned=" << profile.orders_scanned
	          << " orders_customer_survivors=" << profile.orders_customer_survivors
	          << " orders_date_survivors=" << profile.orders_date_survivors << " lineitem_rows=" << profile.lineitem_rows
	          << " lineitem_part_hits=" << profile.lineitem_part_hits
	          << " lineitem_order_hits=" << profile.lineitem_order_hits
	          << " lineitem_supplier_hits=" << profile.lineitem_supplier_hits
	          << " lineitem_target_nation=" << profile.lineitem_target_nation
	          << " lineitem_revenue_contribs=" << profile.lineitem_revenue_contribs
	          << " lineitem_flat_rows=" << profile.lineitem_flat_rows
	          << " lineitem_dict_rows=" << profile.lineitem_dict_rows
	          << " lineitem_fallback_rows=" << profile.lineitem_fallback_rows
	          << " part_flag_bytes=" << profile.part_flag_bytes << " supplier_dense_bytes=" << profile.supplier_dense_bytes
	          << " order_slot_bytes=" << profile.order_slot_bytes
	          << " max_lineitem_worker_ns=" << profile.max_lineitem_worker_ns
	          << " avg_lineitem_worker_ns=" << profile.avg_lineitem_worker_ns << "\n";
}

class KeyBitmap {
public:
	bool Contains(int64_t key) const {
		if (key < 0) {
			return false;
		}
		const auto bit = static_cast<uint64_t>(key);
		const auto word_index = bit >> 6;
		if (word_index >= words.size()) {
			return false;
		}
		return (words[word_index] & (uint64_t {1} << (bit & 63))) != 0;
	}

	void Insert(int64_t key) {
		if (key < 0) {
			return;
		}
		const auto bit = static_cast<uint64_t>(key);
		const auto word_index = bit >> 6;
		if (word_index >= words.size()) {
			EnsureCapacity(bit);
		}
		words[word_index] |= (uint64_t {1} << (bit & 63));
	}

private:
	void EnsureCapacity(uint64_t max_bit_inclusive) {
		const auto needed_words = (max_bit_inclusive >> 6) + 1;
		if (needed_words <= words.size()) {
			return;
		}
		auto new_size = words.empty() ? needed_words : words.size();
		while (new_size < needed_words) {
			new_size *= 2;
		}
		words.resize(new_size, 0);
	}

	std::vector<uint64_t> words;
};

class QualifyingPartSet {
public:
	bool Contains(int64_t partkey) const {
		if (partkey < 0) {
			return false;
		}
		const auto index = static_cast<size_t>(partkey);
		return index < flags.size() && flags[index] != 0;
	}

	void Insert(int64_t partkey) {
		if (partkey < 0) {
			return;
		}
		const auto index = static_cast<size_t>(partkey);
		if (index >= flags.size()) {
			EnsureCapacity(partkey);
		}
		flags[index] = 1;
	}

	size_t QualifyingCount() const {
		size_t count = 0;
		for (const auto flag : flags) {
			count += flag != 0;
		}
		return count;
	}

	size_t FlagBytes() const {
		return flags.size();
	}

	void CollectKeys(std::vector<int64_t> &keys) const {
		keys.clear();
		for (size_t index = 0; index < flags.size(); index++) {
			if (flags[index] != 0) {
				keys.push_back(static_cast<int64_t>(index));
			}
		}
	}

	void ReserveAtLeast(int64_t max_partkey) {
		if (max_partkey >= 0) {
			EnsureCapacity(max_partkey);
		}
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
};

class SupplierNationStore {
public:
	explicit SupplierNationStore(int64_t default_capacity) : default_capacity(default_capacity) {
		EnsureCapacity(default_capacity);
	}

	void WarmInitialize() {
		if (initialized) {
			return;
		}
		values.assign(static_cast<size_t>(default_capacity), kAbsentNation);
		capacity = default_capacity;
		initialized = true;
	}

	void Store(int64_t suppkey, int32_t nationkey) {
		if (!initialized) {
			WarmInitialize();
		}
		if (suppkey < 0) {
			overflow[suppkey] = nationkey;
			return;
		}
		if (suppkey >= capacity) {
			if (suppkey > 1'000'000'000) {
				overflow[suppkey] = nationkey;
				return;
			}
			EnsureCapacity(suppkey + 1);
		}
		values[static_cast<size_t>(suppkey)] = nationkey;
	}

	int32_t Load(int64_t suppkey) const {
		if (suppkey < 0) {
			const auto it = overflow.find(suppkey);
			return it == overflow.end() ? kAbsentNation : it->second;
		}
		if (suppkey >= 0 && suppkey < capacity) {
			return values[static_cast<size_t>(suppkey)];
		}
		const auto it = overflow.find(suppkey);
		return it == overflow.end() ? kAbsentNation : it->second;
	}

	int32_t LoadNonNegUnchecked(uint64_t suppkey) const {
		return values[suppkey];
	}

	size_t DenseBytes() const {
		return values.size() * sizeof(int32_t);
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
		values.resize(static_cast<size_t>(target), kAbsentNation);
		capacity = target;
	}

	int64_t default_capacity;
	bool initialized = false;
	int64_t capacity = 0;
	std::vector<int32_t> values;
	std::unordered_map<int64_t, int32_t> overflow;
};

class OrderYearSlotStore {
public:
	explicit OrderYearSlotStore(int64_t default_capacity = kDefaultOrderCapacity) {
		EnsureCapacity(default_capacity);
	}

	void StoreSlot(int64_t orderkey, int8_t slot) {
		if (orderkey < 0) {
			overflow[orderkey] = slot;
			return;
		}
		if (orderkey >= capacity) {
			if (orderkey > 1'000'000'000) {
				overflow[orderkey] = slot;
				return;
			}
			EnsureCapacity(orderkey + 1);
		}
		slots[static_cast<size_t>(orderkey)] = slot;
	}

	void ReserveAtLeast(int64_t new_capacity) {
		EnsureCapacity(new_capacity);
	}

	int8_t LoadSlot(int64_t orderkey) const {
		if (orderkey < 0) {
			const auto it = overflow.find(orderkey);
			return it == overflow.end() ? static_cast<int8_t>(-1) : it->second;
		}
		if (orderkey >= 0 && orderkey < capacity) {
			return slots[static_cast<size_t>(orderkey)];
		}
		const auto it = overflow.find(orderkey);
		return it == overflow.end() ? static_cast<int8_t>(-1) : it->second;
	}

	int8_t LoadSlotUnchecked(uint64_t orderkey) const {
		return slots[orderkey];
	}

	size_t SlotBytes() const {
		return slots.size();
	}

	void CollectQualifyingOrderKeys(std::vector<int64_t> &keys) const {
		keys.clear();
		for (int64_t orderkey = 0; orderkey < capacity; orderkey++) {
			if (slots[static_cast<size_t>(orderkey)] >= 0) {
				keys.push_back(orderkey);
			}
		}
		for (const auto &entry : overflow) {
			if (entry.second >= 0) {
				keys.push_back(entry.first);
			}
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
		slots.resize(static_cast<size_t>(target), static_cast<int8_t>(-1));
		capacity = target;
	}

	int64_t capacity = 0;
	std::vector<int8_t> slots;
	std::unordered_map<int64_t, int8_t> overflow;
};

struct Q8FrozenState {
	const Q8Parameters &params;
	QualifyingPartSet qualifying_parts;
	SupplierNationStore supplier_nations;
	KeyBitmap qualifying_customers;
	OrderYearSlotStore order_year_slots;
	bool order_year_slots_probe_ready = false;
	std::vector<int64_t> qualifying_part_keys;
	std::vector<int64_t> qualifying_order_keys;

	explicit Q8FrozenState(const Q8Parameters &params_p)
	    : params(params_p), supplier_nations(kDefaultSupplierCapacity), order_year_slots(kDefaultOrderCapacity) {
		supplier_nations.WarmInitialize();
		for (const auto partkey : params.bind_part_keys) {
			qualifying_parts.Insert(partkey);
			qualifying_part_keys.push_back(partkey);
		}
	}

	bool HasRegionNations() const {
		for (const auto flag : params.region_nations) {
			if (flag) {
				return true;
			}
		}
		return false;
	}

	void PrepareOrderProbeState() {
		order_year_slots_probe_ready = order_year_slots.SlotBytes() >= static_cast<size_t>(kDefaultOrderCapacity);
	}

	void FinalizeSemiJoinKeys() {
		std::sort(qualifying_part_keys.begin(), qualifying_part_keys.end());
		std::sort(qualifying_order_keys.begin(), qualifying_order_keys.end());
	}
};

struct Q8WorkerLineitemProfile {
	int64_t elapsed_ns = 0;
	size_t rows = 0;
};

int32_t LoadSupplierNation(const Q8FrozenState &state, int64_t suppkey) {
	return suppkey >= 0 && suppkey < kDefaultSupplierCapacity
	           ? state.supplier_nations.LoadNonNegUnchecked(static_cast<uint64_t>(suppkey))
	           : state.supplier_nations.Load(suppkey);
}

int8_t LoadOrderYearSlot(const Q8FrozenState &state, int64_t orderkey) {
	return state.order_year_slots_probe_ready
	           ? state.order_year_slots.LoadSlotUnchecked(static_cast<uint64_t>(orderkey))
	           : state.order_year_slots.LoadSlot(orderkey);
}

int ProbeLineitemSurvivor(const Q8FrozenState &state, int64_t partkey, int64_t orderkey, int64_t suppkey,
                          int32_t &out_supp_nation, Q8ParallelProfile *profile) {
	if (!state.qualifying_parts.Contains(partkey)) {
		return -1;
	}
	if (profile) {
		profile->lineitem_part_hits++;
	}
	const int8_t slot = LoadOrderYearSlot(state, orderkey);
	if (slot < 0 || slot >= kYearSlotCount) {
		return -1;
	}
	if (profile) {
		profile->lineitem_order_hits++;
	}
	const int32_t supp_nation = LoadSupplierNation(state, suppkey);
	if (supp_nation == kAbsentNation) {
		return -1;
	}
	if (profile) {
		profile->lineitem_supplier_hits++;
		if (state.params.target_nationkey >= 0 && supp_nation == state.params.target_nationkey) {
			profile->lineitem_target_nation++;
		}
	}
	out_supp_nation = supp_nation;
	return static_cast<int>(slot);
}

void AccumulateLineitemVolume(const Q8Parameters &params, int slot, int32_t supp_nation, int64_t extendedprice_raw,
                              int64_t discount_raw, std::array<VolumeState, kYearSlotCount> &volume_by_year) {
	auto &volume_state = volume_by_year[static_cast<size_t>(slot)];
	AddVolume(volume_state.total_volume, extendedprice_raw, discount_raw);
	if (params.target_nationkey >= 0 && supp_nation == params.target_nationkey) {
		AddVolume(volume_state.nation_volume, extendedprice_raw, discount_raw);
	}
}

void AppendWorkerSemiJoinKeys(std::vector<int64_t> &dst, std::vector<std::vector<int64_t>> &worker_keys) {
	size_t extra = 0;
	for (const auto &keys : worker_keys) {
		extra += keys.size();
	}
	dst.reserve(dst.size() + extra);
	for (auto &keys : worker_keys) {
		dst.insert(dst.end(), std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end()));
		keys.clear();
	}
}

void ConsumePartChunk(duckdb::DataChunk &chunk, Q8FrozenState &state, Q8ParallelProfile *profile,
                      std::vector<int64_t> *out_part_keys = nullptr) {
	auto &partkey_vector = chunk.data[Q8PartChunkLayout::PARTKEY_INDEX];
	auto &type_vector = chunk.data[Q8PartChunkLayout::TYPE_INDEX];
	const auto &suffix = state.params.part_type;
	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(type_vector)) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->parts_scanned++;
			}
			if (!PartTypeMatchesSuffix(types[row], suffix)) {
				continue;
			}
			if (profile) {
				profile->parts_matching_type++;
			}
			state.qualifying_parts.Insert(partkeys[row]);
			if (out_part_keys != nullptr) {
				out_part_keys->push_back(partkeys[row]);
			}
		}
		return;
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat type_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	type_vector.ToUnifiedFormat(chunk.size(), type_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto types = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto type_index = type_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!type_format.validity.AllValid() && !type_format.validity.RowIsValidUnsafe(type_index))) {
			continue;
		}
		if (profile) {
			profile->parts_scanned++;
		}
		if (!PartTypeMatchesSuffix(types[type_index], suffix)) {
			continue;
		}
		if (profile) {
			profile->parts_matching_type++;
		}
		state.qualifying_parts.Insert(partkeys[partkey_index]);
		if (out_part_keys != nullptr) {
			out_part_keys->push_back(partkeys[partkey_index]);
		}
	}
}

void ConsumeSupplierChunk(duckdb::DataChunk &chunk, Q8FrozenState &state, Q8ParallelProfile *profile) {
	auto &suppkey_vector = chunk.data[Q8SupplierChunkLayout::SUPPKEY_INDEX];
	auto &nationkey_vector = chunk.data[Q8SupplierChunkLayout::NATIONKEY_INDEX];
	if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector)) {
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->suppliers_scanned++;
			}
			state.supplier_nations.Store(suppkeys[row], nationkeys[row]);
			if (profile) {
				profile->suppliers_stored++;
			}
		}
		return;
	}
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	nationkey_vector.ToUnifiedFormat(chunk.size(), nationkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
			continue;
		}
		if (profile) {
			profile->suppliers_scanned++;
		}
		state.supplier_nations.Store(suppkeys[suppkey_index], nationkeys[nationkey_index]);
		if (profile) {
			profile->suppliers_stored++;
		}
	}
}

void ConsumeCustomerChunk(duckdb::DataChunk &chunk, Q8FrozenState &state, Q8ParallelProfile *profile) {
	if (!state.HasRegionNations()) {
		return;
	}
	auto &custkey_vector = chunk.data[Q8CustomerChunkLayout::CUSTKEY_INDEX];
	auto &nationkey_vector = chunk.data[Q8CustomerChunkLayout::NATIONKEY_INDEX];
	if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(nationkey_vector)) {
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->customers_scanned++;
			}
			const auto nationkey = nationkeys[row];
			if (nationkey < 0 || nationkey >= kMaxNationSlots ||
			    !state.params.region_nations[static_cast<size_t>(nationkey)]) {
				continue;
			}
			if (profile) {
				profile->customers_in_region++;
			}
			state.qualifying_customers.Insert(custkeys[row]);
		}
		return;
	}
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	nationkey_vector.ToUnifiedFormat(chunk.size(), nationkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto nationkey_index = nationkey_format.sel->get_index(row);
		if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
			continue;
		}
		if (profile) {
			profile->customers_scanned++;
		}
		const auto nationkey = nationkeys[nationkey_index];
		if (nationkey < 0 || nationkey >= kMaxNationSlots ||
		    !state.params.region_nations[static_cast<size_t>(nationkey)]) {
			continue;
		}
		if (profile) {
			profile->customers_in_region++;
		}
		state.qualifying_customers.Insert(custkeys[custkey_index]);
	}
}

void ConsumeOrdersChunk(duckdb::DataChunk &chunk, Q8FrozenState &state, Q8ParallelProfile *profile,
                        std::vector<int64_t> *out_order_keys = nullptr) {
	if (!state.HasRegionNations()) {
		return;
	}
	auto &orderkey_vector = chunk.data[Q8OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q8OrdersChunkLayout::CUSTKEY_INDEX];
	auto &orderdate_vector = chunk.data[Q8OrdersChunkLayout::ORDERDATE_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) && IsFlatAllValid(orderdate_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(orderdate_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->orders_scanned++;
			}
			if (!state.qualifying_customers.Contains(custkeys[row])) {
				continue;
			}
			if (profile) {
				profile->orders_customer_survivors++;
			}
			const auto year = static_cast<int16_t>(duckdb::Date::ExtractYear(orderdates[row]));
			if (year < kMinYear || year > kMaxYear) {
				continue;
			}
			if (profile) {
				profile->orders_date_survivors++;
			}
			const int slot = static_cast<int>(year - kMinYear);
			state.order_year_slots.StoreSlot(orderkeys[row], static_cast<int8_t>(slot));
			if (out_order_keys != nullptr) {
				out_order_keys->push_back(orderkeys[row]);
			}
		}
		return;
	}
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat orderdate_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	orderdate_vector.ToUnifiedFormat(chunk.size(), orderdate_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto orderdate_index = orderdate_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index))) {
			continue;
		}
		if (profile) {
			profile->orders_scanned++;
		}
		if (!state.qualifying_customers.Contains(custkeys[custkey_index])) {
			continue;
		}
		if (profile) {
			profile->orders_customer_survivors++;
		}
		const auto year = static_cast<int16_t>(duckdb::Date::ExtractYear(orderdates[orderdate_index]));
		if (year < kMinYear || year > kMaxYear) {
			continue;
		}
		if (profile) {
			profile->orders_date_survivors++;
		}
		const int slot = static_cast<int>(year - kMinYear);
		state.order_year_slots.StoreSlot(orderkeys[orderkey_index], static_cast<int8_t>(slot));
		if (out_order_keys != nullptr) {
			out_order_keys->push_back(orderkeys[orderkey_index]);
		}
	}
}

void ConsumeFusedCustomer(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                          duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                          Q8FrozenState &state, Q8ParallelProfile *profile) {
	const auto &column_ids = scan_state.GetColumnIds();
	if (column_ids.size() != kCustomerProjectedColumnCount) {
		throw duckdb::InternalException("Q8 parallel fused customer expected %llu projected columns, got %llu",
		                                static_cast<unsigned long long>(kCustomerProjectedColumnCount),
		                                static_cast<unsigned long long>(column_ids.size()));
	}

	static constexpr duckdb::idx_t kScanIndices[] = {kCustomerCustkeyScanIndex, kCustomerNationkeyScanIndex};
	duckdb::Vector custkey_vec(duckdb::LogicalType::BIGINT);
	duckdb::Vector nationkey_vec(duckdb::LogicalType::INTEGER);
	duckdb::Vector *payloads[] = {&custkey_vec, &nationkey_vec};
	const auto scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kScanIndices, 2, payloads,
	                                                       duckdb::UpdateScanType::STANDARD);

	const bool identity = sel.data() == nullptr;
	if (IsFlatAllValid(custkey_vec) && IsFlatAllValid(nationkey_vec) && scan_count >= count) {
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vec);
		auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vec);
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (profile) {
				profile->customers_scanned++;
			}
			const auto vector_index = identity ? row : sel.get_index(row);
			const auto nationkey = nationkeys[vector_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots ||
			    !state.params.region_nations[static_cast<size_t>(nationkey)]) {
				continue;
			}
			if (profile) {
				profile->customers_in_region++;
			}
			state.qualifying_customers.Insert(custkeys[vector_index]);
		}
		return;
	}

	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat nationkey_format;
	custkey_vec.ToUnifiedFormat(scan_count, custkey_format);
	nationkey_vec.ToUnifiedFormat(scan_count, nationkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base_index = identity ? row : sel.get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(base_index);
		const auto nationkey_index = nationkey_format.sel->get_index(base_index);
		if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
			continue;
		}
		if (profile) {
			profile->customers_scanned++;
		}
		const auto nationkey = nationkeys[nationkey_index];
		if (nationkey < 0 || nationkey >= kMaxNationSlots ||
		    !state.params.region_nations[static_cast<size_t>(nationkey)]) {
			continue;
		}
		if (profile) {
			profile->customers_in_region++;
		}
		state.qualifying_customers.Insert(custkeys[custkey_index]);
	}
}

void ConsumeFusedOrders(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                        duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                        Q8FrozenState &state, Q8ParallelProfile *profile) {
	const auto &column_ids = scan_state.GetColumnIds();
	if (column_ids.size() != kOrdersProjectedColumnCount) {
		throw duckdb::InternalException("Q8 parallel fused orders expected %llu projected columns, got %llu",
		                                static_cast<unsigned long long>(kOrdersProjectedColumnCount),
		                                static_cast<unsigned long long>(column_ids.size()));
	}

	static constexpr duckdb::idx_t kKeyScanIndices[] = {kOrdersOrderkeyScanIndex, kOrdersCustkeyScanIndex};
	duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
	duckdb::Vector custkey_vec(duckdb::LogicalType::BIGINT);
	duckdb::Vector *key_payload[] = {&orderkey_vec, &custkey_vec};
	const auto scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kKeyScanIndices, 2, key_payload,
	                                                       duckdb::UpdateScanType::STANDARD);

	struct OrderSurvivor {
		duckdb::idx_t vector_index = 0;
		int64_t orderkey = 0;
	};
	OrderSurvivor survivors[STANDARD_VECTOR_SIZE];
	duckdb::idx_t match_count = 0;
	const bool identity = sel.data() == nullptr;
	if (IsFlatAllValid(orderkey_vec) && IsFlatAllValid(custkey_vec) && scan_count >= count) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vec);
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (profile) {
				profile->orders_scanned++;
			}
			const auto vector_index = identity ? row : sel.get_index(row);
			if (!state.qualifying_customers.Contains(custkeys[vector_index])) {
				continue;
			}
			if (profile) {
				profile->orders_customer_survivors++;
			}
			survivors[match_count++] = OrderSurvivor {vector_index, orderkeys[vector_index]};
		}
	} else {
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat custkey_format;
		orderkey_vec.ToUnifiedFormat(scan_count, orderkey_format);
		custkey_vec.ToUnifiedFormat(scan_count, custkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto base_index = identity ? row : sel.get_index(row);
			const auto orderkey_index = orderkey_format.sel->get_index(base_index);
			const auto custkey_index = custkey_format.sel->get_index(base_index);
			if ((!orderkey_format.validity.AllValid() &&
			     !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!custkey_format.validity.AllValid() &&
			     !custkey_format.validity.RowIsValidUnsafe(custkey_index))) {
				continue;
			}
			if (profile) {
				profile->orders_scanned++;
			}
			if (!state.qualifying_customers.Contains(custkeys[custkey_index])) {
				continue;
			}
			if (profile) {
				profile->orders_customer_survivors++;
			}
			survivors[match_count++] = OrderSurvivor {base_index, orderkeys[orderkey_index]};
		}
	}

	if (match_count == 0) {
		row_group.SkipProjectedColumn(scan_state, kOrdersOrderdateScanIndex);
		return;
	}

	duckdb::SelectionVector survivor_sel(match_count);
	for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
		survivor_sel.set_index(match_index, survivors[match_index].vector_index);
	}

	duckdb::Vector orderdate_vec(duckdb::LogicalType::DATE);
	row_group.SelectProjectedColumn(transaction, scan_state, kOrdersOrderdateScanIndex, survivor_sel, match_count,
	                              orderdate_vec, duckdb::UpdateScanType::STANDARD);

	if (IsFlatAllValid(orderdate_vec)) {
		auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(orderdate_vec);
		for (duckdb::idx_t row = 0; row < match_count; row++) {
			const auto year = static_cast<int16_t>(duckdb::Date::ExtractYear(orderdates[row]));
			if (year < kMinYear || year > kMaxYear) {
				continue;
			}
			if (profile) {
				profile->orders_date_survivors++;
			}
			const int slot = static_cast<int>(year - kMinYear);
			state.order_year_slots.StoreSlot(survivors[row].orderkey, static_cast<int8_t>(slot));
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderdate_format;
	orderdate_vec.ToUnifiedFormat(match_count, orderdate_format);
	auto orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
	for (duckdb::idx_t row = 0; row < match_count; row++) {
		const auto orderdate_index = orderdate_format.sel->get_index(row);
		if (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) {
			continue;
		}
		const auto year = static_cast<int16_t>(duckdb::Date::ExtractYear(orderdates[orderdate_index]));
		if (year < kMinYear || year > kMaxYear) {
			continue;
		}
		if (profile) {
			profile->orders_date_survivors++;
		}
		const int slot = static_cast<int>(year - kMinYear);
		state.order_year_slots.StoreSlot(survivors[row].orderkey, static_cast<int8_t>(slot));
	}
}

struct Q8LineitemFusedSurvivor {
	duckdb::idx_t vector_index = 0;
	int slot = -1;
};

struct Q8LineitemFusedWorkerState {
	duckdb::RowGroup *active_row_group = nullptr;
	bool active_row_group_skipped = false;
};

void SkipFusedLineitemPayload(duckdb::RowGroup &row_group, duckdb::CollectionScanState &scan_state) {
	row_group.SkipProjectedColumn(scan_state, kLineitemSuppkeyScanIndex);
	row_group.SkipProjectedColumn(scan_state, kLineitemExtendedpriceScanIndex);
	row_group.SkipProjectedColumn(scan_state, kLineitemDiscountScanIndex);
}

duckdb::idx_t CollectFusedPartSurvivors(duckdb::idx_t count, const int64_t *partkeys, const Q8FrozenState &state,
                                        duckdb::SelectionVector &part_sel) {
	duckdb::idx_t part_count = 0;
	for (duckdb::idx_t row = 0; row < count; row++) {
		if (!state.qualifying_parts.Contains(partkeys[row])) {
			continue;
		}
		part_sel.set_index(part_count++, row);
	}
	return part_count;
}

duckdb::idx_t CollectFusedPartSurvivorsUnified(duckdb::idx_t count, duckdb::UnifiedVectorFormat &partkey_format,
                                               const Q8FrozenState &state, duckdb::SelectionVector &part_sel) {
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	duckdb::idx_t part_count = 0;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		if (!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) {
			continue;
		}
		if (!state.qualifying_parts.Contains(partkeys[partkey_index])) {
			continue;
		}
		part_sel.set_index(part_count++, row);
	}
	return part_count;
}

duckdb::idx_t CollectFusedOrderProbeSurvivors(duckdb::idx_t count, int64_t *orderkeys, const Q8FrozenState &state,
                                              Q8LineitemFusedSurvivor *survivors) {
	duckdb::idx_t match_count = 0;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const int8_t slot = LoadOrderYearSlot(state, orderkeys[row]);
		if (slot < 0 || slot >= kYearSlotCount) {
			continue;
		}
		survivors[match_count++] = Q8LineitemFusedSurvivor {row, static_cast<int>(slot)};
	}
	return match_count;
}

duckdb::idx_t CollectFusedOrderProbeSurvivorsUnified(duckdb::idx_t count, duckdb::UnifiedVectorFormat &orderkey_format,
                                                       const Q8FrozenState &state, Q8LineitemFusedSurvivor *survivors) {
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	duckdb::idx_t match_count = 0;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		if (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) {
			continue;
		}
		const int8_t slot = LoadOrderYearSlot(state, orderkeys[orderkey_index]);
		if (slot < 0 || slot >= kYearSlotCount) {
			continue;
		}
		survivors[match_count++] = Q8LineitemFusedSurvivor {row, static_cast<int>(slot)};
	}
	return match_count;
}

void AccumulateFusedLineitem(duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
                             duckdb::RowGroup &row_group, duckdb::SelectionVector &sel, duckdb::idx_t count,
                             const Q8FrozenState &state, const Q8Parameters &params,
                             std::array<VolumeState, kYearSlotCount> &volume_by_year,
                             Q8LineitemFusedWorkerState &fused_state, Q8ParallelProfile *profile) {
	if (!state.HasRegionNations()) {
		return;
	}
	if (state.qualifying_part_keys.empty() || state.qualifying_order_keys.empty()) {
		return;
	}

	const auto &column_ids = scan_state.GetColumnIds();
	if (column_ids.size() != kLineitemProjectedColumnCount) {
		throw duckdb::InternalException("Q8 parallel fused lineitem expected %llu projected columns, got %llu",
		                                static_cast<unsigned long long>(kLineitemProjectedColumnCount),
		                                static_cast<unsigned long long>(column_ids.size()));
	}

	if (profile) {
		profile->lineitem_rows += count;
	}

	if (&row_group != fused_state.active_row_group) {
		fused_state.active_row_group = &row_group;
		fused_state.active_row_group_skipped = false;

		int64_t order_min = 0;
		int64_t order_max = 0;
		if (TryGetRowGroupNumericMinMax(row_group, column_ids[kLineitemOrderkeyScanIndex], order_min, order_max) &&
		    !SortedKeysOverlapRange(state.qualifying_order_keys, order_min, order_max)) {
			fused_state.active_row_group_skipped = true;
		}
		if (!fused_state.active_row_group_skipped) {
			int64_t part_min = 0;
			int64_t part_max = 0;
			if (TryGetRowGroupNumericMinMax(row_group, column_ids[kLineitemPartkeyScanIndex], part_min, part_max) &&
			    !SortedKeysOverlapRange(state.qualifying_part_keys, part_min, part_max)) {
				fused_state.active_row_group_skipped = true;
			}
		}
	}

	if (fused_state.active_row_group_skipped) {
		row_group.SkipProjectedColumn(scan_state, kLineitemPartkeyScanIndex);
		row_group.SkipProjectedColumn(scan_state, kLineitemOrderkeyScanIndex);
		SkipFusedLineitemPayload(row_group, scan_state);
		return;
	}

	if (count == 0) {
		row_group.SkipProjectedColumn(scan_state, kLineitemPartkeyScanIndex);
		row_group.SkipProjectedColumn(scan_state, kLineitemOrderkeyScanIndex);
		SkipFusedLineitemPayload(row_group, scan_state);
		return;
	}

	duckdb::Vector partkey_vec(duckdb::LogicalType::BIGINT);
	row_group.SelectProjectedColumn(transaction, scan_state, kLineitemPartkeyScanIndex, sel, count, partkey_vec,
	                                duckdb::UpdateScanType::STANDARD);

	duckdb::SelectionVector part_sel(count);
	duckdb::idx_t part_count = 0;
	if (IsFlatAllValid(partkey_vec)) {
		part_count = CollectFusedPartSurvivors(count, duckdb::FlatVector::GetData<int64_t>(partkey_vec), state, part_sel);
	} else {
		duckdb::UnifiedVectorFormat partkey_format;
		partkey_vec.ToUnifiedFormat(count, partkey_format);
		part_count = CollectFusedPartSurvivorsUnified(count, partkey_format, state, part_sel);
	}
	if (part_count == 0) {
		row_group.SkipProjectedColumn(scan_state, kLineitemOrderkeyScanIndex);
		SkipFusedLineitemPayload(row_group, scan_state);
		return;
	}

	duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
	row_group.SelectProjectedColumn(transaction, scan_state, kLineitemOrderkeyScanIndex, part_sel, part_count,
	                                orderkey_vec, duckdb::UpdateScanType::STANDARD);

	Q8LineitemFusedSurvivor survivors[STANDARD_VECTOR_SIZE];
	duckdb::idx_t match_count = 0;
	if (IsFlatAllValid(orderkey_vec) && part_count > 0) {
		match_count = CollectFusedOrderProbeSurvivors(part_count, duckdb::FlatVector::GetData<int64_t>(orderkey_vec),
		                                            state, survivors);
	} else {
		duckdb::UnifiedVectorFormat orderkey_format;
		orderkey_vec.ToUnifiedFormat(part_count, orderkey_format);
		match_count = CollectFusedOrderProbeSurvivorsUnified(part_count, orderkey_format, state, survivors);
	}
	if (match_count == 0) {
		SkipFusedLineitemPayload(row_group, scan_state);
		return;
	}

	duckdb::SelectionVector payload_sel(match_count);
	for (duckdb::idx_t row = 0; row < match_count; row++) {
		payload_sel.set_index(row, part_sel.get_index(survivors[row].vector_index));
	}

	duckdb::Vector suppkey_vec(duckdb::LogicalType::BIGINT);
	duckdb::Vector extendedprice_vec(duckdb::LogicalType::DECIMAL(15, 2));
	duckdb::Vector discount_vec(duckdb::LogicalType::DECIMAL(15, 2));
	row_group.SelectProjectedColumn(transaction, scan_state, kLineitemSuppkeyScanIndex, payload_sel, match_count,
	                                suppkey_vec, duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kLineitemExtendedpriceScanIndex, payload_sel, match_count,
	                                extendedprice_vec, duckdb::UpdateScanType::STANDARD);
	row_group.SelectProjectedColumn(transaction, scan_state, kLineitemDiscountScanIndex, payload_sel, match_count,
	                                discount_vec, duckdb::UpdateScanType::STANDARD);

	if (IsFlatAllValid(suppkey_vec) && IsFlatAllValid(extendedprice_vec) && IsFlatAllValid(discount_vec)) {
		if (profile) {
			profile->lineitem_flat_rows += match_count;
		}
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vec);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
		for (duckdb::idx_t row = 0; row < match_count; row++) {
			const int32_t supp_nation = LoadSupplierNation(state, suppkeys[row]);
			if (supp_nation == kAbsentNation) {
				continue;
			}
			if (profile) {
				profile->lineitem_revenue_contribs++;
			}
			AccumulateLineitemVolume(params, survivors[row].slot, supp_nation, extendedprices[row], discounts[row],
			                         volume_by_year);
		}
		return;
	}

	if (profile) {
		profile->lineitem_fallback_rows += match_count;
	}
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	suppkey_vec.ToUnifiedFormat(match_count, suppkey_format);
	extendedprice_vec.ToUnifiedFormat(match_count, extendedprice_format);
	discount_vec.ToUnifiedFormat(match_count, discount_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
	for (duckdb::idx_t row = 0; row < match_count; row++) {
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
		    (!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
			continue;
		}
		const int32_t supp_nation = LoadSupplierNation(state, suppkeys[suppkey_index]);
		if (supp_nation == kAbsentNation) {
			continue;
		}
		if (profile) {
			profile->lineitem_revenue_contribs++;
		}
		AccumulateLineitemVolume(params, survivors[row].slot, supp_nation, extendedprices[extendedprice_index],
		                         discounts[discount_index], volume_by_year);
	}
}

int64_t EstimateTpchMaxPartkey(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	NativeParallelScanHandle handle;
	handle.Initialize(context, bind_data);
	return static_cast<int64_t>(handle.storage->GetTotalRows());
}

void BuildQ8FrozenState(duckdb::ClientContext &context,
                        const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs,
                        Q8FrozenState &state, int64_t order_capacity, duckdb::idx_t build_worker_count,
                        Q8ParallelProfile *profile) {
	using clock = std::chrono::steady_clock;
	if (profile) {
		for (const auto flag : state.params.region_nations) {
			profile->nations_in_region += flag ? 1 : 0;
		}
	}

	{
		const auto t0 = clock::now();
		state.qualifying_parts.ReserveAtLeast(EstimateTpchMaxPartkey(context, *bound_inputs[0]));
		Q8ParallelProfile *part_profile = build_worker_count == 1 ? profile : nullptr;
		std::vector<std::vector<int64_t>> worker_part_keys(build_worker_count);
		RunParallelChunkScan(context, *bound_inputs[0], build_worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     ConsumePartChunk(chunk, state, part_profile, &worker_part_keys[worker_id]);
		                     });
		AppendWorkerSemiJoinKeys(state.qualifying_part_keys, worker_part_keys);
		if (profile) {
			profile->part_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->qualifying_parts = state.qualifying_parts.QualifyingCount();
			profile->part_flag_bytes = state.qualifying_parts.FlagBytes();
		}
	}

	{
		const auto t0 = clock::now();
		auto supplier_source = CreateNativeChunkSource(context, *bound_inputs[1]);
		while (supplier_source->Next()) {
			ConsumeSupplierChunk(supplier_source->Chunk(), state, profile);
		}
		if (profile) {
			profile->supplier_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->supplier_dense_bytes = state.supplier_nations.DenseBytes();
		}
	}

	if (!state.HasRegionNations()) {
		return;
	}

	{
		const auto t0 = clock::now();
		RunNativeFilteredScanConsumer(
		    context, *bound_inputs[2],
		    [&](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state, duckdb::RowGroup &row_group,
		        duckdb::SelectionVector &sel, duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			    ConsumeFusedCustomer(transaction, scan_state, row_group, sel, count, state, profile);
			    return duckdb::FilteredScanConsumerResult::CONTINUE;
		    });
		if (profile) {
			profile->customer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
		}
	}

	state.order_year_slots.ReserveAtLeast(order_capacity);

	{
		const auto t0 = clock::now();
		// Always use the chunk orders path: the fused orders path mis-decodes DATE
		// physical storage (int32) as date_t after SelectProjectedColumn, dropping
		// most qualifying orders. Serial Q8 uses the same chunk path.
		Q8ParallelProfile *orders_profile = build_worker_count == 1 ? profile : nullptr;
		std::vector<std::vector<int64_t>> worker_order_keys(build_worker_count);
		RunParallelChunkScan(context, *bound_inputs[3], build_worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     ConsumeOrdersChunk(chunk, state, orders_profile, &worker_order_keys[worker_id]);
		                     });
		AppendWorkerSemiJoinKeys(state.qualifying_order_keys, worker_order_keys);
		if (profile) {
			profile->orders_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
			profile->order_slot_bytes = state.order_year_slots.SlotBytes();
		}
	}

	state.PrepareOrderProbeState();
	state.FinalizeSemiJoinKeys();
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const Q8FrozenState &state, const Q8Parameters &params,
                             std::array<VolumeState, kYearSlotCount> &volume_by_year, Q8ParallelProfile *profile) {
	if (!state.HasRegionNations()) {
		return;
	}
	if (profile) {
		profile->lineitem_rows += chunk.size();
	}

	auto &partkey_vector = chunk.data[Q8LineitemChunkLayout::PARTKEY_INDEX];
	auto &orderkey_vector = chunk.data[Q8LineitemChunkLayout::ORDERKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q8LineitemChunkLayout::SUPPKEY_INDEX];
	auto &extendedprice_vector = chunk.data[Q8LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q8LineitemChunkLayout::DISCOUNT_INDEX];

	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) &&
	    IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
		if (profile) {
			profile->lineitem_flat_rows += chunk.size();
		}
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			int32_t supp_nation = kAbsentNation;
			const int slot =
			    ProbeLineitemSurvivor(state, partkeys[row], orderkeys[row], suppkeys[row], supp_nation, profile);
			if (slot < 0) {
				continue;
			}
			AccumulateLineitemVolume(params, slot, supp_nation, extendedprices[row], discounts[row], volume_by_year);
			if (profile) {
				profile->lineitem_revenue_contribs++;
			}
		}
		return;
	}

	if (IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(orderkey_vector) &&
	    IsDictionaryOverFlat(suppkey_vector) && IsDictionaryOverFlat(extendedprice_vector) &&
	    IsDictionaryOverFlat(discount_vector)) {
		if (profile) {
			profile->lineitem_dict_rows += chunk.size();
		}
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
		auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
		auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
		auto extendedprices =
		    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
		auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
		auto &extendedprice_validity =
		    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
		auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey_index = partkey_sel.get_index(row);
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto suppkey_index = suppkey_sel.get_index(row);
			if (!partkey_validity.RowIsValid(partkey_index) || !orderkey_validity.RowIsValid(orderkey_index) ||
			    !suppkey_validity.RowIsValid(suppkey_index)) {
				if (profile) {
					profile->lineitem_fallback_rows++;
				}
				continue;
			}
			int32_t supp_nation = kAbsentNation;
			const int slot = ProbeLineitemSurvivor(state, partkeys[partkey_index], orderkeys[orderkey_index],
			                                       suppkeys[suppkey_index], supp_nation, profile);
			if (slot < 0) {
				continue;
			}
			const auto extendedprice_index = extendedprice_sel.get_index(row);
			const auto discount_index = discount_sel.get_index(row);
			if (!extendedprice_validity.RowIsValid(extendedprice_index) ||
			    !discount_validity.RowIsValid(discount_index)) {
				if (profile) {
					profile->lineitem_fallback_rows++;
				}
				continue;
			}
			AccumulateLineitemVolume(params, slot, supp_nation, extendedprices[extendedprice_index],
			                         discounts[discount_index], volume_by_year);
			if (profile) {
				profile->lineitem_revenue_contribs++;
			}
		}
		return;
	}

	if (profile) {
		profile->lineitem_fallback_rows += chunk.size();
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index))) {
			continue;
		}
		int32_t supp_nation = kAbsentNation;
		const int slot = ProbeLineitemSurvivor(state, partkeys[partkey_index], orderkeys[orderkey_index],
		                                       suppkeys[suppkey_index], supp_nation, profile);
		if (slot < 0) {
			continue;
		}
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		const auto discount_index = discount_format.sel->get_index(row);
		if ((!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
		    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
			continue;
		}
		AccumulateLineitemVolume(params, slot, supp_nation, extendedprices[extendedprice_index],
		                         discounts[discount_index], volume_by_year);
		if (profile) {
			profile->lineitem_revenue_contribs++;
		}
	}
}

void MergeVolumeStates(std::array<VolumeState, kYearSlotCount> &global,
                       const std::array<VolumeState, kYearSlotCount> &local) {
	for (int slot = 0; slot < kYearSlotCount; slot++) {
#if defined(__SIZEOF_INT128__)
		global[static_cast<size_t>(slot)].nation_volume += local[static_cast<size_t>(slot)].nation_volume;
		global[static_cast<size_t>(slot)].total_volume += local[static_cast<size_t>(slot)].total_volume;
#else
		if (!duckdb::Hugeint::TryAddInPlace(global[static_cast<size_t>(slot)].nation_volume,
		                                    local[static_cast<size_t>(slot)].nation_volume) ||
		    !duckdb::Hugeint::TryAddInPlace(global[static_cast<size_t>(slot)].total_volume,
		                                    local[static_cast<size_t>(slot)].total_volume)) {
			throw duckdb::OutOfRangeException("Overflow while merging Q8 volume");
		}
#endif
	}
}

class Q8ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q8ParallelKernel(Q8Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 5 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2] ||
		    !bound_inputs[3] || !bound_inputs[4]) {
			throw duckdb::InternalException("Q8 parallel expects bound part, supplier, customer, orders, lineitem scans");
		}

		using clock = std::chrono::steady_clock;
		const bool profile_enabled = Q8ProfileEnabled();
		Q8ParallelProfile profile;
		const auto total_t0 = clock::now();

		Q8FrozenState frozen(params);
		const auto order_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[3]);
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);

		const auto build_t0 = clock::now();
		BuildQ8FrozenState(context, bound_inputs, frozen, order_capacity, worker_count,
		                   profile_enabled ? &profile : nullptr);
		profile.build_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - build_t0).count();

		std::vector<CacheLinePadded<std::array<VolumeState, kYearSlotCount>>> workers(worker_count);
		std::vector<Q8WorkerLineitemProfile> lineitem_worker_profiles(profile_enabled ? static_cast<size_t>(worker_count)
		                                                                              : 0);
		std::vector<Q8LineitemFusedWorkerState> lineitem_fused_workers(worker_count);

		if (frozen.HasRegionNations()) {
			const auto lineitem_t0 = clock::now();
			const bool use_fused_lineitem =
			    !frozen.qualifying_part_keys.empty() && !frozen.qualifying_order_keys.empty();
			if (use_fused_lineitem) {
				RunParallelFilteredScan(
				    context, *bound_inputs[4], worker_count,
				    [&](duckdb::idx_t worker_id, duckdb::TransactionData transaction,
				        duckdb::CollectionScanState &scan_state, duckdb::RowGroup &row_group,
				        duckdb::SelectionVector &sel, duckdb::idx_t count) {
					    const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
					    AccumulateFusedLineitem(transaction, scan_state, row_group, sel, count, frozen, params,
					                            workers[worker_id].value,
					                            lineitem_fused_workers[static_cast<size_t>(worker_id)],
					                            profile_enabled && worker_count == 1 ? &profile : nullptr);
					    if (profile_enabled) {
						    auto &worker_profile = lineitem_worker_profiles[worker_id];
						    worker_profile.rows += count;
						    worker_profile.elapsed_ns +=
						        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - worker_t0).count();
					    }
				    });
			} else {
				RunParallelChunkScan(context, *bound_inputs[4], worker_count,
				                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
					                     const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
					                     AccumulateLineitemChunk(chunk, frozen, params, workers[worker_id].value,
					                                             profile_enabled && worker_count == 1 ? &profile : nullptr);
					                     if (profile_enabled) {
						                     auto &worker_profile = lineitem_worker_profiles[worker_id];
						                     worker_profile.rows += chunk.size();
						                     worker_profile.elapsed_ns +=
						                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() -
						                                                                            worker_t0)
						                             .count();
					                     }
				                     });
			}
			profile.lineitem_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();
			if (profile_enabled) {
				int64_t worker_ns_sum = 0;
				for (const auto &worker_profile : lineitem_worker_profiles) {
					worker_ns_sum += worker_profile.elapsed_ns;
					profile.max_lineitem_worker_ns =
					    std::max(profile.max_lineitem_worker_ns, worker_profile.elapsed_ns);
				}
				profile.avg_lineitem_worker_ns =
				    lineitem_worker_profiles.empty()
				        ? 0
				        : worker_ns_sum / static_cast<int64_t>(lineitem_worker_profiles.size());
			}

			const auto merge_t0 = clock::now();
			std::array<VolumeState, kYearSlotCount> merged {};
			for (const auto &worker : workers) {
				MergeVolumeStates(merged, worker.value);
			}
			profile.merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();

			{
				const auto output_t0 = clock::now();
				output_rows.clear();
				for (int slot = 0; slot < kYearSlotCount; slot++) {
					const auto &state = merged[static_cast<size_t>(slot)];
					if (state.total_volume == 0) {
						continue;
					}
					Q8OutputRow row;
					row.year = kMinYear + slot;
					row.mkt_share = ComputeMarketShare(state);
					output_rows.push_back(std::move(row));
				}
				std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
				profile.output_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_t0).count();
			}
		} else {
			output_rows.clear();
		}

		profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
		if (profile_enabled) {
			PrintQ8ParallelProfile(profile);
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.year));
			output.SetValue(1, row, duckdb::Value::DOUBLE(entry.mkt_share));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q8Parameters params;
	std::vector<Q8OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ8ParallelKernel(const Q8Parameters &params) {
	return duckdb::make_uniq<Q8ParallelKernel>(params);
}

} // namespace accelerator_codegen
