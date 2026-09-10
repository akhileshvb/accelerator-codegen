#include "tpch/q9/duckdb/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kProfitDecimalWidth = 38;
constexpr uint8_t kProfitDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int64_t kDefaultPartCapacity = 200001;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int16_t kAbsentYear = 0;
constexpr int32_t kAbsentNation = -1;
constexpr int32_t kNationCount = 25;
constexpr int64_t kYearBase = 1992;
constexpr int64_t kYearCount = 7; // 1992..1998 inclusive

using Clock = std::chrono::steady_clock;

#if defined(__SIZEOF_INT128__)
using q9_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q9_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddProfit(q9_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw, int64_t supplycost_raw,
               int64_t quantity_raw) {
	const q9_acc_t revenue =
	    static_cast<q9_acc_t>(extendedprice_raw) * static_cast<q9_acc_t>(100 - discount_raw);
	const q9_acc_t cost = static_cast<q9_acc_t>(supplycost_raw) * static_cast<q9_acc_t>(quantity_raw);
	accumulator += revenue - cost;
}
#else
using q9_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q9_acc_t &value) {
	return value;
}

void AddProfit(q9_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw, int64_t supplycost_raw,
               int64_t quantity_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t revenue_value;
	duckdb::hugeint_t cost_lhs;
	duckdb::hugeint_t cost_rhs;
	duckdb::hugeint_t cost_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, revenue_value) ||
	    !duckdb::Hugeint::TryConvert(supplycost_raw, cost_lhs) ||
	    !duckdb::Hugeint::TryConvert(quantity_raw, cost_rhs) ||
	    !duckdb::Hugeint::TryMultiply(cost_lhs, cost_rhs, cost_value)) {
		throw duckdb::OutOfRangeException("Overflow while computing Q9 profit");
	}
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, revenue_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q9 profit");
	}
	cost_value = duckdb::Hugeint::Negate(cost_value);
	if (!duckdb::Hugeint::TryAddInPlace(accumulator, cost_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q9 profit");
	}
}
#endif

#ifdef ACCEL_EXPERIMENTAL_Q9_OPTS
bool ReadBoolEnv(const char *name, bool default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}
#endif

struct Q9RuntimeConfig {
	bool dense_orders = true;
	bool dense_supplier = true;
	bool part_bitmap = true;
	bool packed_partsupp = true;
	bool flat_lineitem = true;
	bool dense_agg = true;
};

Q9RuntimeConfig LoadQ9Config() {
	Q9RuntimeConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q9_OPTS
	// Retained defaults after profile-guided A/B (every-seed wins vs prior baseline).
	config.dense_orders = ReadBoolEnv("ACCEL_Q9_DENSE_ORDERS", true);
	config.dense_supplier = ReadBoolEnv("ACCEL_Q9_DENSE_SUPPLIER", true);
	config.part_bitmap = ReadBoolEnv("ACCEL_Q9_PART_BITMAP", true);
	config.packed_partsupp = ReadBoolEnv("ACCEL_Q9_PACKED_PARTSUPP", true);
	config.flat_lineitem = ReadBoolEnv("ACCEL_Q9_FLAT_LINEITEM", true);
	config.dense_agg = ReadBoolEnv("ACCEL_Q9_DENSE_AGG", true);
#endif
	return config;
}

void RecordLayout(Q9LayoutHistogram *histogram, duckdb::Vector &vector) {
	if (!histogram) {
		return;
	}
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		if (duckdb::FlatVector::Validity(vector).AllValid()) {
			histogram->flat_all_valid += 1;
		} else {
			histogram->flat_nullable += 1;
		}
		return;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			if (duckdb::FlatVector::Validity(child).AllValid()) {
				histogram->dict_flat_all_valid += 1;
			} else {
				histogram->dict_flat_nullable += 1;
			}
			return;
		}
		break;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		if (duckdb::ConstantVector::Validity(vector).RowIsValid(0)) {
			histogram->constant_valid += 1;
			return;
		}
		break;
	default:
		break;
	}
	histogram->other += 1;
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

struct PartsuppKey {
	int64_t partkey;
	int64_t suppkey;

	bool operator==(const PartsuppKey &other) const {
		return partkey == other.partkey && suppkey == other.suppkey;
	}
};

struct PartsuppKeyHash {
	std::size_t operator()(const PartsuppKey &key) const {
		const auto h1 = std::hash<int64_t> {}(key.partkey);
		const auto h2 = std::hash<int64_t> {}(key.suppkey);
		return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
	}
};

uint64_t PackPartsuppKey(int64_t partkey, int64_t suppkey) {
	// 32|32 packing is collision-free for non-negative keys with both domains < 2^32
	// (TPC-H through SF100: partkey <= 2e7, suppkey <= 1e6).
	D_ASSERT(partkey >= 0 && partkey < (int64_t {1} << 32));
	D_ASSERT(suppkey >= 0 && suppkey < (int64_t {1} << 32));
	return (static_cast<uint64_t>(partkey) << 32) | static_cast<uint32_t>(suppkey);
}

struct AggregateKey {
	int32_t nationkey;
	int64_t year;

	bool operator==(const AggregateKey &other) const {
		return nationkey == other.nationkey && year == other.year;
	}
};

struct AggregateKeyHash {
	std::size_t operator()(const AggregateKey &key) const {
		const auto h1 = std::hash<int32_t> {}(key.nationkey);
		const auto h2 = std::hash<int64_t> {}(key.year);
		return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
	}
};

struct Q9OutputRow {
	std::string nation;
	int64_t year;
	duckdb::hugeint_t sum_profit;
};

bool BetterOutputRow(const Q9OutputRow &lhs, const Q9OutputRow &rhs) {
	if (lhs.nation != rhs.nation) {
		return lhs.nation < rhs.nation;
	}
	return lhs.year > rhs.year;
}

bool NameContainsColor(const duckdb::string_t &name, const duckdb::string_t &color) {
	const auto haystack_size = name.GetSize();
	const auto needle_size = color.GetSize();
	if (needle_size == 0) {
		return true;
	}
	if (needle_size > haystack_size) {
		return false;
	}
	const auto *haystack = reinterpret_cast<const unsigned char *>(name.GetData());
	const auto *needle = reinterpret_cast<const unsigned char *>(color.GetData());
	const auto *location = static_cast<const unsigned char *>(std::memchr(haystack, needle[0], haystack_size));
	while (location != nullptr) {
		const auto offset = static_cast<duckdb::idx_t>(location - haystack);
		if (offset + needle_size > haystack_size) {
			return false;
		}
		if (std::memcmp(location, needle, needle_size) == 0) {
			return true;
		}
		const auto remaining = haystack_size - (offset + 1);
		if (remaining == 0) {
			return false;
		}
		location = static_cast<const unsigned char *>(std::memchr(location + 1, needle[0], remaining));
	}
	return false;
}

class OrderYearStore {
public:
	explicit OrderYearStore(bool use_dense_p) : use_dense(use_dense_p) {
	}

	void WarmInitialize(Q9ProfileCounters *profile) {
		if (!use_dense || initialized) {
			return;
		}
		const auto start = profile ? Clock::now() : Clock::time_point {};
		EnsureCapacity(kDefaultOrderCapacity);
		initialized = true;
		if (profile) {
			profile->orders_init_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
	}

	void Insert(int64_t orderkey, int64_t year) {
		if (use_dense) {
			if (orderkey >= 0 && orderkey < capacity) {
				dense_years[static_cast<size_t>(orderkey)] = static_cast<int16_t>(year);
				return;
			}
			if (orderkey >= capacity && orderkey <= 1'000'000'000) {
				EnsureCapacity(orderkey + 1);
				dense_years[static_cast<size_t>(orderkey)] = static_cast<int16_t>(year);
				return;
			}
			overflow[orderkey] = year;
			return;
		}
		hash_years[orderkey] = year;
	}

	bool Lookup(int64_t orderkey, int64_t &year_out) const {
		if (use_dense) {
			if (orderkey >= 0 && orderkey < capacity) {
				const auto year = dense_years[static_cast<size_t>(orderkey)];
				if (year != kAbsentYear) {
					year_out = year;
					return true;
				}
				return false;
			}
			auto it = overflow.find(orderkey);
			if (it == overflow.end()) {
				return false;
			}
			year_out = it->second;
			return true;
		}
		auto it = hash_years.find(orderkey);
		if (it == hash_years.end()) {
			return false;
		}
		year_out = it->second;
		return true;
	}

	duckdb::idx_t EstimatedBytes() const {
		if (use_dense) {
			return static_cast<duckdb::idx_t>(capacity) * sizeof(int16_t) +
			       overflow.size() * (sizeof(int64_t) + sizeof(int64_t));
		}
		return hash_years.size() * (sizeof(int64_t) + sizeof(int64_t));
	}

	duckdb::idx_t OverflowCount() const {
		return overflow.size();
	}

private:
	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2) ? std::numeric_limits<int64_t>::max()
			                                                                             : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		dense_years.resize(static_cast<size_t>(target), kAbsentYear);
		capacity = target;
	}

	bool use_dense;
	bool initialized = false;
	int64_t capacity = 0;
	std::vector<int16_t> dense_years;
	std::unordered_map<int64_t, int64_t> overflow;
	std::unordered_map<int64_t, int64_t> hash_years;
};

class PartMembership {
public:
	explicit PartMembership(bool use_bitmap_p) : use_bitmap(use_bitmap_p) {
		if (use_bitmap) {
			const auto words = static_cast<size_t>((kDefaultPartCapacity + 63) / 64);
			bits.assign(words, 0);
			bit_capacity = kDefaultPartCapacity;
		}
	}

	void Insert(int64_t partkey) {
		++count;
		if (!use_bitmap) {
			hash.insert(partkey);
			return;
		}
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

	bool Contains(int64_t partkey) const {
		if (!use_bitmap) {
			return hash.find(partkey) != hash.end();
		}
		if (partkey >= 0 && partkey < bit_capacity) {
			return (bits[static_cast<size_t>(partkey) >> 6] >> (partkey & 63)) & 1ULL;
		}
		return overflow.find(partkey) != overflow.end();
	}

	bool Empty() const {
		return count == 0;
	}

	duckdb::idx_t Size() const {
		return count;
	}

	duckdb::idx_t EstimatedBytes() const {
		if (use_bitmap) {
			return bits.size() * sizeof(uint64_t) + overflow.size() * sizeof(int64_t);
		}
		return hash.size() * sizeof(int64_t);
	}

private:
	bool use_bitmap;
	int64_t bit_capacity = 0;
	duckdb::idx_t count = 0;
	std::vector<uint64_t> bits;
	std::unordered_set<int64_t> overflow;
	std::unordered_set<int64_t> hash;
};

class SupplierNationStore {
public:
	explicit SupplierNationStore(bool use_dense_p) : use_dense(use_dense_p) {
		if (use_dense) {
			dense.assign(static_cast<size_t>(kDefaultSupplierCapacity), kAbsentNation);
			capacity = kDefaultSupplierCapacity;
		}
	}

	void Insert(int64_t suppkey, int32_t nationkey) {
		++count;
		if (!use_dense) {
			hash[suppkey] = nationkey;
			return;
		}
		if (suppkey >= 0 && suppkey < capacity) {
			dense[static_cast<size_t>(suppkey)] = nationkey;
			return;
		}
		if (suppkey >= capacity && suppkey <= 1'000'000'000) {
			int64_t target = suppkey + 1;
			if (capacity > 0) {
				const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2)
				                            ? std::numeric_limits<int64_t>::max()
				                            : capacity * 2;
				if (doubled > target) {
					target = doubled;
				}
			}
			dense.resize(static_cast<size_t>(target), kAbsentNation);
			capacity = target;
			dense[static_cast<size_t>(suppkey)] = nationkey;
			return;
		}
		overflow[suppkey] = nationkey;
	}

	bool Lookup(int64_t suppkey, int32_t &nationkey_out) const {
		if (!use_dense) {
			auto it = hash.find(suppkey);
			if (it == hash.end()) {
				return false;
			}
			nationkey_out = it->second;
			return true;
		}
		if (suppkey >= 0 && suppkey < capacity) {
			const auto nation = dense[static_cast<size_t>(suppkey)];
			if (nation == kAbsentNation) {
				return false;
			}
			nationkey_out = nation;
			return true;
		}
		auto it = overflow.find(suppkey);
		if (it == overflow.end()) {
			return false;
		}
		nationkey_out = it->second;
		return true;
	}

	duckdb::idx_t Size() const {
		return count;
	}

	duckdb::idx_t EstimatedBytes() const {
		if (use_dense) {
			return dense.size() * sizeof(int32_t) + overflow.size() * (sizeof(int64_t) + sizeof(int32_t));
		}
		return hash.size() * (sizeof(int64_t) + sizeof(int32_t));
	}

private:
	bool use_dense;
	int64_t capacity = 0;
	duckdb::idx_t count = 0;
	std::vector<int32_t> dense;
	std::unordered_map<int64_t, int32_t> overflow;
	std::unordered_map<int64_t, int32_t> hash;
};

class ProfitAggregator {
public:
	explicit ProfitAggregator(bool use_dense_p) : use_dense(use_dense_p) {
		if (use_dense) {
			dense_acc.assign(static_cast<size_t>(kNationCount * kYearCount), 0);
			dense_present.assign(static_cast<size_t>(kNationCount * kYearCount), 0);
		}
	}

	void Add(int32_t nationkey, int64_t year, int64_t extendedprice_raw, int64_t discount_raw, int64_t supplycost_raw,
	         int64_t quantity_raw) {
		if (use_dense && nationkey >= 0 && nationkey < kNationCount && year >= kYearBase &&
		    year < kYearBase + kYearCount) {
			const auto idx = static_cast<size_t>(nationkey * kYearCount + (year - kYearBase));
			AddProfit(dense_acc[idx], extendedprice_raw, discount_raw, supplycost_raw, quantity_raw);
			dense_present[idx] = 1;
			return;
		}
		AddProfit(hash_acc[{nationkey, year}], extendedprice_raw, discount_raw, supplycost_raw, quantity_raw);
	}

	template <class Callback>
	void ForEach(Callback &&callback) const {
		if (use_dense) {
			for (int32_t nation = 0; nation < kNationCount; nation++) {
				for (int64_t year_offset = 0; year_offset < kYearCount; year_offset++) {
					const auto idx = static_cast<size_t>(nation * kYearCount + year_offset);
					if (!dense_present[idx]) {
						continue;
					}
					callback(nation, kYearBase + year_offset, dense_acc[idx]);
				}
			}
		}
		for (const auto &entry : hash_acc) {
			callback(entry.first.nationkey, entry.first.year, entry.second);
		}
	}

	duckdb::idx_t GroupCount() const {
		duckdb::idx_t count = hash_acc.size();
		if (use_dense) {
			for (auto present : dense_present) {
				count += present ? 1 : 0;
			}
		}
		return count;
	}

	duckdb::idx_t EstimatedBytes() const {
		duckdb::idx_t bytes = hash_acc.size() * (sizeof(AggregateKey) + sizeof(q9_acc_t));
		if (use_dense) {
			bytes += dense_acc.size() * sizeof(q9_acc_t) + dense_present.size();
		}
		return bytes;
	}

private:
	bool use_dense;
	std::vector<q9_acc_t> dense_acc;
	std::vector<uint8_t> dense_present;
	std::unordered_map<AggregateKey, q9_acc_t, AggregateKeyHash> hash_acc;
};

class Q9Kernel final : public KernelInstance {
public:
	explicit Q9Kernel(Q9Parameters params_p, Q9ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(LoadQ9Config()), color_needle(params.color),
	      selected_parts(config.part_bitmap), supplier_nations(config.dense_supplier),
	      order_years(config.dense_orders), aggregates(config.dense_agg) {
		if (profile) {
			profile->used_dense_orders = config.dense_orders;
			profile->used_dense_supplier = config.dense_supplier;
			profile->used_part_bitmap = config.part_bitmap;
			profile->used_packed_partsupp = config.packed_partsupp;
			profile->used_flat_lineitem = config.flat_lineitem;
			profile->used_dense_agg = config.dense_agg;
		}
		order_years.WarmInitialize(profile);
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		const auto start = profile ? Clock::now() : Clock::time_point {};
		switch (static_cast<Q9InputId>(input_id)) {
		case Q9InputId::PART:
			ConsumePart(input);
			BreakPhase(start, profile ? &profile->part_consume_ns : nullptr);
			return;
		case Q9InputId::SUPPLIER:
			ConsumeSupplier(input);
			BreakPhase(start, profile ? &profile->supplier_consume_ns : nullptr);
			return;
		case Q9InputId::NATION:
			ConsumeNation(input);
			BreakPhase(start, profile ? &profile->nation_consume_ns : nullptr);
			return;
		case Q9InputId::ORDERS:
			ConsumeOrders(input);
			BreakPhase(start, profile ? &profile->orders_consume_ns : nullptr);
			return;
		case Q9InputId::PARTSUPP:
			ConsumePartsupp(input);
			BreakPhase(start, profile ? &profile->partsupp_consume_ns : nullptr);
			return;
		case Q9InputId::LINEITEM:
			ConsumeLineitem(input);
			BreakPhase(start, profile ? &profile->lineitem_consume_ns : nullptr);
			return;
		}
		throw duckdb::InternalException("Q9 kernel received unexpected input id %llu",
		                                static_cast<unsigned long long>(input_id));
	}

	void Finalize() override {
		const auto start = profile ? Clock::now() : Clock::time_point {};
		output_rows.clear();
		aggregates.ForEach([&](int32_t nationkey, int64_t year, const q9_acc_t &acc) {
			auto nation_it = nation_names.find(nationkey);
			if (nation_it == nation_names.end()) {
				return;
			}
			Q9OutputRow row;
			row.nation = nation_it->second;
			row.year = year;
			row.sum_profit = FromAccumulator(acc);
			output_rows.push_back(std::move(row));
		});
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		output_offset = 0;
		finalized = true;
		if (profile) {
			profile->group_count = aggregates.GroupCount();
			profile->output_rows = output_rows.size();
			profile->selected_parts_bytes = selected_parts.EstimatedBytes();
			profile->supplier_map_bytes = supplier_nations.EstimatedBytes();
			profile->nation_map_bytes = 0;
			for (const auto &entry : nation_names) {
				profile->nation_map_bytes += sizeof(int32_t) + entry.second.size();
			}
			profile->orders_map_bytes = order_years.EstimatedBytes();
			profile->orders_overflow = static_cast<int64_t>(order_years.OverflowCount());
			profile->partsupp_map_bytes =
			    config.packed_partsupp
			        ? packed_partsupp_costs.size() * (sizeof(uint64_t) + sizeof(int64_t))
			        : partsupp_costs.size() * (sizeof(PartsuppKey) + sizeof(int64_t));
			profile->aggregate_bytes = aggregates.EstimatedBytes();
			profile->finalize_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= output_rows.size()) {
			return false;
		}
		const duckdb::idx_t remaining = output_rows.size() - output_offset;
		const duckdb::idx_t count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &entry = output_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value(entry.nation));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.year));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.sum_profit, kProfitDecimalWidth, kProfitDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	static void BreakPhase(Clock::time_point start, int64_t *counter) {
		if (counter) {
			*counter += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
	}

	bool PartsuppLookup(int64_t partkey, int64_t suppkey, int64_t &cost_out) const {
		if (config.packed_partsupp) {
			auto it = packed_partsupp_costs.find(PackPartsuppKey(partkey, suppkey));
			if (it == packed_partsupp_costs.end()) {
				return false;
			}
			cost_out = it->second;
			return true;
		}
		auto it = partsupp_costs.find({partkey, suppkey});
		if (it == partsupp_costs.end()) {
			return false;
		}
		cost_out = it->second;
		return true;
	}

	bool PartsuppEmpty() const {
		return config.packed_partsupp ? packed_partsupp_costs.empty() : partsupp_costs.empty();
	}

	void ConsumePart(duckdb::DataChunk &input) {
		if (profile) {
			profile->part_rows += input.size();
			RecordLayout(&profile->layout_p_name, input.data[Q9PartChunkLayout::NAME_INDEX]);
		}
		auto &partkey_vector = input.data[Q9PartChunkLayout::PARTKEY_INDEX];
		auto &name_vector = input.data[Q9PartChunkLayout::NAME_INDEX];

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat name_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);

		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		const duckdb::string_t color(color_needle);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			if (!NameContainsColor(names[name_index], color)) {
				continue;
			}
			selected_parts.Insert(partkeys[partkey_index]);
		}
		if (profile) {
			profile->part_selected = selected_parts.Size();
		}
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		if (profile) {
			profile->supplier_rows += input.size();
		}
		auto &suppkey_vector = input.data[Q9SupplierChunkLayout::SUPPKEY_INDEX];
		auto &nationkey_vector = input.data[Q9SupplierChunkLayout::NATIONKEY_INDEX];

		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat nationkey_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);

		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!nationkey_format.validity.AllValid() &&
			     !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
				continue;
			}
			supplier_nations.Insert(suppkeys[suppkey_index], nationkeys[nationkey_index]);
		}
	}

	void ConsumeNation(duckdb::DataChunk &input) {
		if (profile) {
			profile->nation_rows += input.size();
		}
		auto &nationkey_vector = input.data[Q9NationChunkLayout::NATIONKEY_INDEX];
		auto &name_vector = input.data[Q9NationChunkLayout::NAME_INDEX];

		duckdb::UnifiedVectorFormat nationkey_format;
		duckdb::UnifiedVectorFormat name_format;
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);

		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!nationkey_format.validity.AllValid() &&
			     !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			nation_names[nationkeys[nationkey_index]] = names[name_index].GetString();
		}
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		if (profile) {
			profile->orders_rows += input.size();
			RecordLayout(&profile->layout_o_orderkey, input.data[Q9OrdersChunkLayout::ORDERKEY_INDEX]);
			RecordLayout(&profile->layout_o_orderdate, input.data[Q9OrdersChunkLayout::ORDERDATE_INDEX]);
		}
		auto &orderkey_vector = input.data[Q9OrdersChunkLayout::ORDERKEY_INDEX];
		auto &orderdate_vector = input.data[Q9OrdersChunkLayout::ORDERDATE_INDEX];

		const auto format_start = profile ? Clock::now() : Clock::time_point {};
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat orderdate_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		orderdate_vector.ToUnifiedFormat(input.size(), orderdate_format);
		if (profile) {
			profile->orders_format_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - format_start).count();
		}

		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);

		const auto insert_start = profile ? Clock::now() : Clock::time_point {};
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto orderdate_index = orderdate_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!orderdate_format.validity.AllValid() &&
			     !orderdate_format.validity.RowIsValidUnsafe(orderdate_index))) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			const auto year = static_cast<int64_t>(duckdb::Date::ExtractYear(orderdates[orderdate_index]));
			if (profile) {
				if (!profile->orderkey_domain_seen) {
					profile->orderkey_min = orderkey;
					profile->orderkey_max = orderkey;
					profile->orderkey_domain_seen = true;
				} else {
					profile->orderkey_min = std::min(profile->orderkey_min, orderkey);
					profile->orderkey_max = std::max(profile->orderkey_max, orderkey);
				}
			}
			order_years.Insert(orderkey, year);
		}
		if (profile) {
			profile->orders_insert_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - insert_start).count();
		}
	}

	void ConsumePartsupp(duckdb::DataChunk &input) {
		if (profile) {
			profile->partsupp_rows += input.size();
			RecordLayout(&profile->layout_ps_partkey, input.data[Q9PartsuppChunkLayout::PARTKEY_INDEX]);
		}
		if (selected_parts.Empty()) {
			return;
		}
		auto &partkey_vector = input.data[Q9PartsuppChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q9PartsuppChunkLayout::SUPPKEY_INDEX];
		auto &supplycost_vector = input.data[Q9PartsuppChunkLayout::SUPPLYCOST_INDEX];

		const auto format_start = profile ? Clock::now() : Clock::time_point {};
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat supplycost_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		supplycost_vector.ToUnifiedFormat(input.size(), supplycost_format);
		if (profile) {
			profile->partsupp_format_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - format_start).count();
		}

		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);

		const auto insert_start = profile ? Clock::now() : Clock::time_point {};
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto supplycost_index = supplycost_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!supplycost_format.validity.AllValid() &&
			     !supplycost_format.validity.RowIsValidUnsafe(supplycost_index))) {
				continue;
			}
			const auto partkey = partkeys[partkey_index];
			if (!selected_parts.Contains(partkey)) {
				if (profile) {
					profile->partsupp_part_miss += 1;
				}
				continue;
			}
			if (profile) {
				profile->partsupp_part_hit += 1;
			}
			const auto suppkey = suppkeys[suppkey_index];
			const auto cost = supplycosts[supplycost_index];
			if (config.packed_partsupp) {
				packed_partsupp_costs[PackPartsuppKey(partkey, suppkey)] = cost;
			} else {
				partsupp_costs[{partkey, suppkey}] = cost;
			}
		}
		if (profile) {
			profile->partsupp_insert_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - insert_start).count();
			profile->partsupp_selected =
			    config.packed_partsupp ? packed_partsupp_costs.size() : partsupp_costs.size();
		}
	}

	void ProbeLineitemRow(int64_t partkey, int64_t suppkey, int64_t orderkey, int64_t quantity_raw,
	                      int64_t extendedprice_raw, int64_t discount_raw) {
		if (!selected_parts.Contains(partkey)) {
			if (profile) {
				profile->line_part_miss += 1;
			}
			return;
		}
		if (profile) {
			profile->line_part_hit += 1;
		}

		int32_t nationkey = 0;
		if (!supplier_nations.Lookup(suppkey, nationkey)) {
			if (profile) {
				profile->line_supplier_miss += 1;
			}
			return;
		}
		if (profile) {
			profile->line_supplier_hit += 1;
		}

		int64_t year = 0;
		if (!order_years.Lookup(orderkey, year)) {
			if (profile) {
				profile->line_order_miss += 1;
			}
			return;
		}
		if (profile) {
			profile->line_order_hit += 1;
		}

		int64_t supplycost = 0;
		if (!PartsuppLookup(partkey, suppkey, supplycost)) {
			if (profile) {
				profile->line_partsupp_miss += 1;
			}
			return;
		}
		if (profile) {
			profile->line_partsupp_hit += 1;
		}

		aggregates.Add(nationkey, year, extendedprice_raw, discount_raw, supplycost, quantity_raw);
		if (profile) {
			profile->lineitem_survivors += 1;
		}
	}

	void ConsumeLineitemFlat(duckdb::DataChunk &input) {
		auto *partkeys = duckdb::FlatVector::GetData<int64_t>(input.data[Q9LineitemChunkLayout::PARTKEY_INDEX]);
		auto *suppkeys = duckdb::FlatVector::GetData<int64_t>(input.data[Q9LineitemChunkLayout::SUPPKEY_INDEX]);
		auto *orderkeys = duckdb::FlatVector::GetData<int64_t>(input.data[Q9LineitemChunkLayout::ORDERKEY_INDEX]);
		auto *quantities = duckdb::FlatVector::GetData<int64_t>(input.data[Q9LineitemChunkLayout::QUANTITY_INDEX]);
		auto *extendedprices =
		    duckdb::FlatVector::GetData<int64_t>(input.data[Q9LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
		auto *discounts = duckdb::FlatVector::GetData<int64_t>(input.data[Q9LineitemChunkLayout::DISCOUNT_INDEX]);

		const auto probe_start = profile ? Clock::now() : Clock::time_point {};
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			ProbeLineitemRow(partkeys[row], suppkeys[row], orderkeys[row], quantities[row], extendedprices[row],
			                 discounts[row]);
		}
		if (profile) {
			profile->lineitem_probe_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - probe_start).count();
			profile->lineitem_flat_chunks += 1;
		}
	}

	void ConsumeLineitemGeneric(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q9LineitemChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q9LineitemChunkLayout::SUPPKEY_INDEX];
		auto &orderkey_vector = input.data[Q9LineitemChunkLayout::ORDERKEY_INDEX];
		auto &quantity_vector = input.data[Q9LineitemChunkLayout::QUANTITY_INDEX];
		auto &extendedprice_vector = input.data[Q9LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q9LineitemChunkLayout::DISCOUNT_INDEX];

		const auto format_start = profile ? Clock::now() : Clock::time_point {};
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat quantity_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		quantity_vector.ToUnifiedFormat(input.size(), quantity_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		if (profile) {
			profile->lineitem_format_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - format_start).count();
			profile->lineitem_generic_chunks += 1;
			if (partkey_format.sel == orderkey_format.sel && partkey_format.sel == suppkey_format.sel) {
				profile->shared_sel_chunks += 1;
			}
		}

		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

		const auto probe_start = profile ? Clock::now() : Clock::time_point {};
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto quantity_index = quantity_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);

			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				continue;
			}

			ProbeLineitemRow(partkeys[partkey_index], suppkeys[suppkey_index], orderkeys[orderkey_index],
			                 quantities[quantity_index], extendedprices[extendedprice_index],
			                 discounts[discount_index]);
		}
		if (profile) {
			profile->lineitem_probe_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - probe_start).count();
		}
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		if (profile) {
			profile->lineitem_rows += input.size();
			RecordLayout(&profile->layout_l_partkey, input.data[Q9LineitemChunkLayout::PARTKEY_INDEX]);
			RecordLayout(&profile->layout_l_suppkey, input.data[Q9LineitemChunkLayout::SUPPKEY_INDEX]);
			RecordLayout(&profile->layout_l_orderkey, input.data[Q9LineitemChunkLayout::ORDERKEY_INDEX]);
			RecordLayout(&profile->layout_l_quantity, input.data[Q9LineitemChunkLayout::QUANTITY_INDEX]);
			RecordLayout(&profile->layout_l_extendedprice, input.data[Q9LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
			RecordLayout(&profile->layout_l_discount, input.data[Q9LineitemChunkLayout::DISCOUNT_INDEX]);
		}
		if (selected_parts.Empty() || PartsuppEmpty()) {
			return;
		}

		if (config.flat_lineitem && IsFlatAllValid(input.data[Q9LineitemChunkLayout::PARTKEY_INDEX]) &&
		    IsFlatAllValid(input.data[Q9LineitemChunkLayout::SUPPKEY_INDEX]) &&
		    IsFlatAllValid(input.data[Q9LineitemChunkLayout::ORDERKEY_INDEX]) &&
		    IsFlatAllValid(input.data[Q9LineitemChunkLayout::QUANTITY_INDEX]) &&
		    IsFlatAllValid(input.data[Q9LineitemChunkLayout::EXTENDEDPRICE_INDEX]) &&
		    IsFlatAllValid(input.data[Q9LineitemChunkLayout::DISCOUNT_INDEX])) {
			ConsumeLineitemFlat(input);
			return;
		}
		ConsumeLineitemGeneric(input);
	}

	Q9Parameters params;
	Q9ProfileCounters *profile = nullptr;
	Q9RuntimeConfig config;
	std::string color_needle;

	PartMembership selected_parts;
	SupplierNationStore supplier_nations;
	std::unordered_map<int32_t, std::string> nation_names;
	OrderYearStore order_years;
	std::unordered_map<PartsuppKey, int64_t, PartsuppKeyHash> partsupp_costs;
	std::unordered_map<uint64_t, int64_t> packed_partsupp_costs;
	ProfitAggregator aggregates;

	std::vector<Q9OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ9Kernel(const Q9Parameters &params) {
	return duckdb::make_uniq<Q9Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ9ProfilingKernel(const Q9Parameters &params, Q9ProfileCounters &counters) {
	return duckdb::make_uniq<Q9Kernel>(params, &counters);
}

} // namespace accelerator_codegen
