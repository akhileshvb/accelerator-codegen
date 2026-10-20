#include "sec_edgar/q2/duckdb/aux/duckdb_scan.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <exception>
#include <stdexcept>
#include <unordered_set>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/native_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace accelerator_codegen::aux_state::query {
namespace scan = ::accelerator_codegen::aux_state::engine::duckdb::scan;
namespace {

using ::duckdb::DataChunk;
using ::duckdb::idx_t;
using ::duckdb::string_t;
using ::duckdb::UnifiedVectorFormat;

std::string Quote(const std::string &s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string QuoteList(const std::vector<std::string> &values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += Quote(values[i]);
  }
  return out;
}

// Streams the result of `sql` chunk by chunk.
template <typename Fn>
void Stream(::duckdb::Connection &connection, const std::string &sql, Fn &&fn) {
  auto result = scan::ScanConnection(connection).SendQuery(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error("sec_edgar q2 scan failed: " +
                             (result ? result->GetError() : std::string("null")));
  }
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      break;
    }
    fn(*chunk);
  }
  if (result->HasError()) {
    throw std::runtime_error("sec_edgar q2 scan failed: " + result->GetError());
  }
}

// Typed, selection-aware access to one chunk column.
template <typename T> struct Col {
  UnifiedVectorFormat format;
  const T *data = nullptr;
  void Init(DataChunk &chunk, idx_t column) {
    chunk.data[column].ToUnifiedFormat(chunk.size(), format);
    data = UnifiedVectorFormat::GetData<T>(format);
  }
  idx_t Index(idx_t row) const { return format.sel->get_index(row); }
  bool Valid(idx_t row) const { return format.validity.RowIsValid(Index(row)); }
  const T &Get(idx_t row) const { return data[Index(row)]; }
};

std::string_view View(const string_t &s) {
  return std::string_view(s.GetData(), s.GetSize());
}

} // namespace

void SecEdgarQ2DuckdbScan::ScanNum(
    const std::function<void(const SecEdgarQ2NumInput &)> &consumer) {
  Stream(connection,
         "SELECT adsh, tag, uom, value FROM num WHERE adsh IS NOT NULL AND "
         "tag IS NOT NULL AND uom IS NOT NULL",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, tag, uom;
           Col<double> value;
           adsh.Init(chunk, 0);
           tag.Init(chunk, 1);
           uom.Init(chunk, 2);
           value.Init(chunk, 3);
           SecEdgarQ2NumInput in;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             in.adsh.assign(View(adsh.Get(r)));
             in.tag.assign(View(tag.Get(r)));
             in.uom.assign(View(uom.Get(r)));
             in.value_is_null = !value.Valid(r);
             in.value = in.value_is_null ? 0.0 : value.Get(r);
             consumer(in);
           }
         });
}

void SecEdgarQ2DuckdbScan::ScanSub(
    const std::function<void(const SecEdgarQ2SubInput &)> &consumer) {
  scan::ScanQueryDecoded(
      connection, "SELECT adsh, name, fy FROM sub",
      [](const DataChunk &chunk, idx_t row) {
        const auto name = chunk.GetValue(1, row);
        const auto fy = chunk.GetValue(2, row);
        SecEdgarQ2SubInput in;
        in.adsh = chunk.GetValue(0, row).ToString();
        in.name_is_null = name.IsNull();
        in.name = in.name_is_null ? std::string() : name.ToString();
        in.fy_is_null = fy.IsNull();
        in.fy = in.fy_is_null ? 0 : fy.GetValue<int32_t>();
        return in;
      },
      [&](SecEdgarQ2SubInput &&in) { consumer(in); });
}

void SecEdgarQ2DuckdbScan::ScanUomRowCounts(
    const std::function<void(const std::string &, uint64_t)> &consumer) {
  Stream(connection,
         "SELECT uom, count(*) FROM num WHERE value IS NOT NULL AND uom IS NOT "
         "NULL AND adsh IS NOT NULL AND tag IS NOT NULL GROUP BY uom",
         [&](DataChunk &chunk) {
           Col<string_t> uom;
           Col<int64_t> rows;
           uom.Init(chunk, 0);
           rows.Init(chunk, 1);
           std::string name;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             name.assign(View(uom.Get(r)));
             consumer(name, static_cast<uint64_t>(rows.Get(r)));
           }
         });
}

void SecEdgarQ2DuckdbScan::ScanNumValueCounts(
    const std::vector<std::string> &uoms,
    const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) {
  if (uoms.empty()) {
    return;
  }
  Stream(connection,
         "SELECT adsh, tag, uom, value, count(*) FROM num WHERE value IS NOT "
         "NULL AND adsh IS NOT NULL AND tag IS NOT NULL AND uom IN (" +
             QuoteList(uoms) + ") GROUP BY adsh, tag, uom, value",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, tag, uom;
           Col<double> value;
           Col<int64_t> rows;
           adsh.Init(chunk, 0);
           tag.Init(chunk, 1);
           uom.Init(chunk, 2);
           value.Init(chunk, 3);
           rows.Init(chunk, 4);
           for (idx_t r = 0; r < chunk.size(); ++r) {
             consumer({View(adsh.Get(r)), View(tag.Get(r)), View(uom.Get(r)),
                       value.Get(r), static_cast<uint64_t>(rows.Get(r))});
           }
         });
}

namespace {

constexpr uint32_t kIdUnset = 0xFFFFFFFFu;
constexpr uint32_t kIdNull = 0xFFFFFFFEu;

inline uint64_t StrHash(const char *s, size_t n) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (n * 0xC2B2AE3D27D4EB4Full);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, s + i, 8);
    h = (h ^ w) * 0x9FB21C651E98DF25ull;
    h ^= h >> 29;
  }
  if (i < n) {
    uint64_t w = 0;
    std::memcpy(&w, s + i, n - i);
    h = (h ^ w) * 0x9FB21C651E98DF25ull;
  }
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ull;
  return h ^ (h >> 32);
}

// String -> id cache of the build scan.  16-byte slots (full hash, id, arena
// offset) so a probe is one cache line, plus an arena holding each string
// once for the exact comparison.  Lookups come in batches whose slots (and
// then arena lines) are prefetched ahead of the probes.
class StrIdMap {
public:
  StrIdMap() { slots_.assign(1024, Slot{0, kIdUnset, 0}); }

  // ids[i] for strs[i] (i < n); unknown strings get an id from `assign`.
  template <typename Assign>
  void Resolve(const string_t *const *strs, const uint64_t *hashes, size_t n, uint32_t *ids,
               Assign &&assign) {
    constexpr size_t kSlotAhead = 16;
    constexpr size_t kArenaAhead = 8;
    for (size_t i = 0; i < n && i < kSlotAhead; ++i) {
      __builtin_prefetch(&slots_[hashes[i] & mask_]);
    }
    for (size_t i = 0; i < n; ++i) {
      if (i + kSlotAhead < n) {
        __builtin_prefetch(&slots_[hashes[i + kSlotAhead] & mask_]);
      }
      if (i + kArenaAhead < n) {
        const Slot &s = slots_[hashes[i + kArenaAhead] & mask_];
        if (s.id != kIdUnset) {
          __builtin_prefetch(arena_.data() + size_t{s.off} * 8);
        }
      }
      const string_t &str = *strs[i];
      const uint32_t found = Find(hashes[i], str.GetData(), str.GetSize());
      if (found != kIdUnset) {
        ids[i] = found;
        continue;
      }
      const uint32_t id = assign(std::string_view(str.GetData(), str.GetSize()));
      Insert(hashes[i], str.GetData(), str.GetSize(), id);
      ids[i] = id;
    }
  }

private:
  struct Slot {
    uint64_t hash;
    uint32_t id; // kIdUnset = empty
    uint32_t off; // arena offset / 8
  };

  uint32_t Find(uint64_t h, const char *s, size_t n) const {
    for (size_t i = h & mask_;; i = (i + 1) & mask_) {
      const Slot &slot = slots_[i];
      if (slot.id == kIdUnset) {
        return kIdUnset;
      }
      if (slot.hash == h) {
        const char *p = arena_.data() + size_t{slot.off} * 8;
        uint32_t len;
        std::memcpy(&len, p, 4);
        if (len == n && std::memcmp(p + 4, s, n) == 0) {
          return slot.id;
        }
      }
    }
  }
  void Insert(uint64_t h, const char *s, size_t n, uint32_t id) {
    if ((size_ + 1) * 2 > slots_.size()) {
      Grow();
    }
    const size_t off = arena_.size();
    if (off / 8 > std::numeric_limits<uint32_t>::max() || n > std::numeric_limits<uint32_t>::max()) {
      throw std::length_error("sec_edgar q2: build string cache overflow");
    }
    const size_t padded = (4 + n + 7) & ~size_t{7};
    arena_.resize(off + padded);
    const uint32_t len = static_cast<uint32_t>(n);
    std::memcpy(arena_.data() + off, &len, 4);
    std::memcpy(arena_.data() + off + 4, s, n);
    size_t i = h & mask_;
    while (slots_[i].id != kIdUnset) {
      i = (i + 1) & mask_;
    }
    slots_[i] = Slot{h, id, static_cast<uint32_t>(off / 8)};
    ++size_;
  }
  void Grow() {
    std::vector<Slot> old(slots_.size() * 2, Slot{0, kIdUnset, 0});
    old.swap(slots_);
    mask_ = slots_.size() - 1;
    for (const Slot &s : old) {
      if (s.id == kIdUnset) {
        continue;
      }
      size_t i = s.hash & mask_;
      while (slots_[i].id != kIdUnset) {
        i = (i + 1) & mask_;
      }
      slots_[i] = s;
    }
  }

  std::vector<Slot> slots_;
  size_t mask_ = 1023;
  size_t size_ = 0;
  std::vector<char> arena_;
};

// Resolves one string column of a chunk to ids.  Dictionary vectors (the
// storage hands them out for dictionary-compressed segments) are resolved
// once per dictionary, in one batch: the ids of the current dictionary are
// cached, keyed by its child buffer, which is kept referenced so the address
// cannot be reused.  Other vectors are resolved row by row in one batch.
class IdColumn {
public:
  enum class Kind { Uom, Adsh, Tag };
  IdColumn(Kind kind, SecEdgarQ2KeyMap &keys) : kind_(kind), keys_(keys) {}

  // out[i] = id or kIdNull, i < count.
  void Resolve(::duckdb::Vector &v, idx_t count, uint32_t *out) {
    switch (v.GetVectorType()) {
    case ::duckdb::VectorType::DICTIONARY_VECTOR: {
      auto &child = ::duckdb::DictionaryVector::Child(v);
      const auto dict_size = ::duckdb::DictionaryVector::DictionarySize(v);
      if (child.GetVectorType() != ::duckdb::VectorType::FLAT_VECTOR || !dict_size.IsValid()) {
        break;
      }
      const auto *data = ::duckdb::FlatVector::GetData<string_t>(child);
      const idx_t size = dict_size.GetIndex();
      if (data != dict_data_ || size != dict_ids_.size()) {
        held_.Reference(child);
        dict_data_ = data;
        dict_ids_.resize(size);
        Batch(data, ::duckdb::FlatVector::Validity(child), nullptr, size, dict_ids_.data());
      }
      const auto &sel = ::duckdb::DictionaryVector::SelVector(v);
      const uint32_t *ids = dict_ids_.data();
      for (idx_t i = 0; i < count; ++i) {
        out[i] = ids[sel.get_index(i)];
      }
      return;
    }
    case ::duckdb::VectorType::CONSTANT_VECTOR: {
      uint32_t id = kIdNull;
      if (!::duckdb::ConstantVector::IsNull(v)) {
        Batch(::duckdb::ConstantVector::GetData<string_t>(v), ::duckdb::ValidityMask(), nullptr,
              1, &id);
      }
      std::fill(out, out + count, id);
      return;
    }
    default:
      break;
    }
    UnifiedVectorFormat f;
    v.ToUnifiedFormat(count, f);
    Batch(UnifiedVectorFormat::GetData<string_t>(f), f.validity, f.sel, count, out);
  }

private:
  // out[i] for row (sel ? sel[i] : i) of data, i < count.
  void Batch(const string_t *data, const ::duckdb::ValidityMask &validity,
             const ::duckdb::SelectionVector *sel, idx_t count, uint32_t *out) {
    strs_.resize(count);
    hashes_.resize(count);
    slots_.resize(count);
    size_t m = 0;
    // Rows repeating the previous row's string (common in flat vectors) are
    // resolved after the batch from that row.
    repeat_.clear();
    const string_t *prev = nullptr;
    for (idx_t i = 0; i < count; ++i) {
      const idx_t e = sel ? sel->get_index(i) : i;
      if (!validity.RowIsValid(e)) {
        out[i] = kIdNull;
        prev = nullptr;
        continue;
      }
      if (prev && *prev == data[e]) {
        repeat_.push_back(static_cast<uint32_t>(i));
        continue;
      }
      prev = data + e;
      strs_[m] = data + e;
      hashes_[m] = StrHash(data[e].GetData(), data[e].GetSize());
      slots_[m] = static_cast<uint32_t>(i);
      ++m;
    }
    ids_.resize(m);
    map_.Resolve(strs_.data(), hashes_.data(), m, ids_.data(), [&](std::string_view s) {
      switch (kind_) {
      case Kind::Uom:
        return keys_.Uom(s);
      case Kind::Adsh:
        return keys_.Adsh(s);
      case Kind::Tag:
        return keys_.Tag(s);
      }
      return kIdNull;
    });
    for (size_t j = 0; j < m; ++j) {
      out[slots_[j]] = ids_[j];
    }
    for (const uint32_t i : repeat_) {
      out[i] = out[i - 1];
    }
  }

  Kind kind_;
  SecEdgarQ2KeyMap &keys_;
  ::duckdb::Vector held_{::duckdb::LogicalType::VARCHAR, ::duckdb::idx_t(1)};
  const string_t *dict_data_ = nullptr;
  std::vector<uint32_t> dict_ids_;
  StrIdMap map_;
  std::vector<const string_t *> strs_;
  std::vector<uint64_t> hashes_;
  std::vector<uint32_t> slots_;
  std::vector<uint32_t> ids_;
  std::vector<uint32_t> repeat_;
};

} // namespace


bool SecEdgarQ2DuckdbScan::ScanNumIds(
    SecEdgarQ2KeyMap &keys, const std::function<void(const SecEdgarQ2IdBatch &)> &batch) {
  auto &conn = scan::ScanConnection(connection);
  if (!conn.context) {
    return false;
  }
  auto &context = *conn.context;
  NativeScanSpec spec;
  spec.table_name = "num";
  const auto column = [](const char *name, ::duckdb::LogicalType type,
                         ::duckdb::PhysicalType physical) {
    return NativeColumnSpec{name, name, std::move(type), physical};
  };
  spec.projected_columns.push_back(
      column("adsh", ::duckdb::LogicalType::VARCHAR, ::duckdb::PhysicalType::VARCHAR));
  spec.projected_columns.push_back(
      column("tag", ::duckdb::LogicalType::VARCHAR, ::duckdb::PhysicalType::VARCHAR));
  spec.projected_columns.push_back(
      column("uom", ::duckdb::LogicalType::VARCHAR, ::duckdb::PhysicalType::VARCHAR));
  spec.projected_columns.push_back(
      column("value", ::duckdb::LogicalType::DOUBLE, ::duckdb::PhysicalType::DOUBLE));

  IdColumn adsh_col(IdColumn::Kind::Adsh, keys);
  IdColumn tag_col(IdColumn::Kind::Tag, keys);
  IdColumn uom_col(IdColumn::Kind::Uom, keys);
  std::vector<uint32_t> a(STANDARD_VECTOR_SIZE), t(STANDARD_VECTOR_SIZE), u(STANDARD_VECTOR_SIZE);
  std::vector<int64_t> k(STANDARD_VECTOR_SIZE);
  bool delivered = false;
  const auto consume = [&](DataChunk &chunk) {
    const idx_t n = chunk.size();
    if (n > a.size()) {
      a.resize(n);
      t.resize(n);
      u.resize(n);
      k.resize(n);
    }
    UnifiedVectorFormat vf;
    chunk.data[3].ToUnifiedFormat(n, vf);
    const auto *values = UnifiedVectorFormat::GetData<double>(vf);
    // Rows with a NULL key column or value are dropped (they belong to no group).
    uom_col.Resolve(chunk.data[2], n, u.data());
    adsh_col.Resolve(chunk.data[0], n, a.data());
    tag_col.Resolve(chunk.data[1], n, t.data());
    size_t m = 0;
    for (idx_t i = 0; i < n; ++i) {
      const idx_t vi = vf.sel->get_index(i);
      if (!vf.validity.RowIsValid(vi) || a[i] == kIdNull || t[i] == kIdNull ||
          u[i] == kIdNull) {
        continue;
      }
      a[m] = a[i];
      t[m] = t[i];
      u[m] = u[i];
      k[m] = Q2ValueKey(values[vi]);
      ++m;
    }
    if (m != 0) {
      delivered = true;
      SecEdgarQ2IdBatch b;
      b.n = m;
      b.uom = u.data();
      b.adsh = a.data();
      b.tag = t.data();
      b.key = k.data();
      batch(b);
    }
  };
  const bool started = !context.transaction.HasActiveTransaction();
  if (started) {
    context.transaction.BeginTransaction();
  }
  try {
    ScanNativeBaseTable(context, spec, consume);
    if (started) {
      context.transaction.Commit();
    }
  } catch (...) {
    if (started && context.transaction.HasActiveTransaction()) {
      context.transaction.Rollback(nullptr);
    }
    if (delivered) {
      throw;
    }
    return false; // e.g. an unexpected schema: the SQL path handles it
  }
  return true;
}

void SecEdgarQ2DuckdbScan::LookupNumValueCounts(
    const std::string &uom,
    const std::vector<std::pair<std::string, std::string>> &groups,
    const std::function<void(const SecEdgarQ2ValueCountRow &)> &consumer) {
  if (groups.empty()) {
    return;
  }
  std::vector<std::string> adshs, tags;
  std::unordered_set<std::string> exact;
  for (const auto &g : groups) {
    adshs.push_back(g.first);
    tags.push_back(g.second);
    exact.insert(g.first + std::string(1, '\0') + g.second);
  }
  Stream(connection,
         "SELECT adsh, tag, value, count(*) FROM num WHERE uom = " + Quote(uom) +
             " AND value IS NOT NULL AND adsh IN (" + QuoteList(adshs) +
             ") AND tag IN (" + QuoteList(tags) + ") GROUP BY adsh, tag, value",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, tag;
           Col<double> value;
           Col<int64_t> rows;
           adsh.Init(chunk, 0);
           tag.Init(chunk, 1);
           value.Init(chunk, 2);
           rows.Init(chunk, 3);
           std::string key;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             key.assign(View(adsh.Get(r)));
             key.push_back('\0');
             key.append(View(tag.Get(r)));
             if (exact.count(key) == 0) {
               continue;
             }
             consumer({View(adsh.Get(r)), View(tag.Get(r)), uom, value.Get(r),
                       static_cast<uint64_t>(rows.Get(r))});
           }
         });
}

void SecEdgarQ2DuckdbScan::LookupSub(
    const std::vector<std::string> &adshs,
    const std::function<void(const SecEdgarQ2SubInput &)> &consumer) {
  if (adshs.empty()) {
    return;
  }
  Stream(connection,
         "SELECT adsh, name, fy FROM sub WHERE adsh IN (" + QuoteList(adshs) + ")",
         [&](DataChunk &chunk) {
           Col<string_t> adsh, name;
           Col<int32_t> fy;
           adsh.Init(chunk, 0);
           name.Init(chunk, 1);
           fy.Init(chunk, 2);
           SecEdgarQ2SubInput in;
           for (idx_t r = 0; r < chunk.size(); ++r) {
             in.adsh.assign(View(adsh.Get(r)));
             in.name_is_null = !name.Valid(r);
             in.name.assign(in.name_is_null ? std::string_view() : View(name.Get(r)));
             in.fy_is_null = !fy.Valid(r);
             in.fy = in.fy_is_null ? 0 : fy.Get(r);
             consumer(in);
           }
         });
}

void SecEdgarQ2DuckdbScan::ExecuteDirect(
    int64_t fy, const std::string &uom,
    const std::function<void(const SecEdgarQ2DirectRow &)> &consumer) {
  const std::string u = Quote(uom);
  Stream(connection,
         "SELECT s.name, n.tag, n.value FROM num n "
         "JOIN sub s ON n.adsh = s.adsh "
         "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num "
         "WHERE uom = " +
             u +
             " AND value IS NOT NULL GROUP BY adsh, tag) m "
             "ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
             "WHERE n.uom = " +
             u + " AND s.fy = " + std::to_string(fy) +
             " AND n.value IS NOT NULL "
             "ORDER BY n.value DESC, s.name, n.tag LIMIT " +
             std::to_string(kSecEdgarQ2ResultLimit),
         [&](DataChunk &chunk) {
           Col<string_t> name, tag;
           Col<double> value;
           name.Init(chunk, 0);
           tag.Init(chunk, 1);
           value.Init(chunk, 2);
           for (idx_t r = 0; r < chunk.size(); ++r) {
             const bool null_name = !name.Valid(r);
             consumer({null_name ? std::string_view() : View(name.Get(r)),
                       null_name, View(tag.Get(r)), value.Get(r)});
           }
         });
}

} // namespace accelerator_codegen::aux_state::query
