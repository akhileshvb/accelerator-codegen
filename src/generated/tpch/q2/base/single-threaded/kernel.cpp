#include "tpch/q2/base/query.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr duckdb::idx_t kOutputLimit = 100;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr duckdb::idx_t kPartsuppPartkeyScanIndex = 0;
constexpr duckdb::idx_t kPartsuppSuppkeyScanIndex = 1;
constexpr duckdb::idx_t kPartsuppSupplycostScanIndex = 2;
constexpr duckdb::idx_t kPartsuppProjectedColumnCount = 3;
constexpr duckdb::idx_t kSupplierSuppkeyScanIndex = 0;
constexpr duckdb::idx_t kSupplierNameScanIndex = 1;
constexpr duckdb::idx_t kSupplierAddressScanIndex = 2;
constexpr duckdb::idx_t kSupplierPhoneScanIndex = 3;
constexpr duckdb::idx_t kSupplierCommentScanIndex = 4;
constexpr duckdb::idx_t kSupplierVarcharProjectedColumnCount = 5;

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool SqlLikeAt(const char *text, duckdb::idx_t text_len, const char *pattern, duckdb::idx_t pattern_len,
               duckdb::idx_t text_pos, duckdb::idx_t pattern_pos) {
	while (pattern_pos < pattern_len) {
		const char pattern_ch = pattern[pattern_pos];
		if (pattern_ch == '%') {
			if (pattern_pos + 1 == pattern_len) {
				return true;
			}
			for (duckdb::idx_t start = text_pos; start <= text_len; start++) {
				if (SqlLikeAt(text, text_len, pattern, pattern_len, start, pattern_pos + 1)) {
					return true;
				}
			}
			return false;
		}
		if (pattern_ch == '_') {
			if (text_pos >= text_len) {
				return false;
			}
			text_pos++;
			pattern_pos++;
			continue;
		}
		if (text_pos >= text_len || text[text_pos] != pattern_ch) {
			return false;
		}
		text_pos++;
		pattern_pos++;
	}
	return text_pos == text_len;
}

bool SqlLike(const duckdb::string_t &text, const std::string &pattern) {
	return SqlLikeAt(text.GetData(), text.GetSize(), pattern.data(), pattern.size(), 0, 0);
}

struct TwoPartLikeMatcher {
	std::string part1;
	std::string part2;
	bool enabled = false;

	explicit TwoPartLikeMatcher(const std::string &pattern) {
		if (pattern.size() < 4 || pattern.front() != '%' || pattern.back() != '%') {
			return;
		}
		const auto first_end = pattern.find('%', 1);
		if (first_end == std::string::npos || first_end == 1) {
			return;
		}
		const auto second_start = first_end + 1;
		const auto second_end = pattern.find('%', second_start);
		if (second_end == std::string::npos || second_end == second_start) {
			return;
		}
		if (pattern.find('%', second_end + 1) != std::string::npos) {
			return;
		}
		part1 = pattern.substr(1, first_end - 1);
		part2 = pattern.substr(second_start, second_end - second_start);
		enabled = !part1.empty() && !part2.empty();
	}

	bool Matches(const duckdb::string_t &text) const {
		if (!enabled) {
			return false;
		}
		const std::string_view haystack(text.GetData(), text.GetSize());
		size_t pos = 0;
		while (pos + part1.size() <= haystack.size()) {
			const auto found = haystack.find(part1, pos);
			if (found == std::string_view::npos) {
				return false;
			}
			if (haystack.find(part2, found + part1.size()) != std::string_view::npos) {
				return true;
			}
			pos = found + 1;
		}
		return false;
	}
};

struct SupplierCandidate {
	int64_t suppkey = 0;
	int64_t supplycost_raw = 0;
};

struct Q2OutputRow {
	int64_t acctbal_raw = 0;
	std::string supplier_name;
	std::string nation_name;
	int64_t partkey = 0;
	std::string mfgr;
	std::string address;
	std::string phone;
	std::string comment;
};

bool BetterOutputRow(const Q2OutputRow &lhs, const Q2OutputRow &rhs) {
	if (lhs.acctbal_raw != rhs.acctbal_raw) {
		return lhs.acctbal_raw > rhs.acctbal_raw;
	}
	if (lhs.nation_name != rhs.nation_name) {
		return lhs.nation_name < rhs.nation_name;
	}
	if (lhs.supplier_name != rhs.supplier_name) {
		return lhs.supplier_name < rhs.supplier_name;
	}
	return lhs.partkey < rhs.partkey;
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

class SupplierDenseStore {
public:
	void Store(int64_t suppkey, int32_t nationkey, int64_t acctbal_raw) {
		if (suppkey < 0) {
			return;
		}
		if (suppkey >= capacity) {
			EnsureCapacity(suppkey + 1);
		}
		const auto index = static_cast<size_t>(suppkey);
		nationkeys[index] = nationkey;
		acctbals[index] = acctbal_raw;
		has_data[index] = 1;
	}

	bool HasData(int64_t suppkey) const {
		return suppkey >= 0 && suppkey < capacity && has_data[static_cast<size_t>(suppkey)] != 0;
	}

	int32_t NationKey(int64_t suppkey) const {
		return nationkeys[static_cast<size_t>(suppkey)];
	}

	int64_t AcctBalRaw(int64_t suppkey) const {
		return acctbals[static_cast<size_t>(suppkey)];
	}

	void EnsureDefaultCapacity() {
		if (capacity < kDefaultSupplierCapacity) {
			EnsureCapacity(kDefaultSupplierCapacity);
		}
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
			target = std::max(target, kDefaultSupplierCapacity);
		}
		const auto target_size = static_cast<size_t>(target);
		nationkeys.resize(target_size, 0);
		acctbals.resize(target_size, 0);
		has_data.resize(target_size, 0);
		capacity = target;
	}

	int64_t capacity = 0;
	std::vector<int32_t> nationkeys;
	std::vector<int64_t> acctbals;
	std::vector<uint8_t> has_data;
};

class QualifyingPartSet {
public:
	bool Contains(int64_t partkey) const {
		if (partkey < 0) {
			return false;
		}
		const auto index = static_cast<size_t>(partkey);
		return index < flags.size() && flags[index] != 0;
	}

	void Insert(int64_t partkey, std::string mfgr) {
		if (partkey < 0) {
			return;
		}
		const auto index = static_cast<size_t>(partkey);
		if (index >= flags.size()) {
			EnsureCapacity(partkey);
		}
		if (flags[index] == 0) {
			flags[index] = 1;
			mfgr_by_part.emplace(partkey, std::move(mfgr));
		}
	}

	const std::string *Mfgr(int64_t partkey) const {
		const auto it = mfgr_by_part.find(partkey);
		return it == mfgr_by_part.end() ? nullptr : &it->second;
	}

	bool Empty() const {
		return mfgr_by_part.empty();
	}

private:
	void EnsureCapacity(int64_t partkey) {
		const auto required = static_cast<size_t>(partkey) + 1;
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
	std::unordered_map<int64_t, std::string> mfgr_by_part;
};

struct SupplierStrings {
	std::string name;
	std::string address;
	std::string phone;
	std::string comment;
};

class Q2Kernel final : public KernelInstance {
public:
	explicit Q2Kernel(Q2Parameters params_p)
	    : params(std::move(params_p)), part_type_matcher(params.part_type_pattern),
	      fallback_part_type_pattern(params.part_type_pattern) {
		supplier_store.EnsureDefaultCapacity();
	}

	void RunFusedPartsuppScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_partsupp_scanned || qualifying_parts.Empty()) {
			fused_partsupp_scanned = true;
			return;
		}

		static constexpr duckdb::idx_t kKeyScanIndices[] = {kPartsuppPartkeyScanIndex, kPartsuppSuppkeyScanIndex};

		auto consumer = [this](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                       duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			if (scan_state.GetColumnIds().size() != kPartsuppProjectedColumnCount) {
				throw duckdb::InternalException("Q2 fused partsupp expected %llu projected columns",
				                                static_cast<unsigned long long>(kPartsuppProjectedColumnCount));
			}

			duckdb::Vector partkey_vec(duckdb::LogicalType::BIGINT);
			duckdb::Vector suppkey_vec(duckdb::LogicalType::BIGINT);
			duckdb::Vector *key_payload[] = {&partkey_vec, &suppkey_vec};
			const auto scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kKeyScanIndices, 2,
			                                                       key_payload, duckdb::UpdateScanType::STANDARD);

			struct PartsuppSurvivor {
				duckdb::idx_t vector_index = 0;
				int64_t partkey = 0;
				int64_t suppkey = 0;
			};
			PartsuppSurvivor survivors[STANDARD_VECTOR_SIZE];
			duckdb::idx_t match_count = 0;
			const bool identity = sel.data() == nullptr;

			if (IsFlatAllValid(partkey_vec) && IsFlatAllValid(suppkey_vec) && scan_count >= count) {
				auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vec);
				auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vec);
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto vector_index = identity ? row : sel.get_index(row);
					const auto partkey = partkeys[vector_index];
					if (!qualifying_parts.Contains(partkey)) {
						continue;
					}
					const auto suppkey = suppkeys[vector_index];
					if (!supplier_store.HasData(suppkey)) {
						continue;
					}
					survivors[match_count++] = PartsuppSurvivor {vector_index, partkey, suppkey};
				}
			} else {
				duckdb::UnifiedVectorFormat partkey_format;
				duckdb::UnifiedVectorFormat suppkey_format;
				partkey_vec.ToUnifiedFormat(scan_count, partkey_format);
				suppkey_vec.ToUnifiedFormat(scan_count, suppkey_format);
				auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
				auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto base_index = sel.get_index(row);
					const auto partkey_index = partkey_format.sel->get_index(base_index);
					const auto suppkey_index = suppkey_format.sel->get_index(base_index);
					if ((!partkey_format.validity.AllValid() &&
					     !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
					    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index))) {
						continue;
					}
					const auto partkey = partkeys[partkey_index];
					if (!qualifying_parts.Contains(partkey)) {
						continue;
					}
					const auto suppkey = suppkeys[suppkey_index];
					if (!supplier_store.HasData(suppkey)) {
						continue;
					}
					survivors[match_count++] = PartsuppSurvivor {base_index, partkey, suppkey};
				}
			}

			if (match_count == 0) {
				return duckdb::FilteredScanConsumerResult::CONTINUE;
			}

			duckdb::Vector supplycost_vec(duckdb::LogicalType::DECIMAL(15, 2));
			row_group.ScanProjectedColumn(transaction, scan_state, kPartsuppSupplycostScanIndex, supplycost_vec,
			                              duckdb::UpdateScanType::STANDARD);

			if (IsFlatAllValid(supplycost_vec) && scan_count >= count) {
				auto supplycosts = duckdb::FlatVector::GetData<int64_t>(supplycost_vec);
				for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
					const auto &survivor = survivors[match_index];
					ConsiderPartsuppRow(survivor.partkey, survivor.suppkey,
					                  supplycosts[survivor.vector_index]);
				}
			} else {
				duckdb::UnifiedVectorFormat supplycost_format;
				supplycost_vec.ToUnifiedFormat(scan_count, supplycost_format);
				auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);
				for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
					const auto &survivor = survivors[match_index];
					const auto cost_index = supplycost_format.sel->get_index(survivor.vector_index);
					if (!supplycost_format.validity.AllValid() &&
					    !supplycost_format.validity.RowIsValidUnsafe(cost_index)) {
						continue;
					}
					ConsiderPartsuppRow(survivor.partkey, survivor.suppkey, supplycosts[cost_index]);
				}
			}
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		};

		RunNativeFilteredScanConsumer(context, bind_data, consumer);
		fused_partsupp_scanned = true;
	}

	void RunFusedSupplierVarcharScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_supplier_varchar_scanned) {
			return;
		}
		RebuildNeededSuppkeys();
		if (needed_suppkeys.empty()) {
			fused_supplier_varchar_scanned = true;
			return;
		}

		static constexpr duckdb::idx_t kPayloadScanIndices[] = {
		    kSupplierNameScanIndex, kSupplierAddressScanIndex, kSupplierPhoneScanIndex, kSupplierCommentScanIndex};

		auto consumer = [this](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                       duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			if (scan_state.GetColumnIds().size() != kSupplierVarcharProjectedColumnCount) {
				throw duckdb::InternalException("Q2 fused supplier varchar expected %llu projected columns",
				                                static_cast<unsigned long long>(kSupplierVarcharProjectedColumnCount));
			}

			duckdb::Vector suppkey_vec(duckdb::LogicalType::BIGINT);
			const auto scan_count = row_group.ScanProjectedColumn(transaction, scan_state, kSupplierSuppkeyScanIndex,
			                                                      suppkey_vec, duckdb::UpdateScanType::STANDARD);

			struct SupplierMatch {
				duckdb::idx_t vector_index = 0;
				int64_t suppkey = 0;
			};
			SupplierMatch matches[STANDARD_VECTOR_SIZE];
			duckdb::idx_t match_count = 0;
			const bool identity = sel.data() == nullptr;

			if (IsFlatAllValid(suppkey_vec) && scan_count >= count) {
				auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vec);
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto vector_index = identity ? row : sel.get_index(row);
					const auto suppkey = suppkeys[vector_index];
					if (needed_suppkeys.find(suppkey) == needed_suppkeys.end()) {
						continue;
					}
					matches[match_count++] = SupplierMatch {vector_index, suppkey};
				}
			} else {
				duckdb::UnifiedVectorFormat suppkey_format;
				suppkey_vec.ToUnifiedFormat(scan_count, suppkey_format);
				auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto base_index = sel.get_index(row);
					const auto suppkey_index = suppkey_format.sel->get_index(base_index);
					if (!suppkey_format.validity.AllValid() &&
					    !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) {
						continue;
					}
					const auto suppkey = suppkeys[suppkey_index];
					if (needed_suppkeys.find(suppkey) == needed_suppkeys.end()) {
						continue;
					}
					matches[match_count++] = SupplierMatch {base_index, suppkey};
				}
			}

			if (match_count == 0) {
				return duckdb::FilteredScanConsumerResult::CONTINUE;
			}

			duckdb::Vector name_vec(duckdb::LogicalType::VARCHAR);
			duckdb::Vector address_vec(duckdb::LogicalType::VARCHAR);
			duckdb::Vector phone_vec(duckdb::LogicalType::VARCHAR);
			duckdb::Vector comment_vec(duckdb::LogicalType::VARCHAR);
			duckdb::Vector *payloads[] = {&name_vec, &address_vec, &phone_vec, &comment_vec};
			row_group.ScanProjectedColumns(transaction, scan_state, kPayloadScanIndices, 4, payloads,
			                               duckdb::UpdateScanType::STANDARD);

			if (IsFlatAllValid(name_vec) && IsFlatAllValid(address_vec) && IsFlatAllValid(phone_vec) &&
			    IsFlatAllValid(comment_vec) && scan_count >= count) {
				auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vec);
				auto addresses = duckdb::FlatVector::GetData<duckdb::string_t>(address_vec);
				auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vec);
				auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vec);
				for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
					const auto &match = matches[match_index];
					const auto vector_index = match.vector_index;
					StoreSupplierStrings(match.suppkey, names[vector_index], addresses[vector_index],
					                     phones[vector_index], comments[vector_index]);
				}
			} else {
				duckdb::UnifiedVectorFormat name_format;
				duckdb::UnifiedVectorFormat address_format;
				duckdb::UnifiedVectorFormat phone_format;
				duckdb::UnifiedVectorFormat comment_format;
				name_vec.ToUnifiedFormat(scan_count, name_format);
				address_vec.ToUnifiedFormat(scan_count, address_format);
				phone_vec.ToUnifiedFormat(scan_count, phone_format);
				comment_vec.ToUnifiedFormat(scan_count, comment_format);
				for (duckdb::idx_t match_index = 0; match_index < match_count; match_index++) {
					const auto &match = matches[match_index];
					const auto vector_index = match.vector_index;
					const auto name_index = name_format.sel->get_index(vector_index);
					const auto address_index = address_format.sel->get_index(vector_index);
					const auto phone_index = phone_format.sel->get_index(vector_index);
					const auto comment_index = comment_format.sel->get_index(vector_index);
					if ((!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
					    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index)) ||
					    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index)) ||
					    (!comment_format.validity.AllValid() && !comment_format.validity.RowIsValidUnsafe(comment_index))) {
						continue;
					}
					StoreSupplierStrings(
					    match.suppkey,
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index],
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index],
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index],
					    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comment_format)[comment_index]);
				}
			}
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		};

		RunNativeFilteredScanConsumer(context, bind_data, consumer);
		fused_supplier_varchar_scanned = true;
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q2InputId>(input_id)) {
		case Q2InputId::PART:
			ConsumePart(input);
			return;
		case Q2InputId::SUPPLIER_NUMERIC:
			ConsumeSupplierNumeric(input);
			return;
		case Q2InputId::PARTSUPP:
			if (fused_partsupp_scanned) {
				throw duckdb::InternalException("Q2 partsupp already consumed via fused scan-consumer");
			}
			ConsumePartsupp(input);
			return;
		case Q2InputId::SUPPLIER_VARCHAR:
			if (fused_supplier_varchar_scanned) {
				throw duckdb::InternalException("Q2 supplier varchar already consumed via fused scan-consumer");
			}
			ConsumeSupplierVarchar(input);
			return;
		default:
			throw duckdb::InternalException("Q2 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		output_rows.reserve(part_candidates.size());
		for (const auto &entry : part_candidates) {
			const int64_t partkey = entry.first;
			const auto *mfgr = qualifying_parts.Mfgr(partkey);
			if (mfgr == nullptr) {
				continue;
			}
			for (const auto &candidate : entry.second) {
				if (!supplier_store.HasData(candidate.suppkey)) {
					continue;
				}
				const int32_t nationkey = supplier_store.NationKey(candidate.suppkey);
				const auto nation_it = params.nation_names.find(nationkey);
				if (nation_it == params.nation_names.end()) {
					continue;
				}
				const auto strings_it = supplier_strings.find(candidate.suppkey);
				if (strings_it == supplier_strings.end()) {
					continue;
				}
				Q2OutputRow output_row;
				output_row.acctbal_raw = supplier_store.AcctBalRaw(candidate.suppkey);
				output_row.supplier_name = strings_it->second.name;
				output_row.nation_name = nation_it->second;
				output_row.partkey = partkey;
				output_row.mfgr = *mfgr;
				output_row.address = strings_it->second.address;
				output_row.phone = strings_it->second.phone;
				output_row.comment = strings_it->second.comment;
				output_rows.push_back(std::move(output_row));
			}
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		if (output_rows.size() > kOutputLimit) {
			output_rows.resize(kOutputLimit);
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
			output.SetValue(0, row, duckdb::Value::DECIMAL(duckdb::hugeint_t(entry.acctbal_raw), 15, 2));
			output.SetValue(1, row, duckdb::Value(entry.supplier_name));
			output.SetValue(2, row, duckdb::Value(entry.nation_name));
			output.SetValue(3, row, duckdb::Value::BIGINT(entry.partkey));
			output.SetValue(4, row, duckdb::Value(entry.mfgr));
			output.SetValue(5, row, duckdb::Value(entry.address));
			output.SetValue(6, row, duckdb::Value(entry.phone));
			output.SetValue(7, row, duckdb::Value(entry.comment));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	bool IsRegionNation(int32_t nationkey) const {
		return nationkey >= 0 && static_cast<size_t>(nationkey) < params.region_nations.size() &&
		       params.region_nations[static_cast<size_t>(nationkey)];
	}

	bool PartTypeMatches(const duckdb::string_t &type_value) const {
		if (part_type_matcher.enabled) {
			return part_type_matcher.Matches(type_value);
		}
		return SqlLike(type_value, fallback_part_type_pattern);
	}

	void ConsumePart(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q2PartChunkLayout::PARTKEY_INDEX];
		auto &mfgr_vector = input.data[Q2PartChunkLayout::MFGR_INDEX];
		auto &type_vector = input.data[Q2PartChunkLayout::TYPE_INDEX];
		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(mfgr_vector) && IsFlatAllValid(type_vector)) {
			const auto count = input.size();
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto mfgrs = duckdb::FlatVector::GetData<duckdb::string_t>(mfgr_vector);
			auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_vector);
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!PartTypeMatches(types[row])) {
					continue;
				}
				qualifying_parts.Insert(partkeys[row], ReadString(mfgrs[row]));
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat mfgr_format;
		duckdb::UnifiedVectorFormat type_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		mfgr_vector.ToUnifiedFormat(input.size(), mfgr_format);
		type_vector.ToUnifiedFormat(input.size(), type_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto mfgr_index = mfgr_format.sel->get_index(row);
			const auto type_index = type_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!mfgr_format.validity.AllValid() && !mfgr_format.validity.RowIsValidUnsafe(mfgr_index)) ||
			    (!type_format.validity.AllValid() && !type_format.validity.RowIsValidUnsafe(type_index))) {
				continue;
			}
			const auto type_value =
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format)[type_index];
			if (!PartTypeMatches(type_value)) {
				continue;
			}
			qualifying_parts.Insert(partkeys[partkey_index],
			                        ReadString(
			                            duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(mfgr_format)[mfgr_index]));
		}
	}

	void ConsumeSupplierNumeric(duckdb::DataChunk &input) {
		auto &suppkey_vector = input.data[Q2SupplierNumericChunkLayout::SUPPKEY_INDEX];
		auto &nationkey_vector = input.data[Q2SupplierNumericChunkLayout::NATIONKEY_INDEX];
		auto &acctbal_vector = input.data[Q2SupplierNumericChunkLayout::ACCTBAL_INDEX];
		if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector) && IsFlatAllValid(acctbal_vector)) {
			const auto count = input.size();
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
			for (duckdb::idx_t row = 0; row < count; row++) {
				const int32_t nationkey = nationkeys[row];
				if (!IsRegionNation(nationkey)) {
					continue;
				}
				supplier_store.Store(suppkeys[row], nationkey, acctbals[row]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat nationkey_format;
		duckdb::UnifiedVectorFormat acctbal_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		acctbal_vector.ToUnifiedFormat(input.size(), acctbal_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(acctbal_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			const auto acctbal_index = acctbal_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!nationkey_format.validity.AllValid() &&
			     !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
			    (!acctbal_format.validity.AllValid() && !acctbal_format.validity.RowIsValidUnsafe(acctbal_index))) {
				continue;
			}
			const int32_t nationkey = nationkeys[nationkey_index];
			if (!IsRegionNation(nationkey)) {
				continue;
			}
			supplier_store.Store(suppkeys[suppkey_index], nationkey, acctbals[acctbal_index]);
		}
	}

	void StoreSupplierStrings(int64_t suppkey, const duckdb::string_t &name, const duckdb::string_t &address,
	                          const duckdb::string_t &phone, const duckdb::string_t &comment) {
		auto &strings = supplier_strings[suppkey];
		strings.name = ReadString(name);
		strings.address = ReadString(address);
		strings.phone = ReadString(phone);
		strings.comment = ReadString(comment);
	}

	void ConsumeSupplierVarchar(duckdb::DataChunk &input) {
		RebuildNeededSuppkeys();
		if (needed_suppkeys.empty()) {
			return;
		}
		auto &suppkey_vector = input.data[Q2SupplierVarcharChunkLayout::SUPPKEY_INDEX];
		auto &name_vector = input.data[Q2SupplierVarcharChunkLayout::NAME_INDEX];
		auto &address_vector = input.data[Q2SupplierVarcharChunkLayout::ADDRESS_INDEX];
		auto &phone_vector = input.data[Q2SupplierVarcharChunkLayout::PHONE_INDEX];
		auto &comment_vector = input.data[Q2SupplierVarcharChunkLayout::COMMENT_INDEX];
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat address_format;
		duckdb::UnifiedVectorFormat phone_format;
		duckdb::UnifiedVectorFormat comment_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);
		address_vector.ToUnifiedFormat(input.size(), address_format);
		phone_vector.ToUnifiedFormat(input.size(), phone_format);
		comment_vector.ToUnifiedFormat(input.size(), comment_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto address_index = address_format.sel->get_index(row);
			const auto phone_index = phone_format.sel->get_index(row);
			const auto comment_index = comment_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index)) ||
			    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index)) ||
			    (!comment_format.validity.AllValid() && !comment_format.validity.RowIsValidUnsafe(comment_index))) {
				continue;
			}
			const int64_t suppkey = suppkeys[suppkey_index];
			if (needed_suppkeys.find(suppkey) == needed_suppkeys.end()) {
				continue;
			}
			StoreSupplierStrings(
			    suppkey, duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index],
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index],
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index],
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comment_format)[comment_index]);
		}
	}

	void RebuildNeededSuppkeys() {
		needed_suppkeys.clear();
		for (const auto &entry : part_candidates) {
			for (const auto &candidate : entry.second) {
				needed_suppkeys.insert(candidate.suppkey);
			}
		}
	}

	void ConsiderPartsuppRow(int64_t partkey, int64_t suppkey, int64_t supplycost_raw) {
		if (!qualifying_parts.Contains(partkey)) {
			return;
		}
		if (!supplier_store.HasData(suppkey)) {
			return;
		}

		auto [min_it, inserted] = min_supplycost_by_part.try_emplace(partkey, supplycost_raw);
		if (inserted) {
			part_candidates[partkey] = {{suppkey, supplycost_raw}};
			return;
		}

		if (supplycost_raw < min_it->second) {
			min_it->second = supplycost_raw;
			part_candidates[partkey] = {{suppkey, supplycost_raw}};
			return;
		}
		if (supplycost_raw == min_it->second) {
			part_candidates[partkey].push_back({suppkey, supplycost_raw});
		}
	}

	void ConsumePartsupp(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q2PartsuppChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q2PartsuppChunkLayout::SUPPKEY_INDEX];
		auto &supplycost_vector = input.data[Q2PartsuppChunkLayout::SUPPLYCOST_INDEX];
		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(supplycost_vector)) {
			const auto count = input.size();
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto supplycosts = duckdb::FlatVector::GetData<int64_t>(supplycost_vector);
			for (duckdb::idx_t row = 0; row < count; row++) {
				ConsiderPartsuppRow(partkeys[row], suppkeys[row], supplycosts[row]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat supplycost_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		supplycost_vector.ToUnifiedFormat(input.size(), supplycost_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto supplycost_index = supplycost_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!supplycost_format.validity.AllValid() &&
			     !supplycost_format.validity.RowIsValidUnsafe(supplycost_index))) {
				continue;
			}
			ConsiderPartsuppRow(partkeys[partkey_index], suppkeys[suppkey_index], supplycosts[supplycost_index]);
		}
	}

	Q2Parameters params;
	TwoPartLikeMatcher part_type_matcher;
	std::string fallback_part_type_pattern;
	QualifyingPartSet qualifying_parts;
	SupplierDenseStore supplier_store;
	std::unordered_map<int64_t, SupplierStrings> supplier_strings;
	std::unordered_map<int64_t, int64_t> min_supplycost_by_part;
	std::unordered_map<int64_t, std::vector<SupplierCandidate>> part_candidates;
	std::unordered_set<int64_t> needed_suppkeys;
	bool fused_partsupp_scanned = false;
	bool fused_supplier_varchar_scanned = false;
	std::vector<Q2OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

bool Q2FusedScansEnabled() {
	return true;
}

void Q2RunFusedPartsuppScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	static_cast<Q2Kernel &>(kernel).RunFusedPartsuppScan(context, bind_data);
}

void Q2RunFusedSupplierVarcharScan(duckdb::ClientContext &context, KernelInstance &kernel,
                                   const duckdb::FunctionData &bind_data) {
	static_cast<Q2Kernel &>(kernel).RunFusedSupplierVarcharScan(context, bind_data);
}

duckdb::unique_ptr<KernelInstance> CreateQ2Kernel(const Q2Parameters &params) {
	return duckdb::make_uniq<Q2Kernel>(params);
}

} // namespace accelerator_codegen
