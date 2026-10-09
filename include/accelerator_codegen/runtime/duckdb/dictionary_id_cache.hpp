// Maps the rows of a DuckDB VARCHAR vector to caller-defined integer ids,
// resolving each distinct string of a storage dictionary once.
//
// Scans of dictionary-compressed columns hand out DICTIONARY vectors whose
// child is the segment's reusable dictionary (DictionaryVector::DictionaryId
// names it; it stays the same for every vector of the segment).  The cache
// keeps one id per dictionary entry for the current dictionary, so a
// 2048-row vector costs 2048 array loads plus one lookup per entry not seen
// before, instead of 2048 string hash lookups.  Any other vector shape
// (flat, FSST, constant, sliced without a reusable dictionary) falls back to
// one lookup per row, skipping runs of the same string.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {

class DictionaryIdCache {
public:
  static constexpr uint32_t kUnresolved = 0xFFFFFFFEu;

  // `null_id`: the id of NULL rows.
  explicit DictionaryIdCache(uint32_t null_id) : null_id_(null_id) {}

  // out[i] = lookup(row i's string) for rows 0..n-1 (null rows: null_id).
  // `lookup(const char *data, uint32_t size) -> uint32_t` must not return
  // kUnresolved.
  template <typename Lookup>
  void Resolve(::duckdb::Vector &vec, ::duckdb::idx_t n, Lookup &&lookup, uint32_t *out) {
    using namespace ::duckdb;
    if (vec.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
      const auto size = DictionaryVector::DictionarySize(vec);
      auto &child = DictionaryVector::Child(vec);
      if (size.IsValid() && child.GetVectorType() == VectorType::FLAT_VECTOR) {
        const auto &id = DictionaryVector::DictionaryId(vec);
        if (!id.empty()) {
          if (id != dict_id_) {
            dict_id_ = id;
            map_.assign(size.GetIndex(), kUnresolved);
          }
          const auto &sel = DictionaryVector::SelVector(vec);
          const auto *data = FlatVector::GetData<string_t>(child);
          const auto &validity = FlatVector::Validity(child);
          uint32_t *map = map_.data();
          for (idx_t i = 0; i < n; ++i) {
            const idx_t k = sel.get_index(i);
            uint32_t m = map[k];
            if (m == kUnresolved) {
              m = validity.RowIsValid(k) ? lookup(data[k].GetData(), static_cast<uint32_t>(data[k].GetSize()))
                                         : null_id_;
              map[k] = m;
            }
            out[i] = m;
          }
          dict_rows += n;
          return;
        }
      }
    }
    other_rows += n;
    if (vec.GetVectorType() == VectorType::DICTIONARY_VECTOR) dict_other_rows += n;
    UnifiedVectorFormat f;
    vec.ToUnifiedFormat(n, f);
    const auto *data = UnifiedVectorFormat::GetData<string_t>(f);
    const char *last_ptr = nullptr;
    uint32_t last_size = 0, last_id = 0;
    bool have_last = false;
    for (idx_t i = 0; i < n; ++i) {
      const idx_t k = f.sel->get_index(i);
      if (!f.validity.RowIsValid(k)) {
        out[i] = null_id_;
        continue;
      }
      const auto &s = data[k];
      const uint32_t sz = static_cast<uint32_t>(s.GetSize());
      const char *p = s.GetData();
      if (have_last && sz == last_size && (p == last_ptr || std::memcmp(p, last_ptr, sz) == 0)) {
        out[i] = last_id;
        continue;
      }
      last_id = lookup(p, sz);
      last_ptr = p;
      last_size = sz;
      have_last = true;
      out[i] = last_id;
    }
  }

  uint64_t dict_rows = 0, other_rows = 0, dict_other_rows = 0;

private:
  uint32_t null_id_;
  std::string dict_id_;
  std::vector<uint32_t> map_;
};

} // namespace accelerator_codegen
