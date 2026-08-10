#include "tpch/q19/base/query.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

using Clock = std::chrono::steady_clock;
#if defined(__SIZEOF_INT128__)
using AccT = __int128_t;
#else
using AccT = duckdb::hugeint_t;
#endif

constexpr size_t kDefaultPartCapacity = 200001;

struct Q19RuntimeConfig {
	bool dense_part = true;
	bool part_dict_fast = true;
	bool lineitem_dict_fast = true;
};

#ifdef ACCEL_EXPERIMENTAL_Q19_OPTS
bool ReadBoolEnv(const char *name, bool default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}
#endif

Q19RuntimeConfig ReadQ19RuntimeConfig() {
	Q19RuntimeConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q19_OPTS
	config.dense_part = ReadBoolEnv("ACCEL_Q19_DENSE_PART", true);
	config.part_dict_fast = ReadBoolEnv("ACCEL_Q19_PART_DICT_FAST", true);
	config.lineitem_dict_fast = ReadBoolEnv("ACCEL_Q19_LINEITEM_DICT_FAST", true);
#endif
	return config;
}

struct BranchBounds {
	const std::string *brand = nullptr;
	int64_t qty_low_raw = 0;
	int64_t qty_high_raw = 0;
	int32_t size_low = 0;
	int32_t size_high = 0;
	std::array<const char *, 4> containers {};
};

enum class ContainerFamily : uint8_t {
	None = 0,
	SM = 1,
	MED = 2,
	LG = 3,
};

constexpr uint8_t kContainerBitSM = 1;
constexpr uint8_t kContainerBitMED = 2;
constexpr uint8_t kContainerBitLG = 4;

static const std::array<const char *, 4> kSmContainers = {"SM CASE", "SM BOX", "SM PACK", "SM PKG"};
static const std::array<const char *, 4> kMedContainers = {"MED BAG", "MED BOX", "MED PKG", "MED PACK"};
static const std::array<const char *, 4> kLgContainers = {"LG CASE", "LG BOX", "LG PACK", "LG PKG"};

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

void RecordLayout(Q19LayoutHistogram *histogram, duckdb::Vector &vector) {
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

void TrackPath(Q19InputProfile *profile, duckdb::DataChunk &input, int path) {
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

bool StringEquals(const duckdb::string_t &lhs, const char *rhs) {
	const auto rhs_len = std::strlen(rhs);
	return lhs.GetSize() == rhs_len && std::memcmp(lhs.GetData(), rhs, rhs_len) == 0;
}

bool ContainerInExactSet(const duckdb::string_t &container, const std::array<const char *, 4> &containers) {
	for (const auto *candidate : containers) {
		if (StringEquals(container, candidate)) {
			return true;
		}
	}
	return false;
}

bool ShipmodeMatches(const duckdb::string_t &shipmode) {
	return StringEquals(shipmode, "AIR") || StringEquals(shipmode, "AIR REG");
}

duckdb::hugeint_t AccToHugeint(AccT value) {
#if defined(__SIZEOF_INT128__)
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
#else
	return value;
#endif
}

class PartBranchStore {
public:
	explicit PartBranchStore(bool use_dense_p) : use_dense(use_dense_p) {
	}

	int64_t InitializeNs() {
		if (!use_dense || initialized) {
			return 0;
		}
		const auto start = Clock::now();
		branch_masks.assign(kDefaultPartCapacity, 0);
		capacity = branch_masks.size();
		initialized = true;
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
	}

	void Store(int64_t partkey, uint8_t mask) {
		if (!use_dense || partkey < 0) {
			overflow[partkey] = mask;
			return;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key >= capacity) {
			if (partkey > 1'000'000'000) {
				overflow[partkey] = mask;
				return;
			}
			EnsureCapacity(key + 1);
		}
		if (branch_masks[key] == 0) {
			stored_count += 1;
		}
		branch_masks[key] = mask;
	}

	inline void StoreNonNegUnchecked(uint64_t partkey, uint8_t mask) {
		if (branch_masks[partkey] == 0) {
			stored_count += 1;
		}
		branch_masks[partkey] = mask;
	}

	bool CanProbeUnchecked(int64_t partkey) const {
		return use_dense && partkey >= 0 && static_cast<size_t>(partkey) < capacity;
	}

	uint8_t Probe(int64_t partkey) const {
		if (!use_dense || partkey < 0) {
			const auto it = overflow.find(partkey);
			return it == overflow.end() ? 0 : it->second;
		}
		const auto key = static_cast<size_t>(partkey);
		if (key < capacity) {
			return branch_masks[key];
		}
		const auto it = overflow.find(partkey);
		return it == overflow.end() ? 0 : it->second;
	}

	inline uint8_t ProbeNonNegUnchecked(uint64_t partkey) const {
		return branch_masks[partkey];
	}

	duckdb::idx_t Cardinality() const {
		return stored_count + overflow.size();
	}

	duckdb::idx_t MemoryBytes() const {
		return branch_masks.size() + overflow.size() * sizeof(typename decltype(overflow)::value_type) +
		       overflow.bucket_count() * sizeof(void *);
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
		branch_masks.resize(target, 0);
		capacity = branch_masks.size();
	}

	bool use_dense;
	bool initialized = false;
	size_t capacity = 0;
	duckdb::idx_t stored_count = 0;
	std::vector<uint8_t> branch_masks;
	std::unordered_map<int64_t, uint8_t> overflow;
};

class Q19Kernel final : public KernelInstance {
public:
	explicit Q19Kernel(Q19Parameters params_p, Q19ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(ReadQ19RuntimeConfig()),
	      part_branches(config.dense_part) {
		branches_[0] = {&params.brand1, params.qty_low1 * 100, (params.qty_low1 + 10) * 100, 1, 5, kSmContainers};
		branches_[1] = {&params.brand2, params.qty_low2 * 100, (params.qty_low2 + 10) * 100, 1, 10, kMedContainers};
		branches_[2] = {&params.brand3, params.qty_low3 * 100, (params.qty_low3 + 10) * 100, 1, 15, kLgContainers};
		const auto init_ns = part_branches.InitializeNs();
		if (profile) {
			profile->part_state_init_ns += init_ns;
			profile->used_dense_part = part_branches.UsesDense();
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q19InputId>(input_id)) {
		case Q19InputId::PART:
			ConsumePartChunk(input);
			return;
		case Q19InputId::LINEITEM:
			ConsumeLineitemChunk(input);
			return;
		default:
			throw duckdb::InternalException("Q19 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		const auto start = Clock::now();
		if (profile) {
			profile->part_map_cardinality = part_branches.Cardinality();
			profile->part_map_memory_bytes = part_branches.MemoryBytes();
			profile->part_overflow_entries = part_branches.OverflowSize();
			profile->finalize_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
		}
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		output.SetCardinality(1);
		if (!has_revenue) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DECIMAL(38, 4)));
		} else {
			output.SetValue(0, 0, duckdb::Value::DECIMAL(revenue_sum, 38, 4));
		}
		emitted = true;
		return true;
	}

private:
	uint8_t ClassifyPart(const duckdb::string_t &brand, const duckdb::string_t &container, int32_t size) const {
		uint8_t mask = 0;
		for (duckdb::idx_t branch = 0; branch < 3; branch++) {
			const auto &spec = branches_[branch];
			if (!StringEquals(brand, *spec.brand)) {
				continue;
			}
			if (!ContainerInExactSet(container, spec.containers)) {
				continue;
			}
			if (size < spec.size_low || size > spec.size_high) {
				continue;
			}
			mask |= static_cast<uint8_t>(1U << branch);
		}
		return mask;
	}

	void StorePart(int64_t partkey, uint8_t mask) {
		if (mask == 0) {
			return;
		}
		part_branches.Store(partkey, mask);
		if (profile) {
			profile->part_rows_stored += 1;
		}
	}

	void ConsumePartChunk(duckdb::DataChunk &input) {
		const auto build_start = Clock::now();
		if (profile) {
			RecordLayout(&profile->layout_p_partkey, input.data[Q19PartChunkLayout::PARTKEY_INDEX]);
			RecordLayout(&profile->layout_p_brand, input.data[Q19PartChunkLayout::BRAND_INDEX]);
			profile->part_rows_scanned += input.size();
		}

		auto &partkey_vector = input.data[Q19PartChunkLayout::PARTKEY_INDEX];
		auto &brand_vector = input.data[Q19PartChunkLayout::BRAND_INDEX];
		auto &container_vector = input.data[Q19PartChunkLayout::CONTAINER_INDEX];
		auto &size_vector = input.data[Q19PartChunkLayout::SIZE_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(brand_vector) && IsFlatAllValid(container_vector) &&
		    IsFlatAllValid(size_vector)) {
			TrackPath(profile ? &profile->part : nullptr, input, 0);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(brand_vector);
			auto containers = duckdb::FlatVector::GetData<duckdb::string_t>(container_vector);
			auto sizes = duckdb::FlatVector::GetData<int32_t>(size_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				StorePart(partkeys[row], ClassifyPart(brands[row], containers[row], sizes[row]));
			}
			if (profile) {
				profile->part_build_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - build_start).count();
			}
			return;
		}

		if (config.part_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(brand_vector) &&
		    IsDictionaryOverFlat(container_vector) && IsDictionaryOverFlat(size_vector)) {
			TrackPath(profile ? &profile->part : nullptr, input, 1);
			if (profile) {
				profile->used_part_dict_fast = true;
			}
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &brand_sel = duckdb::DictionaryVector::SelVector(brand_vector);
			auto &container_sel = duckdb::DictionaryVector::SelVector(container_vector);
			auto &size_sel = duckdb::DictionaryVector::SelVector(size_vector);
			const bool shared_sel = &partkey_sel == &brand_sel && &partkey_sel == &container_sel &&
			                        &partkey_sel == &size_sel;
			if (profile && shared_sel) {
				profile->part.shared_sel_chunks += 1;
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(brand_vector));
			auto containers =
			    duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(container_vector));
			auto sizes = duckdb::FlatVector::GetData<int32_t>(duckdb::DictionaryVector::Child(size_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &brand_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(brand_vector));
			auto &container_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(container_vector));
			auto &size_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(size_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto brand_index = brand_sel.get_index(row);
				const auto container_index = container_sel.get_index(row);
				const auto size_index = size_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !brand_validity.RowIsValid(brand_index) ||
				    !container_validity.RowIsValid(container_index) || !size_validity.RowIsValid(size_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				StorePart(partkeys[partkey_index],
				          ClassifyPart(brands[brand_index], containers[container_index], sizes[size_index]));
			}
			if (profile) {
				profile->part_build_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - build_start).count();
			}
			return;
		}

		TrackPath(profile ? &profile->part : nullptr, input, 2);
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat brand_format;
		duckdb::UnifiedVectorFormat container_format;
		duckdb::UnifiedVectorFormat size_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		brand_vector.ToUnifiedFormat(input.size(), brand_format);
		container_vector.ToUnifiedFormat(input.size(), container_format);
		size_vector.ToUnifiedFormat(input.size(), size_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto sizes = duckdb::UnifiedVectorFormat::GetData<int32_t>(size_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto brand_index = brand_format.sel->get_index(row);
			const auto container_index = container_format.sel->get_index(row);
			const auto size_index = size_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!brand_format.validity.AllValid() && !brand_format.validity.RowIsValidUnsafe(brand_index)) ||
			    (!container_format.validity.AllValid() &&
			     !container_format.validity.RowIsValidUnsafe(container_index)) ||
			    (!size_format.validity.AllValid() && !size_format.validity.RowIsValidUnsafe(size_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto brand_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(brand_format)[brand_index];
			const auto container_value =
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(container_format)[container_index];
			StorePart(partkeys[partkey_index],
			          ClassifyPart(brand_value, container_value, sizes[size_index]));
		}
		if (profile) {
			profile->part_build_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - build_start).count();
		}
	}

	bool MatchesBranchMask(uint8_t mask, int64_t quantity_raw) const {
		for (duckdb::idx_t branch = 0; branch < 3; branch++) {
			if ((mask & static_cast<uint8_t>(1U << branch)) == 0) {
				continue;
			}
			const auto &spec = branches_[branch];
			if (quantity_raw >= spec.qty_low_raw && quantity_raw <= spec.qty_high_raw) {
				return true;
			}
		}
		return false;
	}

	void AddRevenue(int64_t extendedprice_raw, int64_t discount_raw) {
#if defined(__SIZEOF_INT128__)
		const AccT contribution =
		    static_cast<AccT>(extendedprice_raw) * static_cast<AccT>(100 - discount_raw);
		if (has_revenue) {
			AccT lhs = (static_cast<AccT>(revenue_sum.upper) << 64) + revenue_sum.lower;
			lhs += contribution;
			revenue_sum = AccToHugeint(lhs);
		} else {
			revenue_sum = AccToHugeint(contribution);
			has_revenue = true;
		}
#else
		duckdb::hugeint_t price_value;
		duckdb::hugeint_t factor_value;
		duckdb::hugeint_t product_value;
		if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
		    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
		    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value)) {
			throw duckdb::OutOfRangeException("Overflow while computing Q19 revenue product");
		}
		if (!has_revenue) {
			revenue_sum = product_value;
			has_revenue = true;
		} else if (!duckdb::Hugeint::TryAddInPlace(revenue_sum, product_value)) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q19 revenue");
		}
#endif
		if (profile) {
			profile->lineitem_rows_matched += 1;
		}
	}

	bool ProcessLineitemRow(int64_t partkey, int64_t quantity_raw, int64_t extendedprice_raw, int64_t discount_raw,
	                        const duckdb::string_t &shipmode, const duckdb::string_t &shipinstruct) {
		if (!StringEquals(shipinstruct, "DELIVER IN PERSON")) {
			if (profile) {
				profile->shipinstruct_rejected_rows += 1;
			}
			return false;
		}
		if (!ShipmodeMatches(shipmode)) {
			if (profile) {
				profile->shipmode_rejected_rows += 1;
			}
			return false;
		}
		const auto branch_mask = part_branches.Probe(partkey);
		if (branch_mask == 0) {
			if (profile) {
				profile->part_miss_rows += 1;
			}
			return false;
		}
		if (!MatchesBranchMask(branch_mask, quantity_raw)) {
			return false;
		}
		AddRevenue(extendedprice_raw, discount_raw);
		return true;
	}

	void ConsumeLineitemChunk(duckdb::DataChunk &input) {
		const auto probe_start = Clock::now();
		if (profile) {
			RecordLayout(&profile->layout_l_partkey, input.data[Q19LineitemChunkLayout::PARTKEY_INDEX]);
			RecordLayout(&profile->layout_l_quantity, input.data[Q19LineitemChunkLayout::QUANTITY_INDEX]);
			profile->lineitem_rows_scanned += input.size();
		}

		auto &partkey_vector = input.data[Q19LineitemChunkLayout::PARTKEY_INDEX];
		auto &quantity_vector = input.data[Q19LineitemChunkLayout::QUANTITY_INDEX];
		auto &extendedprice_vector = input.data[Q19LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q19LineitemChunkLayout::DISCOUNT_INDEX];
		auto &shipmode_vector = input.data[Q19LineitemChunkLayout::SHIPMODE_INDEX];
		auto &shipinstruct_vector = input.data[Q19LineitemChunkLayout::SHIPINSTRUCT_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(quantity_vector) && IsFlatAllValid(extendedprice_vector) &&
		    IsFlatAllValid(discount_vector) && IsFlatAllValid(shipmode_vector) && IsFlatAllValid(shipinstruct_vector)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, 0);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			auto shipmodes = duckdb::FlatVector::GetData<duckdb::string_t>(shipmode_vector);
			auto shipinstructs = duckdb::FlatVector::GetData<duckdb::string_t>(shipinstruct_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				(void)ProcessLineitemRow(partkeys[row], quantities[row], extendedprices[row], discounts[row],
				                         shipmodes[row], shipinstructs[row]);
			}
			if (profile) {
				profile->lineitem_probe_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - probe_start).count();
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(quantity_vector) &&
		    IsDictionaryOverFlat(extendedprice_vector) && IsDictionaryOverFlat(discount_vector) &&
		    IsDictionaryOverFlat(shipmode_vector) && IsDictionaryOverFlat(shipinstruct_vector)) {
			TrackPath(profile ? &profile->lineitem : nullptr, input, 1);
			if (profile) {
				profile->used_lineitem_dict_fast = true;
			}
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
			auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
			auto &shipmode_sel = duckdb::DictionaryVector::SelVector(shipmode_vector);
			auto &shipinstruct_sel = duckdb::DictionaryVector::SelVector(shipinstruct_vector);
			const bool shared_sel = &partkey_sel == &quantity_sel && &partkey_sel == &extendedprice_sel &&
			                        &partkey_sel == &discount_sel && &partkey_sel == &shipmode_sel &&
			                        &partkey_sel == &shipinstruct_sel;
			if (profile && shared_sel) {
				profile->lineitem.shared_sel_chunks += 1;
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto quantities = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(quantity_vector));
			auto extendedprices =
			    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
			auto shipmodes =
			    duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(shipmode_vector));
			auto shipinstructs =
			    duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(shipinstruct_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &quantity_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(quantity_vector));
			auto &extendedprice_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
			auto &shipmode_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(shipmode_vector));
			auto &shipinstruct_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(shipinstruct_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto quantity_index = quantity_sel.get_index(row);
				const auto extendedprice_index = extendedprice_sel.get_index(row);
				const auto discount_index = discount_sel.get_index(row);
				const auto shipmode_index = shipmode_sel.get_index(row);
				const auto shipinstruct_index = shipinstruct_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !quantity_validity.RowIsValid(quantity_index) ||
				    !extendedprice_validity.RowIsValid(extendedprice_index) ||
				    !discount_validity.RowIsValid(discount_index) || !shipmode_validity.RowIsValid(shipmode_index) ||
				    !shipinstruct_validity.RowIsValid(shipinstruct_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				(void)ProcessLineitemRow(partkeys[partkey_index], quantities[quantity_index],
				                         extendedprices[extendedprice_index], discounts[discount_index],
				                         shipmodes[shipmode_index], shipinstructs[shipinstruct_index]);
			}
			if (profile) {
				profile->lineitem_probe_ns +=
				    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - probe_start).count();
			}
			return;
		}

		TrackPath(profile ? &profile->lineitem : nullptr, input, 2);
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat quantity_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		duckdb::UnifiedVectorFormat shipmode_format;
		duckdb::UnifiedVectorFormat shipinstruct_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		quantity_vector.ToUnifiedFormat(input.size(), quantity_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		shipmode_vector.ToUnifiedFormat(input.size(), shipmode_format);
		shipinstruct_vector.ToUnifiedFormat(input.size(), shipinstruct_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto quantity_index = quantity_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			const auto shipmode_index = shipmode_format.sel->get_index(row);
			const auto shipinstruct_index = shipinstruct_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index)) ||
			    (!shipmode_format.validity.AllValid() && !shipmode_format.validity.RowIsValidUnsafe(shipmode_index)) ||
			    (!shipinstruct_format.validity.AllValid() &&
			     !shipinstruct_format.validity.RowIsValidUnsafe(shipinstruct_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto shipmode = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipmode_format)[shipmode_index];
			const auto shipinstruct =
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipinstruct_format)[shipinstruct_index];
			(void)ProcessLineitemRow(partkeys[partkey_index], quantities[quantity_index],
			                         extendedprices[extendedprice_index], discounts[discount_index], shipmode,
			                         shipinstruct);
		}
		if (profile) {
			profile->lineitem_probe_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - probe_start).count();
		}
	}

	Q19Parameters params;
	Q19ProfileCounters *profile;
	Q19RuntimeConfig config;
	PartBranchStore part_branches;
	std::array<BranchBounds, 3> branches_;
	duckdb::hugeint_t revenue_sum = duckdb::Hugeint::Convert(0);
	bool has_revenue = false;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ19Kernel(const Q19Parameters &params) {
	return duckdb::make_uniq<Q19Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ19ProfilingKernel(const Q19Parameters &params,
                                                            Q19ProfileCounters *profile) {
	return duckdb::make_uniq<Q19Kernel>(params, profile);
}

} // namespace accelerator_codegen
