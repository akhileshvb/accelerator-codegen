#include "tpch/q6/base/query.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

class SingleChunkSource final : public ChunkSource {
public:
    explicit SingleChunkSource(duckdb::DataChunk &chunk_p) : chunk_ref(chunk_p) {
    }

    bool Next() override {
        if (consumed) {
            return false;
        }
        consumed = true;
        return chunk_ref.size() != 0;
    }

    duckdb::DataChunk &Chunk() override {
        return chunk_ref;
    }

private:
    duckdb::DataChunk &chunk_ref;
    bool consumed = false;
};

class Q6Kernel final : public KernelInstance {
public:
    explicit Q6Kernel(Q6Parameters params_p) : params(std::move(params_p)) {
    }

    void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
        if (input_id != 0) {
            throw duckdb::InternalException("Q6 kernel received unexpected input id %llu",
                                            static_cast<unsigned long long>(input_id));
        }
        SingleChunkSource source(input);
        auto partial = ExecuteQ6(source, params);
        if (partial.is_null) {
            return;
        }
        if (result.is_null) {
            result = partial;
            return;
        }
#if defined(__SIZEOF_INT128__)
        __int128_t lhs = (static_cast<__int128_t>(result.value.upper) << 64) + result.value.lower;
        __int128_t rhs = (static_cast<__int128_t>(partial.value.upper) << 64) + partial.value.lower;
        __int128_t combined = lhs + rhs;
        result.value = duckdb::hugeint_t(static_cast<int64_t>(combined >> 64), static_cast<uint64_t>(combined));
#else
        if (!duckdb::Hugeint::TryAddInPlace(result.value, partial.value)) {
            throw duckdb::OutOfRangeException("Overflow while accumulating Q6 revenue");
        }
#endif
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

#if defined(__SIZEOF_INT128__)
using native_hugeint_t = __int128_t;

native_hugeint_t ToNativeHugeint(int64_t value) {
    return static_cast<native_hugeint_t>(value);
}

duckdb::hugeint_t FromNativeHugeint(native_hugeint_t value) {
    return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}
#endif

} // namespace

DecimalSumResult ExecuteQ6(ChunkSource &lineitem_scan, const Q6Parameters &params) {
    (void)params;
    DecimalSumResult result {duckdb::Hugeint::Convert(0), true};

#if defined(__SIZEOF_INT128__)
    native_hugeint_t native_sum = 0;
    bool native_has_value = false;
#endif

    while (lineitem_scan.Next()) {
        auto &chunk = lineitem_scan.Chunk();

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
                    native_sum += ToNativeHugeint(extendedprices[row]) * ToNativeHugeint(discounts[row]);
                }
                native_has_value = native_has_value || chunk.size() != 0;
                continue;
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
            native_sum += ToNativeHugeint(extendedprices[extendedprice_index]) * ToNativeHugeint(discounts[discount_index]);
            native_has_value = true;
#else
            duckdb::hugeint_t price_value;
            duckdb::hugeint_t discount_value;
            duckdb::hugeint_t product_value;
            if (!duckdb::Hugeint::TryConvert(extendedprices[extendedprice_index], price_value) ||
                !duckdb::Hugeint::TryConvert(discounts[discount_index], discount_value) ||
                !duckdb::Hugeint::TryMultiply(price_value, discount_value, product_value)) {
                throw duckdb::OutOfRangeException("Overflow while computing Q6 revenue product");
            }

            if (result.is_null) {
                result.value = product_value;
                result.is_null = false;
            } else if (!duckdb::Hugeint::TryAddInPlace(result.value, product_value)) {
                throw duckdb::OutOfRangeException("Overflow while accumulating Q6 revenue");
            }
#endif
        }
    }

#if defined(__SIZEOF_INT128__)
    if (native_has_value) {
        result.value = FromNativeHugeint(native_sum);
        result.is_null = false;
    }
#endif

    return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ6Kernel(const Q6Parameters &params) {
    return duckdb::make_uniq<Q6Kernel>(params);
}

} // namespace accelerator_codegen
