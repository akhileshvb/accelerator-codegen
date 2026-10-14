#include "tpch/q15/duckdb/base/q15_revenue.hpp"
#include "tpch/q15/duckdb/base/query.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;

struct SupplierRow {
	std::string name;
	std::string address;
	std::string phone;
};

struct Q15OutputRow {
	int64_t suppkey = 0;
	std::string name;
	std::string address;
	std::string phone;
	duckdb::hugeint_t revenue;
};

bool BetterOutputRow(const Q15OutputRow &lhs, const Q15OutputRow &rhs) {
	return lhs.suppkey < rhs.suppkey;
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

class Q15Kernel final : public KernelInstance {
public:
	explicit Q15Kernel(Q15Parameters params_p) : params(std::move(params_p)) {
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q15InputId>(input_id)) {
		case Q15InputId::LINEITEM:
			aggregator.Consume(input, params);
			return;
		case Q15InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		default:
			throw duckdb::InternalException("Q15 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		const auto &store = aggregator.Store();
		duckdb::hugeint_t max_revenue;
		bool has_max_revenue = false;
		for (const auto suppkey : store.Active()) {
			const auto revenue = q15::ToHugeint(store.Revenue(suppkey));
			if (!has_max_revenue || duckdb::Hugeint::GreaterThan(revenue, max_revenue)) {
				max_revenue = revenue;
				has_max_revenue = true;
			}
		}

		output_rows.clear();
		if (has_max_revenue) {
			output_rows.reserve(store.Active().size());
			for (const auto suppkey : store.Active()) {
				const auto revenue = q15::ToHugeint(store.Revenue(suppkey));
				if (!duckdb::Hugeint::Equals(revenue, max_revenue)) {
					continue;
				}
				const auto supplier_it = suppliers.find(suppkey);
				if (supplier_it == suppliers.end()) {
					continue;
				}
				Q15OutputRow row;
				row.suppkey = suppkey;
				row.name = supplier_it->second.name;
				row.address = supplier_it->second.address;
				row.phone = supplier_it->second.phone;
				row.revenue = revenue;
				output_rows.push_back(std::move(row));
			}
			std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
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
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &entry = output_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.suppkey));
			output.SetValue(1, row, duckdb::Value(entry.name));
			output.SetValue(2, row, duckdb::Value(entry.address));
			output.SetValue(3, row, duckdb::Value(entry.phone));
			output.SetValue(4, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void ConsumeSupplier(duckdb::DataChunk &input) {
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat address_format;
		duckdb::UnifiedVectorFormat phone_format;
		input.data[Q15SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(), suppkey_format);
		input.data[Q15SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(input.size(), name_format);
		input.data[Q15SupplierChunkLayout::ADDRESS_INDEX].ToUnifiedFormat(input.size(), address_format);
		input.data[Q15SupplierChunkLayout::PHONE_INDEX].ToUnifiedFormat(input.size(), phone_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto address_index = address_format.sel->get_index(row);
			const auto phone_index = phone_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index)) ||
			    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index))) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			if (!aggregator.Store().Has(suppkey)) {
				continue;
			}
			SupplierRow supplier;
			supplier.name = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]);
			supplier.address = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index]);
			supplier.phone = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index]);
			suppliers.emplace(suppkey, std::move(supplier));
		}
	}

	Q15Parameters params;
	q15::ChunkAggregator aggregator;
	std::unordered_map<int64_t, SupplierRow> suppliers;
	std::vector<Q15OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ15Kernel(const Q15Parameters &params) {
	return duckdb::make_uniq<Q15Kernel>(params);
}

} // namespace accelerator_codegen
