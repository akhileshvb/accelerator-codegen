#include "accelerator_codegen/tpch_q14.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

using Clock = std::chrono::steady_clock;
#if defined(__SIZEOF_INT128__)
using AccT = __int128_t;
#else
using AccT = int64_t;
#endif

// TPC-H SF1 partkeys are 1..200000.
constexpr size_t kDefaultPartCapacity = 200001;

enum class PromoClass : uint8_t {
	Absent = 0,
	NonPromo = 1,
	Promo = 2,
};

struct Q14RuntimeConfig {
	bool dense_part = true;
	bool part_dict_fast = true;
	bool lineitem_dict_fast = true;
	bool skip_base_date_residual = false;
};

#ifdef ACCEL_EXPERIMENTAL_Q14_OPTS
bool ReadBoolEnv(const char *name, bool default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}
#endif

Q14RuntimeConfig ReadQ14RuntimeConfig() {
	Q14RuntimeConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q14_OPTS
	config.dense_part = ReadBoolEnv("ACCEL_Q14_DENSE_PART", true);
	config.part_dict_fast = ReadBoolEnv("ACCEL_Q14_PART_DICT_FAST", true);
	config.lineitem_dict_fast = ReadBoolEnv("ACCEL_Q14_LINEITEM_DICT_FAST", true);
	config.skip_base_date_residual = ReadBoolEnv("ACCEL_Q14_SKIP_BASE_DATE_RESIDUAL", false);
#endif
	return config;
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

void RecordLayout(Q14LayoutHistogram *histogram, duckdb::Vector &vector) {
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
		} else {
			histogram->other += 1;
		}
		return;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		if (!duckdb::ConstantVector::IsNull(vector)) {
			histogram->constant_all_valid += 1;
		} else {
			histogram->other += 1;
		}
		return;
	default:
		histogram->other += 1;
		return;
	}
}

void TrackPath(Q14InputProfile *profile, duckdb::DataChunk &input, int path) {
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

bool IsPromoType(const duckdb::string_t &value) {
	static constexpr const char kPromo[] = "PROMO";
	constexpr auto kPromoSize = sizeof(kPromo) - 1;
	return value.GetSize() >= kPromoSize && std::memcmp(value.GetData(), kPromo, kPromoSize) == 0;
}

AccT DiscountedRevenueRaw(int64_t extendedprice_raw, int64_t discount_raw) {
	return static_cast<AccT>(extendedprice_raw) * static_cast<AccT>(100 - discount_raw);
}

duckdb::hugeint_t AccToHugeint(AccT value) {
#if defined(__SIZEOF_INT128__)
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
#else
	return duckdb::hugeint_t(value);
#endif
}

double DecimalSumToDouble(AccT raw_scale4) {
	auto decimal_value = duckdb::Value::DECIMAL(AccToHugeint(raw_scale4), 38, 4);
	return decimal_value.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
}

class PartPromoStore {
public:
	explicit PartPromoStore(bool use_dense_p) : use_dense(use_dense_p) {
	}

	int64_t InitializeNs() {
		if (!use_dense || initialized) {
			return 0;
		}
		const auto start = Clock::now();
		classes.assign(kDefaultPartCapacity, static_cast<uint8_t>(PromoClass::Absent));
		capacity = classes.size();
		initialized = true;
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
	}

	void Store(int64_t partkey, PromoClass promo) {
		if (!use_dense || partkey < 0) {
			overflow[partkey] = promo;
			return;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key >= capacity) {
			if (partkey > static_cast<int64_t>(capacity) * 2 && partkey > 2'000'000) {
				overflow[partkey] = promo;
				return;
			}
			EnsureCapacity(key + 1);
		}
		if (classes[key] == static_cast<uint8_t>(PromoClass::Absent)) {
			stored_count += 1;
		}
		classes[key] = static_cast<uint8_t>(promo);
	}

	inline void StoreNonNegUnchecked(uint64_t partkey, PromoClass promo) {
		if (classes[partkey] == static_cast<uint8_t>(PromoClass::Absent)) {
			stored_count += 1;
		}
		classes[partkey] = static_cast<uint8_t>(promo);
	}

	bool CanProbeUnchecked(int64_t partkey) const {
		return use_dense && partkey >= 0 && static_cast<size_t>(partkey) < capacity;
	}

	PromoClass Probe(int64_t partkey) const {
		if (!use_dense || partkey < 0) {
			const auto it = overflow.find(partkey);
			return it == overflow.end() ? PromoClass::Absent : it->second;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key < capacity) {
			return static_cast<PromoClass>(classes[key]);
		}
		const auto it = overflow.find(partkey);
		return it == overflow.end() ? PromoClass::Absent : it->second;
	}

	inline PromoClass ProbeNonNegUnchecked(uint64_t partkey) const {
		return static_cast<PromoClass>(classes[partkey]);
	}

	duckdb::idx_t Cardinality() const {
		return stored_count + overflow.size();
	}

	duckdb::idx_t MemoryBytes() const {
		return classes.size() + overflow.size() * sizeof(typename decltype(overflow)::value_type) +
		       overflow.bucket_count() * sizeof(void *);
	}

	duckdb::idx_t OverflowSize() const {
		return overflow.size();
	}

	bool UsesDense() const {
		return use_dense;
	}

	size_t Capacity() const {
		return capacity;
	}

private:
	void EnsureCapacity(size_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		classes.resize(new_capacity, static_cast<uint8_t>(PromoClass::Absent));
		capacity = classes.size();
	}

	bool use_dense;
	bool initialized = false;
	size_t capacity = 0;
	duckdb::idx_t stored_count = 0;
	std::vector<uint8_t> classes;
	std::unordered_map<int64_t, PromoClass> overflow;
};

class Q14Kernel final : public KernelInstance {
public:
	explicit Q14Kernel(Q14Parameters params_p, Q14ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(ReadQ14RuntimeConfig()),
	      part_promo(config.dense_part) {
		const auto init_ns = part_promo.InitializeNs();
		if (profile) {
			profile->part_state_init_ns += init_ns;
			profile->used_dense_part = part_promo.UsesDense();
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q14InputId>(input_id)) {
		case Q14InputId::PART:
			ConsumePartChunk(input);
			return;
		case Q14InputId::LINEITEM:
			ConsumeLineitemChunk(input);
			return;
		}
		throw duckdb::InternalException("Q14 kernel received unexpected input id %llu",
		                                static_cast<unsigned long long>(input_id));
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		const auto start = Clock::now();
		if (!saw_contribution) {
			result_is_null = true;
		} else if (total_revenue_raw == 0) {
			denominator_is_zero = true;
			if (promo_revenue_raw == 0) {
				result_value = std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0);
			} else {
				result_value = std::numeric_limits<double>::infinity();
			}
		} else {
			const AccT numerator_raw = static_cast<AccT>(10000) * promo_revenue_raw;
			const auto numerator_d = duckdb::Value::DECIMAL(AccToHugeint(numerator_raw), 38, 6)
			                             .DefaultCastAs(duckdb::LogicalType::DOUBLE)
			                             .GetValue<double>();
			const auto total_d = DecimalSumToDouble(total_revenue_raw);
			result_value = numerator_d / total_d;
		}
		const auto end = Clock::now();
		if (profile) {
			profile->part_map_cardinality = part_promo.Cardinality();
			profile->part_map_memory_bytes = part_promo.MemoryBytes();
			profile->part_overflow_entries = part_promo.OverflowSize();
			profile->emitted_rows = 1;
			profile->result_is_null = result_is_null;
			profile->denominator_is_zero = denominator_is_zero;
			profile->finalize_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		if (result_is_null) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DOUBLE));
		} else {
			output.SetValue(0, 0, duckdb::Value::DOUBLE(result_value));
		}
		output.SetCardinality(1);
		emitted = true;
		return true;
	}

private:
	void CountPromo(PromoClass promo) {
		if (!profile) {
			return;
		}
		if (promo == PromoClass::Promo) {
			profile->part_promo_count += 1;
		} else if (promo == PromoClass::NonPromo) {
			profile->part_non_promo_count += 1;
		}
	}

	void StorePart(int64_t partkey, PromoClass promo) {
		part_promo.Store(partkey, promo);
		CountPromo(promo);
	}

	void ConsumePartChunk(duckdb::DataChunk &input) {
		const auto build_start = Clock::now();
		if (profile) {
			RecordLayout(&profile->layout_p_partkey, input.data[Q14PartChunkLayout::PARTKEY_INDEX]);
			RecordLayout(&profile->layout_p_type, input.data[Q14PartChunkLayout::TYPE_INDEX]);
			profile->part_rows_scanned += input.size();
		}

		auto &partkey_vector = input.data[Q14PartChunkLayout::PARTKEY_INDEX];
		auto &type_vector = input.data[Q14PartChunkLayout::TYPE_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(type_vector)) {
			TrackPath(profile ? &profile->part : nullptr, input, 0);
			ConsumePartFlatFlat(partkey_vector, type_vector, input.size(), build_start);
			return;
		}

		if (config.part_dict_fast && IsFlatAllValid(partkey_vector) && IsDictionaryOverFlat(type_vector)) {
			TrackPath(profile ? &profile->part : nullptr, input, 1);
			ConsumePartFlatDictType(partkey_vector, type_vector, input.size(), build_start);
			return;
		}

		if (config.part_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(type_vector)) {
			TrackPath(profile ? &profile->part : nullptr, input, 1);
			ConsumePartDictDict(partkey_vector, type_vector, input.size(), build_start);
			return;
		}

		TrackPath(profile ? &profile->part : nullptr, input, 2);
		ConsumePartGeneric(partkey_vector, type_vector, input.size(), build_start);
	}

	void ConsumePartFlatFlat(duckdb::Vector &partkey_vector, duckdb::Vector &type_vector, duckdb::idx_t count,
	                         Clock::time_point build_start) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_vector);
		const auto work_start = Clock::now();
		if (profile) {
			profile->part_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_start - build_start).count();
		}
		const auto capacity = part_promo.Capacity();
		if (part_promo.UsesDense() && capacity > 0) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto partkey = partkeys[row];
				const auto promo = IsPromoType(types[row]) ? PromoClass::Promo : PromoClass::NonPromo;
				if (partkey >= 0 && static_cast<size_t>(partkey) < capacity) {
					part_promo.StoreNonNegUnchecked(static_cast<uint64_t>(partkey), promo);
					CountPromo(promo);
				} else {
					StorePart(partkey, promo);
				}
			}
		} else {
			for (duckdb::idx_t row = 0; row < count; row++) {
				StorePart(partkeys[row], IsPromoType(types[row]) ? PromoClass::Promo : PromoClass::NonPromo);
			}
		}
		if (profile) {
			const auto work_end = Clock::now();
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - work_start).count();
			profile->part_classify_ns += loop_ns / 2;
			profile->part_insert_ns += loop_ns - loop_ns / 2;
			profile->part_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - build_start).count();
		}
	}

	void ConsumePartFlatDictType(duckdb::Vector &partkey_vector, duckdb::Vector &type_vector, duckdb::idx_t count,
	                             Clock::time_point build_start) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto &type_sel = duckdb::DictionaryVector::SelVector(type_vector);
		auto &type_child = duckdb::DictionaryVector::Child(type_vector);
		auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_child);
		auto &type_validity = duckdb::FlatVector::Validity(type_child);
		const auto work_start = Clock::now();
		if (profile) {
			profile->part_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_start - build_start).count();
		}

		PromoClass class_cache[STANDARD_VECTOR_SIZE];
		bool class_valid[STANDARD_VECTOR_SIZE] = {};
		duckdb::idx_t cache_hits = 0;
		const auto capacity = part_promo.Capacity();
		const bool dense = part_promo.UsesDense() && capacity > 0;

		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto type_index = type_sel.get_index(row);
			PromoClass promo = PromoClass::NonPromo;
			if (type_index < STANDARD_VECTOR_SIZE && class_valid[type_index]) {
				promo = class_cache[type_index];
				cache_hits += 1;
			} else {
				if (!type_validity.AllValid() && !type_validity.RowIsValidUnsafe(type_index)) {
					promo = PromoClass::NonPromo;
					if (profile) {
						profile->part_null_type_count += 1;
					}
				} else {
					promo = IsPromoType(types[type_index]) ? PromoClass::Promo : PromoClass::NonPromo;
				}
				if (type_index < STANDARD_VECTOR_SIZE) {
					class_cache[type_index] = promo;
					class_valid[type_index] = true;
				}
			}
			const auto partkey = partkeys[row];
			if (dense && partkey >= 0 && static_cast<size_t>(partkey) < capacity) {
				part_promo.StoreNonNegUnchecked(static_cast<uint64_t>(partkey), promo);
				CountPromo(promo);
			} else {
				StorePart(partkey, promo);
			}
		}
		if (profile) {
			profile->part_dict_cache_hits += cache_hits;
			const auto work_end = Clock::now();
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - work_start).count();
			profile->part_classify_ns += loop_ns / 2;
			profile->part_insert_ns += loop_ns - loop_ns / 2;
			profile->part_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - build_start).count();
		}
	}

	void ConsumePartDictDict(duckdb::Vector &partkey_vector, duckdb::Vector &type_vector, duckdb::idx_t count,
	                         Clock::time_point build_start) {
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &type_sel = duckdb::DictionaryVector::SelVector(type_vector);
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
		auto &type_child = duckdb::DictionaryVector::Child(type_vector);
		auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_child);
		auto &type_validity = duckdb::FlatVector::Validity(type_child);
		const bool shared_sel = &partkey_sel == &type_sel;
		const auto work_start = Clock::now();
		if (profile) {
			profile->part_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_start - build_start).count();
		}

		PromoClass class_cache[STANDARD_VECTOR_SIZE];
		bool class_valid[STANDARD_VECTOR_SIZE] = {};
		const auto capacity = part_promo.Capacity();
		const bool dense = part_promo.UsesDense() && capacity > 0;

		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto partkey_index = partkey_sel.get_index(row);
			const auto type_index = shared_sel ? partkey_index : type_sel.get_index(row);
			PromoClass promo = PromoClass::NonPromo;
			if (type_index < STANDARD_VECTOR_SIZE && class_valid[type_index]) {
				promo = class_cache[type_index];
			} else {
				if (!type_validity.AllValid() && !type_validity.RowIsValidUnsafe(type_index)) {
					promo = PromoClass::NonPromo;
					if (profile) {
						profile->part_null_type_count += 1;
					}
				} else {
					promo = IsPromoType(types[type_index]) ? PromoClass::Promo : PromoClass::NonPromo;
				}
				if (type_index < STANDARD_VECTOR_SIZE) {
					class_cache[type_index] = promo;
					class_valid[type_index] = true;
				}
			}
			const auto partkey = partkeys[partkey_index];
			if (dense && partkey >= 0 && static_cast<size_t>(partkey) < capacity) {
				part_promo.StoreNonNegUnchecked(static_cast<uint64_t>(partkey), promo);
				CountPromo(promo);
			} else {
				StorePart(partkey, promo);
			}
		}
		if (profile) {
			const auto work_end = Clock::now();
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - work_start).count();
			profile->part_classify_ns += loop_ns / 2;
			profile->part_insert_ns += loop_ns - loop_ns / 2;
			profile->part_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - build_start).count();
		}
	}

	void ConsumePartGeneric(duckdb::Vector &partkey_vector, duckdb::Vector &type_vector, duckdb::idx_t count,
	                        Clock::time_point build_start) {
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat type_format;
		partkey_vector.ToUnifiedFormat(count, partkey_format);
		type_vector.ToUnifiedFormat(count, type_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto types = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format);
		const auto work_start = Clock::now();
		if (profile) {
			profile->part_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_start - build_start).count();
		}
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto type_index = type_format.sel->get_index(row);
			if (!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) {
				continue;
			}
			PromoClass promo = PromoClass::NonPromo;
			if (!type_format.validity.AllValid() && !type_format.validity.RowIsValidUnsafe(type_index)) {
				if (profile) {
					profile->part_null_type_count += 1;
				}
			} else {
				promo = IsPromoType(types[type_index]) ? PromoClass::Promo : PromoClass::NonPromo;
			}
			StorePart(partkeys[partkey_index], promo);
		}
		if (profile) {
			const auto work_end = Clock::now();
			const auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - work_start).count();
			profile->part_classify_ns += loop_ns / 2;
			profile->part_insert_ns += loop_ns - loop_ns / 2;
			profile->part_total_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(work_end - build_start).count();
		}
	}

	void ConsumeLineitemChunk(duckdb::DataChunk &input) {
		const auto decode_start = Clock::now();
		if (profile) {
			RecordLayout(&profile->layout_l_partkey, input.data[Q14LineitemChunkLayout::PARTKEY_INDEX]);
			RecordLayout(&profile->layout_l_extendedprice, input.data[Q14LineitemChunkLayout::EXTENDEDPRICE_INDEX]);
			RecordLayout(&profile->layout_l_discount, input.data[Q14LineitemChunkLayout::DISCOUNT_INDEX]);
			RecordLayout(&profile->layout_l_shipdate, input.data[Q14LineitemChunkLayout::SHIPDATE_INDEX]);
			profile->lineitem_rows_scanned += input.size();
		}

		auto &partkey_vector = input.data[Q14LineitemChunkLayout::PARTKEY_INDEX];
		auto &price_vector = input.data[Q14LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q14LineitemChunkLayout::DISCOUNT_INDEX];
		auto &shipdate_vector = input.data[Q14LineitemChunkLayout::SHIPDATE_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(price_vector) && IsFlatAllValid(discount_vector) &&
		    IsFlatAllValid(shipdate_vector)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, 0);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto prices = duckdb::FlatVector::GetData<int64_t>(price_vector);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(shipdate_vector);
			if (profile) {
				profile->lineitem_scan_decode_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decode_start).count();
			}
			ProcessAlignedLineitem(partkeys, nullptr, prices, nullptr, discounts, nullptr, shipdates, nullptr,
			                       input.size(), /*require_date_residual=*/true);
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(price_vector) &&
		    IsDictionaryOverFlat(discount_vector) && IsDictionaryOverFlat(shipdate_vector)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, 1);
			ConsumeLineitemDict(partkey_vector, price_vector, discount_vector, shipdate_vector, input.size(),
			                    decode_start);
			return;
		}

		TrackPath(profile ? &profile->lineitem : nullptr, input, 2);
		ConsumeLineitemGeneric(partkey_vector, price_vector, discount_vector, shipdate_vector, input.size(),
		                       decode_start);
	}

	void ConsumeLineitemDict(duckdb::Vector &partkey_vector, duckdb::Vector &price_vector,
	                         duckdb::Vector &discount_vector, duckdb::Vector &shipdate_vector, duckdb::idx_t count,
	                         Clock::time_point decode_start) {
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &price_sel = duckdb::DictionaryVector::SelVector(price_vector);
		auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
		auto &shipdate_sel = duckdb::DictionaryVector::SelVector(shipdate_vector);
		const bool shared_sel = &partkey_sel == &price_sel && &partkey_sel == &discount_sel &&
		                        &partkey_sel == &shipdate_sel;
		if (profile && shared_sel) {
			profile->lineitem_shared_sel_chunks += 1;
		}

		auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
		auto prices = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(price_vector));
		auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
		auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(shipdate_vector));
		auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
		auto &price_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(price_vector));
		auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
		auto &shipdate_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(shipdate_vector));

		const bool skip_date_residual =
		    config.skip_base_date_residual &&
		    shipdate_vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR;

		if (profile) {
			profile->lineitem_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decode_start).count();
		}

		if (shared_sel && partkey_validity.AllValid() && price_validity.AllValid() && discount_validity.AllValid() &&
		    shipdate_validity.AllValid()) {
			ProcessAlignedLineitem(partkeys, &partkey_sel, prices, &price_sel, discounts, &discount_sel, shipdates,
			                       &shipdate_sel, count, !skip_date_residual);
			return;
		}

		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto partkey_index = partkey_sel.get_index(row);
			const auto price_index = price_sel.get_index(row);
			const auto discount_index = discount_sel.get_index(row);
			const auto shipdate_index = shipdate_sel.get_index(row);
			if ((!partkey_validity.AllValid() && !partkey_validity.RowIsValidUnsafe(partkey_index)) ||
			    (!price_validity.AllValid() && !price_validity.RowIsValidUnsafe(price_index)) ||
			    (!discount_validity.AllValid() && !discount_validity.RowIsValidUnsafe(discount_index)) ||
			    (!shipdate_validity.AllValid() && !shipdate_validity.RowIsValidUnsafe(shipdate_index))) {
				continue;
			}
			ProcessLineitemRow(partkeys[partkey_index], prices[price_index], discounts[discount_index],
			                   shipdates[shipdate_index], !skip_date_residual);
		}
	}

	void ConsumeLineitemGeneric(duckdb::Vector &partkey_vector, duckdb::Vector &price_vector,
	                            duckdb::Vector &discount_vector, duckdb::Vector &shipdate_vector, duckdb::idx_t count,
	                            Clock::time_point decode_start) {
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat price_format;
		duckdb::UnifiedVectorFormat discount_format;
		duckdb::UnifiedVectorFormat shipdate_format;
		const auto unified_start = Clock::now();
		partkey_vector.ToUnifiedFormat(count, partkey_format);
		price_vector.ToUnifiedFormat(count, price_format);
		discount_vector.ToUnifiedFormat(count, discount_format);
		shipdate_vector.ToUnifiedFormat(count, shipdate_format);
		const auto unified_end = Clock::now();
		if (profile) {
			profile->lineitem_scan_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(unified_start - decode_start).count();
			profile->lineitem_unified_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(unified_end - unified_start).count();
		}

		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto prices = duckdb::UnifiedVectorFormat::GetData<int64_t>(price_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);

		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto price_index = price_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			const auto shipdate_index = shipdate_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!price_format.validity.AllValid() && !price_format.validity.RowIsValidUnsafe(price_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index)) ||
			    (!shipdate_format.validity.AllValid() && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index))) {
				continue;
			}
			ProcessLineitemRow(partkeys[partkey_index], prices[price_index], discounts[discount_index],
			                   shipdates[shipdate_index], true);
		}
	}

	void ProcessAlignedLineitem(const int64_t *partkeys, const duckdb::SelectionVector *partkey_sel,
	                            const int64_t *prices, const duckdb::SelectionVector *price_sel,
	                            const int64_t *discounts, const duckdb::SelectionVector *discount_sel,
	                            const duckdb::date_t *shipdates, const duckdb::SelectionVector *shipdate_sel,
	                            duckdb::idx_t count, bool require_date_residual) {
		const bool identity = partkey_sel == nullptr;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto shipdate_index = identity ? row : shipdate_sel->get_index(row);
			const auto shipdate = shipdates[shipdate_index];
			if (require_date_residual) {
				if (profile) {
					profile->residual_date_checks += 1;
				}
				if (!(shipdate >= params.start_date && shipdate < params.end_date)) {
					if (profile) {
						profile->residual_date_rejects += 1;
					}
					continue;
				}
			} else if (profile) {
				profile->residual_date_skips += 1;
			}
			if (profile) {
				profile->date_survivors += 1;
			}

			const auto partkey_index = identity ? row : partkey_sel->get_index(row);
			const auto partkey = partkeys[partkey_index];
			PromoClass promo;
			if (part_promo.CanProbeUnchecked(partkey)) {
				promo = part_promo.ProbeNonNegUnchecked(static_cast<uint64_t>(partkey));
			} else {
				promo = part_promo.Probe(partkey);
			}
			if (promo == PromoClass::Absent) {
				if (profile) {
					profile->join_misses += 1;
				}
				continue;
			}
			if (profile) {
				profile->join_hits += 1;
			}

			const auto price_index = identity ? row : price_sel->get_index(row);
			const auto discount_index = identity ? row : discount_sel->get_index(row);
			const auto revenue = DiscountedRevenueRaw(prices[price_index], discounts[discount_index]);
			total_revenue_raw += revenue;
			saw_contribution = true;
			if (promo == PromoClass::Promo) {
				promo_revenue_raw += revenue;
				if (profile) {
					profile->promo_survivors += 1;
				}
			}
		}
	}

	void ProcessLineitemRow(int64_t partkey, int64_t extendedprice_raw, int64_t discount_raw, duckdb::date_t shipdate,
	                        bool require_date_residual) {
		if (require_date_residual) {
			if (profile) {
				profile->residual_date_checks += 1;
			}
			if (!(shipdate >= params.start_date && shipdate < params.end_date)) {
				if (profile) {
					profile->residual_date_rejects += 1;
				}
				return;
			}
		} else if (profile) {
			profile->residual_date_skips += 1;
		}
		if (profile) {
			profile->date_survivors += 1;
		}

		PromoClass promo;
		if (part_promo.CanProbeUnchecked(partkey)) {
			promo = part_promo.ProbeNonNegUnchecked(static_cast<uint64_t>(partkey));
		} else {
			promo = part_promo.Probe(partkey);
		}
		if (promo == PromoClass::Absent) {
			if (profile) {
				profile->join_misses += 1;
			}
			return;
		}
		if (profile) {
			profile->join_hits += 1;
		}

		const auto revenue = DiscountedRevenueRaw(extendedprice_raw, discount_raw);
		total_revenue_raw += revenue;
		saw_contribution = true;
		if (promo == PromoClass::Promo) {
			promo_revenue_raw += revenue;
			if (profile) {
				profile->promo_survivors += 1;
			}
		}
	}

	Q14Parameters params;
	Q14ProfileCounters *profile;
	Q14RuntimeConfig config;
	PartPromoStore part_promo;
	AccT promo_revenue_raw = 0;
	AccT total_revenue_raw = 0;
	bool saw_contribution = false;
	bool result_is_null = false;
	bool denominator_is_zero = false;
	double result_value = 0.0;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

bool Q14DensePartEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q14_OPTS
	return ReadBoolEnv("ACCEL_Q14_DENSE_PART", true);
#else
	return true;
#endif
}

bool Q14PartDictFastEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q14_OPTS
	return ReadBoolEnv("ACCEL_Q14_PART_DICT_FAST", true);
#else
	return true;
#endif
}

bool Q14LineitemDictFastEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q14_OPTS
	return ReadBoolEnv("ACCEL_Q14_LINEITEM_DICT_FAST", true);
#else
	return true;
#endif
}

bool Q14SkipBaseDateResidualEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q14_OPTS
	return ReadBoolEnv("ACCEL_Q14_SKIP_BASE_DATE_RESIDUAL", false);
#else
	return false;
#endif
}

duckdb::unique_ptr<KernelInstance> CreateQ14Kernel(const Q14Parameters &params) {
	return duckdb::make_uniq<Q14Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ14ProfilingKernel(const Q14Parameters &params,
                                                            Q14ProfileCounters &counters) {
	return duckdb::make_uniq<Q14Kernel>(params, &counters);
}

} // namespace accelerator_codegen
