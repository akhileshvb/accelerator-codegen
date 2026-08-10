#include "tpch/q13/base/query.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultCustomerCapacity = 150001;

struct Q13RuntimeConfig {
	bool dense_state = true;
	bool customer_flat_fast = true;
	bool orders_flat_fast = true;
	bool orders_dict_fast = true;
	bool two_part_like = true;
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

struct Q13OutputRow {
	int64_t order_count = 0;
	int64_t customer_count = 0;
};

bool BetterOutputRow(const Q13OutputRow &lhs, const Q13OutputRow &rhs) {
	if (lhs.customer_count != rhs.customer_count) {
		return lhs.customer_count > rhs.customer_count;
	}
	return lhs.order_count > rhs.order_count;
}

class Q13Kernel final : public KernelInstance {
public:
	explicit Q13Kernel(Q13Parameters params_p, Q13ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(),
	      pattern_matcher(params.comment_pattern), fallback_pattern(params.comment_pattern) {
		active_customers.reserve(160000);
		if (profile) {
			profile->used_dense_state = true;
		}
		if (config.two_part_like && pattern_matcher.enabled) {
			if (profile) {
				profile->used_two_part_like = true;
			}
		}
	}

	void EnsureCustomerCapacity(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(custkey) + 1;
		if (required <= customer_valid.size()) {
			return;
		}
		size_t target = required;
		if (!customer_valid.empty()) {
			const size_t doubled = customer_valid.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultCustomerCapacity));
		}
		customer_valid.resize(target, 0);
		order_counts.resize(target, 0);
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q13InputId>(input_id)) {
		case Q13InputId::CUSTOMER:
			ConsumeCustomer(input);
			return;
		case Q13InputId::ORDERS:
			ConsumeOrders(input);
			return;
		default:
			throw duckdb::InternalException("Q13 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		std::vector<int64_t> customers_per_count;
		customers_per_count.reserve(256);
		const auto customer_capacity = static_cast<int64_t>(customer_valid.size());
		for (const auto custkey : active_customers) {
			if (custkey < 0 || custkey >= customer_capacity) {
				continue;
			}
			const auto order_count = order_counts[static_cast<size_t>(custkey)];
			if (order_count >= static_cast<int64_t>(customers_per_count.size())) {
				customers_per_count.resize(static_cast<size_t>(order_count + 1), 0);
			}
			customers_per_count[static_cast<size_t>(order_count)] += 1;
		}

		output_rows.clear();
		output_rows.reserve(customers_per_count.size());
		for (int64_t order_count = 0; order_count < static_cast<int64_t>(customers_per_count.size()); order_count++) {
			const auto customer_count = customers_per_count[static_cast<size_t>(order_count)];
			if (customer_count == 0) {
				continue;
			}
			Q13OutputRow row;
			row.order_count = order_count;
			row.customer_count = customer_count;
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.order_count));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.customer_count));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	bool CommentMatchesPattern(const duckdb::string_t &comment) const {
		if (config.two_part_like && pattern_matcher.enabled) {
			return pattern_matcher.Matches(comment);
		}
		return SqlLike(comment, fallback_pattern);
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		auto &custkey_vector = input.data[Q13CustomerChunkLayout::CUSTKEY_INDEX];
		if (config.customer_flat_fast && IsFlatAllValid(custkey_vector)) {
			if (profile) {
				profile->customer_flat_fast_rows += input.size();
			}
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->customer_rows_scanned += 1;
				}
				const auto custkey = custkeys[row];
				if (custkey < 0) {
					continue;
				}
				EnsureCustomerCapacity(custkey);
				if (customer_valid[static_cast<size_t>(custkey)] == 0) {
					customer_valid[static_cast<size_t>(custkey)] = 1;
					active_customers.push_back(custkey);
				}
			}
			return;
		}

		duckdb::UnifiedVectorFormat custkey_format;
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->customer_rows_scanned += 1;
			}
			const auto custkey_index = custkey_format.sel->get_index(row);
			if (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (custkey < 0) {
				continue;
			}
			EnsureCustomerCapacity(custkey);
			if (customer_valid[static_cast<size_t>(custkey)] == 0) {
				customer_valid[static_cast<size_t>(custkey)] = 1;
				active_customers.push_back(custkey);
			}
		}
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		auto &custkey_vector = input.data[Q13OrdersChunkLayout::CUSTKEY_INDEX];
		auto &comment_vector = input.data[Q13OrdersChunkLayout::COMMENT_INDEX];

		if (config.orders_flat_fast && IsFlatAllValid(custkey_vector) && IsFlatAllValid(comment_vector)) {
			if (profile) {
				profile->orders_flat_fast_rows += input.size();
			}
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->orders_rows_scanned += 1;
				}
				const auto custkey = custkeys[row];
				if (custkey < 0) {
					continue;
				}
				EnsureCustomerCapacity(custkey);
				if (customer_valid[static_cast<size_t>(custkey)] == 0) {
					continue;
				}
				if (CommentMatchesPattern(comments[row])) {
					continue;
				}
				order_counts[static_cast<size_t>(custkey)] += 1;
				if (profile) {
					profile->orders_rows_matched += 1;
				}
			}
			return;
		}

		if (config.orders_dict_fast && IsDictionaryOverFlat(custkey_vector) && IsDictionaryOverFlat(comment_vector)) {
			if (profile) {
				profile->orders_dict_fast_rows += input.size();
			}
			auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
			auto &comment_sel = duckdb::DictionaryVector::SelVector(comment_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
			auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(comment_vector));
			auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
			auto &comment_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(comment_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->orders_rows_scanned += 1;
				}
				const auto custkey_index = custkey_sel.get_index(row);
				const auto comment_index = comment_sel.get_index(row);
				if (!custkey_validity.RowIsValid(custkey_index) || !comment_validity.RowIsValid(comment_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				const auto custkey = custkeys[custkey_index];
				if (custkey < 0) {
					continue;
				}
				EnsureCustomerCapacity(custkey);
				if (customer_valid[static_cast<size_t>(custkey)] == 0) {
					continue;
				}
				if (CommentMatchesPattern(comments[comment_index])) {
					continue;
				}
				order_counts[static_cast<size_t>(custkey)] += 1;
				if (profile) {
					profile->orders_rows_matched += 1;
				}
			}
			return;
		}

		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat comment_format;
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		comment_vector.ToUnifiedFormat(input.size(), comment_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->orders_rows_scanned += 1;
			}
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto comment_index = comment_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!comment_format.validity.AllValid() && !comment_format.validity.RowIsValidUnsafe(comment_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (custkey < 0) {
				continue;
			}
			EnsureCustomerCapacity(custkey);
			if (customer_valid[static_cast<size_t>(custkey)] == 0) {
				continue;
			}
			const auto comment =
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comment_format)[comment_index];
			if (CommentMatchesPattern(comment)) {
				continue;
			}
			order_counts[static_cast<size_t>(custkey)] += 1;
			if (profile) {
				profile->orders_rows_matched += 1;
			}
		}
	}

	Q13Parameters params;
	Q13ProfileCounters *profile;
	Q13RuntimeConfig config;
	TwoPartLikeMatcher pattern_matcher;
	std::string fallback_pattern;
	std::vector<uint8_t> customer_valid;
	std::vector<int32_t> order_counts;
	std::vector<int64_t> active_customers;
	std::vector<Q13OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ13Kernel(const Q13Parameters &params) {
	return duckdb::make_uniq<Q13Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ13ProfilingKernel(const Q13Parameters &params, Q13ProfileCounters *profile) {
	return duckdb::make_uniq<Q13Kernel>(params, profile);
}

} // namespace accelerator_codegen
