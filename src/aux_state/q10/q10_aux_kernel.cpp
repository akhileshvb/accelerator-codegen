#include "accelerator_codegen/tpch_q10_aux_kernel.hpp"

#include "accelerator_codegen/kernel.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_size.hpp"

namespace accelerator_codegen {
namespace {

duckdb::string_t AddStringToVector(duckdb::Vector &vector, const std::string &value) {
	return duckdb::StringVector::AddString(vector, value);
}

class Q10AuxFastKernel final : public KernelInstance {
public:
	Q10AuxFastKernel(const Q10SideStores &side_stores_p, std::vector<Q10RawSortKey> sort_keys_p)
	    : side_stores(side_stores_p), sort_keys(std::move(sort_keys_p)) {
	}

	void ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) override {
	}

	void Finalize() override {
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= sort_keys.size()) {
			return false;
		}
		const auto remaining = sort_keys.size() - output_offset;
		const auto count = std::min<duckdb::idx_t>(remaining, STANDARD_VECTOR_SIZE);
		output.SetCardinality(count);
		for (duckdb::idx_t col = 0; col < output.ColumnCount(); col++) {
			output.data[col].SetVectorType(duckdb::VectorType::FLAT_VECTOR);
		}

		auto custkeys = duckdb::FlatVector::GetData<int64_t>(output.data[0]);
		auto names = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[1]);
		auto revenues = duckdb::FlatVector::GetData<duckdb::hugeint_t>(output.data[2]);
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(output.data[3]);
		auto nation_names = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[4]);
		auto addresses = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[5]);
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[6]);
		auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(output.data[7]);

		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &key = sort_keys[output_offset + row];
			const auto index = static_cast<size_t>(key.custkey);
			const auto nationkey = side_stores.customer_nationkey[index];
			custkeys[row] = key.custkey;
			names[row] = AddStringToVector(output.data[1], side_stores.customer_name[index]);
			revenues[row] = Q10RevenueFromAccumulator(key.revenue_raw);
			acctbals[row] = side_stores.customer_acctbal_raw[index];
			nation_names[row] =
			    AddStringToVector(output.data[4], side_stores.nation_names[static_cast<size_t>(nationkey)]);
			addresses[row] = AddStringToVector(output.data[5], side_stores.customer_address[index]);
			phones[row] = AddStringToVector(output.data[6], side_stores.customer_phone[index]);
			comments[row] = AddStringToVector(output.data[7], side_stores.customer_comment[index]);
		}

		output_offset += count;
		return true;
	}

private:
	const Q10SideStores &side_stores;
	std::vector<Q10RawSortKey> sort_keys;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ10AuxFastKernel(const Q10SideStores &side_stores,
                                                         std::vector<Q10RawSortKey> sort_keys) {
	return duckdb::make_uniq<Q10AuxFastKernel>(side_stores, std::move(sort_keys));
}

} // namespace accelerator_codegen
