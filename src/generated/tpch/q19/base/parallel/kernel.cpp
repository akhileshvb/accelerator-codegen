#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "tpch/q19/base/query.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

#if defined(__SIZEOF_INT128__)
using AccT = __int128_t;
#else
using AccT = duckdb::hugeint_t;
#endif

constexpr size_t kDefaultPartCapacity = 200001;

struct BranchBounds {
	const std::string *brand = nullptr;
	int64_t qty_low_raw = 0;
	int64_t qty_high_raw = 0;
	int32_t size_low = 0;
	int32_t size_high = 0;
	std::array<const char *, 4> containers {};
};

static const std::array<const char *, 4> kSmContainers = {"SM CASE", "SM BOX", "SM PACK", "SM PKG"};
static const std::array<const char *, 4> kMedContainers = {"MED BAG", "MED BOX", "MED PKG", "MED PACK"};
static const std::array<const char *, 4> kLgContainers = {"LG CASE", "LG BOX", "LG PACK", "LG PKG"};

bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

bool StringEquals(const duckdb::string_t &lhs, const char *rhs) {
	const auto rhs_len = std::strlen(rhs);
	return lhs.GetSize() == rhs_len && std::memcmp(lhs.GetData(), rhs, rhs_len) == 0;
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
	PartBranchStore() {
		branch_masks.assign(kDefaultPartCapacity, 0);
		capacity = branch_masks.size();
	}

	void Store(int64_t partkey, uint8_t mask) {
		if (partkey < 0) {
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
		branch_masks[key] = mask;
	}

	bool CanProbeUnchecked(int64_t partkey) const {
		return partkey >= 0 && static_cast<size_t>(partkey) < capacity;
	}

	uint8_t Probe(int64_t partkey) const {
		if (partkey < 0) {
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

	uint8_t ProbeNonNegUnchecked(uint64_t partkey) const {
		return branch_masks[partkey];
	}

	void ReserveAtLeast(int64_t max_partkey) {
		if (max_partkey >= 0) {
			EnsureCapacity(static_cast<size_t>(max_partkey) + 1);
		}
	}

private:
	void EnsureCapacity(size_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		size_t target = new_capacity;
		if (capacity > 0) {
			const size_t doubled = capacity > (std::numeric_limits<size_t>::max() / 2)
			                           ? std::numeric_limits<size_t>::max()
			                           : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		branch_masks.resize(target, 0);
		capacity = branch_masks.size();
	}

	size_t capacity = 0;
	std::vector<uint8_t> branch_masks;
	std::unordered_map<int64_t, uint8_t> overflow;
};

struct Q19WorkerAgg {
	AccT revenue = 0;
	bool has_revenue = false;
};

struct Q19ProbeState {
	const Q19Parameters &params;
	std::array<BranchBounds, 3> branches;
	PartBranchStore part_branches;

	explicit Q19ProbeState(const Q19Parameters &params_p)
	    : params(params_p), part_branches() {
		branches[0] = {&params.brand1, params.qty_low1 * 100, (params.qty_low1 + 10) * 100, 1, 5, kSmContainers};
		branches[1] = {&params.brand2, params.qty_low2 * 100, (params.qty_low2 + 10) * 100, 1, 10, kMedContainers};
		branches[2] = {&params.brand3, params.qty_low3 * 100, (params.qty_low3 + 10) * 100, 1, 15, kLgContainers};
	}

	uint8_t ClassifyPart(const duckdb::string_t &brand, const duckdb::string_t &container, int32_t size) const {
		uint8_t mask = 0;
		for (duckdb::idx_t branch = 0; branch < 3; branch++) {
			const auto &spec = branches[branch];
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

	bool MatchesBranchMask(uint8_t mask, int64_t quantity_raw) const {
		for (duckdb::idx_t branch = 0; branch < 3; branch++) {
			if ((mask & static_cast<uint8_t>(1U << branch)) == 0) {
				continue;
			}
			const auto &spec = branches[branch];
			if (quantity_raw >= spec.qty_low_raw && quantity_raw <= spec.qty_high_raw) {
				return true;
			}
		}
		return false;
	}
};

void ConsumePartChunk(duckdb::DataChunk &chunk, Q19ProbeState &state) {
	auto &partkey_vector = chunk.data[Q19PartChunkLayout::PARTKEY_INDEX];
	auto &brand_vector = chunk.data[Q19PartChunkLayout::BRAND_INDEX];
	auto &container_vector = chunk.data[Q19PartChunkLayout::CONTAINER_INDEX];
	auto &size_vector = chunk.data[Q19PartChunkLayout::SIZE_INDEX];

	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(brand_vector) && IsFlatAllValid(container_vector) &&
	    IsFlatAllValid(size_vector)) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(brand_vector);
		auto containers = duckdb::FlatVector::GetData<duckdb::string_t>(container_vector);
		auto sizes = duckdb::FlatVector::GetData<int32_t>(size_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto mask = state.ClassifyPart(brands[row], containers[row], sizes[row]);
			if (mask != 0) {
				state.part_branches.Store(partkeys[row], mask);
			}
		}
		return;
	}

	if (IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(brand_vector) &&
	    IsDictionaryOverFlat(container_vector) && IsDictionaryOverFlat(size_vector)) {
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &brand_sel = duckdb::DictionaryVector::SelVector(brand_vector);
		auto &container_sel = duckdb::DictionaryVector::SelVector(container_vector);
		auto &size_sel = duckdb::DictionaryVector::SelVector(size_vector);
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
		auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(brand_vector));
		auto containers =
		    duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(container_vector));
		auto sizes = duckdb::FlatVector::GetData<int32_t>(duckdb::DictionaryVector::Child(size_vector));
		auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
		auto &brand_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(brand_vector));
		auto &container_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(container_vector));
		auto &size_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(size_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey_index = partkey_sel.get_index(row);
			const auto brand_index = brand_sel.get_index(row);
			const auto container_index = container_sel.get_index(row);
			const auto size_index = size_sel.get_index(row);
			if (!partkey_validity.RowIsValid(partkey_index) || !brand_validity.RowIsValid(brand_index) ||
			    !container_validity.RowIsValid(container_index) || !size_validity.RowIsValid(size_index)) {
				continue;
			}
			const auto mask = state.ClassifyPart(brands[brand_index], containers[container_index], sizes[size_index]);
			if (mask != 0) {
				state.part_branches.Store(partkeys[partkey_index], mask);
			}
		}
		return;
	}

	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat brand_format;
	duckdb::UnifiedVectorFormat container_format;
	duckdb::UnifiedVectorFormat size_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	brand_vector.ToUnifiedFormat(chunk.size(), brand_format);
	container_vector.ToUnifiedFormat(chunk.size(), container_format);
	size_vector.ToUnifiedFormat(chunk.size(), size_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto sizes = duckdb::UnifiedVectorFormat::GetData<int32_t>(size_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto brand_index = brand_format.sel->get_index(row);
		const auto container_index = container_format.sel->get_index(row);
		const auto size_index = size_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!brand_format.validity.AllValid() && !brand_format.validity.RowIsValidUnsafe(brand_index)) ||
		    (!container_format.validity.AllValid() &&
		     !container_format.validity.RowIsValidUnsafe(container_index)) ||
		    (!size_format.validity.AllValid() && !size_format.validity.RowIsValidUnsafe(size_index))) {
			continue;
		}
		const auto mask = state.ClassifyPart(
		    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(brand_format)[brand_index],
		    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(container_format)[container_index],
		    sizes[size_index]);
		if (mask != 0) {
			state.part_branches.Store(partkeys[partkey_index], mask);
		}
	}
}

int64_t EstimateTpchMaxPartkey(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	NativeParallelScanHandle handle;
	handle.Initialize(context, bind_data);
	return static_cast<int64_t>(handle.storage->GetTotalRows());
}

void BuildPartBranchStore(duckdb::ClientContext &context, const duckdb::FunctionData &part_bind, Q19ProbeState &state,
                          duckdb::idx_t worker_count) {
	state.part_branches.ReserveAtLeast(EstimateTpchMaxPartkey(context, part_bind));
	if (worker_count <= 2) {
		auto source = CreateNativeChunkSource(context, part_bind);
		while (source->Next()) {
			ConsumePartChunk(source->Chunk(), state);
		}
		return;
	}
	RunParallelChunkScan(context, part_bind, worker_count, [&](duckdb::idx_t, duckdb::DataChunk &chunk) {
		ConsumePartChunk(chunk, state);
	});
}

bool ProcessLineitemRow(const Q19ProbeState &state, Q19WorkerAgg &agg, int64_t partkey, int64_t quantity_raw,
                        int64_t extendedprice_raw, int64_t discount_raw, const duckdb::string_t &shipmode,
                        const duckdb::string_t &shipinstruct) {
	if (!StringEquals(shipinstruct, "DELIVER IN PERSON")) {
		return false;
	}
	if (!ShipmodeMatches(shipmode)) {
		return false;
	}
	uint8_t branch_mask;
	if (state.part_branches.CanProbeUnchecked(partkey)) {
		branch_mask = state.part_branches.ProbeNonNegUnchecked(static_cast<uint64_t>(partkey));
	} else {
		branch_mask = state.part_branches.Probe(partkey);
	}
	if (branch_mask == 0 || !state.MatchesBranchMask(branch_mask, quantity_raw)) {
		return false;
	}
#if defined(__SIZEOF_INT128__)
	agg.revenue += static_cast<AccT>(extendedprice_raw) * static_cast<AccT>(100 - discount_raw);
	agg.has_revenue = true;
#else
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while computing Q19 revenue product");
	}
	if (!agg.has_revenue) {
		agg.revenue = product_value;
		agg.has_revenue = true;
	} else if (!duckdb::Hugeint::TryAddInPlace(agg.revenue, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q19 revenue");
	}
#endif
	return true;
}

void AccumulateLineitemChunk(duckdb::DataChunk &chunk, const Q19ProbeState &state, Q19WorkerAgg &agg) {
	auto &partkey_vector = chunk.data[Q19LineitemChunkLayout::PARTKEY_INDEX];
	auto &quantity_vector = chunk.data[Q19LineitemChunkLayout::QUANTITY_INDEX];
	auto &extendedprice_vector = chunk.data[Q19LineitemChunkLayout::EXTENDEDPRICE_INDEX];
	auto &discount_vector = chunk.data[Q19LineitemChunkLayout::DISCOUNT_INDEX];
	auto &shipmode_vector = chunk.data[Q19LineitemChunkLayout::SHIPMODE_INDEX];
	auto &shipinstruct_vector = chunk.data[Q19LineitemChunkLayout::SHIPINSTRUCT_INDEX];

	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(quantity_vector) && IsFlatAllValid(extendedprice_vector) &&
	    IsFlatAllValid(discount_vector) && IsFlatAllValid(shipmode_vector) && IsFlatAllValid(shipinstruct_vector)) {
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		auto shipmodes = duckdb::FlatVector::GetData<duckdb::string_t>(shipmode_vector);
		auto shipinstructs = duckdb::FlatVector::GetData<duckdb::string_t>(shipinstruct_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			(void)ProcessLineitemRow(state, agg, partkeys[row], quantities[row], extendedprices[row], discounts[row],
			                         shipmodes[row], shipinstructs[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(quantity_vector) &&
	    IsDictionaryOverFlat(extendedprice_vector) && IsDictionaryOverFlat(discount_vector) &&
	    IsDictionaryOverFlat(shipmode_vector) && IsDictionaryOverFlat(shipinstruct_vector)) {
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
		auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
		auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
		auto &shipmode_sel = duckdb::DictionaryVector::SelVector(shipmode_vector);
		auto &shipinstruct_sel = duckdb::DictionaryVector::SelVector(shipinstruct_vector);
		const bool shared_sel = &partkey_sel == &quantity_sel && &partkey_sel == &extendedprice_sel &&
		                        &partkey_sel == &discount_sel && &partkey_sel == &shipmode_sel &&
		                        &partkey_sel == &shipinstruct_sel;
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
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
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
				continue;
			}
			(void)ProcessLineitemRow(state, agg, partkeys[partkey_index], quantities[quantity_index],
			                         extendedprices[extendedprice_index], discounts[discount_index],
			                         shipmodes[shipmode_index], shipinstructs[shipinstruct_index]);
		}
		(void)shared_sel;
		return;
	}

	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	duckdb::UnifiedVectorFormat discount_format;
	duckdb::UnifiedVectorFormat shipmode_format;
	duckdb::UnifiedVectorFormat shipinstruct_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	quantity_vector.ToUnifiedFormat(chunk.size(), quantity_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	shipmode_vector.ToUnifiedFormat(chunk.size(), shipmode_format);
	shipinstruct_vector.ToUnifiedFormat(chunk.size(), shipinstruct_format);

	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
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
			continue;
		}
		(void)ProcessLineitemRow(
		    state, agg, partkeys[partkey_index], quantities[quantity_index], extendedprices[extendedprice_index],
		    discounts[discount_index],
		    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipmode_format)[shipmode_index],
		    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(shipinstruct_format)[shipinstruct_index]);
	}
}

class Q19ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q19ParallelKernel(Q19Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q19 parallel expects bound part and lineitem scans");
		}

		using clock = std::chrono::steady_clock;
		Q19ProbeState probe_state(params);

		const auto worker_count = ParallelWorkerCount(context);

		const auto part_t0 = clock::now();
		BuildPartBranchStore(context, *bound_inputs[0], probe_state, worker_count);
		const auto part_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - part_t0).count();

		std::vector<CacheLinePadded<Q19WorkerAgg>> workers(worker_count);

		const auto lineitem_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     AccumulateLineitemChunk(chunk, probe_state, workers[worker_id].value);
		                     });
		const auto lineitem_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

		revenue_sum = duckdb::Hugeint::Convert(0);
		has_revenue = false;
#if defined(__SIZEOF_INT128__)
		AccT merged = 0;
		for (const auto &worker : workers) {
			if (worker.value.has_revenue) {
				merged += worker.value.revenue;
				has_revenue = true;
			}
		}
		if (has_revenue) {
			revenue_sum = AccToHugeint(merged);
		}
#else
		for (const auto &worker : workers) {
			if (!worker.value.has_revenue) {
				continue;
			}
			if (!has_revenue) {
				revenue_sum = worker.value.revenue;
				has_revenue = true;
			} else if (!duckdb::Hugeint::TryAddInPlace(revenue_sum, worker.value.revenue)) {
				throw duckdb::OutOfRangeException("Overflow while merging Q19 revenue");
			}
		}
#endif

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			std::cerr << "q19_parallel_profile part_ns=" << part_ns << " lineitem_ns=" << lineitem_ns
			          << " worker_count=" << worker_count << "\n";
		}
	}

	void Finalize() override {
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
	Q19Parameters params;
	duckdb::hugeint_t revenue_sum = duckdb::Hugeint::Convert(0);
	bool has_revenue = false;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ19ParallelKernel(const Q19Parameters &params) {
	return duckdb::make_uniq<Q19ParallelKernel>(params);
}

} // namespace accelerator_codegen
