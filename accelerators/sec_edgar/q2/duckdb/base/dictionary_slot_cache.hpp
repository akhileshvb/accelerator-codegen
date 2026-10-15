#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {

// Values cached per index of a dictionary vector's dictionary, so each
// dictionary entry is resolved once instead of once per row.
//
// Slots carry over from one chunk to the next only when both chunks read the
// same DuckDB reusable dictionary: such a dictionary has a unique id and fixed
// contents.  A dictionary's address alone does not identify it -- a freed
// dictionary's buffer can be reallocated with other strings behind the same
// indexes -- so a dictionary without an id gets empty slots every chunk.
template <typename T>
class DictionarySlotCache {
public:
	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			filled_.reserve(estimate);
			values_.reserve(estimate);
		}
	}

	// Call once per chunk, before any Find / Store, with the vector whose
	// dictionary indexes the slots.
	void BeginChunk(const duckdb::Vector &vector) {
		if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
			Reset(nullptr, std::string());
			return;
		}
		const auto &child = duckdb::DictionaryVector::Child(vector);
		const auto *data = child.GetData();
		const auto &id = duckdb::DictionaryVector::DictionaryId(vector);
		if (!id.empty() && data == current_data_ && id == current_id_) {
			return;
		}
		Reset(data, id);
	}

	// The value stored for this chunk's dictionary entry `index`, or nullptr.
	const T *Find(duckdb::idx_t index) const {
		return index < filled_.size() && filled_[index] ? &values_[index] : nullptr;
	}

	void Store(duckdb::idx_t index, T value) {
		if (index >= filled_.size()) {
			filled_.resize(index + 1, 0);
			values_.resize(index + 1);
		}
		filled_[index] = 1;
		values_[index] = std::move(value);
	}

private:
	void Reset(const void *data, const std::string &id) {
		current_data_ = data;
		current_id_ = id;
		filled_.clear();
		values_.clear();
	}

	const void *current_data_ = nullptr;
	std::string current_id_;
	std::vector<uint8_t> filled_;
	std::vector<T> values_;
};

} // namespace accelerator_codegen
