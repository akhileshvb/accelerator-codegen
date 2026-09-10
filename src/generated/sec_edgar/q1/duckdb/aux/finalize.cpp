#include "sec_edgar/q1/duckdb/aux/finalize.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace accelerator_codegen {
namespace {

struct GroupKey {
	std::string stmt;
	std::string rfile;
	bool rfile_is_null = false;

	bool operator==(const GroupKey &other) const {
		return rfile_is_null == other.rfile_is_null && stmt == other.stmt && rfile == other.rfile;
	}
};

struct GroupKeyHash {
	size_t operator()(const GroupKey &key) const {
		return std::hash<std::string>()(key.stmt) ^ (std::hash<std::string>()(key.rfile) << 1) ^
		       (key.rfile_is_null ? 0x9e3779b9UL : 0UL);
	}
};

std::mutex g_overflow_mutex;
std::unordered_map<GroupKey, SecEdgarQ1PartitionKey, GroupKeyHash> g_overflow_keys;
SecEdgarQ1PartitionKey g_next_overflow_key = kSecEdgarQ1OverflowPartitionBase;

std::string ToStoredString(const duckdb::string_t &value) {
	const auto size = value.GetSize();
	if (size == 0) {
		return std::string();
	}
	const auto *data = value.GetData();
	if (data == nullptr) {
		return std::string(size, '\0');
	}
	return std::string(data, size);
}

SecEdgarQ1PartitionKey AssignOverflowKey(const std::string &stmt, const std::string &rfile, bool rfile_is_null) {
	GroupKey key {stmt, rfile, rfile_is_null};
	std::lock_guard<std::mutex> guard(g_overflow_mutex);
	const auto it = g_overflow_keys.find(key);
	if (it != g_overflow_keys.end()) {
		return it->second;
	}
	const auto assigned = g_next_overflow_key++;
	g_overflow_keys.emplace(std::move(key), assigned);
	return assigned;
}

void TouchAdshDistinct(SecEdgarQ1GroupPartition &partition, uint32_t adsh_intern_id, int8_t sign) {
	if (adsh_intern_id == std::numeric_limits<uint32_t>::max()) {
		return;
	}
	auto &refcount = partition.adsh_refcount[adsh_intern_id];
	refcount += sign;
	if (refcount <= 0) {
		partition.adsh_refcount.erase(adsh_intern_id);
		partition.distinct_adsh.remove(adsh_intern_id);
		return;
	}
	if (refcount == 1 && sign > 0) {
		partition.distinct_adsh.insert(adsh_intern_id);
	}
}

} // namespace

SecEdgarQ1StmtSlot ClassifySecEdgarQ1Stmt(const duckdb::string_t &value) {
	return ClassifySecEdgarQ1Stmt(ToStoredString(value));
}

SecEdgarQ1StmtSlot ClassifySecEdgarQ1Stmt(const std::string &value) {
	if (value.size() != 2) {
		return SecEdgarQ1StmtSlot::Unknown;
	}
	const auto *data = value.data();
	if (data[0] == 'B' && data[1] == 'S') {
		return SecEdgarQ1StmtSlot::BS;
	}
	if (data[0] == 'I' && data[1] == 'S') {
		return SecEdgarQ1StmtSlot::IS;
	}
	if (data[0] == 'C' && data[1] == 'F') {
		return SecEdgarQ1StmtSlot::CF;
	}
	if (data[0] == 'E' && data[1] == 'Q') {
		return SecEdgarQ1StmtSlot::EQ;
	}
	if (data[0] == 'C' && data[1] == 'I') {
		return SecEdgarQ1StmtSlot::CI;
	}
	return SecEdgarQ1StmtSlot::Unknown;
}

uint16_t SecEdgarQ1FixedSlotIndex(SecEdgarQ1StmtSlot stmt, uint8_t rfile_char) {
	if (stmt == SecEdgarQ1StmtSlot::Unknown) {
		return std::numeric_limits<uint16_t>::max();
	}
	return static_cast<uint16_t>(static_cast<uint8_t>(stmt) * 256U + rfile_char);
}

SecEdgarQ1PartitionKey PartitionKeyFromStmtRfile(const duckdb::string_t &stmt, const duckdb::string_t &rfile,
                                                 bool rfile_is_null) {
	return PartitionKeyFromStmtRfileStrings(ToStoredString(stmt), ToStoredString(rfile), rfile_is_null);
}

SecEdgarQ1PartitionKey PartitionKeyFromStmtRfileStrings(const std::string &stmt, const std::string &rfile,
                                                      bool rfile_is_null) {
	if (stmt.empty()) {
		return std::numeric_limits<SecEdgarQ1PartitionKey>::max();
	}
	if (rfile_is_null || rfile.size() != 1) {
		return AssignOverflowKey(stmt, rfile, rfile_is_null);
	}
	const auto stmt_slot = ClassifySecEdgarQ1Stmt(stmt);
	const auto slot_index = SecEdgarQ1FixedSlotIndex(stmt_slot, static_cast<uint8_t>(rfile[0]));
	if (slot_index == std::numeric_limits<uint16_t>::max()) {
		return AssignOverflowKey(stmt, rfile, rfile_is_null);
	}
	return slot_index;
}

std::vector<SecEdgarQ1PartitionKey> SecEdgarQ1ActiveFixedPartitionKeys() {
	std::vector<SecEdgarQ1PartitionKey> keys;
	keys.reserve(kSecEdgarQ1FixedSlotCount);
	for (SecEdgarQ1PartitionKey key = 0; key < kSecEdgarQ1FixedSlotCount; key++) {
		keys.push_back(key);
	}
	return keys;
}

void ApplySecEdgarQ1RowContribution(SecEdgarQ1GroupPartition &partition, int32_t line, uint32_t adsh_intern_id,
                                    int8_t sign) {
	partition.cnt += sign;
	partition.line_sum += sign * static_cast<int64_t>(line);
	TouchAdshDistinct(partition, adsh_intern_id, sign);
}

SecEdgarQ1OutputRow FinalizeSecEdgarQ1GroupPartition(const SecEdgarQ1GroupPartition &partition) {
	SecEdgarQ1OutputRow row;
	row.stmt = partition.stmt_label;
	row.rfile = partition.rfile_label;
	row.rfile_is_null = partition.rfile_is_null;
	row.cnt = partition.cnt;
	row.num_filings = partition.distinct_adsh.size();
	if (partition.cnt > 0) {
		row.avg_line_num = static_cast<double>(partition.line_sum) / static_cast<double>(partition.cnt);
	}
	return row;
}

std::vector<SecEdgarQ1OutputRow> FinalizeAndSortSecEdgarQ1Rows(std::vector<SecEdgarQ1OutputRow> rows) {
	std::sort(rows.begin(), rows.end(), [](const SecEdgarQ1OutputRow &lhs, const SecEdgarQ1OutputRow &rhs) {
		if (lhs.cnt != rhs.cnt) {
			return lhs.cnt > rhs.cnt;
		}
		if (lhs.stmt != rhs.stmt) {
			return lhs.stmt < rhs.stmt;
		}
		return lhs.rfile < rhs.rfile;
	});
	return rows;
}

} // namespace accelerator_codegen
