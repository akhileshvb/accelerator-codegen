#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/state/partition_key.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

inline constexpr aux_state::PartitionKey kSecEdgarQ1OverflowPartitionBase = 100000;
inline constexpr duckdb::idx_t kSecEdgarQ1FixedSlotCount = 5 * 256;

enum class SecEdgarQ1StmtSlot : uint8_t {
	BS = 0,
	IS = 1,
	CF = 2,
	EQ = 3,
	CI = 4,
	Unknown = 255,
};

using SecEdgarQ1PartitionKey = aux_state::PartitionKey;

class SecEdgarQ1DistinctAdshBitmap {
public:
	explicit SecEdgarQ1DistinctAdshBitmap(duckdb::idx_t initial_words = 0) {
		if (initial_words > 0) {
			bits.assign(initial_words, 0);
			presized_words = bits.size();
		}
	}

	void Presize(duckdb::idx_t expected_ids) {
		const auto words = (expected_ids + 63) / 64;
		if (words > bits.size()) {
			bits.resize(words, 0);
		}
		presized_words = bits.size();
	}

	uint32_t size() const {
		return distinct_count;
	}

	bool insert_presized(uint32_t id) {
		const uint32_t word_idx = id >> 6;
		const uint64_t mask = uint64_t(1) << (id & 63);
		if (word_idx >= presized_words) {
			return insert(id);
		}
		auto &word = bits[word_idx];
		if (word & mask) {
			return false;
		}
		word |= mask;
		distinct_count++;
		return true;
	}

	bool insert(uint32_t id) {
		const uint32_t word_idx = id >> 6;
		const uint64_t mask = uint64_t(1) << (id & 63);
		if (word_idx >= bits.size()) {
			bits.resize(word_idx + 1, 0);
		}
		auto &word = bits[word_idx];
		if (word & mask) {
			return false;
		}
		word |= mask;
		distinct_count++;
		return true;
	}

	bool remove(uint32_t id) {
		const uint32_t word_idx = id >> 6;
		const uint64_t mask = uint64_t(1) << (id & 63);
		if (word_idx >= bits.size()) {
			return false;
		}
		auto &word = bits[word_idx];
		if (!(word & mask)) {
			return false;
		}
		word &= ~mask;
		distinct_count--;
		return true;
	}

private:
	std::vector<uint64_t> bits;
	duckdb::idx_t presized_words = 0;
	uint32_t distinct_count = 0;
};

struct SecEdgarQ1GroupPartition {
	int64_t cnt = 0;
	int64_t line_sum = 0;
	SecEdgarQ1DistinctAdshBitmap distinct_adsh;
	std::unordered_map<uint32_t, int64_t> adsh_refcount;
	std::string stmt_label;
	std::string rfile_label;
	bool rfile_is_null = false;
};

struct SecEdgarQ1OutputRow {
	std::string stmt;
	std::string rfile;
	bool rfile_is_null = false;
	int64_t cnt = 0;
	int64_t num_filings = 0;
	double avg_line_num = 0.0;
};

SecEdgarQ1StmtSlot ClassifySecEdgarQ1Stmt(const duckdb::string_t &value);
SecEdgarQ1StmtSlot ClassifySecEdgarQ1Stmt(const std::string &value);
uint16_t SecEdgarQ1FixedSlotIndex(SecEdgarQ1StmtSlot stmt, uint8_t rfile_char);

SecEdgarQ1PartitionKey PartitionKeyFromStmtRfile(const duckdb::string_t &stmt, const duckdb::string_t &rfile,
                                                 bool rfile_is_null);
SecEdgarQ1PartitionKey PartitionKeyFromStmtRfileStrings(const std::string &stmt, const std::string &rfile,
                                                        bool rfile_is_null);

std::vector<SecEdgarQ1PartitionKey> SecEdgarQ1ActiveFixedPartitionKeys();

void ApplySecEdgarQ1RowContribution(SecEdgarQ1GroupPartition &partition, int32_t line, uint32_t adsh_intern_id,
                                    int8_t sign);

SecEdgarQ1OutputRow FinalizeSecEdgarQ1GroupPartition(const SecEdgarQ1GroupPartition &partition);
std::vector<SecEdgarQ1OutputRow> FinalizeAndSortSecEdgarQ1Rows(std::vector<SecEdgarQ1OutputRow> rows);

} // namespace accelerator_codegen
