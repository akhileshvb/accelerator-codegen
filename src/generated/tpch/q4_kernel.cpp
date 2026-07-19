#include "accelerator_codegen/tpch_q4.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int kPrioritySlotCount = 5;
constexpr duckdb::idx_t kOrderkeyScanIndex = 0;
constexpr duckdb::idx_t kCommitdateScanIndex = 1;
constexpr duckdb::idx_t kReceiptdateScanIndex = 2;
constexpr duckdb::idx_t kLineitemProjectedColumnCount = 3;

using Clock = std::chrono::steady_clock;

#ifdef ACCEL_EXPERIMENTAL_Q4_OPTS
bool ReadBoolEnv(const char *name, bool default_value) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		return default_value;
	}
	return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0;
}
#endif

struct Q4RuntimeConfig {
	bool use_bitmap_set = true;
	bool use_orders_dict_fast = true;
	bool use_fixed_priority = true;
	bool trust_native_date = false;
	bool profile_fine_timers = false;
	bool bitmap_inline = true;
	bool skip_consec_key = false;
	bool word_batch = false;
	bool twopass = false;
	bool branchless = false;
	bool byte_state = false;
	bool run_at_a_time = false;
	bool paired_date = false;
};

Q4RuntimeConfig ReadQ4RuntimeConfig(bool profiling) {
	Q4RuntimeConfig config;
#ifdef ACCEL_EXPERIMENTAL_Q4_OPTS
	config.use_bitmap_set = ReadBoolEnv("ACCEL_Q4_BITMAP_SET", true);
	config.use_orders_dict_fast = ReadBoolEnv("ACCEL_Q4_ORDERS_DICT_FAST", true);
	config.use_fixed_priority = ReadBoolEnv("ACCEL_Q4_FIXED_PRIORITY", true);
	config.trust_native_date = ReadBoolEnv("ACCEL_Q4_TRUST_NATIVE_DATE", false);
	config.profile_fine_timers = profiling && ReadBoolEnv("ACCEL_Q4_FINE_TIMERS", false);
	config.bitmap_inline = ReadBoolEnv("ACCEL_Q4_BITMAP_INLINE", true);
	config.skip_consec_key = ReadBoolEnv("ACCEL_Q4_SKIP_CONSEC_KEY", false);
	config.word_batch = ReadBoolEnv("ACCEL_Q4_BITMAP_WORD_BATCH", false);
	config.twopass = ReadBoolEnv("ACCEL_Q4_LINEITEM_TWOPASS", false);
	config.branchless = ReadBoolEnv("ACCEL_Q4_LINEITEM_BRANCHLESS", false);
	// New experiments (default off).
	config.byte_state = ReadBoolEnv("ACCEL_Q4_BYTE_STATE", false);
	config.run_at_a_time = ReadBoolEnv("ACCEL_Q4_RUN_AT_A_TIME", false);
	// Paired-date: on the chunk path compares date_t::days as int32.
	// With ACCEL_Q4_LINEITEM_FUSED=1, uses RowGroup::TryScanPairedDateLess (Bitpacking FOR
	// decode → bool without materializing DATE Vectors). Storage format unchanged.
	config.paired_date = ReadBoolEnv("ACCEL_Q4_PAIRED_DATE", false);
#else
	(void)profiling;
#endif
	return config;
}

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	auto &child = duckdb::DictionaryVector::Child(vector);
	return child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

void RecordLayout(Q4LayoutHistogram *histogram, duckdb::Vector &vector) {
	if (!histogram) {
		return;
	}
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		if (duckdb::FlatVector::Validity(vector).AllValid()) {
			histogram->flat_all_valid += 1;
		} else {
			histogram->other += 1;
		}
		return;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			if (duckdb::FlatVector::Validity(child).AllValid()) {
				histogram->dict_flat_all_valid += 1;
			} else {
				histogram->dict_flat_nullable += 1;
			}
		} else {
			histogram->other += 1;
		}
		return;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		if (!duckdb::ConstantVector::IsNull(vector)) {
			histogram->constant_all_valid += 1;
		} else {
			histogram->other += 1;
		}
		return;
	default:
		histogram->other += 1;
		return;
	}
}

class QualifyingOrderKeySet {
public:
	enum class Storage : uint8_t { Bitmap, Bytes };

	QualifyingOrderKeySet(bool use_bitmap_p, bool use_bytes_p) {
		if (use_bytes_p) {
			storage = Storage::Bytes;
			bytes.assign(6000001, 0);
			byte_capacity = bytes.size();
		} else if (use_bitmap_p) {
			storage = Storage::Bitmap;
			EnsureCapacity(6000000);
			capacity_words = words.size();
		} else {
			storage = Storage::Bitmap;
			use_overflow_only = true;
		}
	}

	Storage GetStorage() const {
		return storage;
	}

	bool Insert(int64_t orderkey) {
		if (orderkey < 0) {
			return overflow.insert(orderkey).second;
		}
		const auto key = static_cast<uint64_t>(orderkey);
		if (storage == Storage::Bytes) {
			if (key >= byte_capacity) {
				return overflow.insert(orderkey).second;
			}
			const bool was = bytes[key] != 0;
			bytes[key] = 1;
			return !was;
		}
		if (use_overflow_only || (key >> 6) >= capacity_words) {
			return overflow.insert(orderkey).second;
		}
		InsertNonNegUnchecked(key);
		return true;
	}

	inline void InsertNonNegUnchecked(uint64_t orderkey) {
		if (storage == Storage::Bytes) {
			bytes[orderkey] = 1;
			return;
		}
		words[orderkey >> 6] |= (uint64_t {1} << (orderkey & 63));
	}

	bool Contains(int64_t orderkey) const {
		if (orderkey < 0) {
			return overflow.find(orderkey) != overflow.end();
		}
		const auto key = static_cast<uint64_t>(orderkey);
		if (storage == Storage::Bytes) {
			if (key < byte_capacity && bytes[key]) {
				return true;
			}
			return overflow.find(orderkey) != overflow.end();
		}
		if (use_overflow_only) {
			return overflow.find(orderkey) != overflow.end();
		}
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			return overflow.find(orderkey) != overflow.end();
		}
		const auto mask = uint64_t {1} << (key & 63);
		if ((words[word_index] & mask) != 0) {
			return true;
		}
		return overflow.find(orderkey) != overflow.end();
	}

	duckdb::idx_t Size() const {
		return CountMarked() + overflow.size();
	}

	duckdb::idx_t MemoryBytes() const {
		if (storage == Storage::Bytes) {
			return bytes.size() + overflow.size() * sizeof(int64_t) + overflow.bucket_count() * sizeof(void *);
		}
		return words.size() * sizeof(uint64_t) + overflow.size() * sizeof(int64_t) +
		       overflow.bucket_count() * sizeof(void *);
	}

	duckdb::idx_t CapacityBits() const {
		if (storage == Storage::Bytes) {
			return byte_capacity;
		}
		return words.size() * 64;
	}

	duckdb::idx_t ResizeCount() const {
		return resize_count;
	}

	double LoadFactor() const {
		const auto capacity = CapacityBits();
		if (capacity == 0) {
			return 0.0;
		}
		return static_cast<double>(CountMarked()) / static_cast<double>(capacity);
	}

	uint64_t *Words() {
		return words.data();
	}

	uint8_t *Bytes() {
		return bytes.data();
	}

	duckdb::idx_t CapacityWords() const {
		return capacity_words;
	}

	duckdb::idx_t ByteCapacity() const {
		return byte_capacity;
	}

private:
	duckdb::idx_t CountMarked() const {
		if (storage == Storage::Bytes) {
			duckdb::idx_t count = 0;
			for (auto b : bytes) {
				count += b != 0;
			}
			return count;
		}
		duckdb::idx_t count = 0;
		for (auto word : words) {
#if defined(__GNUC__) || defined(__clang__)
			count += static_cast<duckdb::idx_t>(__builtin_popcountll(word));
#else
			while (word) {
				count += word & 1;
				word >>= 1;
			}
#endif
		}
		return count;
	}

	void EnsureCapacity(uint64_t max_bit_inclusive) {
		const auto needed_words = (max_bit_inclusive >> 6) + 1;
		if (needed_words <= words.size()) {
			capacity_words = words.size();
			return;
		}
		auto new_size = words.empty() ? needed_words : words.size();
		while (new_size < needed_words) {
			new_size *= 2;
		}
		words.resize(new_size, 0);
		capacity_words = words.size();
		resize_count += 1;
	}

	Storage storage = Storage::Bitmap;
	bool use_overflow_only = false;
	std::vector<uint64_t> words;
	std::vector<uint8_t> bytes;
	std::unordered_set<int64_t> overflow;
	duckdb::idx_t capacity_words = 0;
	duckdb::idx_t byte_capacity = 0;
	duckdb::idx_t resize_count = 0;
};

struct Q4OutputRow {
	std::string orderpriority;
	int64_t order_count = 0;
};

const char *kKnownPriorities[kPrioritySlotCount] = {"1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"};

int MatchFixedPriority(const duckdb::string_t &value) {
	const auto size = value.GetSize();
	const auto data = value.GetData();
	for (int slot = 0; slot < kPrioritySlotCount; slot++) {
		const auto *known = kKnownPriorities[slot];
		const auto known_size = std::strlen(known);
		if (size == known_size && std::memcmp(data, known, known_size) == 0) {
			return slot;
		}
	}
	return -1;
}

} // namespace

struct Q4Kernel::State {
	Q4RuntimeConfig config;
	QualifyingOrderKeySet qualifying_orderkeys;
	int64_t fixed_priority_counts[kPrioritySlotCount] = {};
	std::unordered_map<std::string, int64_t> generic_priority_counts;
	std::vector<Q4OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool fused_lineitem_scanned = false;
	bool finalized = false;
	int64_t last_qualifying_key = std::numeric_limits<int64_t>::min();
	bool have_last_qualifying_key = false;
	// Cross-vector run-at-a-time state (exact for unordered rows: singleton runs).
	int64_t run_key = std::numeric_limits<int64_t>::min();
	bool have_run = false;
	bool run_qualified = false;

	explicit State(bool profiling)
	    : config(ReadQ4RuntimeConfig(profiling)),
	      qualifying_orderkeys(config.use_bitmap_set && !config.byte_state, config.byte_state) {
	}
};

bool Q4LineitemFusedScanEnabled() {
#ifdef ACCEL_EXPERIMENTAL_Q4_OPTS
	return ReadBoolEnv("ACCEL_Q4_LINEITEM_FUSED", false);
#else
	return false;
#endif
}

Q4Kernel::Q4Kernel(Q4Parameters params_p, Q4ProfileCounters *profile_p)
    : params(std::move(params_p)), profile(profile_p), state(std::make_unique<State>(profile_p != nullptr)) {
}

Q4Kernel::~Q4Kernel() = default;

bool Q4Kernel::FusedLineitemScanned() const {
	return state->fused_lineitem_scanned;
}

void Q4Kernel::ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) {
	switch (static_cast<Q4InputId>(input_id)) {
	case Q4InputId::LINEITEM:
		if (state->fused_lineitem_scanned) {
			throw duckdb::InternalException("Q4 lineitem already consumed via fused scan-consumer");
		}
		ConsumeLineitemChunk(input);
		return;
	case Q4InputId::ORDERS:
		ConsumeOrdersChunk(input);
		return;
	}
	throw duckdb::InternalException("Q4 kernel received unexpected input id %llu",
	                                static_cast<unsigned long long>(input_id));
}

void Q4Kernel::RunFusedLineitemScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	if (state->fused_lineitem_scanned) {
		return;
	}
	if (profile) {
		profile->used_fused_lineitem = true;
	}

	static constexpr duckdb::idx_t kScanIndices[] = {kOrderkeyScanIndex, kCommitdateScanIndex, kReceiptdateScanIndex};
	const bool paired_date = state->config.paired_date;

	auto consumer = [this, paired_date](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
	                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
	                       duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
		const auto &column_ids = scan_state.GetColumnIds();
		if (column_ids.size() != kLineitemProjectedColumnCount) {
			throw duckdb::InternalException("Q4 fused lineitem expected %llu projected columns, got %llu",
			                                static_cast<unsigned long long>(kLineitemProjectedColumnCount),
			                                static_cast<unsigned long long>(column_ids.size()));
		}

		duckdb::Vector orderkey_vec(duckdb::LogicalType::BIGINT);
		bool used_paired = false;
		bool qualify[STANDARD_VECTOR_SIZE];
		duckdb::idx_t scan_count = 0;

		Clock::time_point decode_start;
		if (profile) {
			decode_start = Clock::now();
		}

		if (paired_date) {
			duckdb::idx_t paired_count = 0;
			if (row_group.TryScanPairedDateLess(transaction, scan_state, kCommitdateScanIndex, kReceiptdateScanIndex,
			                                   qualify, paired_count)) {
				used_paired = true;
				scan_count = row_group.ScanProjectedColumn(transaction, scan_state, kOrderkeyScanIndex, orderkey_vec,
				                                           duckdb::UpdateScanType::STANDARD);
				if (scan_count != paired_count) {
					throw duckdb::InternalException(
					    "Q4 paired-date scan count mismatch (orderkey %llu vs dates %llu)",
					    static_cast<unsigned long long>(scan_count), static_cast<unsigned long long>(paired_count));
				}
			}
		}

		duckdb::Vector commitdate_vec(duckdb::LogicalType::DATE);
		duckdb::Vector receiptdate_vec(duckdb::LogicalType::DATE);
		if (!used_paired) {
			duckdb::Vector *payload[] = {&orderkey_vec, &commitdate_vec, &receiptdate_vec};
			scan_count = row_group.ScanProjectedColumns(transaction, scan_state, kScanIndices,
			                                            kLineitemProjectedColumnCount, payload,
			                                            duckdb::UpdateScanType::STANDARD);
		}

		if (profile) {
			profile->fused_decode_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decode_start).count();
			profile->fused_vectors += 1;
			profile->fused_rows += count;
			profile->scanned_lineitems += count;
			RecordLayout(&profile->layout_l_orderkey, orderkey_vec);
			if (!used_paired) {
				RecordLayout(&profile->layout_l_commitdate, commitdate_vec);
				RecordLayout(&profile->layout_l_receiptdate, receiptdate_vec);
			}
		}

		Clock::time_point work_start;
		if (profile) {
			work_start = Clock::now();
		}

		if (used_paired && IsFlatAllValid(orderkey_vec) && scan_count >= count) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
			const bool identity = sel.data() == nullptr;
			if (profile) {
				profile->fused_flat_identity_vectors += identity ? 1 : 0;
				profile->fused_nested_sel_vectors += identity ? 0 : 1;
				profile->lineitem.flat_fast_path_chunks += 1;
				profile->lineitem.flat_fast_path_rows += count;
				profile->lineitem.total_chunks += 1;
				profile->lineitem.total_rows += count;
			}
			if (identity) {
				for (duckdb::idx_t row = 0; row < count; row++) {
					if (!qualify[row]) {
						continue;
					}
					if (profile) {
						profile->qualifying_lineitems += 1;
					}
					state->qualifying_orderkeys.Insert(orderkeys[row]);
				}
			} else {
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto idx = sel.get_index(row);
					if (!qualify[idx]) {
						continue;
					}
					if (profile) {
						profile->qualifying_lineitems += 1;
					}
					state->qualifying_orderkeys.Insert(orderkeys[idx]);
				}
			}
		} else {
		const bool flat_all_valid =
		    IsFlatAllValid(orderkey_vec) && IsFlatAllValid(commitdate_vec) && IsFlatAllValid(receiptdate_vec);
		if (flat_all_valid && scan_count >= count) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vec);
			auto commitdates = duckdb::FlatVector::GetData<duckdb::date_t>(commitdate_vec);
			auto receiptdates = duckdb::FlatVector::GetData<duckdb::date_t>(receiptdate_vec);

			// Identity selection when sel_vector is null (full MVCC-visible vector, no table filters).
			const bool identity = sel.data() == nullptr;

			if (identity) {
				if (profile) {
					profile->fused_flat_identity_vectors += 1;
					profile->lineitem.flat_fast_path_chunks += 1;
					profile->lineitem.flat_fast_path_rows += count;
					profile->lineitem.total_chunks += 1;
					profile->lineitem.total_rows += count;
				}
				for (duckdb::idx_t row = 0; row < count; row++) {
					if (!(commitdates[row] < receiptdates[row])) {
						continue;
					}
					if (profile) {
						profile->qualifying_lineitems += 1;
					}
					state->qualifying_orderkeys.Insert(orderkeys[row]);
				}
			} else {
				if (profile) {
					profile->fused_nested_sel_vectors += 1;
					profile->lineitem.flat_fast_path_chunks += 1;
					profile->lineitem.flat_fast_path_rows += count;
					profile->lineitem.total_chunks += 1;
					profile->lineitem.total_rows += count;
				}
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto idx = sel.get_index(row);
					if (!(commitdates[idx] < receiptdates[idx])) {
						continue;
					}
					if (profile) {
						profile->qualifying_lineitems += 1;
					}
					state->qualifying_orderkeys.Insert(orderkeys[idx]);
				}
			}
		} else {
			if (profile) {
				profile->fused_scalar_fallback_vectors += 1;
				profile->lineitem.generic_fallback_chunks += 1;
				profile->lineitem.generic_fallback_rows += count;
				profile->lineitem.total_chunks += 1;
				profile->lineitem.total_rows += count;
			}
			duckdb::UnifiedVectorFormat orderkey_format;
			duckdb::UnifiedVectorFormat commitdate_format;
			duckdb::UnifiedVectorFormat receiptdate_format;
			orderkey_vec.ToUnifiedFormat(scan_count, orderkey_format);
			commitdate_vec.ToUnifiedFormat(scan_count, commitdate_format);
			receiptdate_vec.ToUnifiedFormat(scan_count, receiptdate_format);
			auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
			auto commitdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(commitdate_format);
			auto receiptdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(receiptdate_format);

			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto base_idx = sel.get_index(row);
				const auto orderkey_index = orderkey_format.sel->get_index(base_idx);
				const auto commit_index = commitdate_format.sel->get_index(base_idx);
				const auto receipt_index = receiptdate_format.sel->get_index(base_idx);
				if ((!orderkey_format.validity.AllValid() &&
				     !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
				    (!commitdate_format.validity.AllValid() &&
				     !commitdate_format.validity.RowIsValidUnsafe(commit_index)) ||
				    (!receiptdate_format.validity.AllValid() &&
				     !receiptdate_format.validity.RowIsValidUnsafe(receipt_index))) {
					continue;
				}
				if (!(commitdates[commit_index] < receiptdates[receipt_index])) {
					continue;
				}
				if (profile) {
					profile->qualifying_lineitems += 1;
				}
				state->qualifying_orderkeys.Insert(orderkeys[orderkey_index]);
			}
		}
		}

		if (profile) {
			profile->fused_predicate_insert_ns +=
			    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - work_start).count();
		}
		return duckdb::FilteredScanConsumerResult::CONTINUE;
	};

	if (!RunNativeFilteredScanConsumer(context, bind_data, consumer)) {
		throw duckdb::InterruptException();
	}
	state->fused_lineitem_scanned = true;
}

void Q4Kernel::Finalize() {
	if (state->finalized) {
		return;
	}

	// Flush open run-at-a-time state from the last lineitem vector.
	if (state->have_run && state->run_qualified) {
		if (state->config.byte_state) {
			const auto key = state->run_key;
			if (key >= 0 && static_cast<uint64_t>(key) < state->qualifying_orderkeys.ByteCapacity()) {
				state->qualifying_orderkeys.Bytes()[static_cast<uint64_t>(key)] = 1;
			} else {
				state->qualifying_orderkeys.Insert(key);
			}
		} else {
			state->qualifying_orderkeys.Insert(state->run_key);
		}
		if (profile) {
			profile->bitmap_writes += 1;
		}
		state->have_run = false;
		state->run_qualified = false;
	}

	if (profile) {
		profile->unique_qualifying_orderkeys = state->qualifying_orderkeys.Size();
		profile->set_memory_bytes = state->qualifying_orderkeys.MemoryBytes();
		profile->set_capacity_bits = state->qualifying_orderkeys.CapacityBits();
		profile->set_rehash_or_resize_count = state->qualifying_orderkeys.ResizeCount();
		profile->load_factor = state->qualifying_orderkeys.LoadFactor();
		profile->duplicate_qualifying_lineitems =
		    profile->qualifying_lineitems > profile->unique_qualifying_orderkeys
		        ? profile->qualifying_lineitems - profile->unique_qualifying_orderkeys
		        : 0;
	}

	state->ordered_rows.clear();
	for (int slot = 0; slot < kPrioritySlotCount; slot++) {
		if (state->fixed_priority_counts[slot] == 0) {
			continue;
		}
		Q4OutputRow row;
		row.orderpriority = kKnownPriorities[slot];
		row.order_count = state->fixed_priority_counts[slot];
		state->ordered_rows.push_back(std::move(row));
	}
	for (const auto &entry : state->generic_priority_counts) {
		Q4OutputRow row;
		row.orderpriority = entry.first;
		row.order_count = entry.second;
		state->ordered_rows.push_back(std::move(row));
	}
	std::sort(state->ordered_rows.begin(), state->ordered_rows.end(),
	          [](const Q4OutputRow &lhs, const Q4OutputRow &rhs) { return lhs.orderpriority < rhs.orderpriority; });
	if (profile) {
		profile->priority_groups = state->ordered_rows.size();
	}
	state->finalized = true;
}

bool Q4Kernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_rows.size()) {
		return false;
	}

	const auto remaining = state->ordered_rows.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_rows[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value(result.orderpriority));
		output.SetValue(1, row, duckdb::Value::BIGINT(result.order_count));
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

namespace {

void TrackPath(Q4InputProfile *input_profile, duckdb::DataChunk &input, int path) {
	if (!input_profile) {
		return;
	}
	input_profile->total_chunks += 1;
	input_profile->total_rows += input.size();
	if (path == 0) {
		input_profile->flat_fast_path_chunks += 1;
		input_profile->flat_fast_path_rows += input.size();
	} else if (path == 1) {
		input_profile->dict_fast_path_chunks += 1;
		input_profile->dict_fast_path_rows += input.size();
	} else {
		input_profile->generic_fallback_chunks += 1;
		input_profile->generic_fallback_rows += input.size();
	}
}

} // namespace

void Q4Kernel::ConsumeLineitemFlatHot(duckdb::DataChunk &input) {
	auto orderkeys = duckdb::FlatVector::GetData<int64_t>(input.data[Q4LineitemChunkLayout::ORDERKEY_INDEX]);
	auto commitdates = duckdb::FlatVector::GetData<duckdb::date_t>(input.data[Q4LineitemChunkLayout::COMMITDATE_INDEX]);
	auto receiptdates = duckdb::FlatVector::GetData<duckdb::date_t>(input.data[Q4LineitemChunkLayout::RECEIPTDATE_INDEX]);
	const auto count = input.size();
	auto &set = state->qualifying_orderkeys;
	const bool use_bytes = state->config.byte_state;
	const bool run_mode = state->config.run_at_a_time;
	const bool paired_date = state->config.paired_date;
	const bool inline_bitmap = state->config.bitmap_inline && state->config.use_bitmap_set && !use_bytes;
	const bool skip_consec = state->config.skip_consec_key;
	const bool word_batch = state->config.word_batch && inline_bitmap;
	const bool twopass = state->config.twopass;
	const bool branchless = state->config.branchless && inline_bitmap;
	const auto capacity_words = set.CapacityWords();
	const auto byte_capacity = set.ByteCapacity();

	auto record_qual = [&]() {
		if (profile) {
			profile->qualifying_lineitems += 1;
		}
	};
	auto record_skip = [&]() {
		if (profile) {
			profile->consecutive_key_skips += 1;
		}
	};
	auto record_write = [&]() {
		if (profile) {
			profile->bitmap_writes += 1;
		}
	};

	auto mark_key = [&](int64_t key) {
		if (use_bytes) {
			if (key >= 0 && static_cast<uint64_t>(key) < byte_capacity) {
				set.Bytes()[static_cast<uint64_t>(key)] = 1;
				record_write();
			} else {
				set.Insert(key);
				record_write();
			}
			return;
		}
		if (inline_bitmap && key >= 0 && (static_cast<uint64_t>(key) >> 6) < capacity_words) {
			set.InsertNonNegUnchecked(static_cast<uint64_t>(key));
			record_write();
		} else {
			set.Insert(key);
			record_write();
		}
	};

	auto row_qualifies = [&](duckdb::idx_t row) -> bool {
		if (paired_date) {
			// Same DATE physical payload as date_t::days; avoids date_t operator call overhead only.
			return commitdates[row].days < receiptdates[row].days;
		}
		return commitdates[row] < receiptdates[row];
	};

	// --- Run-at-a-time: one mark per orderkey run; skip predicates after first hit; cross-vector ---
	if (run_mode) {
		int64_t run_key = state->run_key;
		bool have_run = state->have_run;
		bool run_qualified = state->run_qualified;
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto key = orderkeys[row];
			if (!have_run || key != run_key) {
				if (have_run && run_qualified) {
					mark_key(run_key);
				}
				run_key = key;
				have_run = true;
				run_qualified = row_qualifies(row);
				if (run_qualified) {
					record_qual();
				}
			} else if (run_qualified) {
				// Already know this orderkey qualifies — skip remaining date compares in the run.
				record_skip();
			} else if (row_qualifies(row)) {
				run_qualified = true;
				record_qual();
			}
		}
		state->run_key = run_key;
		state->have_run = have_run;
		state->run_qualified = run_qualified;
		return;
	}

	// --- Byte-array single-pass (no run mode) ---
	if (use_bytes) {
		auto *bytes = set.Bytes();
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (!row_qualifies(row)) {
				continue;
			}
			record_qual();
			const auto key = orderkeys[row];
			if (key >= 0 && static_cast<uint64_t>(key) < byte_capacity) {
				bytes[static_cast<uint64_t>(key)] = 1;
				record_write();
			} else {
				set.Insert(key);
				record_write();
			}
		}
		return;
	}

	auto insert_key = [&](int64_t key) { mark_key(key); };

	// --- Two-pass: build compact qualifying index list, then insert ---
	if (twopass) {
		duckdb::idx_t qual_idx[STANDARD_VECTOR_SIZE];
		duckdb::idx_t qual_count = 0;
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (row_qualifies(row)) {
				qual_idx[qual_count++] = row;
			}
		}
		if (profile) {
			profile->qualifying_lineitems += qual_count;
		}
		for (duckdb::idx_t i = 0; i < qual_count; i++) {
			const auto key = orderkeys[qual_idx[i]];
			if (skip_consec && state->have_last_qualifying_key && key == state->last_qualifying_key) {
				record_skip();
				continue;
			}
			state->last_qualifying_key = key;
			state->have_last_qualifying_key = true;
			insert_key(key);
		}
		return;
	}

	// --- Branchless ---
	if (branchless) {
		auto *words = set.Words();
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto key = orderkeys[row];
			const bool qualifies = row_qualifies(row);
			if (qualifies) {
				record_qual();
			}
			if (skip_consec && qualifies && state->have_last_qualifying_key && key == state->last_qualifying_key) {
				record_skip();
				continue;
			}
			if (qualifies) {
				state->last_qualifying_key = key;
				state->have_last_qualifying_key = true;
			}
			if (key < 0 || (static_cast<uint64_t>(key) >> 6) >= capacity_words) {
				if (qualifies) {
					set.Insert(key);
					record_write();
				}
				continue;
			}
			const auto word = static_cast<uint64_t>(key) >> 6;
			const auto bit_mask = uint64_t {1} << (static_cast<uint64_t>(key) & 63);
			const auto qmask = qualifies ? ~uint64_t {0} : uint64_t {0};
			words[word] |= bit_mask & qmask;
			if (qualifies) {
				record_write();
			}
		}
		return;
	}

	// --- Word-batch single-pass ---
	if (word_batch) {
		uint64_t cur_word = std::numeric_limits<uint64_t>::max();
		uint64_t cur_mask = 0;
		auto *words = set.Words();
		auto flush = [&]() {
			if (cur_word != std::numeric_limits<uint64_t>::max()) {
				words[cur_word] |= cur_mask;
				record_write();
			}
		};
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (!row_qualifies(row)) {
				continue;
			}
			record_qual();
			const auto key = orderkeys[row];
			if (skip_consec && state->have_last_qualifying_key && key == state->last_qualifying_key) {
				record_skip();
				continue;
			}
			state->last_qualifying_key = key;
			state->have_last_qualifying_key = true;
			if (key < 0 || (static_cast<uint64_t>(key) >> 6) >= capacity_words) {
				flush();
				cur_word = std::numeric_limits<uint64_t>::max();
				set.Insert(key);
				record_write();
				continue;
			}
			const auto word = static_cast<uint64_t>(key) >> 6;
			const auto mask = uint64_t {1} << (static_cast<uint64_t>(key) & 63);
			if (word == cur_word) {
				cur_mask |= mask;
			} else {
				flush();
				cur_word = word;
				cur_mask = mask;
			}
		}
		flush();
		return;
	}

	// --- Default hot path: branchy predicate + inline |= ---
	if (!skip_consec && !word_batch && !twopass && !branchless && inline_bitmap) {
		auto *words = set.Words();
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (!row_qualifies(row)) {
				continue;
			}
			record_qual();
			const auto key = orderkeys[row];
			if (key >= 0 && (static_cast<uint64_t>(key) >> 6) < capacity_words) {
				words[static_cast<uint64_t>(key) >> 6] |= (uint64_t {1} << (static_cast<uint64_t>(key) & 63));
				record_write();
			} else {
				set.Insert(key);
				record_write();
			}
		}
		return;
	}

	for (duckdb::idx_t row = 0; row < count; row++) {
		if (!row_qualifies(row)) {
			continue;
		}
		record_qual();
		const auto key = orderkeys[row];
		if (skip_consec && state->have_last_qualifying_key && key == state->last_qualifying_key) {
			record_skip();
			continue;
		}
		state->last_qualifying_key = key;
		state->have_last_qualifying_key = true;
		insert_key(key);
	}
}

void Q4Kernel::ConsumeLineitemChunk(duckdb::DataChunk &input) {
	if (profile) {
		RecordLayout(&profile->layout_l_orderkey, input.data[Q4LineitemChunkLayout::ORDERKEY_INDEX]);
		RecordLayout(&profile->layout_l_commitdate, input.data[Q4LineitemChunkLayout::COMMITDATE_INDEX]);
		RecordLayout(&profile->layout_l_receiptdate, input.data[Q4LineitemChunkLayout::RECEIPTDATE_INDEX]);
		profile->scanned_lineitems += input.size();
	}

	auto &orderkey_vector = input.data[Q4LineitemChunkLayout::ORDERKEY_INDEX];
	auto &commitdate_vector = input.data[Q4LineitemChunkLayout::COMMITDATE_INDEX];
	auto &receiptdate_vector = input.data[Q4LineitemChunkLayout::RECEIPTDATE_INDEX];

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(commitdate_vector) && IsFlatAllValid(receiptdate_vector)) {
		TrackPath(profile ? &profile->lineitem : nullptr, input, 0);
		ConsumeLineitemFlatHot(input);
		return;
	}

	TrackPath(profile ? &profile->lineitem : nullptr, input, 2);
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat commitdate_format;
	duckdb::UnifiedVectorFormat receiptdate_format;
	orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
	commitdate_vector.ToUnifiedFormat(input.size(), commitdate_format);
	receiptdate_vector.ToUnifiedFormat(input.size(), receiptdate_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto commitdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(commitdate_format);
	auto receiptdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(receiptdate_format);
	for (duckdb::idx_t row = 0; row < input.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto commit_index = commitdate_format.sel->get_index(row);
		const auto receipt_index = receiptdate_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!commitdate_format.validity.AllValid() && !commitdate_format.validity.RowIsValidUnsafe(commit_index)) ||
		    (!receiptdate_format.validity.AllValid() && !receiptdate_format.validity.RowIsValidUnsafe(receipt_index))) {
			continue;
		}
		if (!(commitdates[commit_index] < receiptdates[receipt_index])) {
			continue;
		}
		if (profile) {
			profile->qualifying_lineitems += 1;
		}
		state->qualifying_orderkeys.Insert(orderkeys[orderkey_index]);
	}
}

void Q4Kernel::ConsumeOrdersChunk(duckdb::DataChunk &input) {
	// Close any open lineitem run before probing (lineitem scan ends before orders).
	if (state->have_run) {
		if (state->run_qualified) {
			if (state->config.byte_state) {
				const auto key = state->run_key;
				if (key >= 0 && static_cast<uint64_t>(key) < state->qualifying_orderkeys.ByteCapacity()) {
					state->qualifying_orderkeys.Bytes()[static_cast<uint64_t>(key)] = 1;
				} else {
					state->qualifying_orderkeys.Insert(key);
				}
			} else {
				state->qualifying_orderkeys.Insert(state->run_key);
			}
			if (profile) {
				profile->bitmap_writes += 1;
			}
		}
		state->have_run = false;
		state->run_qualified = false;
	}

	auto increment_priority = [&](const duckdb::string_t &priority) {
		if (state->config.use_fixed_priority) {
			const auto slot = MatchFixedPriority(priority);
			if (slot >= 0) {
				state->fixed_priority_counts[slot] += 1;
				if (profile) {
					profile->fixed_priority_hits += 1;
				}
				return;
			}
		}
		state->generic_priority_counts[priority.GetString()] += 1;
		if (profile) {
			profile->generic_priority_hits += 1;
		}
	};

	if (profile) {
		RecordLayout(&profile->layout_o_orderdate, input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX]);
		RecordLayout(&profile->layout_o_orderkey, input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX]);
		RecordLayout(&profile->layout_o_orderpriority, input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX]);
	}

	if (state->config.use_orders_dict_fast && Q4OrdersChunkCanUseDictFastPath(input)) {
		TrackPath(profile ? &profile->orders : nullptr, input, 1);
		auto &orderdate_vector = input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX];
		auto &orderkey_vector = input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX];
		auto &priority_vector = input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX];
		auto &orderdate_sel = duckdb::DictionaryVector::SelVector(orderdate_vector);
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &priority_sel = duckdb::DictionaryVector::SelVector(priority_vector);
		auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(orderdate_vector));
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto priorities = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(priority_vector));

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderdate_index = orderdate_sel.get_index(row);
			if (!state->config.trust_native_date) {
				const auto orderdate = orderdates[orderdate_index];
				if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
					if (profile) {
						profile->residual_date_rejects += 1;
					}
					continue;
				}
			}
			if (profile) {
				profile->date_filter_survivors += 1;
				profile->set_probes += 1;
			}
			const auto orderkey_index = orderkey_sel.get_index(row);
			if (!state->qualifying_orderkeys.Contains(orderkeys[orderkey_index])) {
				if (profile) {
					profile->set_misses += 1;
				}
				continue;
			}
			if (profile) {
				profile->set_hits += 1;
			}
			increment_priority(priorities[priority_sel.get_index(row)]);
		}
		return;
	}

	if (IsFlatAllValid(input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX]) &&
	    IsFlatAllValid(input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX]) &&
	    IsFlatAllValid(input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX])) {
		TrackPath(profile ? &profile->orders : nullptr, input, 0);
		auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX]);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX]);
		auto priorities =
		    duckdb::FlatVector::GetData<duckdb::string_t>(input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX]);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (!state->config.trust_native_date) {
				const auto orderdate = orderdates[row];
				if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
					if (profile) {
						profile->residual_date_rejects += 1;
					}
					continue;
				}
			}
			if (profile) {
				profile->date_filter_survivors += 1;
				profile->set_probes += 1;
			}
			if (!state->qualifying_orderkeys.Contains(orderkeys[row])) {
				if (profile) {
					profile->set_misses += 1;
				}
				continue;
			}
			if (profile) {
				profile->set_hits += 1;
			}
			increment_priority(priorities[row]);
		}
		return;
	}

	TrackPath(profile ? &profile->orders : nullptr, input, 2);
	duckdb::UnifiedVectorFormat orderdate_format;
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat priority_format;
	input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX].ToUnifiedFormat(input.size(), orderdate_format);
	input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX].ToUnifiedFormat(input.size(), orderkey_format);
	input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX].ToUnifiedFormat(input.size(), priority_format);
	auto orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto priorities = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(priority_format);
	for (duckdb::idx_t row = 0; row < input.size(); row++) {
		const auto orderdate_index = orderdate_format.sel->get_index(row);
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto priority_index = priority_format.sel->get_index(row);
		if ((!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) ||
		    (!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!priority_format.validity.AllValid() && !priority_format.validity.RowIsValidUnsafe(priority_index))) {
			continue;
		}
		if (!state->config.trust_native_date) {
			const auto orderdate = orderdates[orderdate_index];
			if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
				if (profile) {
					profile->residual_date_rejects += 1;
				}
				continue;
			}
		}
		if (profile) {
			profile->date_filter_survivors += 1;
			profile->set_probes += 1;
		}
		if (!state->qualifying_orderkeys.Contains(orderkeys[orderkey_index])) {
			if (profile) {
				profile->set_misses += 1;
			}
			continue;
		}
		if (profile) {
			profile->set_hits += 1;
		}
		increment_priority(priorities[priority_index]);
	}
}

bool Q4LineitemChunkCanUseFlatFastPath(duckdb::DataChunk &input) {
	return IsFlatAllValid(input.data[Q4LineitemChunkLayout::ORDERKEY_INDEX]) &&
	       IsFlatAllValid(input.data[Q4LineitemChunkLayout::COMMITDATE_INDEX]) &&
	       IsFlatAllValid(input.data[Q4LineitemChunkLayout::RECEIPTDATE_INDEX]);
}

bool Q4OrdersChunkCanUseDictFastPath(duckdb::DataChunk &input) {
	return IsDictionaryOverFlat(input.data[Q4OrdersChunkLayout::ORDERDATE_INDEX]) &&
	       IsDictionaryOverFlat(input.data[Q4OrdersChunkLayout::ORDERKEY_INDEX]) &&
	       IsDictionaryOverFlat(input.data[Q4OrdersChunkLayout::ORDERPRIORITY_INDEX]);
}

duckdb::unique_ptr<Q4Kernel> CreateQ4KernelTyped(const Q4Parameters &params, Q4ProfileCounters *profile) {
	return duckdb::make_uniq<Q4Kernel>(params, profile);
}

duckdb::unique_ptr<KernelInstance> CreateQ4Kernel(const Q4Parameters &params) {
	return CreateQ4KernelTyped(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateQ4ProfilingKernel(const Q4Parameters &params, Q4ProfileCounters &counters) {
	return CreateQ4KernelTyped(params, &counters);
}

} // namespace accelerator_codegen
