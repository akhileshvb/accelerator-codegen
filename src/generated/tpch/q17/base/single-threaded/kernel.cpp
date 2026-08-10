#include "tpch/q17/base/query.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kDefaultPartCapacity = 200001;

struct Q17RuntimeConfig {
	bool dense_agg = true;
	bool dense_qualifying = true;
	bool lineitem_flat_fast = true;
	bool lineitem_dict_fast = true;
};

#ifdef ACCEL_EXPERIMENTAL_Q17_OPTS
bool ReadBoolEnv(const char *name, bool default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}
#endif

Q17RuntimeConfig ReadQ17RuntimeConfig() {
	Q17RuntimeConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q17_OPTS
	config.dense_agg = ReadBoolEnv("ACCEL_Q17_DENSE_AGG", true);
	config.dense_qualifying = ReadBoolEnv("ACCEL_Q17_DENSE_QUALIFYING", true);
	config.lineitem_flat_fast = ReadBoolEnv("ACCEL_Q17_LINEITEM_FLAT_FAST", true);
	config.lineitem_dict_fast = ReadBoolEnv("ACCEL_Q17_LINEITEM_DICT_FAST", true);
#endif
	return config;
}

struct PartAggSlot {
	int64_t sum_raw = 0;
	int32_t count = 0;
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

void TrackPath(Q17InputProfile *profile, duckdb::DataChunk &input, int path) {
	if (!profile) {
		return;
	}
	profile->total_chunks += 1;
	profile->total_rows += input.size();
	switch (path) {
	case 0:
		profile->flat_fast_path_chunks += 1;
		profile->flat_fast_path_rows += input.size();
		return;
	case 1:
		profile->dict_fast_path_chunks += 1;
		profile->dict_fast_path_rows += input.size();
		return;
	default:
		profile->generic_fallback_chunks += 1;
		profile->generic_fallback_rows += input.size();
		return;
	}
}

bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

class PartAggStore {
public:
	explicit PartAggStore(bool use_dense_p) : use_dense(use_dense_p) {
	}

	int64_t InitializeNs() {
		if (!use_dense || initialized) {
			return 0;
		}
		const auto start = Clock::now();
		slots.assign(kDefaultPartCapacity, PartAggSlot {});
		thresholds.assign(kDefaultPartCapacity, 0.0);
		capacity = slots.size();
		initialized = true;
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
	}

	void Add(int64_t partkey, int64_t quantity_raw) {
		if (!use_dense || partkey < 0) {
			auto &slot = overflow[partkey];
			slot.sum_raw += quantity_raw;
			slot.count += 1;
			return;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key >= capacity) {
			if (partkey > 1'000'000'000) {
				auto &slot = overflow[partkey];
				slot.sum_raw += quantity_raw;
				slot.count += 1;
				return;
			}
			EnsureCapacity(key + 1);
		}
		auto &slot = slots[key];
		if (slot.count == 0) {
			stored_count += 1;
		}
		slot.sum_raw += quantity_raw;
		slot.count += 1;
	}

	inline void AddNonNegUnchecked(uint64_t partkey, int64_t quantity_raw) {
		auto &slot = slots[partkey];
		if (slot.count == 0) {
			stored_count += 1;
		}
		slot.sum_raw += quantity_raw;
		slot.count += 1;
	}

	const PartAggSlot *Probe(int64_t partkey) const {
		if (!use_dense || partkey < 0) {
			const auto it = overflow.find(partkey);
			return it == overflow.end() ? nullptr : &it->second;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key < capacity) {
			return slots[key].count == 0 ? nullptr : &slots[key];
		}
		const auto it = overflow.find(partkey);
		return it == overflow.end() ? nullptr : &it->second;
	}

	inline const PartAggSlot *ProbeNonNegUnchecked(uint64_t partkey) const {
		return slots[partkey].count == 0 ? nullptr : &slots[partkey];
	}

	bool CanProbeUnchecked(int64_t partkey) const {
		return use_dense && partkey >= 0 && static_cast<size_t>(partkey) < capacity;
	}

	double Threshold(int64_t partkey) const {
		if (!use_dense || partkey < 0) {
			const auto it = overflow_threshold.find(partkey);
			return it == overflow_threshold.end() ? 0.0 : it->second;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key < capacity) {
			return thresholds[key];
		}
		const auto it = overflow_threshold.find(partkey);
		return it == overflow_threshold.end() ? 0.0 : it->second;
	}

	void SetThreshold(int64_t partkey, double value) {
		if (!use_dense || partkey < 0) {
			overflow_threshold[partkey] = value;
			return;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key >= capacity) {
			overflow_threshold[partkey] = value;
			return;
		}
		thresholds[key] = value;
	}

	const PartAggSlot &SlotAt(size_t index) const {
		return slots[index];
	}

	size_t Capacity() const {
		return capacity;
	}

	duckdb::idx_t Cardinality() const {
		return stored_count + overflow.size();
	}

	duckdb::idx_t OverflowSize() const {
		return overflow.size();
	}

	duckdb::idx_t MemoryBytes() const {
		return slots.size() * sizeof(PartAggSlot) + thresholds.size() * sizeof(double) +
		       overflow.size() * sizeof(typename decltype(overflow)::value_type) +
		       overflow_threshold.size() * sizeof(typename decltype(overflow_threshold)::value_type);
	}

	bool UsesDense() const {
		return use_dense;
	}

private:
	void EnsureCapacity(size_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		size_t target = new_capacity;
		if (capacity > 0) {
			const size_t doubled = capacity > (std::numeric_limits<size_t>::max() / 2) ? std::numeric_limits<size_t>::max()
			                                                                          : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		slots.resize(target, PartAggSlot {});
		thresholds.resize(target, 0.0);
		capacity = slots.size();
	}

	bool use_dense;
	bool initialized = false;
	size_t capacity = 0;
	duckdb::idx_t stored_count = 0;
	std::vector<PartAggSlot> slots;
	std::vector<double> thresholds;
	std::unordered_map<int64_t, PartAggSlot> overflow;
	std::unordered_map<int64_t, double> overflow_threshold;
};

class QualifyingPartStore {
public:
	explicit QualifyingPartStore(bool use_dense_p) : use_dense(use_dense_p) {
	}

	int64_t InitializeNs() {
		if (!use_dense || initialized) {
			return 0;
		}
		const auto start = Clock::now();
		flags.assign(kDefaultPartCapacity, 0);
		capacity = flags.size();
		initialized = true;
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
	}

	void Mark(int64_t partkey) {
		if (!use_dense || partkey < 0) {
			overflow.insert(partkey);
			return;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key >= capacity) {
			if (partkey > 1'000'000'000) {
				overflow.insert(partkey);
				return;
			}
			EnsureCapacity(key + 1);
		}
		if (flags[key] == 0) {
			stored_count += 1;
		}
		flags[key] = 1;
	}

	inline void MarkNonNegUnchecked(uint64_t partkey) {
		if (flags[partkey] == 0) {
			stored_count += 1;
		}
		flags[partkey] = 1;
	}

	bool IsQualifying(int64_t partkey) const {
		if (!use_dense || partkey < 0) {
			return overflow.find(partkey) != overflow.end();
		}
		const auto key = static_cast<size_t>(partkey);
		if (key < capacity) {
			return flags[key] != 0;
		}
		return overflow.find(partkey) != overflow.end();
	}

	inline bool IsQualifyingNonNegUnchecked(uint64_t partkey) const {
		return flags[partkey] != 0;
	}

	bool CanProbeUnchecked(int64_t partkey) const {
		return use_dense && partkey >= 0 && static_cast<size_t>(partkey) < capacity;
	}

	duckdb::idx_t Cardinality() const {
		return stored_count + overflow.size();
	}

	duckdb::idx_t OverflowSize() const {
		return overflow.size();
	}

	bool UsesDense() const {
		return use_dense;
	}

private:
	void EnsureCapacity(size_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		size_t target = new_capacity;
		if (capacity > 0) {
			const size_t doubled = capacity > (std::numeric_limits<size_t>::max() / 2) ? std::numeric_limits<size_t>::max()
			                                                                          : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		flags.resize(target, 0);
		capacity = flags.size();
	}

	bool use_dense;
	bool initialized = false;
	size_t capacity = 0;
	duckdb::idx_t stored_count = 0;
	std::vector<uint8_t> flags;
	std::unordered_set<int64_t> overflow;
};

class Q17Kernel final : public KernelInstance {
public:
	explicit Q17Kernel(Q17Parameters params_p, Q17ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(ReadQ17RuntimeConfig()),
	      part_agg(config.dense_agg), qualifying_parts(config.dense_qualifying) {
		const auto agg_init_ns = part_agg.InitializeNs();
		const auto qual_init_ns = qualifying_parts.InitializeNs();
		if (profile) {
			profile->lineitem_agg_build_ns += agg_init_ns + qual_init_ns;
			profile->used_dense_agg = part_agg.UsesDense();
			profile->used_dense_qualifying = qualifying_parts.UsesDense();
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q17InputId>(input_id)) {
		case Q17InputId::LINEITEM_AGG:
			ConsumeLineitemAgg(input);
			return;
		case Q17InputId::PART:
			ConsumePart(input);
			return;
		case Q17InputId::LINEITEM_PROBE:
			ConsumeLineitemProbe(input);
			return;
		default:
			throw duckdb::InternalException("Q17 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		const auto start = Clock::now();
		if (has_extendedprice_sum) {
			auto decimal_sum = duckdb::Value::DECIMAL(extendedprice_sum, 38, 2);
			result_value = decimal_sum.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>() / 7.0;
			has_result = true;
		}
		if (profile) {
			profile->part_agg_cardinality = part_agg.Cardinality();
			profile->qualifying_part_cardinality = qualifying_parts.Cardinality();
			profile->part_overflow_entries = part_agg.OverflowSize() + qualifying_parts.OverflowSize();
			profile->part_map_memory_bytes = part_agg.MemoryBytes();
			profile->finalize_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			profile->emitted_rows = 1;
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		output.SetCardinality(1);
		if (!has_result) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DOUBLE));
		} else {
			output.SetValue(0, 0, duckdb::Value::DOUBLE(result_value));
		}
		emitted = true;
		return true;
	}

private:
	void PrepareProbeThresholds() {
		if (thresholds_prepared) {
			return;
		}
		const auto start = Clock::now();
		if (part_agg.UsesDense()) {
			for (size_t key = 0; key < part_agg.Capacity(); key++) {
				if (!qualifying_parts.IsQualifyingNonNegUnchecked(key)) {
					continue;
				}
				const auto &slot = part_agg.SlotAt(key);
				if (slot.count == 0) {
					continue;
				}
				const double avg_quantity = (static_cast<double>(slot.sum_raw) / 100.0) / static_cast<double>(slot.count);
				part_agg.SetThreshold(static_cast<int64_t>(key), 0.2 * avg_quantity);
			}
		}
		thresholds_prepared = true;
		if (profile) {
			profile->threshold_prepare_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
	}

	void ConsumeLineitemAgg(duckdb::DataChunk &input) {
		const auto start = Clock::now();
		if (profile) {
			profile->lineitem_agg_rows_scanned += input.size();
		}
		auto &partkey_vector = input.data[Q17LineitemAggChunkLayout::PARTKEY_INDEX];
		auto &quantity_vector = input.data[Q17LineitemAggChunkLayout::QUANTITY_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(partkey_vector) && IsFlatAllValid(quantity_vector)) {
			TrackPath(profile ? &profile->lineitem_agg : nullptr, input, 0);
			if (profile) {
				profile->used_lineitem_flat_fast = true;
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				part_agg.Add(partkeys[row], quantities[row]);
			}
			if (profile) {
				profile->lineitem_agg_build_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(quantity_vector)) {
			TrackPath(profile ? &profile->lineitem_agg : nullptr, input, 1);
			if (profile) {
				profile->used_lineitem_dict_fast = true;
			}
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto quantities = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(quantity_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &quantity_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(quantity_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto quantity_index = quantity_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !quantity_validity.RowIsValid(quantity_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				part_agg.Add(partkeys[partkey_index], quantities[quantity_index]);
			}
			if (profile) {
				profile->lineitem_agg_build_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		}

		TrackPath(profile ? &profile->lineitem_agg : nullptr, input, 2);
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat quantity_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		quantity_vector.ToUnifiedFormat(input.size(), quantity_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto quantity_index = quantity_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			part_agg.Add(partkeys[partkey_index], quantities[quantity_index]);
		}
		if (profile) {
			profile->lineitem_agg_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
	}

	void ConsumePart(duckdb::DataChunk &input) {
		const auto start = Clock::now();
		if (profile) {
			profile->part_rows_scanned += input.size();
		}
		auto &partkey_vector = input.data[Q17PartChunkLayout::PARTKEY_INDEX];
		auto &brand_vector = input.data[Q17PartChunkLayout::BRAND_INDEX];
		auto &container_vector = input.data[Q17PartChunkLayout::CONTAINER_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(brand_vector) && IsFlatAllValid(container_vector)) {
			TrackPath(profile ? &profile->part : nullptr, input, 0);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(brand_vector);
			auto containers = duckdb::FlatVector::GetData<duckdb::string_t>(container_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (StringEquals(brands[row], params.brand) && StringEquals(containers[row], params.container)) {
					qualifying_parts.Mark(partkeys[row]);
					if (profile) {
						profile->part_rows_matched += 1;
					}
				}
			}
			if (profile) {
				profile->part_build_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		}

		TrackPath(profile ? &profile->part : nullptr, input, 2);
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat brand_format;
		duckdb::UnifiedVectorFormat container_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		brand_vector.ToUnifiedFormat(input.size(), brand_format);
		container_vector.ToUnifiedFormat(input.size(), container_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto brand_index = brand_format.sel->get_index(row);
			const auto container_index = container_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!brand_format.validity.AllValid() && !brand_format.validity.RowIsValidUnsafe(brand_index)) ||
			    (!container_format.validity.AllValid() &&
			     !container_format.validity.RowIsValidUnsafe(container_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto brand_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(brand_format)[brand_index];
			const auto container_value =
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(container_format)[container_index];
			if (!StringEquals(brand_value, params.brand) || !StringEquals(container_value, params.container)) {
				continue;
			}
			qualifying_parts.Mark(partkeys[partkey_index]);
			if (profile) {
				profile->part_rows_matched += 1;
			}
		}
		if (profile) {
			profile->part_build_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
	}

	bool ProcessProbeRow(int64_t partkey, int64_t quantity_raw, int64_t extendedprice_raw) {
		if (!qualifying_parts.IsQualifying(partkey)) {
			return false;
		}
		const auto *agg = part_agg.Probe(partkey);
		if (agg == nullptr || agg->count == 0) {
			return false;
		}
		double threshold = part_agg.Threshold(partkey);
		if (threshold == 0.0) {
			const double avg_quantity =
			    (static_cast<double>(agg->sum_raw) / 100.0) / static_cast<double>(agg->count);
			threshold = 0.2 * avg_quantity;
			part_agg.SetThreshold(partkey, threshold);
		}
		const double quantity = static_cast<double>(quantity_raw) / 100.0;
		if (!(quantity < threshold)) {
			return false;
		}
		if (profile) {
			profile->probe_rows_below_threshold += 1;
		}
#if defined(__SIZEOF_INT128__)
		__int128_t lhs = (static_cast<__int128_t>(extendedprice_sum.upper) << 64) + extendedprice_sum.lower;
		lhs += static_cast<__int128_t>(extendedprice_raw);
		extendedprice_sum = duckdb::hugeint_t(static_cast<int64_t>(lhs >> 64), static_cast<uint64_t>(lhs));
#else
		duckdb::hugeint_t addend;
		if (!duckdb::Hugeint::TryConvert(extendedprice_raw, addend) ||
		    (has_extendedprice_sum && !duckdb::Hugeint::TryAddInPlace(extendedprice_sum, addend))) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q17 extendedprice sum");
		}
		if (!has_extendedprice_sum) {
			extendedprice_sum = addend;
		}
#endif
		has_extendedprice_sum = true;
		return true;
	}

	void ConsumeLineitemProbe(duckdb::DataChunk &input) {
		PrepareProbeThresholds();
		const auto start = Clock::now();
		if (profile) {
			profile->lineitem_probe_rows_scanned += input.size();
		}
		auto &partkey_vector = input.data[Q17LineitemProbeChunkLayout::PARTKEY_INDEX];
		auto &quantity_vector = input.data[Q17LineitemProbeChunkLayout::QUANTITY_INDEX];
		auto &extendedprice_vector = input.data[Q17LineitemProbeChunkLayout::EXTENDEDPRICE_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(partkey_vector) && IsFlatAllValid(quantity_vector) &&
		    IsFlatAllValid(extendedprice_vector)) {
			TrackPath(profile ? &profile->lineitem_probe : nullptr, input, 0);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				(void)ProcessProbeRow(partkeys[row], quantities[row], extendedprices[row]);
			}
			if (profile) {
				profile->lineitem_probe_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(quantity_vector) &&
		    IsDictionaryOverFlat(extendedprice_vector)) {
			TrackPath(profile ? &profile->lineitem_probe : nullptr, input, 1);
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
			auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto quantities = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(quantity_vector));
			auto extendedprices =
			    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &quantity_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(quantity_vector));
			auto &extendedprice_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto quantity_index = quantity_sel.get_index(row);
				const auto extendedprice_index = extendedprice_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !quantity_validity.RowIsValid(quantity_index) ||
				    !extendedprice_validity.RowIsValid(extendedprice_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				(void)ProcessProbeRow(partkeys[partkey_index], quantities[quantity_index],
				                      extendedprices[extendedprice_index]);
			}
			if (profile) {
				profile->lineitem_probe_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			}
			return;
		}

		TrackPath(profile ? &profile->lineitem_probe : nullptr, input, 2);
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat quantity_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		quantity_vector.ToUnifiedFormat(input.size(), quantity_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto quantity_index = quantity_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			(void)ProcessProbeRow(partkeys[partkey_index], quantities[quantity_index],
			                      extendedprices[extendedprice_index]);
		}
		if (profile) {
			profile->lineitem_probe_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
	}

	Q17Parameters params;
	Q17ProfileCounters *profile;
	Q17RuntimeConfig config;
	PartAggStore part_agg;
	QualifyingPartStore qualifying_parts;
	duckdb::hugeint_t extendedprice_sum = duckdb::Hugeint::Convert(0);
	bool has_extendedprice_sum = false;
	double result_value = 0.0;
	bool has_result = false;
	bool thresholds_prepared = false;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ17Kernel(const Q17Parameters &params) {
	return duckdb::make_uniq<Q17Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ17ProfilingKernel(const Q17Parameters &params,
                                                            Q17ProfileCounters *profile) {
	return duckdb::make_uniq<Q17Kernel>(params, profile);
}

} // namespace accelerator_codegen
