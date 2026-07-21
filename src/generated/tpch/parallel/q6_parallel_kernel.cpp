#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q6.hpp"

#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

#if defined(__SIZEOF_INT128__)
using native_hugeint_t = __int128_t;

native_hugeint_t ToNativeHugeint(int64_t value) {
	return static_cast<native_hugeint_t>(value);
}

duckdb::hugeint_t FromNativeHugeint(native_hugeint_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
#endif

struct Q6WorkerAgg {
#if defined(__SIZEOF_INT128__)
	native_hugeint_t native_sum = 0;
	bool has_value = false;
#else
	DecimalSumResult result {duckdb::Hugeint::Convert(0), true};
#endif
};

void AccumulateQ6Chunk(duckdb::DataChunk &chunk, Q6WorkerAgg &agg) {
	auto &discount_vector = chunk.data[Q6LineitemChunkLayout::DISCOUNT_INDEX];
	auto &extendedprice_vector = chunk.data[Q6LineitemChunkLayout::EXTENDEDPRICE_INDEX];

#if defined(__SIZEOF_INT128__)
	if (discount_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	    extendedprice_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
		const auto &discount_validity = duckdb::FlatVector::Validity(discount_vector);
		const auto &extendedprice_validity = duckdb::FlatVector::Validity(extendedprice_vector);
		const auto *discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
		const auto *extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
		if (discount_validity.AllValid() && extendedprice_validity.AllValid()) {
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				agg.native_sum += ToNativeHugeint(extendedprices[row]) * ToNativeHugeint(discounts[row]);
			}
			agg.has_value = agg.has_value || chunk.size() != 0;
			return;
		}
	}
#endif

	duckdb::UnifiedVectorFormat discount_format;
	duckdb::UnifiedVectorFormat extendedprice_format;
	discount_vector.ToUnifiedFormat(chunk.size(), discount_format);
	extendedprice_vector.ToUnifiedFormat(chunk.size(), extendedprice_format);
	auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
	auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto discount_index = discount_format.sel->get_index(row);
		const auto extendedprice_index = extendedprice_format.sel->get_index(row);
		if ((!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index)) ||
		    (!extendedprice_format.validity.AllValid() &&
		     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index))) {
			continue;
		}
#if defined(__SIZEOF_INT128__)
		agg.native_sum +=
		    ToNativeHugeint(extendedprices[extendedprice_index]) * ToNativeHugeint(discounts[discount_index]);
		agg.has_value = true;
#else
		duckdb::hugeint_t price_value;
		duckdb::hugeint_t discount_value;
		duckdb::hugeint_t product_value;
		if (!duckdb::Hugeint::TryConvert(extendedprices[extendedprice_index], price_value) ||
		    !duckdb::Hugeint::TryConvert(discounts[discount_index], discount_value) ||
		    !duckdb::Hugeint::TryMultiply(price_value, discount_value, product_value)) {
			throw duckdb::OutOfRangeException("Overflow while computing Q6 revenue product");
		}
		if (agg.result.is_null) {
			agg.result.value = product_value;
			agg.result.is_null = false;
		} else if (!duckdb::Hugeint::TryAddInPlace(agg.result.value, product_value)) {
			throw duckdb::OutOfRangeException("Overflow while accumulating Q6 revenue");
		}
#endif
	}
}

class Q6ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q6ParallelKernel(Q6Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 1 || !bound_inputs[0]) {
			throw duckdb::InternalException("Q6 parallel expects one bound lineitem scan");
		}
		const auto worker_count = ParallelWorkerCount(context);
		std::vector<CacheLinePadded<Q6WorkerAgg>> workers(worker_count);

		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     AccumulateQ6Chunk(chunk, workers[worker_id].value);
		                     });

		result = {duckdb::Hugeint::Convert(0), true};
#if defined(__SIZEOF_INT128__)
		native_hugeint_t merged = 0;
		bool any = false;
		for (const auto &w : workers) {
			if (w.value.has_value) {
				merged += w.value.native_sum;
				any = true;
			}
		}
		if (any) {
			result.value = FromNativeHugeint(merged);
			result.is_null = false;
		}
#else
		for (const auto &w : workers) {
			if (w.value.result.is_null) {
				continue;
			}
			if (result.is_null) {
				result = w.value.result;
			} else if (!duckdb::Hugeint::TryAddInPlace(result.value, w.value.result.value)) {
				throw duckdb::OutOfRangeException("Overflow while merging Q6 revenue");
			}
		}
#endif
		(void)params;
	}

	void Finalize() override {
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || emitted) {
			return false;
		}
		if (result.is_null) {
			output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DECIMAL(38, 4)));
		} else {
			output.SetValue(0, 0, duckdb::Value::DECIMAL(result.value, 38, 4));
		}
		output.SetCardinality(1);
		emitted = true;
		return true;
	}

private:
	Q6Parameters params;
	DecimalSumResult result {duckdb::Hugeint::Convert(0), true};
	bool finalized = false;
	bool emitted = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ6ParallelKernel(const Q6Parameters &params) {
	return duckdb::make_uniq<Q6ParallelKernel>(params);
}

} // namespace accelerator_codegen
