#include "tpch/q1/duckdb/base/query.hpp"
#include "tpch/q1/duckdb/base/q1_agg.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {
namespace {

//! Consumes the lineitem scan (l_shipdate <= cutoff pushed down by CreateQ1ScanSpec; the scan applies it
//! exactly) and aggregates the six payload columns with q1::Aggregator.
class Q1Kernel final : public KernelInstance {
public:
	explicit Q1Kernel(Q1Parameters params_p) : params(std::move(params_p)) {
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		if (input_id != 0) {
			throw duckdb::InternalException("Q1 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
		const auto n = input.size();
		if (n == 0) {
			return;
		}
		q1::Columns cols {&input.data[Q1LineitemChunkLayout::QUANTITY_INDEX],
		                  &input.data[Q1LineitemChunkLayout::EXTENDEDPRICE_INDEX],
		                  &input.data[Q1LineitemChunkLayout::DISCOUNT_INDEX],
		                  &input.data[Q1LineitemChunkLayout::TAX_INDEX],
		                  &input.data[Q1LineitemChunkLayout::RETURNFLAG_INDEX],
		                  &input.data[Q1LineitemChunkLayout::LINESTATUS_INDEX],
		                  &input.data[Q1LineitemChunkLayout::SHIPDATE_INDEX],
		                  params.cutoff_date.days};
		// The scan may or may not have pushed l_shipdate <= cutoff down; the aggregator re-applies it (cheap,
		// SIMD) so the kernel is exact either way.
		aggregator.Consume(cols, n, nullptr, n);
	}

	void Finalize() override {
		if (finalized) {
			return;
		}
		q1::GroupMap groups;
		aggregator.MergeInto(groups);
		rows = q1::BuildRows(groups);
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized) {
			return false;
		}
		return q1::WriteRows(rows, output_offset, output);
	}

private:
	Q1Parameters params;
	q1::Aggregator aggregator;
	std::vector<q1::OutputRow> rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ1Kernel(const Q1Parameters &parameters) {
	return duckdb::make_uniq<Q1Kernel>(parameters);
}

} // namespace accelerator_codegen
