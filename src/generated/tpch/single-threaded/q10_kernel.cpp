#include "accelerator_codegen/tpch_q10.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kAbsentOrderCustomer = 0;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int32_t kMaxNationSlots = 25;
constexpr duckdb::idx_t kCustomerCustkeyScanIndex = 0;
constexpr duckdb::idx_t kCustomerNameScanIndex = 1;
constexpr duckdb::idx_t kCustomerAcctbalScanIndex = 2;
constexpr duckdb::idx_t kCustomerNationkeyScanIndex = 3;
constexpr duckdb::idx_t kCustomerAddressScanIndex = 4;
constexpr duckdb::idx_t kCustomerPhoneScanIndex = 5;
constexpr duckdb::idx_t kCustomerCommentScanIndex = 6;
constexpr duckdb::idx_t kCustomerProjectedColumnCount = 7;

#if defined(__SIZEOF_INT128__)
using q10_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q10_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddRevenue(q10_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q10_acc_t>(extendedprice_raw) * static_cast<q10_acc_t>(100 - discount_raw);
}
#else
using q10_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q10_acc_t &value) {
	return value;
}

void AddRevenue(q10_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q10 revenue");
	}
}
#endif

struct Q10RuntimeConfig {
	bool dense_state = true;
	bool orders_flat_fast = true;
	bool orders_dict_fast = true;
	bool lineitem_flat_fast = true;
	bool lineitem_dict_fast = true;
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

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

struct Q10OutputRow {
	int64_t custkey = 0;
	std::string name;
	duckdb::hugeint_t revenue;
	duckdb::hugeint_t acctbal;
	std::string nation_name;
	std::string address;
	std::string phone;
	std::string comment;
};

bool BetterOutputRow(const Q10OutputRow &lhs, const Q10OutputRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.revenue, rhs.revenue)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.revenue, lhs.revenue)) {
		return false;
	}
	return lhs.custkey < rhs.custkey;
}

class ActiveOrderStore {
public:
	void Store(int64_t orderkey, int64_t custkey) {
		if (orderkey < 0) {
			return;
		}
		MarkActive(orderkey);
		order_custkeys[orderkey] = custkey;
	}

	bool IsActive(int64_t orderkey) const {
		if (orderkey < 0) {
			return false;
		}
		const auto index = static_cast<size_t>(orderkey);
		return index < order_active_bitmap.size() && order_active_bitmap[index] != 0;
	}

	int64_t Custkey(int64_t orderkey) const {
		const auto it = order_custkeys.find(orderkey);
		if (it == order_custkeys.end()) {
			return kAbsentOrderCustomer;
		}
		return it->second;
	}

private:
	void MarkActive(int64_t orderkey) {
		EnsureOrderBitmapCapacity(orderkey);
		order_active_bitmap[static_cast<size_t>(orderkey)] = 1;
	}

	void EnsureOrderBitmapCapacity(int64_t orderkey) {
		if (orderkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(orderkey + 1);
		if (required <= order_active_bitmap.size()) {
			return;
		}
		size_t target = required;
		if (!order_active_bitmap.empty()) {
			const size_t doubled = order_active_bitmap.size() > (std::numeric_limits<size_t>::max() / 2)
			                           ? std::numeric_limits<size_t>::max()
			                           : order_active_bitmap.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultOrderCapacity));
		}
		order_active_bitmap.resize(target, 0);
	}

	std::vector<uint8_t> order_active_bitmap;
	std::unordered_map<int64_t, int64_t> order_custkeys;
};

class DenseRevenueStore {
public:
	bool Has(int64_t custkey) const {
		if (custkey < 0 || custkey >= capacity) {
			return false;
		}
		return has_revenue[static_cast<size_t>(custkey)] != 0;
	}

	q10_acc_t Load(int64_t custkey) const {
		if (custkey < 0 || custkey >= capacity) {
			return static_cast<q10_acc_t>(0);
		}
		return revenue[static_cast<size_t>(custkey)];
	}

	void Add(int64_t custkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (custkey < 0) {
			return;
		}
		EnsureCapacity(custkey + 1);
		const auto index = static_cast<size_t>(custkey);
		if (!has_revenue[index]) {
			has_revenue[index] = 1;
			active_keys.push_back(custkey);
		}
		AddRevenue(revenue[index], extendedprice_raw, discount_raw);
	}

	const std::vector<int64_t> &ActiveKeys() const {
		return active_keys;
	}

private:
	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2) ? std::numeric_limits<int64_t>::max()
			                                                                               : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, kDefaultCustomerCapacity);
		}
		revenue.resize(static_cast<size_t>(target), 0);
		has_revenue.resize(static_cast<size_t>(target), 0);
		capacity = target;
	}

	int64_t capacity = 0;
	std::vector<q10_acc_t> revenue;
	std::vector<uint8_t> has_revenue;
	std::vector<int64_t> active_keys;
};

class Q10Kernel final : public KernelInstance {
public:
	explicit Q10Kernel(Q10Parameters params_p, Q10ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config() {
		nation_names.fill(std::string());
		if (profile) {
			profile->used_dense_state = true;
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q10InputId>(input_id)) {
		case Q10InputId::NATION:
			ConsumeNation(input);
			return;
		case Q10InputId::ORDERS:
			ConsumeOrders(input);
			return;
		case Q10InputId::LINEITEM:
			ConsumeLineitem(input);
			return;
		case Q10InputId::CUSTOMER:
			if (fused_customer_scanned) {
				throw duckdb::InternalException("Q10 customer already consumed via fused scan-consumer");
			}
			ConsumeCustomer(input);
			return;
		default:
			throw duckdb::InternalException("Q10 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		output_rows.reserve(revenue_by_customer.ActiveKeys().size());
		for (const auto custkey : revenue_by_customer.ActiveKeys()) {
			if (custkey < 0 || custkey >= customer_capacity) {
				continue;
			}
			const auto index = static_cast<size_t>(custkey);
			if (customer_names[index].empty()) {
				continue;
			}
			const auto nationkey = customer_nationkeys[index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto &nation_name = nation_names[static_cast<size_t>(nationkey)];
			if (nation_name.empty()) {
				continue;
			}
			Q10OutputRow row;
			row.custkey = custkey;
			row.name = customer_names[index];
			row.revenue = FromAccumulator(revenue_by_customer.Load(custkey));
			row.acctbal = duckdb::hugeint_t(customer_acctbal_raw[index]);
			row.nation_name = nation_name;
			row.address = customer_addresses[index];
			row.phone = customer_phones[index];
			row.comment = customer_comments[index];
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.custkey));
			output.SetValue(1, row, duckdb::Value(entry.name));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
			output.SetValue(3, row, duckdb::Value::DECIMAL(entry.acctbal, 15, 2));
			output.SetValue(4, row, duckdb::Value(entry.nation_name));
			output.SetValue(5, row, duckdb::Value(entry.address));
			output.SetValue(6, row, duckdb::Value(entry.phone));
			output.SetValue(7, row, duckdb::Value(entry.comment));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

	void RunFusedCustomerScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_customer_scanned) {
			return;
		}
		if (revenue_by_customer.ActiveKeys().empty()) {
			fused_customer_scanned = true;
			return;
		}
		EnsureActiveCustomerBitmap();
		if (profile) {
			profile->used_fused_customer = true;
		}

		static constexpr duckdb::idx_t kOtherScanIndices[] = {
		    kCustomerNameScanIndex,     kCustomerAcctbalScanIndex, kCustomerNationkeyScanIndex,
		    kCustomerAddressScanIndex,  kCustomerPhoneScanIndex,   kCustomerCommentScanIndex,
		};

		struct CustomerMatch {
			duckdb::idx_t batch_row = 0;
			int64_t custkey = 0;
		};

		auto consumer = [this](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                       duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			if (profile) {
				profile->customer_fused_rows_scanned += count;
			}

			duckdb::Vector custkey_vec(duckdb::LogicalType::BIGINT);
			const auto scan_count = row_group.ScanProjectedColumn(transaction, scan_state, kCustomerCustkeyScanIndex,
			                                                      custkey_vec, duckdb::UpdateScanType::STANDARD);
			CustomerMatch matches[STANDARD_VECTOR_SIZE];
			duckdb::idx_t match_count = 0;

			if (IsFlatAllValid(custkey_vec) && scan_count >= count) {
				auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vec);
				const bool identity = sel.data() == nullptr;
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto vector_index = identity ? row : sel.get_index(row);
					const auto custkey = custkeys[vector_index];
					if (custkey < 0 || custkey >= customer_capacity ||
					    active_customer_bitmap[static_cast<size_t>(custkey)] == 0) {
						continue;
					}
					matches[match_count++] = CustomerMatch {row, custkey};
				}
			} else {
				duckdb::UnifiedVectorFormat custkey_format;
				custkey_vec.ToUnifiedFormat(scan_count, custkey_format);
				auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto base_index = sel.get_index(row);
					const auto custkey_index = custkey_format.sel->get_index(base_index);
					if (!custkey_format.validity.AllValid() &&
					    !custkey_format.validity.RowIsValidUnsafe(custkey_index)) {
						if (profile) {
							profile->generic_fallback_rows += 1;
						}
						continue;
					}
					const auto custkey = custkeys[custkey_index];
					if (custkey < 0 || custkey >= customer_capacity ||
					    active_customer_bitmap[static_cast<size_t>(custkey)] == 0) {
						continue;
					}
					matches[match_count++] = CustomerMatch {row, custkey};
				}
			}

			if (match_count == 0) {
				for (const auto column_index : kOtherScanIndices) {
					row_group.SkipProjectedColumn(scan_state, column_index);
				}
				return duckdb::FilteredScanConsumerResult::CONTINUE;
			}

			duckdb::SelectionVector match_sel(match_count);
			for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
				match_sel.set_index(match_index, matches[match_index].batch_row);
			}

			duckdb::Vector name_vec(duckdb::LogicalType::VARCHAR);
			duckdb::Vector acctbal_vec(duckdb::LogicalType::DECIMAL(15, 2));
			duckdb::Vector nationkey_vec(duckdb::LogicalType::INTEGER);
			duckdb::Vector address_vec(duckdb::LogicalType::VARCHAR);
			duckdb::Vector phone_vec(duckdb::LogicalType::VARCHAR);
			duckdb::Vector comment_vec(duckdb::LogicalType::VARCHAR);
			row_group.SelectProjectedColumn(transaction, scan_state, kCustomerNameScanIndex, match_sel, match_count,
			                                name_vec, duckdb::UpdateScanType::STANDARD);
			row_group.SelectProjectedColumn(transaction, scan_state, kCustomerAcctbalScanIndex, match_sel, match_count,
			                                acctbal_vec, duckdb::UpdateScanType::STANDARD);
			row_group.SelectProjectedColumn(transaction, scan_state, kCustomerNationkeyScanIndex, match_sel, match_count,
			                                nationkey_vec, duckdb::UpdateScanType::STANDARD);
			row_group.SelectProjectedColumn(transaction, scan_state, kCustomerAddressScanIndex, match_sel, match_count,
			                                address_vec, duckdb::UpdateScanType::STANDARD);
			row_group.SelectProjectedColumn(transaction, scan_state, kCustomerPhoneScanIndex, match_sel, match_count,
			                                phone_vec, duckdb::UpdateScanType::STANDARD);
			row_group.SelectProjectedColumn(transaction, scan_state, kCustomerCommentScanIndex, match_sel, match_count,
			                                comment_vec, duckdb::UpdateScanType::STANDARD);

			const bool flat_payload = IsFlatAllValid(name_vec) && IsFlatAllValid(acctbal_vec) &&
			                          IsFlatAllValid(nationkey_vec) && IsFlatAllValid(address_vec) &&
			                          IsFlatAllValid(phone_vec) && IsFlatAllValid(comment_vec);
			if (flat_payload) {
				auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vec);
				auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vec);
				auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vec);
				auto addresses = duckdb::FlatVector::GetData<duckdb::string_t>(address_vec);
				auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vec);
				auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vec);
				for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
					const auto &match = matches[match_index];
					StoreCustomerRow(match.custkey, names[match_index], acctbals[match_index], nationkeys[match_index],
					                 addresses[match_index], phones[match_index], comments[match_index]);
					if (profile) {
						profile->customer_fused_rows_matched += 1;
					}
				}
			} else {
				duckdb::UnifiedVectorFormat formats[6];
				name_vec.ToUnifiedFormat(match_count, formats[0]);
				acctbal_vec.ToUnifiedFormat(match_count, formats[1]);
				nationkey_vec.ToUnifiedFormat(match_count, formats[2]);
				address_vec.ToUnifiedFormat(match_count, formats[3]);
				phone_vec.ToUnifiedFormat(match_count, formats[4]);
				comment_vec.ToUnifiedFormat(match_count, formats[5]);
				auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[1]);
				auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(formats[2]);
				for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
					const auto indices = std::array<duckdb::idx_t, 6> {
					    formats[0].sel->get_index(match_index), formats[1].sel->get_index(match_index),
					    formats[2].sel->get_index(match_index), formats[3].sel->get_index(match_index),
					    formats[4].sel->get_index(match_index), formats[5].sel->get_index(match_index)};
					for (duckdb::idx_t col = 0; col < 6; col++) {
						if (!formats[col].validity.AllValid() &&
						    !formats[col].validity.RowIsValidUnsafe(indices[col])) {
							goto skip_match;
						}
					}
					StoreCustomerRow(
					    matches[match_index].custkey,
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[0])[indices[0]],
					    acctbals[indices[1]], nationkeys[indices[2]],
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[3])[indices[3]],
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[4])[indices[4]],
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[5])[indices[5]]);
					if (profile) {
						profile->customer_fused_rows_matched += 1;
					}
				skip_match:;
				}
			}
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		};

		if (!RunNativeFilteredScanConsumer(context, bind_data, consumer)) {
			throw duckdb::InterruptException();
		}
		fused_customer_scanned = true;
	}

	bool FusedCustomerScanned() const {
		return fused_customer_scanned;
	}

private:
	void EnsureCustomerCapacity(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		const auto required = custkey + 1;
		if (required <= customer_capacity) {
			return;
		}
		int64_t target = required;
		if (customer_capacity > 0) {
			const int64_t doubled = customer_capacity > (std::numeric_limits<int64_t>::max() / 2)
			                              ? std::numeric_limits<int64_t>::max()
			                              : customer_capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, kDefaultCustomerCapacity);
		}
		customer_names.resize(static_cast<size_t>(target));
		customer_acctbal_raw.resize(static_cast<size_t>(target), 0);
		customer_nationkeys.resize(static_cast<size_t>(target), 0);
		customer_addresses.resize(static_cast<size_t>(target));
		customer_phones.resize(static_cast<size_t>(target));
		customer_comments.resize(static_cast<size_t>(target));
		active_customer_bitmap.resize(static_cast<size_t>(target), 0);
		customer_capacity = target;
	}

	void StoreCustomerRow(int64_t custkey, duckdb::string_t name, int64_t acctbal_raw, int32_t nationkey,
	                      duckdb::string_t address, duckdb::string_t phone, duckdb::string_t comment) {
		EnsureCustomerCapacity(custkey);
		const auto index = static_cast<size_t>(custkey);
		customer_names[index] = ReadString(name);
		customer_acctbal_raw[index] = acctbal_raw;
		customer_nationkeys[index] = nationkey;
		customer_addresses[index] = ReadString(address);
		customer_phones[index] = ReadString(phone);
		customer_comments[index] = ReadString(comment);
		if (profile) {
			profile->customer_rows_stored += 1;
		}
	}

	void ConsumeNation(duckdb::DataChunk &input) {
		auto &nationkey_vector = input.data[Q10NationChunkLayout::NATIONKEY_INDEX];
		auto &name_vector = input.data[Q10NationChunkLayout::NAME_INDEX];
		if (IsFlatAllValid(nationkey_vector) && IsFlatAllValid(name_vector)) {
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots) {
					continue;
				}
				nation_names[static_cast<size_t>(nationkey)] = ReadString(names[row]);
			}
			return;
		}
		duckdb::UnifiedVectorFormat nationkey_format;
		duckdb::UnifiedVectorFormat name_format;
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto name_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
			nation_names[static_cast<size_t>(nationkey)] = ReadString(name_value);
		}
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		auto &orderkey_vector = input.data[Q10OrdersChunkLayout::ORDERKEY_INDEX];
		auto &custkey_vector = input.data[Q10OrdersChunkLayout::CUSTKEY_INDEX];
		if (config.orders_flat_fast && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector)) {
			if (profile) {
				profile->orders_flat_fast_rows += input.size();
			}
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->orders_rows_scanned += 1;
				}
				order_customers.Store(orderkeys[row], custkeys[row]);
			}
			return;
		}

		if (config.orders_dict_fast && IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(custkey_vector)) {
			if (profile) {
				profile->orders_dict_fast_rows += input.size();
			}
			auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
			auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
			auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->orders_rows_scanned += 1;
				}
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto custkey_index = custkey_sel.get_index(row);
				if (!orderkey_validity.RowIsValid(orderkey_index) || !custkey_validity.RowIsValid(custkey_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				order_customers.Store(orderkeys[orderkey_index], custkeys[custkey_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat custkey_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->orders_rows_scanned += 1;
			}
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto custkey_index = custkey_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			order_customers.Store(orderkeys[orderkey_index], custkeys[custkey_index]);
		}
	}

	void ProcessLineitemRow(int64_t orderkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (!order_customers.IsActive(orderkey)) {
			return;
		}
		const auto custkey = order_customers.Custkey(orderkey);
		if (custkey == kAbsentOrderCustomer) {
			return;
		}
		if (profile) {
			profile->lineitem_rows_matched += 1;
		}
		revenue_by_customer.Add(custkey, extendedprice_raw, discount_raw);
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		if (profile) {
			profile->lineitem_rows_scanned += input.size();
		}
		auto &orderkey_vector = input.data[Q10LineitemChunkLayout::ORDERKEY_INDEX];
		auto &extendedprice_vector = input.data[Q10LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q10LineitemChunkLayout::DISCOUNT_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(extendedprice_vector) &&
		    IsFlatAllValid(discount_vector)) {
			if (profile) {
				profile->lineitem_flat_fast_rows += input.size();
			}
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				ProcessLineitemRow(orderkeys[row], extendedprices[row], discounts[row]);
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(orderkey_vector) &&
		    IsDictionaryOverFlat(extendedprice_vector) && IsDictionaryOverFlat(discount_vector)) {
			if (profile) {
				profile->lineitem_dict_fast_rows += input.size();
			}
			auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			auto extendedprices =
			    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
			auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
			auto &extendedprice_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto extendedprice_index = extendedprice_sel.get_index(row);
				const auto discount_index = discount_sel.get_index(row);
				if (!orderkey_validity.RowIsValid(orderkey_index) ||
				    !extendedprice_validity.RowIsValid(extendedprice_index) ||
				    !discount_validity.RowIsValid(discount_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				ProcessLineitemRow(orderkeys[orderkey_index], extendedprices[extendedprice_index],
				                   discounts[discount_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			ProcessLineitemRow(orderkeys[orderkey_index], extendedprices[extendedprice_index],
			                   discounts[discount_index]);
		}
	}

	void EnsureActiveCustomerBitmap() {
		if (active_customer_bitmap_ready) {
			return;
		}
		for (const auto custkey : revenue_by_customer.ActiveKeys()) {
			if (custkey >= 0) {
				EnsureCustomerCapacity(custkey);
				active_customer_bitmap[static_cast<size_t>(custkey)] = 1;
			}
		}
		active_customer_bitmap_ready = true;
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		if (revenue_by_customer.ActiveKeys().empty()) {
			return;
		}
		EnsureActiveCustomerBitmap();
		auto &custkey_vector = input.data[Q10CustomerChunkLayout::CUSTKEY_INDEX];
		auto &name_vector = input.data[Q10CustomerChunkLayout::NAME_INDEX];
		auto &acctbal_vector = input.data[Q10CustomerChunkLayout::ACCTBAL_INDEX];
		auto &nationkey_vector = input.data[Q10CustomerChunkLayout::NATIONKEY_INDEX];
		auto &address_vector = input.data[Q10CustomerChunkLayout::ADDRESS_INDEX];
		auto &phone_vector = input.data[Q10CustomerChunkLayout::PHONE_INDEX];
		auto &comment_vector = input.data[Q10CustomerChunkLayout::COMMENT_INDEX];

		if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(name_vector) && IsFlatAllValid(acctbal_vector) &&
		    IsFlatAllValid(nationkey_vector) && IsFlatAllValid(address_vector) && IsFlatAllValid(phone_vector) &&
		    IsFlatAllValid(comment_vector)) {
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			auto addresses = duckdb::FlatVector::GetData<duckdb::string_t>(address_vector);
			auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vector);
			auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto custkey = custkeys[row];
				if (custkey < 0 || custkey >= customer_capacity ||
				    active_customer_bitmap[static_cast<size_t>(custkey)] == 0) {
					continue;
				}
				StoreCustomerRow(custkey, names[row], acctbals[row], nationkeys[row], addresses[row], phones[row],
				                 comments[row]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat formats[7];
		for (duckdb::idx_t col = 0; col < 7; col++) {
			input.data[col].ToUnifiedFormat(input.size(), formats[col]);
		}
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[Q10CustomerChunkLayout::CUSTKEY_INDEX]);
		auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(formats[Q10CustomerChunkLayout::ACCTBAL_INDEX]);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(formats[Q10CustomerChunkLayout::NATIONKEY_INDEX]);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto indices = std::array<duckdb::idx_t, 7> {
			    formats[0].sel->get_index(row), formats[1].sel->get_index(row), formats[2].sel->get_index(row),
			    formats[3].sel->get_index(row), formats[4].sel->get_index(row), formats[5].sel->get_index(row),
			    formats[6].sel->get_index(row)};
			for (duckdb::idx_t col = 0; col < 7; col++) {
				if (!formats[col].validity.AllValid() && !formats[col].validity.RowIsValidUnsafe(indices[col])) {
					goto skip_row;
				}
			}
			{
				const auto custkey = custkeys[indices[0]];
				if (custkey < 0 || custkey >= customer_capacity ||
				    active_customer_bitmap[static_cast<size_t>(custkey)] == 0) {
					goto skip_row;
				}
				StoreCustomerRow(
				    custkey,
				    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(formats[Q10CustomerChunkLayout::NAME_INDEX])
				        [indices[1]],
				    acctbals[indices[2]], nationkeys[indices[3]],
				    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(
				        formats[Q10CustomerChunkLayout::ADDRESS_INDEX])[indices[4]],
				    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(
				        formats[Q10CustomerChunkLayout::PHONE_INDEX])[indices[5]],
				    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(
				        formats[Q10CustomerChunkLayout::COMMENT_INDEX])[indices[6]]);
			}
		skip_row:;
		}
	}

	Q10Parameters params;
	Q10ProfileCounters *profile;
	Q10RuntimeConfig config;
	std::array<std::string, kMaxNationSlots> nation_names;
	ActiveOrderStore order_customers;
	DenseRevenueStore revenue_by_customer;
	std::vector<std::string> customer_names;
	std::vector<int64_t> customer_acctbal_raw;
	std::vector<int32_t> customer_nationkeys;
	std::vector<std::string> customer_addresses;
	std::vector<std::string> customer_phones;
	std::vector<std::string> customer_comments;
	std::vector<uint8_t> active_customer_bitmap;
	int64_t customer_capacity = 0;
	bool active_customer_bitmap_ready = false;
	bool fused_customer_scanned = false;
	std::vector<Q10OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

bool Q10FusedScansEnabled() {
	return true;
}

void Q10RunFusedCustomerScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	auto &q10_kernel = static_cast<Q10Kernel &>(kernel);
	q10_kernel.RunFusedCustomerScan(context, bind_data);
}

duckdb::unique_ptr<KernelInstance> CreateQ10Kernel(const Q10Parameters &params) {
	return duckdb::make_uniq<Q10Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ10ProfilingKernel(const Q10Parameters &params, Q10ProfileCounters *profile) {
	return duckdb::make_uniq<Q10Kernel>(params, profile);
}

} // namespace accelerator_codegen
