#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/tpch_q13.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultCustomerCapacity = 150001;

bool Q13ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
}

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

struct Q13FrozenState {
	std::vector<uint8_t> customer_valid;
	std::vector<int64_t> active_customers;

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
	}

	size_t CustomerStateBytes() const {
		return customer_valid.size() + active_customers.size() * sizeof(int64_t);
	}
};

struct Q13WorkerAgg {
	std::vector<int32_t> order_counts;

	void Reset(size_t capacity) {
		order_counts.assign(capacity, 0);
	}
};

struct Q13ParallelProfile {
	int64_t total_ns = 0;
	int64_t customer_build_ns = 0;
	int64_t orders_scan_ns = 0;
	int64_t merge_ns = 0;
	int64_t histogram_ns = 0;
	int64_t sort_ns = 0;
	size_t worker_count = 0;
	size_t customers_scanned = 0;
	size_t customer_domain_size = 0;
	size_t active_customer_count = 0;
	size_t orders_scanned = 0;
	size_t orders_rejected_comment = 0;
	size_t orders_qualifying = 0;
	size_t comments_inspected = 0;
	size_t comment_bytes_inspected = 0;
	size_t customer_count_updates = 0;
	size_t merge_slots = 0;
	size_t histogram_buckets = 0;
	size_t max_order_count = 0;
	size_t customer_state_bytes = 0;
	size_t worker_count_bytes = 0;
	size_t customer_flat_rows = 0;
	size_t customer_fallback_rows = 0;
	size_t orders_flat_rows = 0;
	size_t orders_dict_rows = 0;
	size_t orders_fallback_rows = 0;
	int64_t max_worker_ns = 0;
	int64_t avg_worker_ns = 0;
};

struct Q13WorkerOrdersProfile {
	size_t rows = 0;
	int64_t elapsed_ns = 0;
};

void PrintQ13ParallelProfile(const Q13ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	std::cerr << "q13_parallel_profile total_ns=" << profile.total_ns
	          << " customer_build_ns=" << profile.customer_build_ns << " (" << pct(profile.customer_build_ns) << "%)"
	          << " orders_scan_ns=" << profile.orders_scan_ns << " (" << pct(profile.orders_scan_ns) << "%)"
	          << " merge_ns=" << profile.merge_ns << " (" << pct(profile.merge_ns) << "%)"
	          << " histogram_ns=" << profile.histogram_ns << " (" << pct(profile.histogram_ns) << "%)"
	          << " sort_ns=" << profile.sort_ns << " (" << pct(profile.sort_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " customers_scanned=" << profile.customers_scanned
	          << " customer_domain_size=" << profile.customer_domain_size
	          << " active_customer_count=" << profile.active_customer_count << " orders_scanned=" << profile.orders_scanned
	          << " orders_rejected_comment=" << profile.orders_rejected_comment
	          << " orders_qualifying=" << profile.orders_qualifying << " comments_inspected=" << profile.comments_inspected
	          << " comment_bytes_inspected=" << profile.comment_bytes_inspected
	          << " customer_count_updates=" << profile.customer_count_updates << " merge_slots=" << profile.merge_slots
	          << " histogram_buckets=" << profile.histogram_buckets << " max_order_count=" << profile.max_order_count
	          << " customer_state_bytes=" << profile.customer_state_bytes
	          << " worker_count_bytes=" << profile.worker_count_bytes
	          << " customer_flat_rows=" << profile.customer_flat_rows
	          << " customer_fallback_rows=" << profile.customer_fallback_rows
	          << " orders_flat_rows=" << profile.orders_flat_rows << " orders_dict_rows=" << profile.orders_dict_rows
	          << " orders_fallback_rows=" << profile.orders_fallback_rows << " max_worker_ns=" << profile.max_worker_ns
	          << " avg_worker_ns=" << profile.avg_worker_ns << '\n';
}

void BuildCustomerState(duckdb::ClientContext &context, const duckdb::FunctionData &customer_bind,
                        Q13FrozenState &state, Q13ParallelProfile *profile) {
	auto source = CreateNativeChunkSource(context, customer_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (profile) {
			profile->customers_scanned += chunk.size();
		}
		auto &custkey_vector = chunk.data[Q13CustomerChunkLayout::CUSTKEY_INDEX];
		if (IsFlatAllValid(custkey_vector)) {
			if (profile) {
				profile->customer_flat_rows += chunk.size();
			}
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto custkey = custkeys[row];
				if (custkey < 0) {
					continue;
				}
				state.EnsureCustomerCapacity(custkey);
				const auto index = static_cast<size_t>(custkey);
				if (state.customer_valid[index] == 0) {
					state.customer_valid[index] = 1;
					state.active_customers.push_back(custkey);
				}
			}
			continue;
		}
		if (profile) {
			profile->customer_fallback_rows += chunk.size();
		}
		duckdb::UnifiedVectorFormat custkey_format;
		custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto custkey_index = custkey_format.sel->get_index(row);
			if (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) {
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (custkey < 0) {
				continue;
			}
			state.EnsureCustomerCapacity(custkey);
			const auto index = static_cast<size_t>(custkey);
			if (state.customer_valid[index] == 0) {
				state.customer_valid[index] = 1;
				state.active_customers.push_back(custkey);
			}
		}
	}
}

bool CustomerExists(const Q13FrozenState &state, int64_t custkey) {
	return custkey >= 0 && static_cast<size_t>(custkey) < state.customer_valid.size() &&
	       state.customer_valid[static_cast<size_t>(custkey)] != 0;
}

bool CommentDisqualifies(const duckdb::string_t &comment, const TwoPartLikeMatcher &matcher,
                         const std::string &fallback_pattern, Q13ParallelProfile *profile) {
	if (profile) {
		profile->comments_inspected += 1;
		profile->comment_bytes_inspected += comment.GetSize();
	}
	return matcher.enabled ? matcher.Matches(comment) : SqlLike(comment, fallback_pattern);
}

void ProcessQualifyingOrder(int64_t custkey, Q13WorkerAgg &agg, Q13ParallelProfile *profile) {
	agg.order_counts[static_cast<size_t>(custkey)] += 1;
	if (profile) {
		profile->orders_qualifying += 1;
		profile->customer_count_updates += 1;
	}
}

void AccumulateOrdersChunk(duckdb::DataChunk &chunk, const Q13FrozenState &state, const TwoPartLikeMatcher &matcher,
                           const std::string &fallback_pattern, Q13WorkerAgg &agg, Q13ParallelProfile *profile) {
	if (profile) {
		profile->orders_scanned += chunk.size();
	}
	auto &custkey_vector = chunk.data[Q13OrdersChunkLayout::CUSTKEY_INDEX];
	auto &comment_vector = chunk.data[Q13OrdersChunkLayout::COMMENT_INDEX];

	if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(comment_vector)) {
		if (profile) {
			profile->orders_flat_rows += chunk.size();
		}
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto custkey = custkeys[row];
			if (!CustomerExists(state, custkey)) {
				continue;
			}
			if (CommentDisqualifies(comments[row], matcher, fallback_pattern, profile)) {
				if (profile) {
					profile->orders_rejected_comment += 1;
				}
				continue;
			}
			ProcessQualifyingOrder(custkey, agg, profile);
		}
		return;
	}

	if (IsDictionaryOverFlat(custkey_vector) && IsDictionaryOverFlat(comment_vector)) {
		if (profile) {
			profile->orders_dict_rows += chunk.size();
		}
		auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
		auto &comment_sel = duckdb::DictionaryVector::SelVector(comment_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
		auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(comment_vector));
		auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
		auto &comment_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(comment_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto custkey_index = custkey_sel.get_index(row);
			const auto comment_index = comment_sel.get_index(row);
			if (!custkey_validity.RowIsValid(custkey_index) || !comment_validity.RowIsValid(comment_index)) {
				if (profile) {
					profile->orders_fallback_rows += 1;
				}
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (!CustomerExists(state, custkey)) {
				continue;
			}
			if (CommentDisqualifies(comments[comment_index], matcher, fallback_pattern, profile)) {
				if (profile) {
					profile->orders_rejected_comment += 1;
				}
				continue;
			}
			ProcessQualifyingOrder(custkey, agg, profile);
		}
		return;
	}

	if (profile) {
		profile->orders_fallback_rows += chunk.size();
	}
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat comment_format;
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	comment_vector.ToUnifiedFormat(chunk.size(), comment_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto comment_index = comment_format.sel->get_index(row);
		if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!comment_format.validity.AllValid() && !comment_format.validity.RowIsValidUnsafe(comment_index))) {
			continue;
		}
		const auto custkey = custkeys[custkey_index];
		if (!CustomerExists(state, custkey)) {
			continue;
		}
		const auto comment = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comment_format)[comment_index];
		if (CommentDisqualifies(comment, matcher, fallback_pattern, profile)) {
			if (profile) {
				profile->orders_rejected_comment += 1;
			}
			continue;
		}
		ProcessQualifyingOrder(custkey, agg, profile);
	}
}

void MergeOrderCounts(std::vector<int32_t> &global, const std::vector<int32_t> &local, Q13ParallelProfile *profile) {
	if (local.size() > global.size()) {
		global.resize(local.size(), 0);
	}
	for (size_t i = 0; i < local.size(); i++) {
		global[i] += local[i];
	}
	if (profile) {
		profile->merge_slots += local.size();
	}
}

class Q13ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q13ParallelKernel(Q13Parameters params_p)
	    : params(std::move(params_p)), pattern_matcher(params.comment_pattern), fallback_pattern(params.comment_pattern) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q13 parallel expects bound customer and orders scans");
		}

		using clock = std::chrono::steady_clock;
		const bool profile_enabled = Q13ProfileEnabled();
		Q13ParallelProfile profile;
		const auto total_t0 = clock::now();

		Q13FrozenState frozen;
		const auto customer_t0 = clock::now();
		BuildCustomerState(context, *bound_inputs[0], frozen, profile_enabled ? &profile : nullptr);
		profile.customer_build_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - customer_t0).count();
		profile.customer_domain_size = frozen.customer_valid.size();
		profile.active_customer_count = frozen.active_customers.size();
		profile.customer_state_bytes = frozen.CustomerStateBytes();

		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);
		std::vector<CacheLinePadded<Q13WorkerAgg>> workers(worker_count);
		for (duckdb::idx_t worker_id = 0; worker_id < worker_count; worker_id++) {
			workers[worker_id].value.Reset(frozen.customer_valid.size());
		}
		if (profile_enabled) {
			profile.worker_count_bytes = workers[0].value.order_counts.size() * sizeof(int32_t) * worker_count;
		}

		std::vector<Q13WorkerOrdersProfile> worker_profiles(profile_enabled ? static_cast<size_t>(worker_count) : 0);
		const auto orders_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
			                     AccumulateOrdersChunk(chunk, frozen, pattern_matcher, fallback_pattern,
			                                           workers[worker_id].value,
			                                           profile_enabled && worker_count == 1 ? &profile : nullptr);
			                     if (profile_enabled) {
				                     auto &worker_profile = worker_profiles[worker_id];
				                     worker_profile.rows += chunk.size();
				                     worker_profile.elapsed_ns +=
				                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - worker_t0)
				                             .count();
			                     }
		                     });
		profile.orders_scan_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();
		if (profile_enabled) {
			int64_t worker_ns_sum = 0;
			for (const auto &worker_profile : worker_profiles) {
				worker_ns_sum += worker_profile.elapsed_ns;
				profile.max_worker_ns = std::max(profile.max_worker_ns, worker_profile.elapsed_ns);
			}
			profile.avg_worker_ns =
			    worker_profiles.empty() ? 0 : worker_ns_sum / static_cast<int64_t>(worker_profiles.size());
		}

		std::vector<int32_t> merged(frozen.customer_valid.size(), 0);
		const auto merge_t0 = clock::now();
		for (const auto &worker : workers) {
			MergeOrderCounts(merged, worker.value.order_counts, profile_enabled ? &profile : nullptr);
		}
		profile.merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();

		const auto histogram_t0 = clock::now();
		std::vector<int64_t> customers_per_count;
		customers_per_count.reserve(256);
		const auto customer_capacity = static_cast<int64_t>(frozen.customer_valid.size());
		for (const auto custkey : frozen.active_customers) {
			if (custkey < 0 || custkey >= customer_capacity) {
				continue;
			}
			const auto order_count = merged[static_cast<size_t>(custkey)];
			if (order_count >= static_cast<int64_t>(customers_per_count.size())) {
				customers_per_count.resize(static_cast<size_t>(order_count + 1), 0);
			}
			customers_per_count[static_cast<size_t>(order_count)] += 1;
			if (profile_enabled) {
				profile.max_order_count = std::max(profile.max_order_count, static_cast<size_t>(order_count));
			}
		}
		profile.histogram_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - histogram_t0).count();
		profile.histogram_buckets = customers_per_count.size();

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
		const auto sort_t0 = clock::now();
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		profile.sort_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - sort_t0).count();

		profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
		if (profile_enabled) {
			PrintQ13ParallelProfile(profile);
		}
	}

	void Finalize() override {
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
	Q13Parameters params;
	TwoPartLikeMatcher pattern_matcher;
	std::string fallback_pattern;
	std::vector<Q13OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ13ParallelKernel(const Q13Parameters &params) {
	return duckdb::make_uniq<Q13ParallelKernel>(params);
}

} // namespace accelerator_codegen
