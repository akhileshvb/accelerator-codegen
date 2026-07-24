#include "accelerator_codegen/tpch_q8.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/filter/bloom_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_reorderer.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int32_t kAbsentNation = -1;
constexpr int16_t kAbsentYear = -1;
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

struct Q8RuntimeConfig {
	bool lineitem_flat_fast = true;
	bool lineitem_dict_fast = true;
};

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

template <typename ValueT, ValueT AbsentValue>
class DenseKeyStore {
public:
	explicit DenseKeyStore(int64_t default_capacity_p) : default_capacity(default_capacity_p) {
	}

	void WarmInitialize() {
		if (initialized) {
			return;
		}
		values.assign(static_cast<size_t>(default_capacity), AbsentValue);
		capacity = default_capacity;
		initialized = true;
	}

	void Store(int64_t key, ValueT value) {
		if (key < 0) {
			overflow[key] = value;
			return;
		}
		if (key >= capacity) {
			if (key > 1'000'000'000) {
				overflow[key] = value;
				return;
			}
			EnsureCapacity(key + 1);
		}
		values[static_cast<size_t>(key)] = value;
	}

	ValueT Load(int64_t key) const {
		if (key < 0) {
			const auto it = overflow.find(key);
			return it == overflow.end() ? AbsentValue : it->second;
		}
		if (key >= 0 && key < capacity) {
			return values[static_cast<size_t>(key)];
		}
		const auto it = overflow.find(key);
		return it == overflow.end() ? AbsentValue : it->second;
	}

	inline ValueT LoadNonNegUnchecked(uint64_t key) const {
		return values[key];
	}

	inline void StoreNonNegUnchecked(uint64_t key, ValueT value) {
		values[key] = value;
	}

	bool CanProbeUnchecked(int64_t key) const {
		return key >= 0 && key < capacity;
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
		values.resize(static_cast<size_t>(target), AbsentValue);
		capacity = target;
	}

	int64_t default_capacity;
	bool initialized = false;
	int64_t capacity = 0;
	std::vector<ValueT> values;
	std::unordered_map<int64_t, ValueT> overflow;
};

class OrderKeyBitmap {
public:
	bool Contains(int64_t orderkey) const {
		if (orderkey < 0) {
			return false;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		if (probe_unchecked) {
			return ContainsUnchecked(key);
		}
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			return false;
		}
		const auto mask = uint64_t {1} << (key & 63);
		return (words[word_index] & mask) != 0;
	}

	void Insert(int64_t orderkey) {
		if (orderkey < 0) {
			return;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			EnsureCapacity(key);
		}
		words[word_index] |= uint64_t {1} << (key & 63);
	}

	void EnableUncheckedProbe() {
		EnsureCapacity(6000000);
		probe_unchecked = true;
	}

private:
	bool ContainsUnchecked(uint64_t key) const {
		const auto mask = uint64_t {1} << (key & 63);
		return (words[key >> 6] & mask) != 0;
	}

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
	bool probe_unchecked = false;
};

class CustomerKeyBitmap {
public:
	bool Contains(int64_t custkey) const {
		if (custkey < 0) {
			return false;
		}
		const auto key = static_cast<uint64_t>(custkey);
		if (probe_unchecked) {
			return ContainsUnchecked(key);
		}
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			return false;
		}
		const auto mask = uint64_t {1} << (key & 63);
		return (words[word_index] & mask) != 0;
	}

	void Insert(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		const auto key = static_cast<uint64_t>(custkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			EnsureCapacity(key);
		}
		words[word_index] |= uint64_t {1} << (key & 63);
	}

	void EnableUncheckedProbe() {
		EnsureCapacity(150000);
		probe_unchecked = true;
	}

private:
	bool ContainsUnchecked(uint64_t key) const {
		const auto mask = uint64_t {1} << (key & 63);
		return (words[key >> 6] & mask) != 0;
	}

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
	bool probe_unchecked = false;
};

class OrderYearSlotStore {
public:
	explicit OrderYearSlotStore(int64_t default_capacity_p) : default_capacity(default_capacity_p) {
	}

	void WarmInitialize() {
		if (initialized) {
			return;
		}
		if (capacity == 0) {
			slots.assign(static_cast<size_t>(default_capacity), static_cast<int8_t>(-1));
			capacity = default_capacity;
		}
		initialized = true;
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

	inline int8_t LoadSlotUnchecked(uint64_t orderkey) const {
		return slots[orderkey];
	}

	bool ProbeReady() const {
		return initialized;
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
		slots.resize(static_cast<size_t>(target), static_cast<int8_t>(-1));
		capacity = target;
	}

	int64_t default_capacity;
	bool initialized = false;
	int64_t capacity = 0;
	std::vector<int8_t> slots;
	std::unordered_map<int64_t, int8_t> overflow;
};

bool TryGetRowGroupNumericMinMax(const duckdb::RowGroup &row_group, const duckdb::StorageIndex &column_id,
                                 int64_t &out_min, int64_t &out_max) {
	auto stats = row_group.GetStatistics(column_id);
	if (!stats || !duckdb::NumericStats::HasMinMax(*stats)) {
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

struct Q8LineitemProfile {
	uint64_t row_groups_total = 0;
	uint64_t row_groups_skipped_semi = 0;
	uint64_t row_groups_stopped_early = 0;
	uint64_t vectors_seen = 0;
	uint64_t vectors_order_empty = 0;
	uint64_t order_probe_rows_in = 0;
	uint64_t order_probe_rows_out = 0;
	uint64_t payload_rows = 0;
};

bool Q8ProfileEnabled() {
	const char *env = std::getenv("ACCEL_Q8_PROFILE");
	return env != nullptr && env[0] == '1';
}

void WriteLineitemProfile(const Q8LineitemProfile &profile) {
	fprintf(stderr,
	        "Q8 lineitem profile: row_groups=%llu skipped_semi=%llu stopped_early=%llu vectors=%llu "
	        "vectors_order_empty=%llu order_probe_in=%llu order_probe_out=%llu payload_rows=%llu\n",
	        static_cast<unsigned long long>(profile.row_groups_total),
	        static_cast<unsigned long long>(profile.row_groups_skipped_semi),
	        static_cast<unsigned long long>(profile.row_groups_stopped_early),
	        static_cast<unsigned long long>(profile.vectors_seen),
	        static_cast<unsigned long long>(profile.vectors_order_empty),
	        static_cast<unsigned long long>(profile.order_probe_rows_in),
	        static_cast<unsigned long long>(profile.order_probe_rows_out),
	        static_cast<unsigned long long>(profile.payload_rows));
	fflush(stderr);
	const char *profile_path = std::getenv("ACCEL_Q8_PROFILE_PATH");
	if (profile_path == nullptr || profile_path[0] == '\0') {
		profile_path = "accel_q8_lineitem_profile.txt";
	}
	FILE *profile_out = std::fopen(profile_path, "w");
	if (profile_out != nullptr) {
		fprintf(profile_out,
		        "row_groups=%llu skipped_semi=%llu stopped_early=%llu vectors=%llu "
		        "vectors_order_empty=%llu order_probe_in=%llu order_probe_out=%llu payload_rows=%llu\n",
		        static_cast<unsigned long long>(profile.row_groups_total),
		        static_cast<unsigned long long>(profile.row_groups_skipped_semi),
		        static_cast<unsigned long long>(profile.row_groups_stopped_early),
		        static_cast<unsigned long long>(profile.vectors_seen),
		        static_cast<unsigned long long>(profile.vectors_order_empty),
		        static_cast<unsigned long long>(profile.order_probe_rows_in),
		        static_cast<unsigned long long>(profile.order_probe_rows_out),
		        static_cast<unsigned long long>(profile.payload_rows));
		std::fclose(profile_out);
	}
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

private:
	void EnsureCapacity(int64_t partkey) {
		const auto required = static_cast<size_t>(partkey) + 1;
		if (required <= flags.size()) {
			return;
		}
		size_t target = required;
		if (!flags.empty()) {
			const size_t doubled = flags.size() > (std::numeric_limits<size_t>::max() / 2) ? std::numeric_limits<size_t>::max()
			                                                                                 : flags.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		flags.resize(target, 0);
	}

	std::vector<uint8_t> flags;
};

struct LineitemSurvivor {
	duckdb::idx_t vector_index = 0;
	int64_t orderkey = 0;
	int year_slot = -1;
};

class Q8Kernel final : public KernelInstance {
public:
	explicit Q8Kernel(Q8Parameters params_p)
	    : params(std::move(params_p)), order_year_slots(kDefaultOrderCapacity), supplier_nations(kDefaultSupplierCapacity) {
		supplier_nations.WarmInitialize();
		for (const auto partkey : params.bind_part_keys) {
			InsertQualifyingPart(partkey);
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q8InputId>(input_id)) {
		case Q8InputId::PART:
			ConsumePart(input);
			return;
		case Q8InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		case Q8InputId::CUSTOMER:
			if (fused_customer_scanned) {
				throw duckdb::InternalException("Q8 customer already consumed via fused scan-consumer");
			}
			ConsumeCustomer(input);
			return;
		case Q8InputId::ORDERS:
			if (fused_orders_scanned) {
				throw duckdb::InternalException("Q8 orders already consumed via fused scan-consumer");
			}
			ConsumeOrders(input);
			return;
		case Q8InputId::LINEITEM:
			if (fused_lineitem_scanned) {
				throw duckdb::InternalException("Q8 lineitem already consumed via fused scan-consumer");
			}
			ConsumeLineitem(input);
			return;
		default:
			throw duckdb::InternalException("Q8 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		for (int slot = 0; slot < kYearSlotCount; slot++) {
			const auto &state = volume_by_year[static_cast<size_t>(slot)];
			if (state.total_volume == 0) {
				continue;
			}
			Q8OutputRow row;
			row.year = kMinYear + slot;
			row.mkt_share = ComputeMarketShare(state);
			output_rows.push_back(std::move(row));
		}
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.year));
			output.SetValue(1, row, duckdb::Value::DOUBLE(entry.mkt_share));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

	void RunFusedCustomerScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_customer_scanned) {
			return;
		}
		if (!has_qualifying_region_nations()) {
			fused_customer_scanned = true;
			return;
		}

		static constexpr duckdb::idx_t kScanIndices[] = {kCustomerCustkeyScanIndex, kCustomerNationkeyScanIndex};

		auto consumer = [this](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                       duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			const auto &column_ids = scan_state.GetColumnIds();
			if (column_ids.size() != kCustomerProjectedColumnCount) {
				throw duckdb::InternalException("Q8 fused customer expected %llu projected columns, got %llu",
				                                static_cast<unsigned long long>(kCustomerProjectedColumnCount),
				                                static_cast<unsigned long long>(column_ids.size()));
			}

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
					const auto vector_index = identity ? row : sel.get_index(row);
					const auto nationkey = nationkeys[vector_index];
					if (nationkey < 0 || nationkey >= kMaxNationSlots ||
					    !params.region_nations[static_cast<size_t>(nationkey)]) {
						continue;
					}
					qualifying_customers.Insert(custkeys[vector_index]);
				}
				return duckdb::FilteredScanConsumerResult::CONTINUE;
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
				    (!nationkey_format.validity.AllValid() &&
				     !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
					continue;
				}
				const auto nationkey = nationkeys[nationkey_index];
				if (nationkey < 0 || nationkey >= kMaxNationSlots ||
				    !params.region_nations[static_cast<size_t>(nationkey)]) {
					continue;
				}
				qualifying_customers.Insert(custkeys[custkey_index]);
			}
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		};

		RunNativeFilteredScanConsumer(context, bind_data, consumer);
		qualifying_customers.EnableUncheckedProbe();
		qualifying_customers_probe_ready = true;
		fused_customer_scanned = true;
	}

	void RunFusedOrdersScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_orders_scanned) {
			return;
		}
		if (!has_qualifying_region_nations()) {
			fused_orders_scanned = true;
			return;
		}
		if (!qualifying_customers_probe_ready) {
			qualifying_customers.EnableUncheckedProbe();
			qualifying_customers_probe_ready = true;
		}
		order_year_slots.WarmInitialize();

		static constexpr duckdb::idx_t kKeyScanIndices[] = {kOrdersOrderkeyScanIndex, kOrdersCustkeyScanIndex};
		static constexpr duckdb::idx_t kPayloadScanIndices[] = {kOrdersOrderdateScanIndex};

		struct OrderSurvivor {
			duckdb::idx_t vector_index = 0;
			int64_t orderkey = 0;
		};

		auto consumer = [this](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                       duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			const auto &column_ids = scan_state.GetColumnIds();
			if (column_ids.size() != kOrdersProjectedColumnCount) {
				throw duckdb::InternalException("Q8 fused orders expected %llu projected columns, got %llu",
				                                static_cast<unsigned long long>(kOrdersProjectedColumnCount),
				                                static_cast<unsigned long long>(column_ids.size()));
			}

			duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
			duckdb::Vector custkey_vec(duckdb::LogicalType::BIGINT);
			duckdb::Vector *key_payload[] = {&orderkey_vec, &custkey_vec};
			const auto scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kKeyScanIndices, 2,
			                                                       key_payload, duckdb::UpdateScanType::STANDARD);

			OrderSurvivor survivors[STANDARD_VECTOR_SIZE];
			duckdb::idx_t match_count = 0;
			const bool identity = sel.data() == nullptr;
			if (IsFlatAllValid(orderkey_vec) && IsFlatAllValid(custkey_vec) && scan_count >= count) {
				auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
				auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vec);
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto vector_index = identity ? row : sel.get_index(row);
					if (!qualifying_customers.Contains(custkeys[vector_index])) {
						continue;
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
					if (!qualifying_customers.Contains(custkeys[custkey_index])) {
						continue;
					}
					survivors[match_count++] = OrderSurvivor {base_index, orderkeys[orderkey_index]};
				}
			}

			if (match_count == 0) {
				row_group.SkipProjectedColumn(scan_state, kOrdersOrderdateScanIndex);
				return duckdb::FilteredScanConsumerResult::CONTINUE;
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
				for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
					const auto year = static_cast<int16_t>(duckdb::Date::ExtractYear(orderdates[match_index]));
					if (year < kMinYear || year > kMaxYear) {
						continue;
					}
					const int slot = static_cast<int>(year - kMinYear);
					InsertQualifyingOrder(survivors[match_index].orderkey);
					order_year_slots.StoreSlot(survivors[match_index].orderkey, static_cast<int8_t>(slot));
				}
				return duckdb::FilteredScanConsumerResult::CONTINUE;
			}

			duckdb::UnifiedVectorFormat orderdate_format;
			orderdate_vec.ToUnifiedFormat(match_count, orderdate_format);
			auto orderdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(orderdate_format);
			for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
				const auto orderdate_index = orderdate_format.sel->get_index(match_index);
				if (!orderdate_format.validity.AllValid() &&
				    !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) {
					continue;
				}
				const auto year =
				    static_cast<int16_t>(duckdb::Date::ExtractYear(duckdb::date_t(orderdates[orderdate_index])));
				if (year < kMinYear || year > kMaxYear) {
					continue;
				}
				const int slot = static_cast<int>(year - kMinYear);
				InsertQualifyingOrder(survivors[match_index].orderkey);
				order_year_slots.StoreSlot(survivors[match_index].orderkey, static_cast<int8_t>(slot));
			}
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		};

		RunNativeFilteredScanConsumer(context, bind_data, consumer);
		PrepareOrderProbeState();
		fused_orders_scanned = true;
	}

	void InsertQualifyingOrder(int64_t orderkey) {
		qualifying_orders.Insert(orderkey);
		qualifying_order_keys.push_back(orderkey);
	}

	void InsertQualifyingPart(int64_t partkey) {
		qualifying_parts.Insert(partkey);
		qualifying_part_keys.push_back(partkey);
	}

	void PrepareLineitemSemiJoinState() {
		if (semijoin_keys_ready) {
			return;
		}
		std::sort(qualifying_order_keys.begin(), qualifying_order_keys.end());
		qualifying_order_keys.erase(std::unique(qualifying_order_keys.begin(), qualifying_order_keys.end()),
		                            qualifying_order_keys.end());
		std::sort(qualifying_part_keys.begin(), qualifying_part_keys.end());
		qualifying_part_keys.erase(std::unique(qualifying_part_keys.begin(), qualifying_part_keys.end()),
		                           qualifying_part_keys.end());
		if (!qualifying_order_keys.empty()) {
			qualifying_orders_min = qualifying_order_keys.front();
			qualifying_orders_max = qualifying_order_keys.back();
		}
		if (!qualifying_part_keys.empty()) {
			qualifying_parts_min = qualifying_part_keys.front();
			qualifying_parts_max = qualifying_part_keys.back();
		}
		semijoin_keys_ready = true;
	}

	void SkipLineitemPayloadColumns(duckdb::RowGroup &row_group, duckdb::CollectionScanState &scan_state) const {
		row_group.SkipProjectedColumn(scan_state, kLineitemSuppkeyScanIndex);
		row_group.SkipProjectedColumn(scan_state, kLineitemExtendedpriceScanIndex);
		row_group.SkipProjectedColumn(scan_state, kLineitemDiscountScanIndex);
	}

	duckdb::idx_t CollectOrderProbeSurvivorsCompact(duckdb::idx_t count, int64_t *orderkeys, LineitemSurvivor *survivors) const {
		duckdb::idx_t match_count = 0;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto orderkey = orderkeys[row];
			if (!qualifying_orders.Contains(orderkey)) {
				continue;
			}
			const int slot = order_year_slots_probe_ready
			                     ? static_cast<int>(order_year_slots.LoadSlotUnchecked(static_cast<uint64_t>(orderkey)))
			                     : static_cast<int>(order_year_slots.LoadSlot(orderkey));
			if (slot < 0 || slot >= kYearSlotCount) {
				continue;
			}
			survivors[match_count++] = LineitemSurvivor {row, orderkey, slot};
		}
		return match_count;
	}

	duckdb::idx_t CollectOrderProbeSurvivorsCompactUnified(duckdb::idx_t count, duckdb::UnifiedVectorFormat &orderkey_format,
	                                                       LineitemSurvivor *survivors) const {
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		duckdb::idx_t match_count = 0;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			if (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			if (!qualifying_orders.Contains(orderkey)) {
				continue;
			}
			const int slot = order_year_slots_probe_ready
			                     ? static_cast<int>(order_year_slots.LoadSlotUnchecked(static_cast<uint64_t>(orderkey)))
			                     : static_cast<int>(order_year_slots.LoadSlot(orderkey));
			if (slot < 0 || slot >= kYearSlotCount) {
				continue;
			}
			survivors[match_count++] = LineitemSurvivor {row, orderkey, slot};
		}
		return match_count;
	}

	duckdb::FilteredScanConsumerResult ConsumeFusedLineitemVectorFull(duckdb::TransactionData transaction,
	                                                                duckdb::CollectionScanState &scan_state,
	                                                                duckdb::RowGroup &row_group,
	                                                                duckdb::SelectionVector &sel,
	                                                                duckdb::idx_t count) {
		if (count == 0) {
			row_group.SkipProjectedColumn(scan_state, kLineitemPartkeyScanIndex);
			row_group.SkipProjectedColumn(scan_state, kLineitemOrderkeyScanIndex);
			SkipLineitemPayloadColumns(row_group, scan_state);
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		}

		duckdb::Vector partkey_vec(duckdb::LogicalType::BIGINT);
		duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
		duckdb::Vector suppkey_vec(duckdb::LogicalType::BIGINT);
		duckdb::Vector extendedprice_vec(duckdb::LogicalType::DECIMAL(15, 2));
		duckdb::Vector discount_vec(duckdb::LogicalType::DECIMAL(15, 2));
		row_group.SelectProjectedColumn(transaction, scan_state, kLineitemPartkeyScanIndex, sel, count, partkey_vec,
		                                duckdb::UpdateScanType::STANDARD);
		row_group.SelectProjectedColumn(transaction, scan_state, kLineitemOrderkeyScanIndex, sel, count, orderkey_vec,
		                                duckdb::UpdateScanType::STANDARD);
		row_group.SelectProjectedColumn(transaction, scan_state, kLineitemSuppkeyScanIndex, sel, count, suppkey_vec,
		                                duckdb::UpdateScanType::STANDARD);
		row_group.SelectProjectedColumn(transaction, scan_state, kLineitemExtendedpriceScanIndex, sel, count,
		                                extendedprice_vec, duckdb::UpdateScanType::STANDARD);
		row_group.SelectProjectedColumn(transaction, scan_state, kLineitemDiscountScanIndex, sel, count, discount_vec,
		                                duckdb::UpdateScanType::STANDARD);

		if (IsFlatAllValid(partkey_vec) && IsFlatAllValid(suppkey_vec) && IsFlatAllValid(orderkey_vec) &&
		    IsFlatAllValid(extendedprice_vec) && IsFlatAllValid(discount_vec)) {
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vec);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vec);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
			for (duckdb::idx_t row = 0; row < count; row++) {
				(void)ProcessLineitemRow(partkeys[row], suppkeys[row], orderkeys[row], extendedprices[row],
				                         discounts[row]);
			}
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		partkey_vec.ToUnifiedFormat(count, partkey_format);
		suppkey_vec.ToUnifiedFormat(count, suppkey_format);
		orderkey_vec.ToUnifiedFormat(count, orderkey_format);
		extendedprice_vec.ToUnifiedFormat(count, extendedprice_format);
		discount_vec.ToUnifiedFormat(count, discount_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				continue;
			}
			(void)ProcessLineitemRow(partkeys[partkey_index], suppkeys[suppkey_index], orderkeys[orderkey_index],
			                         extendedprices[extendedprice_index], discounts[discount_index]);
		}
		return duckdb::FilteredScanConsumerResult::CONTINUE;
	}

	duckdb::idx_t CollectPartSurvivorSelection(duckdb::idx_t count, const int64_t *partkeys,
	                                           duckdb::SelectionVector &part_sel) const {
		duckdb::idx_t part_count = 0;
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (!qualifying_parts.Contains(partkeys[row])) {
				continue;
			}
			part_sel.set_index(part_count++, row);
		}
		return part_count;
	}

	duckdb::idx_t CollectPartSurvivorSelectionUnified(duckdb::idx_t count, duckdb::UnifiedVectorFormat &partkey_format,
	                                                duckdb::SelectionVector &part_sel) const {
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		duckdb::idx_t part_count = 0;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			if (!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) {
				continue;
			}
			if (!qualifying_parts.Contains(partkeys[partkey_index])) {
				continue;
			}
			part_sel.set_index(part_count++, row);
		}
		return part_count;
	}

	duckdb::FilteredScanConsumerResult ConsumeFusedLineitemVectorOrderProbe(duckdb::TransactionData transaction,
	                                                                      duckdb::CollectionScanState &scan_state,
	                                                                      duckdb::RowGroup &row_group,
	                                                                      duckdb::SelectionVector &sel,
	                                                                      duckdb::idx_t count, bool part_filter_pushed) {
		(void)part_filter_pushed;
		if (count == 0) {
			row_group.SkipProjectedColumn(scan_state, kLineitemPartkeyScanIndex);
			row_group.SkipProjectedColumn(scan_state, kLineitemOrderkeyScanIndex);
			SkipLineitemPayloadColumns(row_group, scan_state);
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		}

		if (lineitem_profile_enabled) {
			lineitem_profile.vectors_seen += 1;
			lineitem_profile.order_probe_rows_in += count;
		}

		duckdb::Vector partkey_vec(duckdb::LogicalType::BIGINT);
		row_group.SelectProjectedColumn(transaction, scan_state, kLineitemPartkeyScanIndex, sel, count, partkey_vec,
		                                duckdb::UpdateScanType::STANDARD);

		duckdb::SelectionVector part_sel(count);
		duckdb::idx_t part_count = 0;
		if (IsFlatAllValid(partkey_vec)) {
			part_count = CollectPartSurvivorSelection(count, duckdb::FlatVector::GetData<int64_t>(partkey_vec), part_sel);
		} else {
			duckdb::UnifiedVectorFormat partkey_format;
			partkey_vec.ToUnifiedFormat(count, partkey_format);
			part_count = CollectPartSurvivorSelectionUnified(count, partkey_format, part_sel);
		}
		if (part_count == 0) {
			if (lineitem_profile_enabled) {
				lineitem_profile.vectors_order_empty += 1;
			}
			row_group.SkipProjectedColumn(scan_state, kLineitemOrderkeyScanIndex);
			SkipLineitemPayloadColumns(row_group, scan_state);
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		}

		duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
		row_group.SelectProjectedColumn(transaction, scan_state, kLineitemOrderkeyScanIndex, part_sel, part_count,
		                                orderkey_vec, duckdb::UpdateScanType::STANDARD);

		LineitemSurvivor survivors[STANDARD_VECTOR_SIZE];
		duckdb::idx_t match_count = 0;
		if (IsFlatAllValid(orderkey_vec) && part_count > 0) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
			match_count = CollectOrderProbeSurvivorsCompact(part_count, orderkeys, survivors);
		} else {
			duckdb::UnifiedVectorFormat orderkey_format;
			orderkey_vec.ToUnifiedFormat(part_count, orderkey_format);
			match_count = CollectOrderProbeSurvivorsCompactUnified(part_count, orderkey_format, survivors);
		}

		if (match_count == 0) {
			if (lineitem_profile_enabled) {
				lineitem_profile.vectors_order_empty += 1;
			}
			SkipLineitemPayloadColumns(row_group, scan_state);
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		}

		if (lineitem_profile_enabled) {
			lineitem_profile.order_probe_rows_out += match_count;
			lineitem_profile.payload_rows += match_count;
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

		for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
			survivors[match_index].vector_index = match_index;
		}
		AccumulateFusedLineitemPayloads(match_count, match_count, survivors, suppkey_vec, extendedprice_vec, discount_vec);
		return duckdb::FilteredScanConsumerResult::CONTINUE;
	}

	void EnsurePartKeysSeeded() {
		if (!params.bind_part_keys.empty() && qualifying_part_keys.empty()) {
			for (const auto partkey : params.bind_part_keys) {
				InsertQualifyingPart(partkey);
			}
		}
	}

	void BuildOrderkeyBloomFilter(duckdb::ClientContext &context) {
		if (qualifying_order_keys.empty()) {
			return;
		}
		orderkey_bloom_filter.Initialize(context, qualifying_order_keys.size());
		duckdb::Vector orderkeys(duckdb::LogicalType::BIGINT, qualifying_order_keys.size());
		orderkeys.SetVectorType(duckdb::VectorType::FLAT_VECTOR);
		auto orderkey_data = duckdb::FlatVector::GetData<int64_t>(orderkeys);
		for (duckdb::idx_t index = 0; index < qualifying_order_keys.size(); index++) {
			orderkey_data[index] = qualifying_order_keys[index];
		}
		duckdb::Vector hashes(duckdb::LogicalType::HASH, qualifying_order_keys.size());
		duckdb::VectorOperations::Hash(orderkeys, hashes, qualifying_order_keys.size());
		orderkey_bloom_filter.InsertHashes(hashes, qualifying_order_keys.size());
		orderkey_bloom_ready = true;
	}

	void RunFusedLineitemScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_lineitem_scanned) {
			return;
		}
		lineitem_profile_enabled = Q8ProfileEnabled();
		if (!has_qualifying_region_nations()) {
			fused_lineitem_scanned = true;
			return;
		}
		EnsurePartKeysSeeded();
		PrepareOrderProbeState();
		PrepareLineitemSemiJoinState();
		if (qualifying_order_keys.empty() || qualifying_part_keys.empty()) {
			fused_lineitem_scanned = true;
			return;
		}

		const auto orderkey_storage = GetNativeScanColumnStorageIndex(bind_data, "l_orderkey");

		NativeScanConsumerOptions scan_options;
		scan_options.row_group_order = duckdb::make_uniq<duckdb::RowGroupOrderOptions>(
		    orderkey_storage, duckdb::OrderByStatistics::MIN, duckdb::OrderType::ASCENDING,
		    duckdb::OrderByNullType::NULLS_LAST, duckdb::OrderByColumnType::NUMERIC);

		duckdb::RowGroup *active_row_group = nullptr;
		bool active_row_group_skipped = false;

		auto consumer = [this, &active_row_group, &active_row_group_skipped](
		                    duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                    duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                    duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			const auto &column_ids = scan_state.GetColumnIds();
			if (column_ids.size() != kLineitemProjectedColumnCount) {
				throw duckdb::InternalException("Q8 fused lineitem expected %llu projected columns, got %llu",
				                                static_cast<unsigned long long>(kLineitemProjectedColumnCount),
				                                static_cast<unsigned long long>(column_ids.size()));
			}

			if (&row_group != active_row_group) {
				if (lineitem_profile_enabled) {
					lineitem_profile.row_groups_total += 1;
				}
				active_row_group = &row_group;
				active_row_group_skipped = false;

				int64_t order_min = 0;
				int64_t order_max = 0;
				const bool has_order_stats =
				    TryGetRowGroupNumericMinMax(row_group, column_ids[kLineitemOrderkeyScanIndex], order_min, order_max);
				if (has_order_stats && order_min > qualifying_orders_max) {
					if (lineitem_profile_enabled) {
						lineitem_profile.row_groups_stopped_early += 1;
					}
					return duckdb::FilteredScanConsumerResult::STOP;
				}
				if (has_order_stats && !SortedKeysOverlapRange(qualifying_order_keys, order_min, order_max)) {
						active_row_group_skipped = true;
					}
				if (!active_row_group_skipped) {
					int64_t part_min = 0;
					int64_t part_max = 0;
					if (TryGetRowGroupNumericMinMax(row_group, column_ids[kLineitemPartkeyScanIndex], part_min, part_max) &&
					    !SortedKeysOverlapRange(qualifying_part_keys, part_min, part_max)) {
						active_row_group_skipped = true;
					}
				}
				if (active_row_group_skipped && lineitem_profile_enabled) {
					lineitem_profile.row_groups_skipped_semi += 1;
				}
			}
			if (active_row_group_skipped) {
				row_group.SkipProjectedColumn(scan_state, kLineitemPartkeyScanIndex);
				row_group.SkipProjectedColumn(scan_state, kLineitemOrderkeyScanIndex);
				SkipLineitemPayloadColumns(row_group, scan_state);
				return duckdb::FilteredScanConsumerResult::CONTINUE;
			}

			return ConsumeFusedLineitemVectorOrderProbe(transaction, scan_state, row_group, sel, count, false);
		};

		RunNativeFilteredScanConsumer(context, bind_data, consumer, &scan_options);
		if (lineitem_profile_enabled) {
			WriteLineitemProfile(lineitem_profile);
		}
		fused_lineitem_scanned = true;
	}

private:
	void PrepareOrderProbeState() {
		if (!qualifying_orders_probe_ready) {
			qualifying_orders.EnableUncheckedProbe();
			qualifying_orders_probe_ready = true;
		}
		if (!order_year_slots.ProbeReady()) {
			order_year_slots.WarmInitialize();
		}
		order_year_slots_probe_ready = order_year_slots.ProbeReady();
	}

	bool ProcessLineitemRow(int64_t partkey, int64_t suppkey, int64_t orderkey, int64_t extendedprice_raw,
	                        int64_t discount_raw) {
		if (!qualifying_parts.Contains(partkey)) {
			return false;
		}
		if (!qualifying_orders.Contains(orderkey)) {
			return false;
		}
		return AccumulateLineitemPayload(suppkey, orderkey, extendedprice_raw, discount_raw);
	}

	bool AccumulateLineitemPayload(int64_t suppkey, int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		const int32_t supp_nation = suppkey >= 0 && suppkey < kDefaultSupplierCapacity
		                                ? supplier_nations.LoadNonNegUnchecked(static_cast<uint64_t>(suppkey))
		                                : supplier_nations.Load(suppkey);
		if (supp_nation == kAbsentNation) {
			return false;
		}
		const int8_t slot = order_year_slots_probe_ready
		                        ? order_year_slots.LoadSlotUnchecked(static_cast<uint64_t>(orderkey))
		                        : order_year_slots.LoadSlot(orderkey);
		if (slot < 0 || slot >= kYearSlotCount) {
			return false;
		}
		auto &state = volume_by_year[static_cast<size_t>(slot)];
		AddVolume(state.total_volume, extendedprice_raw, discount_raw);
		if (params.target_nationkey >= 0 && supp_nation == params.target_nationkey) {
			AddVolume(state.nation_volume, extendedprice_raw, discount_raw);
		}
		return true;
	}

	bool AccumulateLineitemPayloadWithSlot(int64_t suppkey, int year_slot, int64_t extendedprice_raw,
	                                     int64_t discount_raw) {
		if (year_slot < 0 || year_slot >= kYearSlotCount) {
			return false;
		}
		const int32_t supp_nation = suppkey >= 0 && suppkey < kDefaultSupplierCapacity
		                                ? supplier_nations.LoadNonNegUnchecked(static_cast<uint64_t>(suppkey))
		                                : supplier_nations.Load(suppkey);
		if (supp_nation == kAbsentNation) {
			return false;
		}
		auto &state = volume_by_year[static_cast<size_t>(year_slot)];
		AddVolume(state.total_volume, extendedprice_raw, discount_raw);
		if (params.target_nationkey >= 0 && supp_nation == params.target_nationkey) {
			AddVolume(state.nation_volume, extendedprice_raw, discount_raw);
		}
		return true;
	}

	void AccumulateFusedLineitemPayloads(duckdb::idx_t scan_count, duckdb::idx_t match_count,
	                                     const LineitemSurvivor *survivors, duckdb::Vector &suppkey_vec,
	                                     duckdb::Vector &extendedprice_vec, duckdb::Vector &discount_vec) {
		if (match_count == 0) {
			return;
		}
		if (IsFlatAllValid(suppkey_vec) && IsFlatAllValid(extendedprice_vec) && IsFlatAllValid(discount_vec) &&
		    scan_count >= survivors[match_count - 1].vector_index + 1) {
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vec);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vec);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vec);
			for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
				const auto &survivor = survivors[match_index];
				const auto vector_index = survivor.vector_index;
				(void)AccumulateLineitemPayloadWithSlot(suppkeys[vector_index], survivor.year_slot,
				                                      extendedprices[vector_index], discounts[vector_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		suppkey_vec.ToUnifiedFormat(scan_count, suppkey_format);
		extendedprice_vec.ToUnifiedFormat(scan_count, extendedprice_format);
		discount_vec.ToUnifiedFormat(scan_count, discount_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

		for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
			const auto &survivor = survivors[match_index];
			const auto vector_index = survivor.vector_index;
			const auto suppkey_index = suppkey_format.sel->get_index(vector_index);
			const auto extendedprice_index = extendedprice_format.sel->get_index(vector_index);
			const auto discount_index = discount_format.sel->get_index(vector_index);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				continue;
			}
			(void)AccumulateLineitemPayloadWithSlot(suppkeys[suppkey_index], survivor.year_slot,
			                                        extendedprices[extendedprice_index], discounts[discount_index]);
		}
	}

	duckdb::idx_t CollectFusedLineitemSurvivors(duckdb::idx_t count, duckdb::idx_t scan_count, bool identity_sel,
	                                            duckdb::SelectionVector &sel, int64_t *partkeys, int64_t *orderkeys,
	                                            LineitemSurvivor *survivors) const {
		duckdb::idx_t match_count = 0;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto vector_index = identity_sel ? row : sel.get_index(row);
			const auto partkey = partkeys[vector_index];
			if (!qualifying_parts.Contains(partkey)) {
				continue;
			}
			const auto orderkey = orderkeys[vector_index];
			if (!qualifying_orders.Contains(orderkey)) {
				continue;
			}
			const int slot = order_year_slots_probe_ready
			                     ? static_cast<int>(order_year_slots.LoadSlotUnchecked(static_cast<uint64_t>(orderkey)))
			                     : static_cast<int>(order_year_slots.LoadSlot(orderkey));
			if (slot < 0 || slot >= kYearSlotCount) {
				continue;
			}
			survivors[match_count++] = LineitemSurvivor {vector_index, orderkey, slot};
			(void)scan_count;
		}
		return match_count;
	}

	duckdb::idx_t CollectFusedLineitemSurvivorsUnified(duckdb::idx_t count, duckdb::idx_t scan_count,
	                                                   duckdb::SelectionVector &sel,
	                                                   duckdb::UnifiedVectorFormat &partkey_format,
	                                                   duckdb::UnifiedVectorFormat &orderkey_format,
	                                                   LineitemSurvivor *survivors) const {
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		duckdb::idx_t match_count = 0;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto base_index = sel.get_index(row);
			const auto partkey_index = partkey_format.sel->get_index(base_index);
			const auto orderkey_index = orderkey_format.sel->get_index(base_index);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index))) {
				continue;
			}
			const auto partkey = partkeys[partkey_index];
			if (!qualifying_parts.Contains(partkey)) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			if (!qualifying_orders.Contains(orderkey)) {
				continue;
			}
			const int slot = order_year_slots_probe_ready
			                     ? static_cast<int>(order_year_slots.LoadSlotUnchecked(static_cast<uint64_t>(orderkey)))
			                     : static_cast<int>(order_year_slots.LoadSlot(orderkey));
			if (slot < 0 || slot >= kYearSlotCount) {
				continue;
			}
			survivors[match_count++] = LineitemSurvivor {base_index, orderkey, slot};
			(void)scan_count;
		}
		return match_count;
	}

	bool has_qualifying_region_nations() const {
		for (const auto flag : params.region_nations) {
			if (flag) {
				return true;
			}
		}
		return false;
	}

	bool PartTypeMatchesSuffix(const std::string &part_type) const {
		const auto &suffix = params.part_type;
		if (part_type.size() < suffix.size()) {
			return false;
		}
		return part_type.compare(part_type.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	void ConsumePart(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q8PartChunkLayout::PARTKEY_INDEX];
		auto &type_vector = input.data[Q8PartChunkLayout::TYPE_INDEX];
		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(type_vector)) {
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto part_type = duckdb::FlatVector::GetData<duckdb::string_t>(type_vector)[row].GetString();
				if (!PartTypeMatchesSuffix(part_type)) {
					continue;
				}
				InsertQualifyingPart(partkeys[row]);
			}
			return;
		}
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat type_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		type_vector.ToUnifiedFormat(input.size(), type_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto type_index = type_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!type_format.validity.AllValid() && !type_format.validity.RowIsValidUnsafe(type_index))) {
				continue;
			}
			const auto part_type = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format)[type_index].GetString();
			if (!PartTypeMatchesSuffix(part_type)) {
				continue;
			}
			InsertQualifyingPart(partkeys[partkey_index]);
		}
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		auto &suppkey_vector = input.data[Q8SupplierChunkLayout::SUPPKEY_INDEX];
		auto &nationkey_vector = input.data[Q8SupplierChunkLayout::NATIONKEY_INDEX];
		if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector)) {
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto suppkey = suppkeys[row];
				supplier_nations.Store(suppkey, nationkeys[row]);
			}
			return;
		}
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
			    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
				continue;
			}
			supplier_nations.Store(suppkeys[suppkey_index], nationkeys[nationkey_index]);
		}
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		if (!has_qualifying_region_nations()) {
			return;
		}
		auto &custkey_vector = input.data[Q8CustomerChunkLayout::CUSTKEY_INDEX];
		auto &nationkey_vector = input.data[Q8CustomerChunkLayout::NATIONKEY_INDEX];
		if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(nationkey_vector)) {
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots || !params.region_nations[static_cast<size_t>(nationkey)]) {
					continue;
				}
				qualifying_customers.Insert(custkeys[row]);
			}
			return;
		}
		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat nationkey_format;
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots || !params.region_nations[static_cast<size_t>(nationkey)]) {
				continue;
			}
			qualifying_customers.Insert(custkeys[custkey_index]);
		}
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		if (!has_qualifying_region_nations()) {
			return;
		}
		auto &orderkey_vector = input.data[Q8OrdersChunkLayout::ORDERKEY_INDEX];
		auto &custkey_vector = input.data[Q8OrdersChunkLayout::CUSTKEY_INDEX];
		auto &orderdate_vector = input.data[Q8OrdersChunkLayout::ORDERDATE_INDEX];
		if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) && IsFlatAllValid(orderdate_vector)) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(orderdate_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (!qualifying_customers.Contains(custkeys[row])) {
					continue;
				}
				const auto year = static_cast<int16_t>(duckdb::Date::ExtractYear(orderdates[row]));
				if (year < kMinYear || year > kMaxYear) {
					continue;
				}
				const auto orderkey = orderkeys[row];
				const int slot = static_cast<int>(year - kMinYear);
				InsertQualifyingOrder(orderkey);
				order_year_slots.StoreSlot(orderkey, static_cast<int8_t>(slot));
			}
			return;
		}
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat orderdate_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		orderdate_vector.ToUnifiedFormat(input.size(), orderdate_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto orderdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(orderdate_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto orderdate_index = orderdate_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index))) {
				continue;
			}
			if (!qualifying_customers.Contains(custkeys[custkey_index])) {
				continue;
			}
			const auto year =
			    static_cast<int16_t>(duckdb::Date::ExtractYear(duckdb::date_t(orderdates[orderdate_index])));
			if (year < kMinYear || year > kMaxYear) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			const int slot = static_cast<int>(year - kMinYear);
			InsertQualifyingOrder(orderkey);
			order_year_slots.StoreSlot(orderkey, static_cast<int8_t>(slot));
		}
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		if (!has_qualifying_region_nations()) {
			return;
		}
		PrepareOrderProbeState();
		auto &partkey_vector = input.data[Q8LineitemChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q8LineitemChunkLayout::SUPPKEY_INDEX];
		auto &orderkey_vector = input.data[Q8LineitemChunkLayout::ORDERKEY_INDEX];
		auto &extendedprice_vector = input.data[Q8LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q8LineitemChunkLayout::DISCOUNT_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) &&
		    IsFlatAllValid(orderkey_vector) && IsFlatAllValid(extendedprice_vector) &&
		    IsFlatAllValid(discount_vector)) {
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				(void)ProcessLineitemRow(partkeys[row], suppkeys[row], orderkeys[row], extendedprices[row],
				                         discounts[row]);
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(suppkey_vector) &&
		    IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(extendedprice_vector) &&
		    IsDictionaryOverFlat(discount_vector)) {
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			auto extendedprices =
			    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
			auto &extendedprice_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto extendedprice_index = extendedprice_sel.get_index(row);
				const auto discount_index = discount_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !suppkey_validity.RowIsValid(suppkey_index) ||
				    !orderkey_validity.RowIsValid(orderkey_index) ||
				    !extendedprice_validity.RowIsValid(extendedprice_index) ||
				    !discount_validity.RowIsValid(discount_index)) {
					continue;
				}
				(void)ProcessLineitemRow(partkeys[partkey_index], suppkeys[suppkey_index], orderkeys[orderkey_index],
				                         extendedprices[extendedprice_index], discounts[discount_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				continue;
			}
			(void)ProcessLineitemRow(partkeys[partkey_index], suppkeys[suppkey_index], orderkeys[orderkey_index],
			                         extendedprices[extendedprice_index], discounts[discount_index]);
		}
	}

	Q8Parameters params;
	Q8RuntimeConfig config;
	CustomerKeyBitmap qualifying_customers;
	DenseKeyStore<int32_t, kAbsentNation> supplier_nations;
	OrderYearSlotStore order_year_slots;
	OrderKeyBitmap qualifying_orders;
	bool qualifying_customers_probe_ready = false;
	bool qualifying_orders_probe_ready = false;
	bool order_year_slots_probe_ready = false;
	QualifyingPartSet qualifying_parts;
	std::vector<int64_t> qualifying_order_keys;
	std::vector<int64_t> qualifying_part_keys;
	int64_t qualifying_orders_min = std::numeric_limits<int64_t>::max();
	int64_t qualifying_orders_max = std::numeric_limits<int64_t>::min();
	int64_t qualifying_parts_min = std::numeric_limits<int64_t>::max();
	int64_t qualifying_parts_max = std::numeric_limits<int64_t>::min();
	bool semijoin_keys_ready = false;
	duckdb::BloomFilter orderkey_bloom_filter;
	bool orderkey_bloom_ready = false;
	Q8LineitemProfile lineitem_profile {};
	bool lineitem_profile_enabled = false;
	std::array<VolumeState, kYearSlotCount> volume_by_year {};
	std::vector<Q8OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool fused_customer_scanned = false;
	bool fused_orders_scanned = false;
	bool fused_lineitem_scanned = false;
	bool finalized = false;
};

} // namespace

bool Q8FusedScansEnabled() {
	return true;
}

bool Q8LineitemFusedScanEnabled() {
	return Q8FusedScansEnabled();
}

void Q8RunFusedCustomerScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	auto &q8_kernel = static_cast<Q8Kernel &>(kernel);
	q8_kernel.RunFusedCustomerScan(context, bind_data);
}

void Q8RunFusedOrdersScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	auto &q8_kernel = static_cast<Q8Kernel &>(kernel);
	q8_kernel.RunFusedOrdersScan(context, bind_data);
}

void Q8RunFusedLineitemScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	auto &q8_kernel = static_cast<Q8Kernel &>(kernel);
	q8_kernel.RunFusedLineitemScan(context, bind_data);
}

duckdb::unique_ptr<KernelInstance> CreateQ8Kernel(const Q8Parameters &params) {
	return duckdb::make_uniq<Q8Kernel>(params);
}

} // namespace accelerator_codegen
