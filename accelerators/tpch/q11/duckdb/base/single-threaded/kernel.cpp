#include "tpch/q11/duckdb/base/query.hpp"
#include "tpch/q11/duckdb/base/q11_common.hpp"

#include <algorithm>
#include <cstring>
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

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

class Q11Kernel final : public KernelInstance {
public:
	explicit Q11Kernel(Q11Parameters params_p, Q11ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p) {
		if (profile) {
			profile->used_dense_state = true;
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q11InputId>(input_id)) {
		case Q11InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		case Q11InputId::PARTSUPP:
			ConsumePartsupp(input);
			return;
		default:
			throw duckdb::InternalException("Q11 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		if (has_total_value) {
			const q11::acc_t total = total_sum.Get();
			const q11::ThresholdTest test(total, params.fraction);
			output_rows.reserve(active_parts.size());
			for (const auto partkey : active_parts) {
				const q11::acc_t part_value = parts.Get(static_cast<size_t>(partkey));
				if (!test.Passes(part_value)) {
					continue;
				}
				output_rows.push_back(q11::OutputRow {partkey, part_value});
			}
			std::sort(output_rows.begin(), output_rows.end(), q11::BetterOutputRow);
		}
		output_offset = 0;
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= output_rows.size()) {
			return false;
		}
		const duckdb::idx_t remaining = output_rows.size() - output_offset;
		const duckdb::idx_t count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		auto keys = duckdb::FlatVector::GetData<int64_t>(output.data[0]);
		auto values = duckdb::FlatVector::GetData<duckdb::hugeint_t>(output.data[1]);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &entry = output_rows[output_offset + row];
			keys[row] = entry.partkey;
			values[row] = q11::ToHugeint(entry.value);
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void EnsurePartCapacity(int64_t partkey) {
		const size_t required = static_cast<size_t>(partkey) + 1;
		if (required <= parts.lo.size()) {
			return;
		}
		const size_t target = std::max<size_t>({required, parts.lo.size() * 2, 200001});
		parts.EnsureCapacity(target);
		part_has_value.resize(target, 0);
	}

	inline void AddPartValue(int64_t partkey, int64_t supplycost_raw, int64_t availqty) {
		has_total_value = true;
		total_sum.MulAdd(supplycost_raw, availqty);
		if (ACCEL_UNLIKELY(partkey < 0)) {
			return;
		}
		if (ACCEL_UNLIKELY(static_cast<size_t>(partkey) >= parts.lo.size())) {
			EnsurePartCapacity(partkey);
		}
		const auto index = static_cast<size_t>(partkey);
		if (!part_has_value[index]) {
			part_has_value[index] = 1;
			active_parts.push_back(partkey);
		}
		parts.MulAdd(index, supplycost_raw, availqty);
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		if (params.target_nationkey < 0) {
			return;
		}
		auto &suppkey_vector = input.data[Q11SupplierChunkLayout::SUPPKEY_INDEX];
		if (IsFlatAllValid(suppkey_vector)) {
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				suppliers.Mark(suppkeys[row]);
			}
			if (profile) {
				profile->supplier_flat_fast_rows += input.size();
				profile->supplier_rows_scanned += input.size();
				profile->supplier_rows_matched += input.size();
			}
			return;
		}
		duckdb::UnifiedVectorFormat suppkey_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto idx = suppkey_format.sel->get_index(row);
			if (!suppkey_format.validity.RowIsValid(idx)) {
				continue;
			}
			suppliers.Mark(suppkeys[idx]);
		}
	}

	void ConsumePartsupp(duckdb::DataChunk &input) {
		if (!suppliers.any) {
			return;
		}
		const size_t n = input.size();
		if (profile) {
			profile->partsupp_rows_scanned += n;
		}
		auto &partkey_vector = input.data[Q11PartsuppChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q11PartsuppChunkLayout::SUPPKEY_INDEX];
		auto &availqty_vector = input.data[Q11PartsuppChunkLayout::AVAILQTY_INDEX];
		auto &supplycost_vector = input.data[Q11PartsuppChunkLayout::SUPPLYCOST_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(availqty_vector) &&
		    IsFlatAllValid(supplycost_vector)) {
			const auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			const auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			const auto availqtys = duckdb::FlatVector::GetData<int64_t>(availqty_vector);
			const auto supplycosts = duckdb::FlatVector::GetData<int64_t>(supplycost_vector);
			if (sel.size() < n + simd::kSelSlack) {
				sel.resize(n + simd::kSelSlack);
			}
			const size_t count = q11::SelectQualifying(suppliers, suppkeys, n, sel.data());
			for (size_t k = 0; k < count; k++) {
				const uint32_t row = sel[k];
				AddPartValue(partkeys[row], supplycosts[row], availqtys[row]);
			}
			if (profile) {
				profile->partsupp_flat_fast_rows += n;
				profile->partsupp_rows_matched += count;
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat availqty_format;
		duckdb::UnifiedVectorFormat supplycost_format;
		partkey_vector.ToUnifiedFormat(n, partkey_format);
		suppkey_vector.ToUnifiedFormat(n, suppkey_format);
		availqty_vector.ToUnifiedFormat(n, availqty_format);
		supplycost_vector.ToUnifiedFormat(n, supplycost_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto availqtys = duckdb::UnifiedVectorFormat::GetData<int64_t>(availqty_format);
		auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);
		for (duckdb::idx_t row = 0; row < n; row++) {
			const auto pi = partkey_format.sel->get_index(row);
			const auto si = suppkey_format.sel->get_index(row);
			const auto ai = availqty_format.sel->get_index(row);
			const auto ci = supplycost_format.sel->get_index(row);
			if (!partkey_format.validity.RowIsValid(pi) || !suppkey_format.validity.RowIsValid(si) ||
			    !availqty_format.validity.RowIsValid(ai) || !supplycost_format.validity.RowIsValid(ci)) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			if (!suppliers.Qualifies(suppkeys[si])) {
				continue;
			}
			AddPartValue(partkeys[pi], supplycosts[ci], availqtys[ai]);
			if (profile) {
				profile->partsupp_rows_matched += 1;
			}
		}
	}

	Q11Parameters params;
	Q11ProfileCounters *profile;
	q11::SupplierMap suppliers;
	q11::PartSums parts;
	std::vector<uint8_t> part_has_value;
	std::vector<int64_t> active_parts;
	std::vector<uint32_t> sel;
	q11::WideSum total_sum;
	bool has_total_value = false;
	std::vector<q11::OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ11Kernel(const Q11Parameters &params) {
	return duckdb::make_uniq<Q11Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ11ProfilingKernel(const Q11Parameters &params, Q11ProfileCounters *profile) {
	return duckdb::make_uniq<Q11Kernel>(params, profile);
}

} // namespace accelerator_codegen
