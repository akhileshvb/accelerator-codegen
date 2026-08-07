#include "accelerator_codegen/tpch_q21.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultSupplierCapacity = 10001;

struct Q21RuntimeConfig {
	// Keep the unified-vector path authoritative until the generated fast path
	// has equivalent validity/selection handling for all DuckDB scan layouts.
	bool orders_flat_fast = false;
	bool lineitem_flat_fast = false;
	bool lineitem_dict_fast = false;
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

struct StoredLineitem {
	int64_t orderkey = 0;
	int64_t suppkey = 0;
	int32_t commitdate = 0;
	int32_t receiptdate = 0;
};

struct Q21OutputRow {
	std::string name;
	int64_t numwait = 0;
};

bool BetterOutputRow(const Q21OutputRow &lhs, const Q21OutputRow &rhs) {
	if (lhs.numwait != rhs.numwait) {
		return lhs.numwait > rhs.numwait;
	}
	return lhs.name < rhs.name;
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

class OrderKeyBitmap {
public:
	bool Contains(int64_t orderkey) const {
		if (orderkey < 0) {
			return false;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		if (probe_unchecked) {
			return ContainsUnchecked(key);
		}
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			return false;
		}
		const auto mask = uint64_t {1} << (key & 63);
		return (words[word_index] & mask) != 0;
	}

	bool Insert(int64_t orderkey) {
		if (orderkey < 0) {
			return false;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			EnsureCapacity(key);
		}
		const auto mask = uint64_t {1} << (key & 63);
		const bool was = (words[word_index] & mask) != 0;
		words[word_index] |= mask;
		return !was;
	}

	void EnableUncheckedProbe() {
		EnsureCapacity(6000000);
		probe_unchecked = true;
	}

	void InsertUnchecked(int64_t orderkey) {
		if (orderkey < 0) {
			return;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			EnsureCapacity(key);
		}
		words[word_index] |= uint64_t {1} << (key & 63);
	}

private:
	bool ContainsUnchecked(uint64_t key) const {
		const auto mask = uint64_t {1} << (key & 63);
		return (words[key >> 6] & mask) != 0;
	}

	void EnsureCapacity(uint64_t max_bit_inclusive) {
		const auto needed_words = (max_bit_inclusive >> 6) + 1;
		if (needed_words <= words.size()) {
			return;
		}
		auto new_size = words.empty() ? needed_words : words.size();
		while (new_size < needed_words) {
			new_size *= 2;
		}
		words.resize(new_size, 0);
	}

	std::vector<uint64_t> words;
	bool probe_unchecked = false;
};

class QualifyingSupplierSet {
public:
	bool Contains(int64_t suppkey) const {
		if (suppkey < 0) {
			return false;
		}
		const auto index = static_cast<size_t>(suppkey);
		return index < flags.size() && flags[index] != 0;
	}

	bool Insert(int64_t suppkey) {
		if (suppkey < 0) {
			return false;
		}
		const auto index = static_cast<size_t>(suppkey);
		if (index >= flags.size()) {
			EnsureCapacity(suppkey);
		}
		const bool was = flags[index] != 0;
		flags[index] = 1;
		return !was;
	}

private:
	void EnsureCapacity(int64_t suppkey) {
		const auto required = static_cast<size_t>(suppkey) + 1;
		if (required <= flags.size()) {
			return;
		}
		size_t target = required;
		if (!flags.empty()) {
			const size_t doubled = flags.size() > (std::numeric_limits<size_t>::max() / 2) ? std::numeric_limits<size_t>::max()
			                                                                               : flags.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		flags.resize(target, 0);
	}

	std::vector<uint8_t> flags;
};

class Q21Kernel final : public KernelInstance {
public:
	explicit Q21Kernel(Q21Parameters params_p) : params(std::move(params_p)), config() {
		stored_lines.reserve(1'200'000);
		active_qualifying_suppkeys.reserve(512);
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q21InputId>(input_id)) {
		case Q21InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		case Q21InputId::ORDERS:
			ConsumeOrders(input);
			return;
		case Q21InputId::LINEITEM:
			ConsumeLineitem(input);
			return;
		default:
			throw duckdb::InternalException("Q21 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		std::sort(stored_lines.begin(), stored_lines.end(),
		          [](const StoredLineitem &lhs, const StoredLineitem &rhs) { return lhs.orderkey < rhs.orderkey; });

		for (duckdb::idx_t index = 0; index < stored_lines.size();) {
			const auto orderkey = stored_lines[index].orderkey;
			duckdb::idx_t end = index + 1;
			while (end < stored_lines.size() && stored_lines[end].orderkey == orderkey) {
				end++;
			}
			const auto line_count = end - index;
			for (duckdb::idx_t line_index = index; line_index < end; line_index++) {
				const auto &line = stored_lines[line_index];
				if (!qualifying_suppliers.Contains(line.suppkey)) {
					continue;
				}
				if (line.receiptdate <= line.commitdate) {
					continue;
				}
				bool has_other_supplier = false;
				bool other_supplier_late = false;
				for (duckdb::idx_t other_index = index; other_index < end; other_index++) {
					const auto &other = stored_lines[other_index];
					if (other.suppkey == line.suppkey) {
						continue;
					}
					has_other_supplier = true;
					if (other.receiptdate > other.commitdate) {
						other_supplier_late = true;
						break;
					}
				}
				if (!has_other_supplier || other_supplier_late) {
					continue;
				}
				EnsureSupplierCapacity(line.suppkey);
				wait_count_by_suppkey[static_cast<size_t>(line.suppkey)] += 1;
			}
			(void)line_count;
			index = end;
		}

		output_rows.clear();
		for (const auto suppkey : active_qualifying_suppkeys) {
			if (suppkey < 0 || static_cast<uint64_t>(suppkey) >= static_cast<uint64_t>(supplier_capacity)) {
				continue;
			}
			const auto count = wait_count_by_suppkey[static_cast<size_t>(suppkey)];
			if (count == 0) {
				continue;
			}
			const auto name_it = supplier_names.find(suppkey);
			if (name_it == supplier_names.end()) {
				continue;
			}
			Q21OutputRow row;
			row.name = name_it->second;
			row.numwait = count;
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
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
			output.SetValue(0, row, duckdb::Value(entry.name));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.numwait));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void EnsureSupplierCapacity(int64_t suppkey) {
		if (suppkey < 0) {
			return;
		}
		const auto required = suppkey + 1;
		if (required <= supplier_capacity) {
			return;
		}
		int64_t target = required;
		if (supplier_capacity > 0) {
			const int64_t doubled = supplier_capacity > (std::numeric_limits<int64_t>::max() / 2)
			                              ? std::numeric_limits<int64_t>::max()
			                              : supplier_capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		wait_count_by_suppkey.resize(static_cast<size_t>(target), 0);
		supplier_capacity = target;
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		if (params.target_nationkey < 0) {
			return;
		}
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		input.data[Q21SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(), suppkey_format);
		input.data[Q21SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(input.size(), name_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			if (qualifying_suppliers.Insert(suppkey)) {
				active_qualifying_suppkeys.push_back(suppkey);
			}
			supplier_names.emplace(suppkey,
			                       ReadString(duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]));
		}
	}

	void MarkFinishedOrder(int64_t orderkey) {
		finished_orders.InsertUnchecked(orderkey);
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		auto &orderkey_vector = input.data[Q21OrdersChunkLayout::ORDERKEY_INDEX];

		if (config.orders_flat_fast && IsFlatAllValid(orderkey_vector)) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				MarkFinishedOrder(orderkeys[row]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			if (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) {
				continue;
			}
			MarkFinishedOrder(orderkeys[orderkey_index]);
		}
	}

	void StoreLineitemRow(int64_t orderkey, int64_t suppkey, int32_t commitdate, int32_t receiptdate) {
		stored_lines.push_back(StoredLineitem {orderkey, suppkey, commitdate, receiptdate});
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		if (params.target_nationkey < 0) {
			return;
		}
		if (!finished_orders_probe_ready) {
			// Order-filtered scans can arrive before the orders producer has
			// materialized its bitmap.  Keep the bounds-checked probe here; the
			// unchecked probe assumes a non-null backing allocation and was not
			// safe for that valid pipeline ordering.
			finished_orders_probe_ready = true;
		}
		auto &orderkey_vector = input.data[Q21LineitemChunkLayout::ORDERKEY_INDEX];
		auto &suppkey_vector = input.data[Q21LineitemChunkLayout::SUPPKEY_INDEX];
		auto &commitdate_vector = input.data[Q21LineitemChunkLayout::COMMITDATE_INDEX];
		auto &receiptdate_vector = input.data[Q21LineitemChunkLayout::RECEIPTDATE_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) &&
		    IsFlatAllValid(commitdate_vector) && IsFlatAllValid(receiptdate_vector)) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto commitdates = duckdb::FlatVector::GetData<int32_t>(commitdate_vector);
			auto receiptdates = duckdb::FlatVector::GetData<int32_t>(receiptdate_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey = orderkeys[row];
				if (!finished_orders.Contains(orderkey)) {
					continue;
				}
				StoreLineitemRow(orderkey, suppkeys[row], commitdates[row], receiptdates[row]);
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(suppkey_vector) &&
		    IsDictionaryOverFlat(commitdate_vector) && IsDictionaryOverFlat(receiptdate_vector)) {
			auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &commitdate_sel = duckdb::DictionaryVector::SelVector(commitdate_vector);
			auto &receiptdate_sel = duckdb::DictionaryVector::SelVector(receiptdate_vector);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto commitdates = duckdb::FlatVector::GetData<int32_t>(duckdb::DictionaryVector::Child(commitdate_vector));
			auto receiptdates = duckdb::FlatVector::GetData<int32_t>(duckdb::DictionaryVector::Child(receiptdate_vector));
			auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &commitdate_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(commitdate_vector));
			auto &receiptdate_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(receiptdate_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto commitdate_index = commitdate_sel.get_index(row);
				const auto receiptdate_index = receiptdate_sel.get_index(row);
				if (!orderkey_validity.RowIsValid(orderkey_index) || !suppkey_validity.RowIsValid(suppkey_index) ||
				    !commitdate_validity.RowIsValid(commitdate_index) ||
				    !receiptdate_validity.RowIsValid(receiptdate_index)) {
					continue;
				}
				const auto orderkey = orderkeys[orderkey_index];
				if (!finished_orders.Contains(orderkey)) {
					continue;
				}
				StoreLineitemRow(orderkey, suppkeys[suppkey_index], commitdates[commitdate_index],
				                 receiptdates[receiptdate_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat commitdate_format;
		duckdb::UnifiedVectorFormat receiptdate_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		commitdate_vector.ToUnifiedFormat(input.size(), commitdate_format);
		receiptdate_vector.ToUnifiedFormat(input.size(), receiptdate_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto commitdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(commitdate_format);
		auto receiptdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(receiptdate_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto commitdate_index = commitdate_format.sel->get_index(row);
			const auto receiptdate_index = receiptdate_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!commitdate_format.validity.AllValid() && !commitdate_format.validity.RowIsValidUnsafe(commitdate_index)) ||
			    (!receiptdate_format.validity.AllValid() &&
			     !receiptdate_format.validity.RowIsValidUnsafe(receiptdate_index))) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			if (!finished_orders.Contains(orderkey)) {
				continue;
			}
			StoreLineitemRow(orderkey, suppkeys[suppkey_index], commitdates[commitdate_index],
			                 receiptdates[receiptdate_index]);
		}
	}

	Q21Parameters params;
	Q21RuntimeConfig config;
	bool finished_orders_probe_ready = false;
	int64_t supplier_capacity = 0;
	OrderKeyBitmap finished_orders;
	QualifyingSupplierSet qualifying_suppliers;
	std::vector<int64_t> active_qualifying_suppkeys;
	std::unordered_map<int64_t, std::string> supplier_names;
	std::vector<int64_t> wait_count_by_suppkey;
	std::vector<StoredLineitem> stored_lines;
	std::vector<Q21OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ21Kernel(const Q21Parameters &params) {
	return duckdb::make_uniq<Q21Kernel>(params);
}

} // namespace accelerator_codegen
