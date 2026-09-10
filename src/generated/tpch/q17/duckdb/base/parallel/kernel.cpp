#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "tpch/q17/duckdb/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr size_t kDefaultPartCapacity = 200001;

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

bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

struct CompactQualifyingParts {
	std::vector<uint32_t> compact_index_by_partkey;
	size_t qualifying_count = 0;
	size_t partkey_capacity = 0;
	size_t parts_scanned = 0;
	size_t parts_matched = 0;

	void EnsurePartkeyCapacity(size_t required) {
		if (required <= partkey_capacity) {
			return;
		}
		size_t target = required;
		if (partkey_capacity > 0) {
			const size_t doubled = partkey_capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, kDefaultPartCapacity);
		}
		compact_index_by_partkey.resize(target, 0);
		partkey_capacity = target;
	}

	void MarkQualifying(int64_t partkey) {
		if (partkey < 0) {
			return;
		}
		const auto key = static_cast<size_t>(partkey);
		EnsurePartkeyCapacity(key + 1);
		if (compact_index_by_partkey[key] != 0) {
			return;
		}
		compact_index_by_partkey[key] = static_cast<uint32_t>(qualifying_count + 1);
		qualifying_count += 1;
		parts_matched += 1;
	}

	inline uint32_t CompactIndex(int64_t partkey) const {
		if (partkey < 0 || static_cast<size_t>(partkey) >= partkey_capacity) {
			return 0;
		}
		return compact_index_by_partkey[static_cast<size_t>(partkey)];
	}

	size_t LookupBytes() const {
		return compact_index_by_partkey.size() * sizeof(uint32_t);
	}
};

struct PassProfile {
	size_t rows_in = 0;
	size_t rows_qualifying = 0;
	size_t rows_threshold_survivors = 0;
	size_t flat_rows = 0;
	size_t dict_rows = 0;
	size_t fallback_rows = 0;
	size_t quantities_decoded = 0;
	size_t extendedprices_decoded = 0;
	int64_t process_ns = 0;
};

struct Q17ParallelProfile {
	int64_t part_ns = 0;
	int64_t compact_index_ns = 0;
	int64_t lineitem_agg_ns = 0;
	int64_t quantity_merge_ns = 0;
	int64_t threshold_ns = 0;
	int64_t lineitem_probe_ns = 0;
	int64_t final_reduce_ns = 0;
	size_t worker_count = 0;
	size_t parts_scanned = 0;
	size_t qualifying_parts = 0;
	size_t partkey_capacity = 0;
	size_t lineitem_agg_rows = 0;
	size_t lineitem_agg_qualifying_rows = 0;
	size_t lineitem_probe_rows = 0;
	size_t lineitem_probe_qualifying_rows = 0;
	size_t lineitem_probe_threshold_survivors = 0;
	size_t quantity_state_entries = 0;
	size_t quantity_state_bytes = 0;
	size_t quantity_worker_bytes = 0;
	size_t lookup_bytes = 0;
	size_t agg_flat_rows = 0;
	size_t agg_dict_rows = 0;
	size_t agg_fallback_rows = 0;
	size_t probe_flat_rows = 0;
	size_t probe_dict_rows = 0;
	size_t probe_fallback_rows = 0;
	size_t quantities_decoded = 0;
	size_t extendedprices_decoded = 0;
	int64_t min_worker_agg_ns = 0;
	int64_t max_worker_agg_ns = 0;
	int64_t min_worker_probe_ns = 0;
	int64_t max_worker_probe_ns = 0;
};

void BuildQualifyingParts(duckdb::ClientContext &context, const duckdb::FunctionData &part_bind,
                          const Q17Parameters &params, CompactQualifyingParts &qualifying, PassProfile &profile) {
	auto source = CreateNativeChunkSource(context, part_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		profile.rows_in += chunk.size();
		qualifying.parts_scanned += chunk.size();

		auto &partkey_vector = chunk.data[Q17PartChunkLayout::PARTKEY_INDEX];
		auto &brand_vector = chunk.data[Q17PartChunkLayout::BRAND_INDEX];
		auto &container_vector = chunk.data[Q17PartChunkLayout::CONTAINER_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(brand_vector) && IsFlatAllValid(container_vector)) {
			profile.flat_rows += chunk.size();
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(brand_vector);
			auto containers = duckdb::FlatVector::GetData<duckdb::string_t>(container_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				if (StringEquals(brands[row], params.brand) && StringEquals(containers[row], params.container)) {
					qualifying.MarkQualifying(partkeys[row]);
				}
			}
			continue;
		}

		profile.fallback_rows += chunk.size();
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat brand_format;
		duckdb::UnifiedVectorFormat container_format;
		partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
		brand_vector.ToUnifiedFormat(chunk.size(), brand_format);
		container_vector.ToUnifiedFormat(chunk.size(), container_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto brand_index = brand_format.sel->get_index(row);
			const auto container_index = container_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!brand_format.validity.AllValid() && !brand_format.validity.RowIsValidUnsafe(brand_index)) ||
			    (!container_format.validity.AllValid() &&
			     !container_format.validity.RowIsValidUnsafe(container_index))) {
				continue;
			}
			const auto brand_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(brand_format)[brand_index];
			const auto container_value =
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(container_format)[container_index];
			if (StringEquals(brand_value, params.brand) && StringEquals(container_value, params.container)) {
				qualifying.MarkQualifying(partkeys[partkey_index]);
			}
		}
	}
}

class WorkerQuantityState {
public:
	void Reset(size_t qualifying_count) {
		if (slots.size() != qualifying_count) {
			slots.assign(qualifying_count, PartAggSlot {});
		} else if (!slots.empty()) {
			std::fill(slots.begin(), slots.end(), PartAggSlot {});
		}
	}

	void Add(size_t compact_index, int64_t quantity_raw) {
		auto &slot = slots[compact_index];
		slot.sum_raw += quantity_raw;
		slot.count += 1;
	}

	const std::vector<PartAggSlot> &Slots() const {
		return slots;
	}

	size_t LogicalBytes() const {
		return slots.size() * sizeof(PartAggSlot);
	}

private:
	std::vector<PartAggSlot> slots;
};

void MergeQuantityStates(std::vector<PartAggSlot> &global, const WorkerQuantityState &local) {
	for (size_t index = 0; index < global.size(); index++) {
		global[index].sum_raw += local.Slots()[index].sum_raw;
		global[index].count += local.Slots()[index].count;
	}
}

void PrepareThresholds(const std::vector<PartAggSlot> &global, std::vector<double> &thresholds) {
	thresholds.resize(global.size(), 0.0);
	for (size_t index = 0; index < global.size(); index++) {
		const auto &slot = global[index];
		if (slot.count == 0) {
			continue;
		}
		const double avg_quantity = (static_cast<double>(slot.sum_raw) / 100.0) / static_cast<double>(slot.count);
		thresholds[index] = 0.2 * avg_quantity;
	}
}

void AccumulateLineitemAggChunk(duckdb::DataChunk &chunk, const CompactQualifyingParts &qualifying,
                                WorkerQuantityState &state, PassProfile &profile) {
	profile.rows_in += chunk.size();
	auto &partkey_vector = chunk.data[Q17LineitemAggChunkLayout::PARTKEY_INDEX];
	auto &quantity_vector = chunk.data[Q17LineitemAggChunkLayout::QUANTITY_INDEX];

	const auto accumulate_row = [&](int64_t partkey, int64_t quantity_raw) {
		const auto compact_index = qualifying.CompactIndex(partkey);
		if (compact_index == 0) {
			return;
		}
		state.Add(static_cast<size_t>(compact_index - 1), quantity_raw);
		profile.rows_qualifying += 1;
		profile.quantities_decoded += 1;
	};

	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(quantity_vector)) {
		profile.flat_rows += chunk.size();
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			accumulate_row(partkeys[row], quantities[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(quantity_vector)) {
		profile.dict_rows += chunk.size();
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
		auto quantities = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(quantity_vector));
		auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
		auto &quantity_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(quantity_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey_index = partkey_sel.get_index(row);
			const auto quantity_index = quantity_sel.get_index(row);
			if (!partkey_validity.RowIsValid(partkey_index) || !quantity_validity.RowIsValid(quantity_index)) {
				continue;
			}
			accumulate_row(partkeys[partkey_index], quantities[quantity_index]);
		}
		return;
	}

	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	quantity_vector.ToUnifiedFormat(chunk.size(), quantity_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto quantity_index = quantity_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
			continue;
		}
		accumulate_row(partkeys[partkey_index], quantities[quantity_index]);
	}
}

#if defined(__SIZEOF_INT128__)
using ProbeAccT = __int128_t;
#else
using ProbeAccT = duckdb::hugeint_t;
#endif

struct Q17WorkerProbeAgg {
	ProbeAccT extendedprice_sum = 0;
	bool has_sum = false;
};

void AccumulateLineitemProbeChunk(duckdb::DataChunk &chunk, const CompactQualifyingParts &qualifying,
                                  const std::vector<PartAggSlot> &global_agg, const std::vector<double> &thresholds,
                                  Q17WorkerProbeAgg &agg, PassProfile &profile) {
	profile.rows_in += chunk.size();
	auto &partkey_vector = chunk.data[Q17LineitemProbeChunkLayout::PARTKEY_INDEX];
	auto &quantity_vector = chunk.data[Q17LineitemProbeChunkLayout::QUANTITY_INDEX];
	auto &extendedprice_vector = chunk.data[Q17LineitemProbeChunkLayout::EXTENDEDPRICE_INDEX];

	const auto process_row = [&](int64_t partkey, int64_t quantity_raw, int64_t extendedprice_raw) {
		const auto compact_index = qualifying.CompactIndex(partkey);
		if (compact_index == 0) {
			return;
		}
		profile.rows_qualifying += 1;
		const auto slot_index = static_cast<size_t>(compact_index - 1);
		const auto &slot = global_agg[slot_index];
		if (slot.count == 0) {
			return;
		}
		profile.quantities_decoded += 1;
		const double quantity = static_cast<double>(quantity_raw) / 100.0;
		if (!(quantity < thresholds[slot_index])) {
			return;
		}
		profile.rows_threshold_survivors += 1;
		profile.extendedprices_decoded += 1;
#if defined(__SIZEOF_INT128__)
		agg.extendedprice_sum += static_cast<ProbeAccT>(extendedprice_raw);
		agg.has_sum = true;
#else
		duckdb::hugeint_t addend;
		if (!duckdb::Hugeint::TryConvert(extendedprice_raw, addend)) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q17 extendedprice");
		}
		if (!agg.has_sum) {
			agg.extendedprice_sum = addend;
			agg.has_sum = true;
		} else if (!duckdb::Hugeint::TryAddInPlace(agg.extendedprice_sum, addend)) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q17 extendedprice sum");
		}
#endif
	};

	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(quantity_vector) && IsFlatAllValid(extendedprice_vector)) {
		profile.flat_rows += chunk.size();
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
		auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			process_row(partkeys[row], quantities[row], extendedprices[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(quantity_vector) &&
	    IsDictionaryOverFlat(extendedprice_vector)) {
		profile.dict_rows += chunk.size();
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
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey_index = partkey_sel.get_index(row);
			const auto quantity_index = quantity_sel.get_index(row);
			const auto extendedprice_index = extendedprice_sel.get_index(row);
			if (!partkey_validity.RowIsValid(partkey_index) || !quantity_validity.RowIsValid(quantity_index) ||
			    !extendedprice_validity.RowIsValid(extendedprice_index)) {
				continue;
			}
			process_row(partkeys[partkey_index], quantities[quantity_index], extendedprices[extendedprice_index]);
		}
		return;
	}

	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	quantity_vector.ToUnifiedFormat(chunk.size(), quantity_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto quantity_index = quantity_format.sel->get_index(row);
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index)) ||
		    (!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index))) {
			continue;
		}
		process_row(partkeys[partkey_index], quantities[quantity_index], extendedprices[extendedprice_index]);
	}
}

void PrintQ17ParallelProfile(const Q17ParallelProfile &profile) {
	std::cerr << "q17_parallel_profile"
	          << " part_ns=" << profile.part_ns << " compact_index_ns=" << profile.compact_index_ns
	          << " lineitem_agg_ns=" << profile.lineitem_agg_ns << " quantity_merge_ns=" << profile.quantity_merge_ns
	          << " threshold_ns=" << profile.threshold_ns << " lineitem_probe_ns=" << profile.lineitem_probe_ns
	          << " final_reduce_ns=" << profile.final_reduce_ns << " worker_count=" << profile.worker_count
	          << " parts_scanned=" << profile.parts_scanned << " qualifying_parts=" << profile.qualifying_parts
	          << " partkey_capacity=" << profile.partkey_capacity << " lineitem_agg_rows=" << profile.lineitem_agg_rows
	          << " lineitem_agg_qualifying_rows=" << profile.lineitem_agg_qualifying_rows
	          << " lineitem_probe_rows=" << profile.lineitem_probe_rows
	          << " lineitem_probe_qualifying_rows=" << profile.lineitem_probe_qualifying_rows
	          << " lineitem_probe_threshold_survivors=" << profile.lineitem_probe_threshold_survivors
	          << " quantity_state_entries=" << profile.quantity_state_entries
	          << " quantity_state_bytes=" << profile.quantity_state_bytes
	          << " quantity_worker_bytes=" << profile.quantity_worker_bytes << " lookup_bytes=" << profile.lookup_bytes
	          << " agg_flat_rows=" << profile.agg_flat_rows << " agg_dict_rows=" << profile.agg_dict_rows
	          << " agg_fallback_rows=" << profile.agg_fallback_rows << " probe_flat_rows=" << profile.probe_flat_rows
	          << " probe_dict_rows=" << profile.probe_dict_rows << " probe_fallback_rows=" << profile.probe_fallback_rows
	          << " quantities_decoded=" << profile.quantities_decoded
	          << " extendedprices_decoded=" << profile.extendedprices_decoded
	          << " min_worker_agg_ns=" << profile.min_worker_agg_ns << " max_worker_agg_ns=" << profile.max_worker_agg_ns
	          << " min_worker_probe_ns=" << profile.min_worker_probe_ns
	          << " max_worker_probe_ns=" << profile.max_worker_probe_ns << "\n";
}

class Q17ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q17ParallelKernel(Q17Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 3 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2]) {
			throw duckdb::InternalException("Q17 parallel expects bound lineitem agg, part, and lineitem probe scans");
		}

		using clock = std::chrono::steady_clock;
		Q17ParallelProfile profile;
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);

		CompactQualifyingParts qualifying;
		PassProfile part_stats;
		const auto part_t0 = clock::now();
		BuildQualifyingParts(context, *bound_inputs[1], params, qualifying, part_stats);
		profile.part_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - part_t0).count();
		profile.parts_scanned = qualifying.parts_scanned;
		profile.qualifying_parts = qualifying.qualifying_count;
		profile.partkey_capacity = qualifying.partkey_capacity;
		profile.lookup_bytes = qualifying.LookupBytes();

		if (qualifying.qualifying_count == 0) {
			if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
				PrintQ17ParallelProfile(profile);
			}
			return;
		}

		const auto compact_t0 = clock::now();
		std::vector<PartAggSlot> global_agg(qualifying.qualifying_count);
		std::vector<double> thresholds;
		profile.compact_index_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - compact_t0).count();
		profile.quantity_state_entries = qualifying.qualifying_count;
		profile.quantity_state_bytes = global_agg.size() * sizeof(PartAggSlot);

		struct AggWorkerState {
			WorkerQuantityState quantity;
			PassProfile stats;
		};
		std::vector<CacheLinePadded<AggWorkerState>> agg_workers(static_cast<size_t>(worker_count));
		for (auto &worker : agg_workers) {
			worker.value.quantity.Reset(qualifying.qualifying_count);
		}

		const auto agg_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto chunk_t0 = clock::now();
			                     auto &worker = agg_workers[static_cast<size_t>(worker_id)].value;
			                     AccumulateLineitemAggChunk(chunk, qualifying, worker.quantity, worker.stats);
			                     worker.stats.process_ns +=
			                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0)
			                             .count();
		                     });
		profile.lineitem_agg_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - agg_t0).count();

		const auto merge_t0 = clock::now();
		std::vector<int64_t> agg_worker_ns(static_cast<size_t>(worker_count), 0);
		for (size_t worker_id = 0; worker_id < static_cast<size_t>(worker_count); worker_id++) {
			const auto &worker = agg_workers[worker_id].value;
			MergeQuantityStates(global_agg, worker.quantity);
			profile.lineitem_agg_rows += worker.stats.rows_in;
			profile.lineitem_agg_qualifying_rows += worker.stats.rows_qualifying;
			profile.agg_flat_rows += worker.stats.flat_rows;
			profile.agg_dict_rows += worker.stats.dict_rows;
			profile.agg_fallback_rows += worker.stats.fallback_rows;
			profile.quantities_decoded += worker.stats.quantities_decoded;
			profile.quantity_worker_bytes += worker.quantity.LogicalBytes();
			agg_worker_ns[worker_id] = worker.stats.process_ns;
		}
		agg_workers.clear();
		profile.quantity_merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();

		if (!agg_worker_ns.empty()) {
			profile.min_worker_agg_ns = *std::min_element(agg_worker_ns.begin(), agg_worker_ns.end());
			profile.max_worker_agg_ns = *std::max_element(agg_worker_ns.begin(), agg_worker_ns.end());
		}

		const auto threshold_t0 = clock::now();
		PrepareThresholds(global_agg, thresholds);
		profile.threshold_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - threshold_t0).count();

		struct ProbeWorkerState {
			Q17WorkerProbeAgg sum;
			PassProfile stats;
		};
		std::vector<CacheLinePadded<ProbeWorkerState>> probe_workers(static_cast<size_t>(worker_count));
		const auto probe_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[2], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto chunk_t0 = clock::now();
			                     auto &worker = probe_workers[static_cast<size_t>(worker_id)].value;
			                     AccumulateLineitemProbeChunk(chunk, qualifying, global_agg, thresholds, worker.sum,
			                                                  worker.stats);
			                     worker.stats.process_ns +=
			                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0)
			                             .count();
		                     });
		profile.lineitem_probe_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - probe_t0).count();

		std::vector<int64_t> probe_worker_ns(static_cast<size_t>(worker_count), 0);
		for (size_t worker_id = 0; worker_id < static_cast<size_t>(worker_count); worker_id++) {
			const auto &worker = probe_workers[worker_id].value;
			profile.lineitem_probe_rows += worker.stats.rows_in;
			profile.lineitem_probe_qualifying_rows += worker.stats.rows_qualifying;
			profile.lineitem_probe_threshold_survivors += worker.stats.rows_threshold_survivors;
			profile.probe_flat_rows += worker.stats.flat_rows;
			profile.probe_dict_rows += worker.stats.dict_rows;
			profile.probe_fallback_rows += worker.stats.fallback_rows;
			profile.extendedprices_decoded += worker.stats.extendedprices_decoded;
			probe_worker_ns[worker_id] = worker.stats.process_ns;
		}

		const auto reduce_t0 = clock::now();
		extendedprice_sum = duckdb::Hugeint::Convert(0);
		has_extendedprice_sum = false;
#if defined(__SIZEOF_INT128__)
		ProbeAccT merged = 0;
		for (const auto &worker : probe_workers) {
			if (worker.value.sum.has_sum) {
				merged += worker.value.sum.extendedprice_sum;
				has_extendedprice_sum = true;
			}
		}
		if (has_extendedprice_sum) {
			extendedprice_sum =
			    duckdb::hugeint_t(static_cast<int64_t>(merged >> 64), static_cast<uint64_t>(merged));
		}
#else
		for (const auto &worker : probe_workers) {
			if (!worker.value.sum.has_sum) {
				continue;
			}
			if (!has_extendedprice_sum) {
				extendedprice_sum = worker.value.sum.extendedprice_sum;
				has_extendedprice_sum = true;
			} else if (!duckdb::Hugeint::TryAddInPlace(extendedprice_sum, worker.value.sum.extendedprice_sum)) {
				throw duckdb::OutOfRangeException("Overflow while merging Q17 extendedprice sum");
			}
		}
#endif
		probe_workers.clear();
		profile.final_reduce_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - reduce_t0).count();

		if (!probe_worker_ns.empty()) {
			profile.min_worker_probe_ns = *std::min_element(probe_worker_ns.begin(), probe_worker_ns.end());
			profile.max_worker_probe_ns = *std::max_element(probe_worker_ns.begin(), probe_worker_ns.end());
		}

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			PrintQ17ParallelProfile(profile);
		}
	}

	void Finalize() override {
		if (has_extendedprice_sum) {
			auto decimal_sum = duckdb::Value::DECIMAL(extendedprice_sum, 38, 2);
			result_value = decimal_sum.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>() / 7.0;
			has_result = true;
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
	Q17Parameters params;
	duckdb::hugeint_t extendedprice_sum = duckdb::Hugeint::Convert(0);
	bool has_extendedprice_sum = false;
	double result_value = 0.0;
	bool has_result = false;
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ17ParallelKernel(const Q17Parameters &params) {
	return duckdb::make_uniq<Q17ParallelKernel>(params);
}

} // namespace accelerator_codegen
