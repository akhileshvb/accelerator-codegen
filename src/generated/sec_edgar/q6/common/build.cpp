// The chunked maps rank presence bitmaps with popcount: use the instruction
// (every x86-64 CPU this runs on has it) instead of the libgcc fallback.
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && !defined(__POPCNT__)
#pragma GCC target("popcnt")
#endif

#include "sec_edgar/q6/common/build.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr uint32_t kUnknown = 0xFFFFFFFFu;
constexpr size_t kBlock = 64; // rows per prefetch block

inline void Prefetch(const void *p) { __builtin_prefetch(p, 1, 3); }

inline uint64_t Mix(uint64_t x) noexcept {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDULL;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ULL;
  x ^= x >> 33;
  return x;
}

inline std::string_view View(const SecEdgarQ6Str &s) { return {s.data, s.size}; }

// Per-column id cache keyed by the batch's dictionary.
struct ColumnCache {
  uint64_t token = 0;
  std::vector<uint32_t> ids;
};

// out[row] = resolve(cell) for every row; dictionary-encoded columns resolve
// each dictionary entry once (per dictionary, across batches when the
// dictionary has a token).
template <typename F>
void ResolveColumn(const SecEdgarQ6StrColumn &col, size_t rows, ColumnCache &cache,
                   uint32_t *out, F &&resolve) {
  if (col.codes && col.dict_size != 0) {
    bool cached = col.dict_token != 0 && col.dict_token == cache.token &&
                  cache.ids.size() == col.dict_size;
    if (!cached && (col.dict_token != 0 || col.dict_size <= rows)) {
      cache.token = col.dict_token;
      cache.ids.assign(col.dict_size, kUnknown);
      cached = true;
    }
    if (cached) {
      uint32_t *ids = cache.ids.data();
      for (size_t i = 0; i < rows; ++i) {
        const uint32_t code = col.codes[i];
        if (code >= col.dict_size) {
          out[i] = resolve(col.dict_get(col.dict, code));
          continue;
        }
        uint32_t id = ids[code];
        if (id == kUnknown) {
          id = resolve(col.dict_get(col.dict, code));
          ids[code] = id;
        }
        out[i] = id;
      }
      return;
    }
  }
  for (size_t i = 0; i < rows; ++i) {
    out[i] = resolve(col.At(i));
  }
}

// Build-local string -> pool id index (flat open addressing over copies of
// the strings): far cheaper than the copy-on-write pool index for the tens of
// millions of lookups of a build.
class StrIndex {
public:
  StrIndex() { Rehash(1u << 12); }
  static uint64_t Hash(std::string_view v) noexcept {
    return SecEdgarQ6StringPool::Hash(v.data(), v.size());
  }
  uint32_t Find(uint64_t h, std::string_view v) const noexcept {
    for (size_t i = h & mask_;; i = (i + 1) & mask_) {
      const Slot &s = slots_[i];
      if (s.id == 0) {
        return 0;
      }
      if (s.hash == h && s.len == v.size() &&
          (v.empty() || std::memcmp(s.ptr, v.data(), v.size()) == 0)) {
        return s.id;
      }
    }
  }
  void Insert(uint64_t h, std::string_view v, uint32_t id) {
    if (2 * (size_ + 1) > slots_.size()) {
      Rehash(slots_.size() * 2);
    }
    size_t i = h & mask_;
    while (slots_[i].id != 0) {
      i = (i + 1) & mask_;
    }
    slots_[i] = {h, id, static_cast<uint32_t>(v.size()), Copy(v)};
    ++size_;
  }

private:
  struct Slot {
    uint64_t hash = 0;
    uint32_t id = 0; // 0 = empty
    uint32_t len = 0;
    const char *ptr = nullptr;
  };
  const char *Copy(std::string_view v) {
    if (v.size() > block_left_) {
      const size_t size = std::max<size_t>(v.size(), 1u << 20);
      blocks_.emplace_back(new char[size]);
      block_at_ = blocks_.back().get();
      block_left_ = size;
    }
    char *out = block_at_;
    if (!v.empty()) {
      std::memcpy(out, v.data(), v.size());
    }
    block_at_ += v.size();
    block_left_ -= v.size();
    return out;
  }
  void Rehash(size_t cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
    for (const Slot &s : old) {
      if (s.id == 0) {
        continue;
      }
      size_t i = s.hash & mask_;
      while (slots_[i].id != 0) {
        i = (i + 1) & mask_;
      }
      slots_[i] = s;
    }
  }
  std::vector<Slot> slots_;
  size_t mask_ = 0;
  size_t size_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  char *block_at_ = nullptr;
  size_t block_left_ = 0;
};

inline uint32_t InternVia(StrIndex &ix, SecEdgarQ6StringPool &pool, std::string_view v) {
  const uint64_t h = StrIndex::Hash(v);
  uint32_t id = ix.Find(h, v);
  if (id == 0) {
    id = pool.Intern(v);
    ix.Insert(h, v, id);
  }
  return id;
}

inline uint32_t FindVia(const StrIndex &ix, std::string_view v) {
  return ix.Find(StrIndex::Hash(v), v);
}

// (adsh, tag, version) -> hit, open addressing, fixed size (keys unique).
class HitTable {
public:
  struct Slot {
    uint32_t adsh = 0; // 0 = empty
    uint32_t tag = 0;
    uint32_t ver = 0;
    uint32_t hit = 0;
  };
  void Init(size_t n) {
    size_t cap = 16;
    while (cap < 2 * n) {
      cap <<= 1;
    }
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
  }
  static uint64_t Hash(uint32_t a, uint32_t t, uint32_t v) noexcept {
    return Mix(((static_cast<uint64_t>(t) << 32) | v) ^
               Mix(static_cast<uint64_t>(a) * 0x9E3779B97F4A7C15ULL));
  }
  size_t Home(uint64_t h) const noexcept { return static_cast<size_t>(h) & mask_; }
  const Slot *SlotAt(size_t i) const noexcept { return &slots_[i]; }
  void Insert(size_t i, uint32_t a, uint32_t t, uint32_t v, uint32_t hit) {
    while (slots_[i].adsh != 0) {
      i = (i + 1) & mask_;
    }
    slots_[i] = {a, t, v, hit};
  }
  uint32_t Find(size_t i, uint32_t a, uint32_t t, uint32_t v) const noexcept {
    for (;; i = (i + 1) & mask_) {
      const Slot &s = slots_[i];
      if (s.adsh == 0) {
        return kUnknown;
      }
      if (s.adsh == a && s.tag == t && s.ver == v) {
        return s.hit;
      }
    }
  }

private:
  std::vector<Slot> slots_;
  size_t mask_ = 0;
};

struct GroupKey {
  uint32_t name, tag, plabel, combo;
  bool operator==(const GroupKey &o) const noexcept {
    return name == o.name && tag == o.tag && plabel == o.plabel && combo == o.combo;
  }
};

inline uint64_t GroupKeyHash(const GroupKey &k) noexcept {
  return Mix((static_cast<uint64_t>(k.name) << 32 | k.tag) ^
             Mix((static_cast<uint64_t>(k.plabel) << 32 | k.combo) +
                 0x9E3779B97F4A7C15ULL));
}

// GroupKey -> dense gid (first-seen order), growable open addressing.
class GroupTable {
public:
  GroupTable() { Rehash(1u << 16); }
  size_t Home(uint64_t h) const noexcept { return static_cast<size_t>(h) & mask_; }
  const void *SlotAt(size_t i) const noexcept { return &slots_[i]; }
  // Returns the gid; `added` is set when the key was new (gid == size()-1).
  uint32_t FindOrAdd(const GroupKey &k, uint64_t h, bool &added) {
    added = false;
    for (size_t i = Home(h);; i = (i + 1) & mask_) {
      Slot &s = slots_[i];
      if (s.gid == kUnknown) {
        if (2 * (keys_.size() + 1) > slots_.size()) {
          Rehash(slots_.size() * 2);
          return FindOrAdd(k, h, added);
        }
        s.key = k;
        s.gid = static_cast<uint32_t>(keys_.size());
        keys_.push_back(k);
        added = true;
        return s.gid;
      }
      if (s.key == k) {
        return s.gid;
      }
    }
  }
  const std::vector<GroupKey> &Keys() const { return keys_; }

private:
  struct Slot {
    GroupKey key{};
    uint32_t gid = kUnknown;
  };
  void Rehash(size_t cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(cap, Slot{});
    mask_ = cap - 1;
    for (const Slot &s : old) {
      if (s.gid == kUnknown) {
        continue;
      }
      size_t i = Home(GroupKeyHash(s.key));
      while (slots_[i].gid != kUnknown) {
        i = (i + 1) & mask_;
      }
      slots_[i] = s;
    }
  }
  std::vector<Slot> slots_;
  std::vector<GroupKey> keys_;
  size_t mask_ = 0;
};

} // namespace

struct SecEdgarQ6Builder::Impl {
  SecEdgarQ6Structure s;
  StrIndex adsh_ix, name_ix, tag_ix, ver_ix, stmt_ix, plabel_ix, uom_ix;

  // pre phase
  ColumnCache pa, pt, pv, ps, pp;
  std::vector<uint32_t> ra, rt, rv, rs, rp;
  struct Entry {
    uint32_t adsh, tv, stmt, plabel;
  };
  std::vector<Entry> entries;
  std::vector<uint32_t> tv_tag{0}, tv_ver{0}; // by tv id
  uint32_t last_tag = 0, last_ver = 0, last_tv = 0;

  // hits: distinct (adsh, tv) of the sorted entries
  HitTable hits;
  std::vector<uint32_t> hit_first; // + sentinel
  std::vector<uint32_t> hit_adsh;
  struct HitAgg {
    uint32_t uom = 0; // 0 = unused
    uint32_t pad = 0;
    double hi = 0.0;
    double lo = 0.0;
    int64_t cnt = 0;
  };
  std::vector<HitAgg> hit_aggs;
  struct Overflow {
    uint32_t hit = 0;
    uint32_t uom = 0;
    double hi = 0.0;
    double lo = 0.0;
    int64_t cnt = 0;
  };
  core::FlatHashMap<uint32_t> overflow_index; // (hit << 32 | uom) -> index
  std::vector<Overflow> overflow;

  // num phase
  ColumnCache na, nt, nv, nu;
  std::vector<uint32_t> ua, ut, uv, uu;
  std::vector<uint32_t> cand;
  std::vector<uint64_t> cand_hash;
  std::vector<uint32_t> cand_hit;

  uint32_t TvOf(uint32_t tag, uint32_t ver) {
    if (tag == last_tag && ver == last_ver && last_tv != 0) {
      return last_tv;
    }
    const uint32_t tv = s.InternTv(tag, ver);
    if (tv >= tv_tag.size()) {
      tv_tag.resize(tv + 1, 0);
      tv_ver.resize(tv + 1, 0);
      tv_tag[tv] = tag;
      tv_ver[tv] = ver;
    }
    last_tag = tag;
    last_ver = ver;
    last_tv = tv;
    return tv;
  }

  static void AddTo(double &hi, double &lo, int64_t &cnt, double v_hi, double v_lo,
                    int64_t c) {
    const SecEdgarQ6Sum r = SecEdgarQ6SumAdd({hi, lo}, {v_hi, v_lo});
    hi = r.hi;
    lo = r.lo;
    cnt += c;
  }

  void Accumulate(uint32_t hit, uint32_t uom, double value) {
    HitAgg &a = hit_aggs[hit];
    if (a.uom == uom || a.uom == 0) {
      a.uom = uom;
      AddTo(a.hi, a.lo, a.cnt, value, 0.0, 1);
      return;
    }
    const int64_t key = static_cast<int64_t>((static_cast<uint64_t>(hit) << 32) | uom);
    const auto ins = overflow_index.Insert(key, static_cast<uint32_t>(overflow.size()));
    if (ins.second) {
      overflow.push_back({hit, uom, 0.0, 0.0, 0});
    }
    Overflow &o = overflow[*ins.first];
    AddTo(o.hi, o.lo, o.cnt, value, 0.0, 1);
  }
};

SecEdgarQ6Builder::SecEdgarQ6Builder() : impl_(std::make_unique<Impl>()) {}
SecEdgarQ6Builder::~SecEdgarQ6Builder() = default;

void SecEdgarQ6Builder::AddCohort(std::string_view adsh, int32_t fy,
                                  std::string_view name, bool name_is_null) {
  auto &s = impl_->s;
  const uint32_t before = s.adshs.Count();
  const uint64_t h = StrIndex::Hash(adsh);
  uint32_t id = impl_->adsh_ix.Find(h, adsh);
  if (id == 0) {
    id = s.InternAdsh(adsh);
    impl_->adsh_ix.Insert(h, adsh, id);
  }
  if (id <= before) {
    return; // duplicate adsh: keep the first
  }
  s.adsh_name.Set(id, name_is_null ? 0 : InternVia(impl_->name_ix, s.names, name));
  s.adsh_fy.Set(id, static_cast<uint32_t>(fy));
}

void SecEdgarQ6Builder::AddPreBatch(const SecEdgarQ6PreBatch &batch) {
  Impl &m = *impl_;
  auto &s = m.s;
  const size_t n = batch.rows;
  if (n == 0) {
    return;
  }
  m.ra.resize(n);
  m.rt.resize(n);
  m.rv.resize(n);
  m.rs.resize(n);
  m.rp.resize(n);
  ResolveColumn(batch.adsh, n, m.pa, m.ra.data(), [&](SecEdgarQ6Str x) {
    return x.data ? FindVia(m.adsh_ix, View(x)) : 0u;
  });
  ResolveColumn(batch.tag, n, m.pt, m.rt.data(), [&](SecEdgarQ6Str x) {
    return x.data ? InternVia(m.tag_ix, s.tags, View(x)) : 0u;
  });
  ResolveColumn(batch.version, n, m.pv, m.rv.data(), [&](SecEdgarQ6Str x) {
    return x.data ? InternVia(m.ver_ix, s.versions, View(x)) : 0u;
  });
  ResolveColumn(batch.stmt, n, m.ps, m.rs.data(), [&](SecEdgarQ6Str x) {
    return x.data ? InternVia(m.stmt_ix, s.stmts, View(x)) : 0u;
  });
  ResolveColumn(batch.plabel, n, m.pp, m.rp.data(), [&](SecEdgarQ6Str x) {
    return x.data ? InternVia(m.plabel_ix, s.plabels, View(x)) : 0u;
  });
  for (size_t i = 0; i < n; ++i) {
    if (m.ra[i] == 0 || m.rt[i] == 0 || m.rv[i] == 0 || m.rs[i] == 0) {
      continue; // not in the cohort / NULL join key or stmt
    }
    m.entries.push_back({m.ra[i], m.TvOf(m.rt[i], m.rv[i]), m.rs[i], m.rp[i]});
  }
}

void SecEdgarQ6Builder::FinishPre() {
  Impl &m = *impl_;
  auto &s = m.s;
  const size_t n = m.entries.size();
  // Counting sort by adsh, then (tv, stmt, plabel) inside each adsh.
  {
    const size_t adshs = static_cast<size_t>(s.adshs.Count()) + 2;
    std::vector<uint32_t> start(adshs + 1, 0);
    for (const auto &e : m.entries) {
      ++start[e.adsh + 1];
    }
    for (size_t i = 1; i < start.size(); ++i) {
      start[i] += start[i - 1];
    }
    std::vector<Impl::Entry> sorted(n);
    {
      std::vector<uint32_t> at(start.begin(), start.end() - 1);
      for (const auto &e : m.entries) {
        sorted[at[e.adsh]++] = e;
      }
    }
    for (size_t a = 0; a + 1 < start.size(); ++a) {
      if (start[a + 1] - start[a] > 1) {
        std::sort(sorted.begin() + start[a], sorted.begin() + start[a + 1],
                  [](const Impl::Entry &x, const Impl::Entry &y) {
                    if (x.tv != y.tv) return x.tv < y.tv;
                    if (x.stmt != y.stmt) return x.stmt < y.stmt;
                    return x.plabel < y.plabel;
                  });
      }
    }
    m.entries.swap(sorted);
  }
  static_assert(sizeof(Impl::Entry) == 4 * sizeof(uint32_t), "entry layout");
  SecEdgarQ6WriteRuns(s, reinterpret_cast<const uint32_t(*)[4]>(m.entries.data()), n);

  size_t hit_count = 0;
  for (size_t i = 0; i < n; ++i) {
    hit_count += i == 0 || m.entries[i].adsh != m.entries[i - 1].adsh ||
                 m.entries[i].tv != m.entries[i - 1].tv;
  }
  m.hits.Init(hit_count);
  m.hit_first.reserve(hit_count + 1);
  m.hit_adsh.reserve(hit_count);
  // Insert in blocks: hash + prefetch, then place.
  uint64_t hashes[kBlock];
  uint32_t keys[kBlock][4];
  size_t pending = 0;
  const auto flush = [&] {
    for (size_t k = 0; k < pending; ++k) {
      m.hits.Insert(m.hits.Home(hashes[k]), keys[k][0], keys[k][1], keys[k][2],
                    keys[k][3]);
    }
    pending = 0;
  };
  for (size_t i = 0; i < n; ++i) {
    const auto &e = m.entries[i];
    if (i != 0 && e.adsh == m.entries[i - 1].adsh && e.tv == m.entries[i - 1].tv) {
      continue;
    }
    const uint32_t h = static_cast<uint32_t>(m.hit_first.size());
    m.hit_first.push_back(static_cast<uint32_t>(i));
    m.hit_adsh.push_back(e.adsh);
    const uint32_t tag = m.tv_tag[e.tv], ver = m.tv_ver[e.tv];
    hashes[pending] = HitTable::Hash(e.adsh, tag, ver);
    keys[pending][0] = e.adsh;
    keys[pending][1] = tag;
    keys[pending][2] = ver;
    keys[pending][3] = h;
    Prefetch(m.hits.SlotAt(m.hits.Home(hashes[pending])));
    if (++pending == kBlock) {
      flush();
    }
  }
  flush();
  m.hit_first.push_back(static_cast<uint32_t>(n));
  m.hit_aggs.assign(hit_count, Impl::HitAgg{});
}

void SecEdgarQ6Builder::AddNumBatch(const SecEdgarQ6NumBatch &batch) {
  Impl &m = *impl_;
  auto &s = m.s;
  const size_t n = batch.rows;
  if (n == 0) {
    return;
  }
  m.ua.resize(n);
  m.ut.resize(n);
  m.uv.resize(n);
  m.uu.resize(n);
  ResolveColumn(batch.adsh, n, m.na, m.ua.data(), [&](SecEdgarQ6Str x) {
    return x.data ? FindVia(m.adsh_ix, View(x)) : 0u;
  });
  ResolveColumn(batch.tag, n, m.nt, m.ut.data(), [&](SecEdgarQ6Str x) {
    return x.data ? FindVia(m.tag_ix, View(x)) : 0u;
  });
  ResolveColumn(batch.version, n, m.nv, m.uv.data(), [&](SecEdgarQ6Str x) {
    return x.data ? FindVia(m.ver_ix, View(x)) : 0u;
  });
  ResolveColumn(batch.uom, n, m.nu, m.uu.data(), [&](SecEdgarQ6Str x) {
    return x.data ? InternVia(m.uom_ix, s.uoms, View(x)) : 0u;
  });
  m.cand.clear();
  m.cand_hash.clear();
  for (size_t i = 0; i < n; ++i) {
    if ((batch.value_valid && !batch.value_valid[i]) || m.ua[i] == 0 ||
        m.ut[i] == 0 || m.uv[i] == 0 || m.uu[i] == 0) {
      continue;
    }
    const uint64_t h = HitTable::Hash(m.ua[i], m.ut[i], m.uv[i]);
    m.cand.push_back(static_cast<uint32_t>(i));
    m.cand_hash.push_back(h);
  }
  const size_t c = m.cand.size();
  m.cand_hit.resize(c);
  for (size_t b = 0; b < c; b += kBlock) {
    const size_t e = std::min(c, b + kBlock);
    for (size_t k = b; k < e; ++k) {
      Prefetch(m.hits.SlotAt(m.hits.Home(m.cand_hash[k])));
    }
    for (size_t k = b; k < e; ++k) {
      const uint32_t i = m.cand[k];
      const uint32_t hit =
          m.hits.Find(m.hits.Home(m.cand_hash[k]), m.ua[i], m.ut[i], m.uv[i]);
      m.cand_hit[k] = hit;
      if (hit != kUnknown) {
        Prefetch(&m.hit_aggs[hit]);
      }
    }
    for (size_t k = b; k < e; ++k) {
      const uint32_t hit = m.cand_hit[k];
      if (hit != kUnknown) {
        const uint32_t i = m.cand[k];
        m.Accumulate(hit, m.uu[i], batch.value[i]);
      }
    }
  }
}

std::shared_ptr<SecEdgarQ6State> SecEdgarQ6Builder::Finish() {
  Impl &m = *impl_;
  auto &s = m.s;

  GroupTable table;
  std::vector<SecEdgarQ6Agg> sums;
  core::FlatHashMap<uint32_t> combo_cache; // (fy << 32 | stmt << 16 | uom) -> combo
  const auto combo_of = [&](int32_t fy, uint32_t stmt, uint32_t uom) {
    if (stmt >= (1u << 16) || uom >= (1u << 16)) {
      return s.FindOrAddCombo(fy, stmt, uom);
    }
    const int64_t key = static_cast<int64_t>(
        (static_cast<uint64_t>(static_cast<uint32_t>(fy)) << 32) | (stmt << 16) | uom);
    if (const uint32_t *c = combo_cache.Find(key)) {
      return *c;
    }
    const uint32_t c = s.FindOrAddCombo(fy, stmt, uom);
    combo_cache.Upsert(key, c);
    return c;
  };

  struct Pending {
    GroupKey key;
    uint64_t hash;
    double hi, lo;
    int64_t cnt;
  };
  std::vector<Pending> block;
  block.reserve(kBlock);
  const auto flush = [&] {
    for (const Pending &p : block) {
      bool added = false;
      const uint32_t gid = table.FindOrAdd(p.key, p.hash, added);
      if (added) {
        sums.push_back({});
      }
      SecEdgarQ6Agg &a = sums[gid];
      Impl::AddTo(a.hi, a.lo, a.cnt, p.hi, p.lo, p.cnt);
    }
    block.clear();
  };
  const auto contribute = [&](uint32_t hit, uint32_t uom, double hi, double lo,
                              int64_t cnt) {
    const uint32_t adsh = m.hit_adsh[hit];
    const uint32_t name = s.adsh_name.Get(adsh);
    const int32_t fy = static_cast<int32_t>(s.adsh_fy.Get(adsh));
    for (uint32_t e = m.hit_first[hit]; e < m.hit_first[hit + 1]; ++e) {
      const auto &entry = m.entries[e];
      Pending p;
      p.key = {name, m.tv_tag[entry.tv], entry.plabel, combo_of(fy, entry.stmt, uom)};
      p.hash = GroupKeyHash(p.key);
      p.hi = hi;
      p.lo = lo;
      p.cnt = cnt;
      Prefetch(table.SlotAt(table.Home(p.hash)));
      block.push_back(p);
      if (block.size() == kBlock) {
        flush();
      }
    }
  };
  for (uint32_t h = 0; h < m.hit_aggs.size(); ++h) {
    const auto &a = m.hit_aggs[h];
    if (a.uom != 0 && a.cnt != 0) {
      contribute(h, a.uom, a.hi, a.lo, a.cnt);
    }
  }
  for (const auto &o : m.overflow) {
    if (o.cnt != 0) {
      contribute(o.hit, o.uom, o.hi, o.lo, o.cnt);
    }
  }
  flush();

  // Release the scan-side tables before materializing the groups.
  m.hit_aggs = {};
  m.overflow = {};
  m.overflow_index = {};
  m.hits = HitTable{};
  m.entries = {};
  m.hit_first = {};
  m.hit_adsh = {};

  const auto &keys = table.Keys();
  s.group_index.Reserve(keys.size() + keys.size() / 2 + 4096);
  constexpr size_t kAhead = 16;
  const auto index_key = [&](size_t g) {
    return SecEdgarQ6GroupKey{keys[g].name, keys[g].tag, keys[g].plabel, keys[g].combo};
  };
  for (uint32_t gid = 0; gid < keys.size(); ++gid) {
    if (gid + kAhead < keys.size()) {
      s.group_index.Prefetch(SecEdgarQ6GroupKeyHash(index_key(gid + kAhead)));
    }
    const GroupKey &k = keys[gid];
    if (s.AddNewGroup(k.name, k.tag, k.plabel, k.combo) != gid) {
      throw std::logic_error("SEC EDGAR Q6 build: group id mismatch");
    }
  }
  s.base_group_count = s.group_count;
  s.adshs.Shrink();
  s.names.Shrink();
  s.tags.Shrink();
  s.versions.Shrink();
  s.plabels.Shrink();
  s.stmts.Shrink();
  s.uoms.Shrink();
  s.runs.Shrink();

  auto state = std::make_shared<SecEdgarQ6State>(SecEdgarQ6MakeEmptyState(std::move(s)));
  {
    SecEdgarQ6AggStore::Writer writer(state->aggs);
    SecEdgarQ6Agg *data = writer.MutableData(sums.size());
    std::copy(sums.begin(), sums.end(), data);
  }
  SecEdgarQ6Rescan(*state);
  return state;
}

} // namespace accelerator_codegen::aux_state::query
