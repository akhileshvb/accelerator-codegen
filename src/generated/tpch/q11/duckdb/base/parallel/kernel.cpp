#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "tpch/q11/duckdb/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int64_t kDefaultPartCapacity = 200001;

#if defined(__SIZEOF_INT128__)
using q11_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q11_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddValue(q11_acc_t &accumulator, int64_t supplycost_raw, int64_t availqty) {
	accumulator += static_cast<q11_acc_t>(supplycost_raw) * static_cast<q11_acc_t>(availqty);
}
#else
using q11_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q11_acc_t &value) {
	return value;
}

void AddValue(q11_acc_t &accumulator, int64_t supplycost_raw, int64_t availqty) {
	duckdb::hugeint_t cost_value;
	duckdb::hugeint_t qty_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(supplycost_raw, cost_value) ||
	    !duckdb::Hugeint::TryConvert(availqty, qty_value) ||
	    !duckdb::Hugeint::TryMultiply(cost_value, qty_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q11 value");
	}
}
#endif

bool Q11ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
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

struct Q11OutputRow {
	int64_t partkey = 0;
	duckdb::hugeint_t value;
};

bool BetterOutputRow(const Q11OutputRow &lhs, const Q11OutputRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.value, rhs.value)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.value, lhs.value)) {
		return false;
	}
	return lhs.partkey < rhs.partkey;
}

bool ValueGreaterThan(const duckdb::hugeint_t &lhs, const duckdb::Value &rhs) {
	const auto lhs_value = duckdb::Value::DECIMAL(lhs, 38, 2);
	return lhs_value > rhs;
}

bool ValueGreaterThanThreshold(q11_acc_t part_acc, q11_acc_t total_acc, double fraction,
                               const duckdb::Value &threshold_value) {
#if defined(__SIZEOF_INT128__)
	// Safe necessary-condition reject for the standard TPC-H SF1 fraction (0.0001).
	if (fraction == 0.0001 && part_acc * 10000 <= total_acc) {
		return false;
	}
#endif
	return ValueGreaterThan(FromAccumulator(part_acc), threshold_value);
}

duckdb::Value ComputeThreshold(const duckdb::hugeint_t &total_value, double fraction) {
	const auto total_decimal = duckdb::Value::DECIMAL(total_value, 38, 2);
	const auto total_as_double = total_decimal.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
	return duckdb::Value::DOUBLE(total_as_double * fraction);
}

struct Q11FrozenState {
	std::vector<uint8_t> supplier_qualifying;
	bool has_qualifying_suppliers = false;

	void EnsureSupplierCapacity(int64_t suppkey) {
		if (suppkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(suppkey) + 1;
		if (required <= supplier_qualifying.size()) {
			return;
		}
		size_t target = required;
		if (!supplier_qualifying.empty()) {
			const size_t doubled = supplier_qualifying.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultSupplierCapacity));
		}
		supplier_qualifying.resize(target, 0);
	}

	void MarkSupplierQualifying(int64_t suppkey) {
		EnsureSupplierCapacity(suppkey);
		supplier_qualifying[static_cast<size_t>(suppkey)] = 1;
		has_qualifying_suppliers = true;
	}

	bool SupplierQualifies(int64_t suppkey) const {
		if (suppkey < 0 || static_cast<size_t>(suppkey) >= supplier_qualifying.size()) {
			return false;
		}
		return supplier_qualifying[static_cast<size_t>(suppkey)] != 0;
	}

	size_t SupplierStateBytes() const {
		return supplier_qualifying.size();
	}
};

struct Q11PartContribution {
	int32_t partkey = 0;
	int64_t supplycost_raw = 0;
	int64_t availqty = 0;
};

struct Q11WorkerAgg {
	std::vector<Q11PartContribution> contributions;
	q11_acc_t total_value = 0;
	bool has_total_value = false;

	void AddPartValue(int64_t partkey, int64_t supplycost_raw, int64_t availqty) {
		contributions.push_back(
		    Q11PartContribution {static_cast<int32_t>(partkey), supplycost_raw, availqty});
		AddValue(total_value, supplycost_raw, availqty);
		has_total_value = true;
	}

	size_t ContributionBytes() const {
		return contributions.size() * sizeof(Q11PartContribution);
	}
};

struct Q11ParallelProfile {
	int64_t total_ns = 0;
	int64_t supplier_build_ns = 0;
	int64_t partsupp_scan_ns = 0;
	int64_t merge_ns = 0;
	int64_t threshold_ns = 0;
	int64_t qualify_ns = 0;
	int64_t sort_ns = 0;
	size_t worker_count = 0;
	int32_t target_nationkey = -1;
	size_t suppliers_scanned = 0;
	size_t suppliers_qualifying = 0;
	size_t supplier_state_bytes = 0;
	size_t partsupp_rows_scanned = 0;
	size_t partsupp_supplier_probes = 0;
	size_t partsupp_supplier_hits = 0;
	size_t value_contributions = 0;
	size_t supplycost_decoded = 0;
	size_t availqty_decoded = 0;
	size_t distinct_touched_parts = 0;
	size_t part_domain_size = 0;
	size_t qualifying_part_count = 0;
	size_t output_candidates = 0;
	size_t worker_value_bytes = 0;
	size_t worker_flag_bytes = 0;
	size_t worker_touched_bytes = 0;
	size_t merge_entries = 0;
	size_t supplier_flat_rows = 0;
	size_t supplier_dict_rows = 0;
	size_t supplier_fallback_rows = 0;
	size_t partsupp_flat_rows = 0;
	size_t partsupp_dict_rows = 0;
	size_t partsupp_fallback_rows = 0;
	int64_t max_worker_ns = 0;
	int64_t avg_worker_ns = 0;
};

struct Q11WorkerPartsuppProfile {
	size_t rows = 0;
	int64_t elapsed_ns = 0;
};

void MergePartsuppProfile(Q11ParallelProfile &dst, const Q11ParallelProfile &src) {
	dst.partsupp_rows_scanned += src.partsupp_rows_scanned;
	dst.partsupp_supplier_probes += src.partsupp_supplier_probes;
	dst.partsupp_supplier_hits += src.partsupp_supplier_hits;
	dst.value_contributions += src.value_contributions;
	dst.supplycost_decoded += src.supplycost_decoded;
	dst.availqty_decoded += src.availqty_decoded;
	dst.partsupp_flat_rows += src.partsupp_flat_rows;
	dst.partsupp_dict_rows += src.partsupp_dict_rows;
	dst.partsupp_fallback_rows += src.partsupp_fallback_rows;
}

bool FractionPrecludesQualifyingParts(double fraction) {
	// Q11 keeps parts with part_value > total_value * fraction. No part value can exceed the
	// global total, so fraction >= 1.0 always yields an empty result set.
	return fraction >= 1.0;
}

void PrintQ11ParallelProfile(const Q11ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	std::cerr << "q11_parallel_profile total_ns=" << profile.total_ns << " supplier_build_ns=" << profile.supplier_build_ns
	          << " (" << pct(profile.supplier_build_ns) << "%)"
	          << " partsupp_scan_ns=" << profile.partsupp_scan_ns << " (" << pct(profile.partsupp_scan_ns) << "%)"
	          << " merge_ns=" << profile.merge_ns << " (" << pct(profile.merge_ns) << "%)"
	          << " threshold_ns=" << profile.threshold_ns << " (" << pct(profile.threshold_ns) << "%)"
	          << " qualify_ns=" << profile.qualify_ns << " (" << pct(profile.qualify_ns) << "%)"
	          << " sort_ns=" << profile.sort_ns << " (" << pct(profile.sort_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " target_nationkey=" << profile.target_nationkey
	          << " suppliers_scanned=" << profile.suppliers_scanned
	          << " suppliers_qualifying=" << profile.suppliers_qualifying
	          << " supplier_state_bytes=" << profile.supplier_state_bytes
	          << " partsupp_rows_scanned=" << profile.partsupp_rows_scanned
	          << " partsupp_supplier_probes=" << profile.partsupp_supplier_probes
	          << " partsupp_supplier_hits=" << profile.partsupp_supplier_hits
	          << " value_contributions=" << profile.value_contributions
	          << " supplycost_decoded=" << profile.supplycost_decoded
	          << " availqty_decoded=" << profile.availqty_decoded
	          << " distinct_touched_parts=" << profile.distinct_touched_parts
	          << " part_domain_size=" << profile.part_domain_size
	          << " qualifying_part_count=" << profile.qualifying_part_count
	          << " output_candidates=" << profile.output_candidates
	          << " worker_value_bytes=" << profile.worker_value_bytes
	          << " worker_flag_bytes=" << profile.worker_flag_bytes
	          << " worker_touched_bytes=" << profile.worker_touched_bytes
	          << " merge_entries=" << profile.merge_entries
	          << " supplier_flat_rows=" << profile.supplier_flat_rows
	          << " supplier_dict_rows=" << profile.supplier_dict_rows
	          << " supplier_fallback_rows=" << profile.supplier_fallback_rows
	          << " partsupp_flat_rows=" << profile.partsupp_flat_rows
	          << " partsupp_dict_rows=" << profile.partsupp_dict_rows
	          << " partsupp_fallback_rows=" << profile.partsupp_fallback_rows
	          << " max_worker_ns=" << profile.max_worker_ns << " avg_worker_ns=" << profile.avg_worker_ns << '\n';
}

void BuildSupplierState(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                        Q11FrozenState &state, Q11ParallelProfile *profile) {
	auto source = CreateNativeChunkSource(context, supplier_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (profile) {
			profile->suppliers_scanned += chunk.size();
		}
		auto &suppkey_vector = chunk.data[Q11SupplierChunkLayout::SUPPKEY_INDEX];
		if (IsFlatAllValid(suppkey_vector)) {
			if (profile) {
				profile->supplier_flat_rows += chunk.size();
			}
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				state.MarkSupplierQualifying(suppkeys[row]);
				if (profile) {
					profile->suppliers_qualifying += 1;
				}
			}
			continue;
		}
		if (IsDictionaryOverFlat(suppkey_vector)) {
			if (profile) {
				profile->supplier_dict_rows += chunk.size();
			}
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto suppkey_index = suppkey_sel.get_index(row);
				if (!suppkey_validity.RowIsValid(suppkey_index)) {
					if (profile) {
						profile->supplier_fallback_rows += 1;
					}
					continue;
				}
				state.MarkSupplierQualifying(suppkeys[suppkey_index]);
				if (profile) {
					profile->suppliers_qualifying += 1;
				}
			}
			continue;
		}
		if (profile) {
			profile->supplier_fallback_rows += chunk.size();
		}
		duckdb::UnifiedVectorFormat suppkey_format;
		suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			if (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) {
				continue;
			}
			state.MarkSupplierQualifying(suppkeys[suppkey_index]);
			if (profile) {
				profile->suppliers_qualifying += 1;
			}
		}
	}
}

void AccumulatePartsuppChunk(duckdb::DataChunk &chunk, const Q11FrozenState &state, Q11WorkerAgg &agg,
                             Q11ParallelProfile *profile) {
	if (!state.has_qualifying_suppliers) {
		return;
	}
	if (profile) {
		profile->partsupp_rows_scanned += chunk.size();
	}
	auto &partkey_vector = chunk.data[Q11PartsuppChunkLayout::PARTKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q11PartsuppChunkLayout::SUPPKEY_INDEX];
	auto &availqty_vector = chunk.data[Q11PartsuppChunkLayout::AVAILQTY_INDEX];
	auto &supplycost_vector = chunk.data[Q11PartsuppChunkLayout::SUPPLYCOST_INDEX];

	if (IsFlatAllValid(suppkey_vector)) {
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(availqty_vector) && IsFlatAllValid(supplycost_vector)) {
			if (profile) {
				profile->partsupp_flat_rows += chunk.size();
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto availqtys = duckdb::FlatVector::GetData<int64_t>(availqty_vector);
			auto supplycosts = duckdb::FlatVector::GetData<int64_t>(supplycost_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				if (profile) {
					profile->partsupp_supplier_probes += 1;
				}
				if (!state.SupplierQualifies(suppkeys[row])) {
					continue;
				}
				if (profile) {
					profile->partsupp_supplier_hits += 1;
					profile->supplycost_decoded += 1;
					profile->availqty_decoded += 1;
					profile->value_contributions += 1;
				}
				agg.AddPartValue(partkeys[row], supplycosts[row], availqtys[row]);
			}
			return;
		}

		if (profile) {
			profile->partsupp_fallback_rows += chunk.size();
		}
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat availqty_format;
		duckdb::UnifiedVectorFormat supplycost_format;
		partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
		availqty_vector.ToUnifiedFormat(chunk.size(), availqty_format);
		supplycost_vector.ToUnifiedFormat(chunk.size(), supplycost_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto availqtys = duckdb::UnifiedVectorFormat::GetData<int64_t>(availqty_format);
		auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			if (profile) {
				profile->partsupp_supplier_probes += 1;
			}
			if (!state.SupplierQualifies(suppkeys[row])) {
				continue;
			}
			if (profile) {
				profile->partsupp_supplier_hits += 1;
			}
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto availqty_index = availqty_format.sel->get_index(row);
			const auto supplycost_index = supplycost_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!availqty_format.validity.AllValid() && !availqty_format.validity.RowIsValidUnsafe(availqty_index)) ||
			    (!supplycost_format.validity.AllValid() &&
			     !supplycost_format.validity.RowIsValidUnsafe(supplycost_index))) {
				continue;
			}
			if (profile) {
				profile->supplycost_decoded += 1;
				profile->availqty_decoded += 1;
				profile->value_contributions += 1;
			}
			agg.AddPartValue(partkeys[partkey_index], supplycosts[supplycost_index], availqtys[availqty_index]);
		}
		return;
	}

	if (IsDictionaryOverFlat(suppkey_vector) && IsDictionaryOverFlat(partkey_vector) &&
	    IsDictionaryOverFlat(availqty_vector) && IsDictionaryOverFlat(supplycost_vector)) {
		if (profile) {
			profile->partsupp_dict_rows += chunk.size();
		}
		auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &availqty_sel = duckdb::DictionaryVector::SelVector(availqty_vector);
		auto &supplycost_sel = duckdb::DictionaryVector::SelVector(supplycost_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
		auto availqtys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(availqty_vector));
		auto supplycosts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(supplycost_vector));
		auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
		auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
		auto &availqty_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(availqty_vector));
		auto &supplycost_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(supplycost_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto suppkey_index = suppkey_sel.get_index(row);
			if (!suppkey_validity.RowIsValid(suppkey_index)) {
				if (profile) {
					profile->partsupp_fallback_rows += 1;
				}
				continue;
			}
			if (profile) {
				profile->partsupp_supplier_probes += 1;
			}
			if (!state.SupplierQualifies(suppkeys[suppkey_index])) {
				continue;
			}
			if (profile) {
				profile->partsupp_supplier_hits += 1;
			}
			const auto partkey_index = partkey_sel.get_index(row);
			const auto availqty_index = availqty_sel.get_index(row);
			const auto supplycost_index = supplycost_sel.get_index(row);
			if (!partkey_validity.RowIsValid(partkey_index) || !availqty_validity.RowIsValid(availqty_index) ||
			    !supplycost_validity.RowIsValid(supplycost_index)) {
				if (profile) {
					profile->partsupp_fallback_rows += 1;
				}
				continue;
			}
			if (profile) {
				profile->supplycost_decoded += 1;
				profile->availqty_decoded += 1;
				profile->value_contributions += 1;
			}
			agg.AddPartValue(partkeys[partkey_index], supplycosts[supplycost_index], availqtys[availqty_index]);
		}
		return;
	}

	if (profile) {
		profile->partsupp_fallback_rows += chunk.size();
	}
	duckdb::UnifiedVectorFormat suppkey_format;
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat availqty_format;
	duckdb::UnifiedVectorFormat supplycost_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	availqty_vector.ToUnifiedFormat(chunk.size(), availqty_format);
	supplycost_vector.ToUnifiedFormat(chunk.size(), supplycost_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto availqtys = duckdb::UnifiedVectorFormat::GetData<int64_t>(availqty_format);
	auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		if (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) {
			continue;
		}
		if (profile) {
			profile->partsupp_supplier_probes += 1;
		}
		if (!state.SupplierQualifies(suppkeys[suppkey_index])) {
			continue;
		}
		if (profile) {
			profile->partsupp_supplier_hits += 1;
		}
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto availqty_index = availqty_format.sel->get_index(row);
		const auto supplycost_index = supplycost_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!availqty_format.validity.AllValid() && !availqty_format.validity.RowIsValidUnsafe(availqty_index)) ||
		    (!supplycost_format.validity.AllValid() && !supplycost_format.validity.RowIsValidUnsafe(supplycost_index))) {
			continue;
		}
		if (profile) {
			profile->supplycost_decoded += 1;
			profile->availqty_decoded += 1;
			profile->value_contributions += 1;
		}
		agg.AddPartValue(partkeys[partkey_index], supplycosts[supplycost_index], availqtys[availqty_index]);
	}
}

void MergeWorkerAggs(const std::vector<CacheLinePadded<Q11WorkerAgg>> &workers, std::vector<q11_acc_t> &merged_values,
                     std::vector<uint8_t> &merged_has_value, std::vector<int64_t> &merged_touched, q11_acc_t &total_value,
                     bool &has_total_value, Q11ParallelProfile *profile) {
	int64_t max_partkey = 0;
	size_t contribution_count = 0;
	for (const auto &worker : workers) {
		contribution_count += worker.value.contributions.size();
		for (const auto &contribution : worker.value.contributions) {
			max_partkey = std::max<int64_t>(max_partkey, contribution.partkey);
		}
	}
	if (max_partkey < 0) {
		return;
	}
	const auto merged_capacity = std::max<int64_t>(max_partkey + 1, kDefaultPartCapacity);
	merged_values.assign(static_cast<size_t>(merged_capacity), 0);
	merged_has_value.assign(static_cast<size_t>(merged_capacity), 0);
	merged_touched.clear();
	merged_touched.reserve(std::min<size_t>(contribution_count, 65536));

	for (const auto &worker : workers) {
		if (worker.value.has_total_value) {
#if defined(__SIZEOF_INT128__)
			total_value += worker.value.total_value;
#else
			if (!has_total_value) {
				total_value = worker.value.total_value;
			} else if (!duckdb::Hugeint::TryAddInPlace(total_value, worker.value.total_value)) {
				throw duckdb::OutOfRangeException("Overflow while merging Q11 total value");
			}
#endif
			has_total_value = true;
		}
		for (const auto &contribution : worker.value.contributions) {
			const auto partkey = static_cast<int64_t>(contribution.partkey);
			const auto index = static_cast<size_t>(partkey);
			if (profile) {
				profile->merge_entries += 1;
			}
			if (!merged_has_value[index]) {
				merged_has_value[index] = 1;
				merged_touched.push_back(partkey);
			}
			AddValue(merged_values[index], contribution.supplycost_raw, contribution.availqty);
		}
	}
}

class Q11ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q11ParallelKernel(Q11Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q11 parallel expects bound supplier and partsupp scans");
		}

		using clock = std::chrono::steady_clock;
		const bool profile_enabled = Q11ProfileEnabled();
		Q11ParallelProfile profile;
		profile.target_nationkey = params.target_nationkey;
		const auto total_t0 = clock::now();

		Q11FrozenState frozen;
		const auto supplier_t0 = clock::now();
		BuildSupplierState(context, *bound_inputs[0], frozen, profile_enabled ? &profile : nullptr);
		profile.supplier_build_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - supplier_t0).count();
		profile.supplier_state_bytes = frozen.SupplierStateBytes();

		if (FractionPrecludesQualifyingParts(params.fraction)) {
			output_rows.clear();
			profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
			if (profile_enabled) {
				PrintQ11ParallelProfile(profile);
			}
			return;
		}

		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);
		std::vector<CacheLinePadded<Q11WorkerAgg>> workers(worker_count);
		for (auto &worker : workers) {
			worker.value.contributions.reserve(8192);
		}
		std::vector<Q11WorkerPartsuppProfile> worker_profiles(profile_enabled ? static_cast<size_t>(worker_count) : 0);
		std::vector<Q11ParallelProfile> worker_partsupp_profiles(profile_enabled ? static_cast<size_t>(worker_count)
		                                                                         : 0);

		const auto partsupp_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
			                     Q11ParallelProfile *chunk_profile =
			                         profile_enabled ? &worker_partsupp_profiles[worker_id] : nullptr;
			                     AccumulatePartsuppChunk(chunk, frozen, workers[worker_id].value, chunk_profile);
			                     if (profile_enabled) {
				                     auto &worker_profile = worker_profiles[worker_id];
				                     worker_profile.rows += chunk.size();
				                     worker_profile.elapsed_ns +=
				                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - worker_t0)
				                             .count();
			                     }
		                     });
		profile.partsupp_scan_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - partsupp_t0).count();
		if (profile_enabled) {
			int64_t worker_ns_sum = 0;
			size_t value_bytes = 0;
			size_t flag_bytes = 0;
			size_t touched_bytes = 0;
			for (const auto &worker : workers) {
				value_bytes += worker.value.ContributionBytes();
			}
			profile.worker_value_bytes = value_bytes;
			profile.worker_flag_bytes = flag_bytes;
			profile.worker_touched_bytes = touched_bytes;
			for (const auto &worker_profile : worker_profiles) {
				worker_ns_sum += worker_profile.elapsed_ns;
				profile.max_worker_ns = std::max(profile.max_worker_ns, worker_profile.elapsed_ns);
			}
			profile.avg_worker_ns =
			    worker_profiles.empty() ? 0 : worker_ns_sum / static_cast<int64_t>(worker_profiles.size());
			for (const auto &worker_partsupp_profile : worker_partsupp_profiles) {
				MergePartsuppProfile(profile, worker_partsupp_profile);
			}
		}

		std::vector<q11_acc_t> merged_values;
		std::vector<uint8_t> merged_has_value;
		std::vector<int64_t> merged_touched;
		q11_acc_t total_value = 0;
		bool has_total_value = false;
		const auto merge_t0 = clock::now();
		MergeWorkerAggs(workers, merged_values, merged_has_value, merged_touched, total_value, has_total_value,
		                profile_enabled ? &profile : nullptr);
		profile.merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		profile.distinct_touched_parts = merged_touched.size();
		profile.part_domain_size = merged_values.size();

		output_rows.clear();
		if (has_total_value) {
			const auto threshold_t0 = clock::now();
			const auto threshold_value = ComputeThreshold(FromAccumulator(total_value), params.fraction);
			profile.threshold_ns =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - threshold_t0).count();

			const auto qualify_t0 = clock::now();
			output_rows.reserve(merged_touched.size());
			for (const auto partkey : merged_touched) {
				if (partkey < 0 || static_cast<size_t>(partkey) >= merged_values.size()) {
					continue;
				}
				const auto part_acc = merged_values[static_cast<size_t>(partkey)];
				if (!ValueGreaterThanThreshold(part_acc, total_value, params.fraction, threshold_value)) {
					continue;
				}
				Q11OutputRow row;
				row.partkey = partkey;
				row.value = FromAccumulator(part_acc);
				output_rows.push_back(std::move(row));
			}
			profile.qualify_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - qualify_t0).count();
			profile.qualifying_part_count = output_rows.size();
			profile.output_candidates = output_rows.size();

			const auto sort_t0 = clock::now();
			std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
			profile.sort_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - sort_t0).count();
		}

		profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
		if (profile_enabled) {
			PrintQ11ParallelProfile(profile);
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.partkey));
			output.SetValue(1, row, duckdb::Value::DECIMAL(entry.value, 38, 2));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q11Parameters params;
	std::vector<Q11OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ11ParallelKernel(const Q11Parameters &params) {
	return duckdb::make_uniq<Q11ParallelKernel>(params);
}

} // namespace accelerator_codegen
