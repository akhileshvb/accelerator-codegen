#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "tpch/common/dense_order_capacity.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "tpch/q9/duckdb/base/query.hpp"
#include "tpch/q9/duckdb/base/q9_fast.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
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
constexpr int64_t kSf10SupplierCapacity = 100001;
constexpr int16_t kAbsentYear = 0;
constexpr int32_t kAbsentNation = -1;
constexpr int32_t kNationCount = 25;
constexpr int64_t kYearBase = 1992;
constexpr int64_t kYearCount = 7;

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

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
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

uint64_t PackPartsuppKey(int64_t partkey, int64_t suppkey) {
	D_ASSERT(partkey >= 0 && partkey < (int64_t {1} << 32));
	D_ASSERT(suppkey >= 0 && suppkey < (int64_t {1} << 32));
	return (static_cast<uint64_t>(partkey) << 32) | static_cast<uint32_t>(suppkey);
}

class OrderYearStore {
public:
	OrderYearStore() {
		years.Reserve(kDefaultOrderCapacity);
	}

	void ReserveAtLeast(int64_t min_capacity) {
		years.Reserve(min_capacity);
	}

	void Insert(int64_t orderkey, int64_t year) {
		years.Insert(orderkey, year);
	}

	//! Concurrent-safe for distinct keys: touches only the dense byte of `orderkey` or the worker-local map.
	void InsertDenseNoGrow(int64_t orderkey, int64_t year,
	                       std::unordered_map<int64_t, int64_t> &local_overflow_map) {
		if (static_cast<uint64_t>(orderkey) < static_cast<uint64_t>(years.Capacity())) {
			const int64_t rel = year - (q9fast::YearCoder::kBase - 1);
			if (rel >= 1 && rel <= q9fast::YearCoder::kYears) {
				years.SetCode(orderkey, static_cast<uint8_t>(rel));
				return;
			}
			years.SetCode(orderkey, q9fast::OrderYears::kOther);
		}
		local_overflow_map[orderkey] = year;
	}

	void MergeOverflow(const std::unordered_map<int64_t, int64_t> &local_overflow_map) {
		for (const auto &entry : local_overflow_map) {
			Insert(entry.first, entry.second);
		}
	}

	bool Lookup(int64_t orderkey, int64_t &year_out) const {
		return years.Lookup(orderkey, year_out);
	}

	q9fast::OrderYears years;
};

class PartMembership {
public:
	PartMembership() {
		const auto words = static_cast<size_t>((kDefaultPartCapacity + 63) / 64);
		bits.assign(words, 0);
		bit_capacity = static_cast<int64_t>(words) * 64;
	}

	void ReserveAtLeast(int64_t min_capacity) {
		if (min_capacity <= bit_capacity) {
			return;
		}
		const auto words = static_cast<size_t>((min_capacity + 63) / 64);
		bits.resize(words, 0);
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

	void MergeFrom(const PartMembership &other) {
		if (other.bit_capacity > bit_capacity) {
			ReserveAtLeast(other.bit_capacity);
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

	const uint32_t *Words32() const {
		return reinterpret_cast<const uint32_t *>(bits.data());
	}

	uint64_t BitCapacityBits() const {
		return static_cast<uint64_t>(bits.size()) * 64;
	}

private:
	int64_t bit_capacity = 0;
	duckdb::idx_t count = 0;
	std::vector<uint64_t> bits;
	std::unordered_set<int64_t> overflow;
};

class SupplierNationStore {
public:
	SupplierNationStore() {
		dense.assign(static_cast<size_t>(kDefaultSupplierCapacity), kAbsentNation);
		capacity = kDefaultSupplierCapacity;
	}

	void ReserveAtLeast(int64_t min_capacity) {
		if (min_capacity <= capacity) {
			return;
		}
		dense.resize(static_cast<size_t>(min_capacity), kAbsentNation);
		capacity = min_capacity;
	}

	void Insert(int64_t suppkey, int32_t nationkey) {
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

	void InsertDenseNoGrow(int64_t suppkey, int32_t nationkey,
	                       std::unordered_map<int64_t, int32_t> &local_overflow_map) {
		if (suppkey >= 0 && suppkey < capacity) {
			dense[static_cast<size_t>(suppkey)] = nationkey;
			return;
		}
		local_overflow_map[suppkey] = nationkey;
	}

	void MergeOverflow(const std::unordered_map<int64_t, int32_t> &local_overflow_map) {
		for (const auto &entry : local_overflow_map) {
			Insert(entry.first, entry.second);
		}
	}

	bool Lookup(int64_t suppkey, int32_t &nationkey_out) const {
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

private:
	int64_t capacity = 0;
	std::vector<int32_t> dense;
	std::unordered_map<int64_t, int32_t> overflow;
};

class ProfitAggregator {
public:
	ProfitAggregator() {
		dense_acc.assign(static_cast<size_t>(kNationCount * kYearCount), 0);
		dense_present.assign(static_cast<size_t>(kNationCount * kYearCount), 0);
	}

	void Add(int32_t nationkey, int64_t year, int64_t extendedprice_raw, int64_t discount_raw, int64_t supplycost_raw,
	         int64_t quantity_raw) {
		if (nationkey >= 0 && nationkey < kNationCount && year >= kYearBase && year < kYearBase + kYearCount) {
			const auto idx = static_cast<size_t>(nationkey * kYearCount + (year - kYearBase));
			AddProfit(dense_acc[idx], extendedprice_raw, discount_raw, supplycost_raw, quantity_raw);
			dense_present[idx] = 1;
			return;
		}
		AddProfit(hash_acc[{nationkey, year}], extendedprice_raw, discount_raw, supplycost_raw, quantity_raw);
	}

	void MergeFrom(const ProfitAggregator &other) {
		for (size_t i = 0; i < dense_acc.size(); i++) {
			if (!other.dense_present[i]) {
				continue;
			}
#if defined(__SIZEOF_INT128__)
			dense_acc[i] += other.dense_acc[i];
#else
			if (!duckdb::Hugeint::TryAddInPlace(dense_acc[i], other.dense_acc[i])) {
				throw duckdb::OutOfRangeException("Overflow while merging Q9 profit");
			}
#endif
			dense_present[i] = 1;
		}
		for (const auto &entry : other.hash_acc) {
#if defined(__SIZEOF_INT128__)
			hash_acc[entry.first] += entry.second;
#else
			if (!duckdb::Hugeint::TryAddInPlace(hash_acc[entry.first], entry.second)) {
				throw duckdb::OutOfRangeException("Overflow while merging Q9 profit");
			}
#endif
		}
	}

	template <class Callback>
	void ForEach(Callback &&callback) const {
		for (int32_t nation = 0; nation < kNationCount; nation++) {
			for (int64_t year_offset = 0; year_offset < kYearCount; year_offset++) {
				const auto idx = static_cast<size_t>(nation * kYearCount + year_offset);
				if (!dense_present[idx]) {
					continue;
				}
				callback(nation, kYearBase + year_offset, dense_acc[idx]);
			}
		}
		for (const auto &entry : hash_acc) {
			callback(entry.first.nationkey, entry.first.year, entry.second);
		}
	}

private:
	std::vector<q9_acc_t> dense_acc;
	std::vector<uint8_t> dense_present;
	std::unordered_map<AggregateKey, q9_acc_t, AggregateKeyHash> hash_acc;
};

void DrainSource(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data,
                 const std::function<void(duckdb::DataChunk &)> &consume) {
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		consume(source->Chunk());
	}
}

class Q9ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q9ParallelKernel(Q9Parameters params_p) : params(std::move(params_p)), color_needle(params.color) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 6) {
			throw duckdb::InternalException("Q9 parallel expects 6 bound inputs");
		}
		for (const auto &input : bound_inputs) {
			if (!input) {
				throw duckdb::InternalException("Q9 parallel received null bound input");
			}
		}

		using clock = std::chrono::steady_clock;
		const auto worker_count = ParallelWorkerCount(context);
		if (const char *audit = std::getenv("ACCEL_PARALLEL_AUDIT"); audit && audit[0] == '1') {
			std::cerr << "ACCEL_PARALLEL_AUDIT q9 parallel_build workers=" << worker_count
			          << " phases=part,supplier,nation_seq,orders_fill,partsupp,lineitem,merge\n";
		}

		PartMembership selected_parts;
		SupplierNationStore supplier_nations;
		supplier_nations.ReserveAtLeast(kSf10SupplierCapacity); // SF10: 100k keys; cheap vs orders map
		std::unordered_map<int32_t, std::string> nation_names;
		OrderYearStore order_years;
		q9fast::FlatU64Map packed_partsupp_costs;
		const q9fast::YearCoder year_coder;

		int64_t part_ns = 0;
		int64_t supplier_ns = 0;
		int64_t nation_ns = 0;
		int64_t orders_ns = 0;
		int64_t partsupp_ns = 0;
		int64_t lineitem_ns = 0;
		int64_t merge_ns = 0;

		{
			const auto part_t0 = clock::now();
			std::vector<CacheLinePadded<PartMembership>> part_workers(worker_count);
			RunParallelChunkScan(context, *bound_inputs[0], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local_parts = part_workers[worker_id].value;
				                     duckdb::UnifiedVectorFormat partkey_format;
				                     duckdb::UnifiedVectorFormat name_format;
				                     input.data[Q9PartChunkLayout::PARTKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                  partkey_format);
				                     input.data[Q9PartChunkLayout::NAME_INDEX].ToUnifiedFormat(input.size(),
				                                                                               name_format);
				                     auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
				                     auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
				                     const duckdb::string_t color(color_needle);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto partkey_index = partkey_format.sel->get_index(row);
					                     const auto name_index = name_format.sel->get_index(row);
					                     if ((!partkey_format.validity.AllValid() &&
					                          !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
					                         (!name_format.validity.AllValid() &&
					                          !name_format.validity.RowIsValidUnsafe(name_index))) {
						                     continue;
					                     }
					                     if (!NameContainsColor(names[name_index], color)) {
						                     continue;
					                     }
					                     local_parts.Insert(partkeys[partkey_index]);
				                     }
			                     });
			for (const auto &w : part_workers) {
				selected_parts.MergeFrom(w.value);
			}
			part_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - part_t0).count();
		}

		{
			const auto supplier_t0 = clock::now();
			std::vector<CacheLinePadded<std::unordered_map<int64_t, int32_t>>> supplier_overflows(worker_count);
			RunParallelChunkScan(context, *bound_inputs[1], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local_overflow = supplier_overflows[worker_id].value;
				                     duckdb::UnifiedVectorFormat suppkey_format;
				                     duckdb::UnifiedVectorFormat nationkey_format;
				                     input.data[Q9SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                      suppkey_format);
				                     input.data[Q9SupplierChunkLayout::NATIONKEY_INDEX].ToUnifiedFormat(
				                         input.size(), nationkey_format);
				                     auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
				                     auto nationkeys =
				                         duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto suppkey_index = suppkey_format.sel->get_index(row);
					                     const auto nationkey_index = nationkey_format.sel->get_index(row);
					                     if ((!suppkey_format.validity.AllValid() &&
					                          !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
					                         (!nationkey_format.validity.AllValid() &&
					                          !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
						                     continue;
					                     }
					                     supplier_nations.InsertDenseNoGrow(suppkeys[suppkey_index],
					                                                        nationkeys[nationkey_index],
					                                                        local_overflow);
				                     }
			                     });
			for (const auto &w : supplier_overflows) {
				supplier_nations.MergeOverflow(w.value);
			}
			supplier_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - supplier_t0).count();
		}

		{
			const auto nation_t0 = clock::now();
			DrainSource(context, *bound_inputs[2], [&](duckdb::DataChunk &input) {
				duckdb::UnifiedVectorFormat nationkey_format;
				duckdb::UnifiedVectorFormat name_format;
				input.data[Q9NationChunkLayout::NATIONKEY_INDEX].ToUnifiedFormat(input.size(), nationkey_format);
				input.data[Q9NationChunkLayout::NAME_INDEX].ToUnifiedFormat(input.size(), name_format);
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
			});
			nation_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - nation_t0).count();
		}

		{
			const auto orders_t0 = clock::now();
			order_years.ReserveAtLeast(EstimateTpchDenseOrderCapacity(context, *bound_inputs[3]));

			std::vector<CacheLinePadded<std::unordered_map<int64_t, int64_t>>> order_overflows(worker_count);
			RunParallelChunkScan(context, *bound_inputs[3], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local_overflow = order_overflows[worker_id].value;
				                     if (IsFlatAllValid(input.data[Q9OrdersChunkLayout::ORDERKEY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9OrdersChunkLayout::ORDERDATE_INDEX])) {
					                     const auto *keys = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9OrdersChunkLayout::ORDERKEY_INDEX]);
					                     const auto *days = duckdb::FlatVector::GetData<int32_t>(
					                         input.data[Q9OrdersChunkLayout::ORDERDATE_INDEX]);
					                     const duckdb::idx_t total = input.size();
					                     const auto capacity = static_cast<uint64_t>(order_years.years.Capacity());
					                     for (duckdb::idx_t off = 0; off < total; off += q9fast::kBlock) {
						                     const auto cnt = std::min<duckdb::idx_t>(q9fast::kBlock, total - off);
						                     alignas(64) uint8_t codes[q9fast::kBlock + 64];
						                     year_coder.FillCodes(days + off, cnt, codes);
						                     for (duckdb::idx_t r = 0; r < cnt; r++) {
							                     const int64_t key = keys[off + r];
							                     const uint8_t code = codes[r];
							                     if (ACCEL_LIKELY(code != 255 && static_cast<uint64_t>(key) < capacity)) {
								                     order_years.years.SetCode(key, code);
							                     } else {
								                     order_years.InsertDenseNoGrow(
								                         key,
								                         static_cast<int64_t>(duckdb::Date::ExtractYear(
								                             duckdb::date_t(days[off + r]))),
								                         local_overflow);
							                     }
						                     }
					                     }
					                     return;
				                     }
				                     duckdb::UnifiedVectorFormat orderkey_format;
				                     duckdb::UnifiedVectorFormat orderdate_format;
				                     input.data[Q9OrdersChunkLayout::ORDERKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                     orderkey_format);
				                     input.data[Q9OrdersChunkLayout::ORDERDATE_INDEX].ToUnifiedFormat(
				                         input.size(), orderdate_format);
				                     auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
				                     auto orderdates =
				                         duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto orderkey_index = orderkey_format.sel->get_index(row);
					                     const auto orderdate_index = orderdate_format.sel->get_index(row);
					                     if ((!orderkey_format.validity.AllValid() &&
					                          !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
					                         (!orderdate_format.validity.AllValid() &&
					                          !orderdate_format.validity.RowIsValidUnsafe(orderdate_index))) {
						                     continue;
					                     }
					                     order_years.InsertDenseNoGrow(
					                         orderkeys[orderkey_index],
					                         static_cast<int64_t>(
					                             duckdb::Date::ExtractYear(orderdates[orderdate_index])),
					                         local_overflow);
				                     }
			                     });
			for (const auto &w : order_overflows) {
				order_years.MergeOverflow(w.value);
			}
			orders_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();
		}

		if (!selected_parts.Empty()) {
			const auto partsupp_t0 = clock::now();
			std::vector<CacheLinePadded<q9fast::FlatU64Map>> partsupp_workers(worker_count);
			RunParallelChunkScan(context, *bound_inputs[4], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &local_costs = partsupp_workers[worker_id].value;
				                     if (IsFlatAllValid(input.data[Q9PartsuppChunkLayout::PARTKEY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9PartsuppChunkLayout::SUPPKEY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9PartsuppChunkLayout::SUPPLYCOST_INDEX])) {
					                     const auto *pk = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9PartsuppChunkLayout::PARTKEY_INDEX]);
					                     const auto *sk = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9PartsuppChunkLayout::SUPPKEY_INDEX]);
					                     const auto *cost = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9PartsuppChunkLayout::SUPPLYCOST_INDEX]);
					                     const duckdb::idx_t total = input.size();
					                     for (duckdb::idx_t off = 0; off < total; off += q9fast::kBlock) {
						                     const auto cnt = std::min<duckdb::idx_t>(q9fast::kBlock, total - off);
						                     alignas(64) uint32_t sel[q9fast::kBlock + simd::kSelSlack];
						                     const auto m = q9fast::SelectInBitmap(
						                         pk + off, cnt, selected_parts.Words32(),
						                         selected_parts.BitCapacityBits(), sel,
						                         [&](int64_t key) { return selected_parts.Contains(key); });
						                     for (size_t j = 0; j < m; j++) {
							                     const auto r = off + sel[j];
							                     local_costs.Insert(PackPartsuppKey(pk[r], sk[r]), cost[r]);
						                     }
					                     }
					                     return;
				                     }
				                     duckdb::UnifiedVectorFormat partkey_format;
				                     duckdb::UnifiedVectorFormat suppkey_format;
				                     duckdb::UnifiedVectorFormat supplycost_format;
				                     input.data[Q9PartsuppChunkLayout::PARTKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                      partkey_format);
				                     input.data[Q9PartsuppChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                      suppkey_format);
				                     input.data[Q9PartsuppChunkLayout::SUPPLYCOST_INDEX].ToUnifiedFormat(
				                         input.size(), supplycost_format);
				                     auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
				                     auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
				                     auto supplycosts =
				                         duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto partkey_index = partkey_format.sel->get_index(row);
					                     const auto suppkey_index = suppkey_format.sel->get_index(row);
					                     const auto supplycost_index = supplycost_format.sel->get_index(row);
					                     if ((!partkey_format.validity.AllValid() &&
					                          !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
					                         (!suppkey_format.validity.AllValid() &&
					                          !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
					                         (!supplycost_format.validity.AllValid() &&
					                          !supplycost_format.validity.RowIsValidUnsafe(supplycost_index))) {
						                     continue;
					                     }
					                     const auto partkey = partkeys[partkey_index];
					                     if (!selected_parts.Contains(partkey)) {
						                     continue;
					                     }
					                     local_costs.Insert(PackPartsuppKey(partkey, suppkeys[suppkey_index]),
					                                        supplycosts[supplycost_index]);
				                     }
			                     });
			for (const auto &w : partsupp_workers) {
				packed_partsupp_costs.MergeFrom(w.value);
			}
			partsupp_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - partsupp_t0).count();
		}

		this->nation_names = std::move(nation_names);

		if (selected_parts.Empty() || packed_partsupp_costs.Empty()) {
			if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
				std::cerr << "q9_parallel_profile part_ns=" << part_ns << " supplier_ns=" << supplier_ns
				          << " nation_ns=" << nation_ns << " orders_ns=" << orders_ns
				          << " partsupp_ns=" << partsupp_ns << " lineitem_ns=" << lineitem_ns
				          << " merge_ns=" << merge_ns << " worker_count=" << worker_count << "\n";
			}
			return;
		}

		std::vector<CacheLinePadded<ProfitAggregator>> workers(worker_count);

		auto probe_row = [&](ProfitAggregator &agg, int64_t partkey, int64_t suppkey, int64_t orderkey,
		                     int64_t quantity_raw, int64_t extendedprice_raw, int64_t discount_raw) {
			if (!selected_parts.Contains(partkey)) {
				return;
			}
			int32_t nationkey = 0;
			if (!supplier_nations.Lookup(suppkey, nationkey)) {
				return;
			}
			int64_t year = 0;
			if (!order_years.Lookup(orderkey, year)) {
				return;
			}
			const int64_t *cost = packed_partsupp_costs.Find(PackPartsuppKey(partkey, suppkey));
			if (!cost) {
				return;
			}
			agg.Add(nationkey, year, extendedprice_raw, discount_raw, *cost, quantity_raw);
		};
		// Same probe, for rows already known to be in the selected-part set.
		auto probe_row_after_part = [&](ProfitAggregator &agg, int64_t partkey, int64_t suppkey, int64_t orderkey,
		                                int64_t quantity_raw, int64_t extendedprice_raw, int64_t discount_raw) {
			int32_t nationkey = 0;
			if (!supplier_nations.Lookup(suppkey, nationkey)) {
				return;
			}
			int64_t year = 0;
			if (!order_years.Lookup(orderkey, year)) {
				return;
			}
			const int64_t *cost = packed_partsupp_costs.Find(PackPartsuppKey(partkey, suppkey));
			if (!cost) {
				return;
			}
			agg.Add(nationkey, year, extendedprice_raw, discount_raw, *cost, quantity_raw);
		};

		{
			const auto lineitem_t0 = clock::now();
			RunParallelChunkScan(context, *bound_inputs[5], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &input) {
				                     auto &agg = workers[worker_id].value;
				                     if (IsFlatAllValid(input.data[Q9LineitemChunkLayout::PARTKEY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9LineitemChunkLayout::SUPPKEY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9LineitemChunkLayout::ORDERKEY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9LineitemChunkLayout::QUANTITY_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9LineitemChunkLayout::EXTENDEDPRICE_INDEX]) &&
				                         IsFlatAllValid(input.data[Q9LineitemChunkLayout::DISCOUNT_INDEX])) {
					                     auto *partkeys = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9LineitemChunkLayout::PARTKEY_INDEX]);
					                     auto *suppkeys = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9LineitemChunkLayout::SUPPKEY_INDEX]);
					                     auto *orderkeys = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9LineitemChunkLayout::ORDERKEY_INDEX]);
					                     auto *quantities = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9LineitemChunkLayout::QUANTITY_INDEX]);
					                     auto *extendedprices = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
					                     auto *discounts = duckdb::FlatVector::GetData<int64_t>(
					                         input.data[Q9LineitemChunkLayout::DISCOUNT_INDEX]);
					                     const duckdb::idx_t total = input.size();
					                     for (duckdb::idx_t off = 0; off < total; off += q9fast::kBlock) {
						                     const auto cnt = std::min<duckdb::idx_t>(q9fast::kBlock, total - off);
						                     alignas(64) uint32_t sel[q9fast::kBlock + simd::kSelSlack];
						                     const auto m = q9fast::SelectInBitmap(
						                         partkeys + off, cnt, selected_parts.Words32(),
						                         selected_parts.BitCapacityBits(), sel,
						                         [&](int64_t key) { return selected_parts.Contains(key); });
						                     for (size_t j = 0; j < m; j++) {
							                     const auto r = off + sel[j];
							                     packed_partsupp_costs.Prefetch(PackPartsuppKey(partkeys[r], suppkeys[r]));
							                     order_years.years.Prefetch(orderkeys[r]);
						                     }
						                     for (size_t j = 0; j < m; j++) {
							                     const auto r = off + sel[j];
							                     probe_row_after_part(agg, partkeys[r], suppkeys[r], orderkeys[r],
							                                          quantities[r], extendedprices[r], discounts[r]);
						                     }
					                     }
					                     return;
				                     }

				                     duckdb::UnifiedVectorFormat partkey_format;
				                     duckdb::UnifiedVectorFormat suppkey_format;
				                     duckdb::UnifiedVectorFormat orderkey_format;
				                     duckdb::UnifiedVectorFormat quantity_format;
				                     duckdb::UnifiedVectorFormat extendedprice_format;
				                     duckdb::UnifiedVectorFormat discount_format;
				                     input.data[Q9LineitemChunkLayout::PARTKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                      partkey_format);
				                     input.data[Q9LineitemChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(),
				                                                                                      suppkey_format);
				                     input.data[Q9LineitemChunkLayout::ORDERKEY_INDEX].ToUnifiedFormat(
				                         input.size(), orderkey_format);
				                     input.data[Q9LineitemChunkLayout::QUANTITY_INDEX].ToUnifiedFormat(
				                         input.size(), quantity_format);
				                     input.data[Q9LineitemChunkLayout::EXTENDEDPRICE_INDEX].ToUnifiedFormat(
				                         input.size(), extendedprice_format);
				                     input.data[Q9LineitemChunkLayout::DISCOUNT_INDEX].ToUnifiedFormat(
				                         input.size(), discount_format);
				                     auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
				                     auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
				                     auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
				                     auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
				                     auto extendedprices =
				                         duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
				                     auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
				                     for (duckdb::idx_t row = 0; row < input.size(); row++) {
					                     const auto partkey_index = partkey_format.sel->get_index(row);
					                     const auto suppkey_index = suppkey_format.sel->get_index(row);
					                     const auto orderkey_index = orderkey_format.sel->get_index(row);
					                     const auto quantity_index = quantity_format.sel->get_index(row);
					                     const auto extendedprice_index = extendedprice_format.sel->get_index(row);
					                     const auto discount_index = discount_format.sel->get_index(row);
					                     if ((!partkey_format.validity.AllValid() &&
					                          !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
					                         (!suppkey_format.validity.AllValid() &&
					                          !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
					                         (!orderkey_format.validity.AllValid() &&
					                          !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
					                         (!quantity_format.validity.AllValid() &&
					                          !quantity_format.validity.RowIsValidUnsafe(quantity_index)) ||
					                         (!extendedprice_format.validity.AllValid() &&
					                          !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
					                         (!discount_format.validity.AllValid() &&
					                          !discount_format.validity.RowIsValidUnsafe(discount_index))) {
						                     continue;
					                     }
					                     probe_row(agg, partkeys[partkey_index], suppkeys[suppkey_index],
					                               orderkeys[orderkey_index], quantities[quantity_index],
					                               extendedprices[extendedprice_index], discounts[discount_index]);
				                     }
			                     });
			lineitem_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();
		}

		{
			const auto merge_t0 = clock::now();
			for (const auto &w : workers) {
				aggregates.MergeFrom(w.value);
			}
			merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		}

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			std::cerr << "q9_parallel_profile part_ns=" << part_ns << " supplier_ns=" << supplier_ns
			          << " nation_ns=" << nation_ns << " orders_ns=" << orders_ns << " partsupp_ns=" << partsupp_ns
			          << " lineitem_ns=" << lineitem_ns << " merge_ns=" << merge_ns
			          << " worker_count=" << worker_count << "\n";
		}
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
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
	Q9Parameters params;
	std::string color_needle;
	std::unordered_map<int32_t, std::string> nation_names;
	ProfitAggregator aggregates;
	std::vector<Q9OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ9ParallelKernel(const Q9Parameters &params) {
	return duckdb::make_uniq<Q9ParallelKernel>(params);
}

} // namespace accelerator_codegen
