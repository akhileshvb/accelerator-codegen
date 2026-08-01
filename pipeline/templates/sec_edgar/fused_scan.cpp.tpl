// Optional fused-scan implementation for SEC-EDGAR Q{{query_number}}.
// When this file exists it is compiled automatically (see cmake/BaseQuery.cmake).
// Wire runtime hooks in src/generated/sec_edgar/sec_edgar_query_runtime.cpp.

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q{{query_number}}.hpp"

#include "duckdb/main/client_context.hpp"

namespace accelerator_codegen {
namespace {

class Q{{query_number}}FusedKernel final : public KernelInstance {
public:
	explicit Q{{query_number}}FusedKernel(const Q{{query_number}}Parameters &params_p) : params(params_p) {
	}

	void ConsumeInput(duckdb::idx_t /*input_id*/, duckdb::DataChunk & /*input*/) override {
		throw duckdb::InternalException("SEC EDGAR Q{{query_number}} fused kernel: call RunFusedPreScan instead");
	}

	void RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_scanned) {
			return;
		}

		const auto consumer = [this](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                             duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                             duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			// Decode projected columns with SelectProjectedColumn per index (see tpch q1_fused_scan.cpp).
			(void)transaction;
			(void)scan_state;
			(void)row_group;
			(void)sel;
			(void)count;
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		};

		if (!RunNativeFilteredScanConsumer(context, bind_data, consumer)) {
			throw duckdb::InterruptException();
		}
		fused_scanned = true;
	}

	void Finalize() override {
		// Materialize aggregates into ordered_rows / output buffer.
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		(void)output;
		return false;
	}

private:
	Q{{query_number}}Parameters params;
	bool fused_scanned = false;
};

} // namespace

void RunSecEdgarQ{{query_number}}FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                                              const duckdb::FunctionData &bind_data) {
	auto &fused_kernel = static_cast<Q{{query_number}}FusedKernel &>(kernel);
	fused_kernel.RunFusedPreScan(context, bind_data);
}

} // namespace accelerator_codegen
