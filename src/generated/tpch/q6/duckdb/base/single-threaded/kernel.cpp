#include "tpch/q6/duckdb/base/q6_filter.hpp"
#include "tpch/q6/duckdb/base/query.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

class Q6Kernel final : public KernelInstance {
public:
    explicit Q6Kernel(Q6Parameters params_p) : params(std::move(params_p)) {
    }

    void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
        if (input_id != 0) {
            throw duckdb::InternalException("Q6 kernel received unexpected input id %llu",
                                            static_cast<unsigned long long>(input_id));
        }
        accumulator.Consume(input, params);
    }

    void Finalize() override {
        finalized = true;
    }

    bool ProduceOutput(duckdb::DataChunk &output) override {
        if (!finalized || emitted) {
            return false;
        }
        if (!accumulator.HasValue()) {
            output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DECIMAL(38, 4)));
        } else {
            output.SetValue(0, 0, duckdb::Value::DECIMAL(accumulator.Value(), 38, 4));
        }
        output.SetCardinality(1);
        emitted = true;
        return true;
    }

private:
    Q6Parameters params;
    q6::Accumulator accumulator;
    bool finalized = false;
    bool emitted = false;
};

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

} // namespace

DecimalSumResult ExecuteQ6(ChunkSource &lineitem_scan, const Q6Parameters &params) {
    q6::Accumulator accumulator;
    while (lineitem_scan.Next()) {
        accumulator.Consume(lineitem_scan.Chunk(), params);
    }
    return DecimalSumResult {accumulator.Value(), !accumulator.HasValue()};
}

duckdb::unique_ptr<KernelInstance> CreateQ6Kernel(const Q6Parameters &params) {
    return duckdb::make_uniq<Q6Kernel>(params);
}

} // namespace accelerator_codegen
