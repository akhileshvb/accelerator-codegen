// The chunked maps rank presence bitmaps with popcount: use the instruction
// (every x86-64 CPU this runs on has it) instead of the libgcc fallback.
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && !defined(__POPCNT__)
#pragma GCC target("popcnt")
#endif

#include "sec_edgar/q6/common/state.hpp"

#include <algorithm>
#include <sys/resource.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define ACCEL_Q6_X86 1
#endif

namespace accelerator_codegen::aux_state::query {
namespace {

constexpr size_t kTopTarget = 512;     // desired cache size after a rescan / trim
constexpr size_t kTopFloorMax = 4096;  // trim when the cache grows past this

std::atomic<bool> g_force_scalar{false};

bool CpuHasAvx2() {
#ifdef ACCEL_Q6_X86
  static const bool has = __builtin_cpu_supports("avx2");
  return has;
#else
  return false;
#endif
}

bool UseAvx2() {
  return CpuHasAvx2() && !g_force_scalar.load(std::memory_order_relaxed);
}

inline uint32_t Load32(const char *p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

inline void Append32(std::string &out, uint32_t v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof v);
}

inline int CmpSum(double a, double b) noexcept {
  if (a == b) {
    return 0;
  }
  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  // NaN involved: DuckDB orders NaN above every other value.
  const bool na = a != a;
  const bool nb = b != b;
  if (na && nb) {
    return 0;
  }
  return na ? 1 : -1;
}

inline bool AtLeast(double sum, double lb) noexcept { return !(sum < lb); }

// ---------------------------------------------------------------------------
// tv-run search over unaligned little-endian u32 arrays inside a run record.
// ---------------------------------------------------------------------------
size_t FindTvScalar(const char *a, size_t n, uint32_t key) noexcept {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = lo + ((hi - lo) >> 1);
    if (Load32(a + 4 * mid) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return (lo < n && Load32(a + 4 * lo) == key) ? lo : n;
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) size_t FindTvAvx2(const char *a, size_t n,
                                                   uint32_t key) noexcept {
  const __m256i k = _mm256_set1_epi32(static_cast<int>(key));
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256i v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + 4 * i));
    const int m = _mm256_movemask_ps(
        _mm256_castsi256_ps(_mm256_cmpeq_epi32(v, k)));
    if (m) {
      return i + static_cast<size_t>(__builtin_ctz(static_cast<unsigned>(m)));
    }
  }
  for (; i < n; ++i) {
    if (Load32(a + 4 * i) == key) {
      return i;
    }
  }
  return n;
}
#endif

inline size_t FindTv(const char *a, size_t n, uint32_t key) noexcept {
#ifdef ACCEL_Q6_X86
  if (n <= 64 && UseAvx2()) {
    return FindTvAvx2(a, n, key);
  }
#endif
  return FindTvScalar(a, n, key);
}

// ---------------------------------------------------------------------------
// Collect ids of active groups of a combo with !(sum < lb). `data` holds the
// aggregates of the version (slots >= size are zero / inactive).
// ---------------------------------------------------------------------------
void CollectScalar(const SecEdgarQ6Structure &s, const SecEdgarQ6Agg *data,
                   size_t size, uint32_t combo, double lb,
                   std::vector<uint32_t> &out) {
  const uint32_t groups =
      static_cast<uint32_t>(std::min<size_t>(s.GroupCount(), size));
  constexpr uint32_t kChunk = 1u << kQ6GroupShift;
  for (uint32_t base = 0; base < groups; base += kChunk) {
    const auto *cmb = s.group_combo.ChunkAt(base >> kQ6GroupShift);
    if (!cmb) {
      continue;
    }
    const uint32_t n = std::min<uint32_t>(kChunk, groups - base);
    for (uint32_t i = 0; i < n; ++i) {
      const SecEdgarQ6Agg &a = data[base + i];
      if ((*cmb)[i] == combo && a.cnt != 0 && AtLeast(a.hi, lb)) {
        out.push_back(base + i);
      }
    }
  }
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) void
CollectAvx2(const SecEdgarQ6Structure &s, const SecEdgarQ6Agg *data, size_t size,
            uint32_t combo, double lb, std::vector<uint32_t> &out) {
  const uint32_t groups =
      static_cast<uint32_t>(std::min<size_t>(s.GroupCount(), size));
  constexpr uint32_t kChunk = 1u << kQ6GroupShift;
  const __m256i cv = _mm256_set1_epi32(static_cast<int>(combo));
  for (uint32_t base = 0; base < groups; base += kChunk) {
    const auto *cmb_chunk = s.group_combo.ChunkAt(base >> kQ6GroupShift);
    if (!cmb_chunk) {
      continue;
    }
    const uint32_t *cmb = cmb_chunk->data();
    const uint32_t n = std::min<uint32_t>(kChunk, groups - base);
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
      unsigned mask = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(
          _mm256_cmpeq_epi32(
              _mm256_loadu_si256(reinterpret_cast<const __m256i *>(cmb + i)), cv))));
      while (mask) {
        const uint32_t j = i + static_cast<uint32_t>(__builtin_ctz(mask));
        mask &= mask - 1;
        const SecEdgarQ6Agg &a = data[base + j];
        if (a.cnt != 0 && AtLeast(a.hi, lb)) {
          out.push_back(base + j);
        }
      }
    }
    for (; i < n; ++i) {
      const SecEdgarQ6Agg &a = data[base + i];
      if (cmb[i] == combo && a.cnt != 0 && AtLeast(a.hi, lb)) {
        out.push_back(base + i);
      }
    }
  }
}
#endif

void Collect(const SecEdgarQ6Structure &s, const SecEdgarQ6Agg *data, size_t size,
             uint32_t combo, double lb, std::vector<uint32_t> &out) {
#ifdef ACCEL_Q6_X86
  if (UseAvx2()) {
    CollectAvx2(s, data, size, combo, lb, out);
    return;
  }
#endif
  CollectScalar(s, data, size, combo, lb, out);
}

inline uint64_t GroupHash(uint32_t n, uint32_t t, uint32_t p, uint32_t c) noexcept {
  uint64_t h = (static_cast<uint64_t>(n) + 1) * 0x9E3779B97F4A7C15ULL ^
               (static_cast<uint64_t>(t) + 0x7F4A7C15ULL) * 0xC2B2AE3D27D4EB4FULL ^
               (static_cast<uint64_t>(p) + 1) * 0x165667B19E3779F9ULL ^
               (static_cast<uint64_t>(c) + 0x5851F42DULL) * 0x94D049BB133111EBULL;
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ULL;
  h ^= h >> 32;
  return h;
}

inline void AppendRun(std::string &record, const std::vector<uint32_t> &tvs,
                      const std::vector<uint32_t> &stmts,
                      const std::vector<uint32_t> &pls) {
  record.clear();
  Append32(record, static_cast<uint32_t>(tvs.size()));
  record.append(reinterpret_cast<const char *>(tvs.data()), 4 * tvs.size());
  record.append(reinterpret_cast<const char *>(stmts.data()), 4 * stmts.size());
  record.append(reinterpret_cast<const char *>(pls.data()), 4 * pls.size());
}

// The entries of `tv` in a serialized run.
SecEdgarQ6Hit HitOfRun(std::string_view run, uint32_t tv_id) {
  SecEdgarQ6Hit hit;
  if (run.size() < 4) {
    return hit;
  }
  const size_t n = Load32(run.data());
  const char *tv = run.data() + 4;
  const size_t idx = FindTv(tv, n, tv_id);
  if (idx == n) {
    return hit;
  }
  size_t end = idx + 1;
  while (end < n && Load32(tv + 4 * end) == tv_id) {
    ++end;
  }
  hit.run = run;
  hit.first = static_cast<uint32_t>(idx);
  hit.count = static_cast<uint32_t>(end - idx);
  return hit;
}

} // namespace

uint32_t SecEdgarQ6Hit::Stmt(uint32_t i) const noexcept {
  const size_t n = Load32(run.data());
  return Load32(run.data() + 4 + 4 * n + 4 * (first + i));
}
uint32_t SecEdgarQ6Hit::Plabel(uint32_t i) const noexcept {
  const size_t n = Load32(run.data());
  return Load32(run.data() + 4 + 8 * n + 4 * (first + i));
}

// ---------------------------------------------------------------------------
// Double-double
// ---------------------------------------------------------------------------
SecEdgarQ6Sum SecEdgarQ6SumAdd(SecEdgarQ6Sum a, SecEdgarQ6Sum b) noexcept {
  const double s = a.hi + b.hi;
  if (!std::isfinite(s)) {
    return {s, 0.0};
  }
  const double bb = s - a.hi;
  double e = (a.hi - (s - bb)) + (b.hi - bb);
  e += a.lo + b.lo;
  const double hi = s + e;
  return {hi, e - (hi - s)};
}

// ---------------------------------------------------------------------------
// Persistent aggregate array
// ---------------------------------------------------------------------------
struct SecEdgarQ6AggStore::Family {
  std::mutex mu;
  std::vector<SecEdgarQ6Agg> data; // values of the root version
};

struct SecEdgarQ6AggStore::Node {
  std::shared_ptr<Family> family;
  // nullptr: this node is the root (owns `family->data`). Otherwise this
  // version = next's version with `log` applied (in reverse order).
  std::shared_ptr<Node> next;
  std::vector<std::pair<uint32_t, SecEdgarQ6Agg>> log;

  ~Node() {
    // Release a long chain iteratively (no deep recursion).
    std::shared_ptr<Node> n = std::move(next);
    while (n && n.use_count() == 1) {
      std::shared_ptr<Node> after = std::move(n->next);
      n = std::move(after);
    }
  }
};

SecEdgarQ6AggStore::SecEdgarQ6AggStore() : node_(std::make_shared<Node>()) {
  node_->family = std::make_shared<Family>();
}

std::mutex &SecEdgarQ6AggStore::Mutex() const { return node_->family->mu; }

void SecEdgarQ6AggStore::Reroot(const std::shared_ptr<Node> &node) {
  if (!node->next) {
    return;
  }
  std::vector<std::shared_ptr<Node>> path;
  for (std::shared_ptr<Node> p = node; p; p = p->next) {
    path.push_back(p);
  }
  auto &data = node->family->data;
  // path.back() is the root; move the root one step at a time towards `node`.
  for (size_t k = path.size() - 1; k-- > 0;) {
    const std::shared_ptr<Node> &x = path[k];
    std::shared_ptr<Node> root = std::move(x->next);
    auto &log = x->log;
    for (size_t i = log.size(); i-- > 0;) {
      std::swap(data[log[i].first], log[i].second);
    }
    std::reverse(log.begin(), log.end());
    root->log = std::move(log);
    log.clear();
    root->next = x;
  }
}

std::pair<const SecEdgarQ6Agg *, size_t> SecEdgarQ6AggStore::RootView() const {
  Reroot(node_);
  const auto &data = node_->family->data;
  return {data.data(), data.size()};
}

SecEdgarQ6Agg SecEdgarQ6AggStore::Get(uint32_t gid) const {
  std::lock_guard<std::mutex> guard(Mutex());
  const auto view = RootView();
  return gid < view.second ? view.first[gid] : SecEdgarQ6Agg{};
}

size_t SecEdgarQ6AggStore::Bytes() const {
  std::lock_guard<std::mutex> guard(Mutex());
  return node_->family->data.capacity() * sizeof(SecEdgarQ6Agg) +
         node_->log.capacity() * sizeof(node_->log[0]) + sizeof(Node);
}

SecEdgarQ6AggStore::Writer::Writer(SecEdgarQ6AggStore &store)
    : lock_(store.Mutex()) {
  Reroot(store.node_);
  data_ = &store.node_->family->data;
  if (store.node_.use_count() > 1) {
    // Other states (or older versions) see this version: fork. The old node
    // becomes "new root + undo log".
    auto fresh = std::make_shared<Node>();
    fresh->family = store.node_->family;
    keep_ = store.node_;
    keep_->next = fresh;
    log_ = &keep_->log;
    store.node_ = std::move(fresh);
  }
}

void SecEdgarQ6AggStore::Writer::Set(uint32_t gid, const SecEdgarQ6Agg &value) {
  if (gid >= data_->size()) {
    Reserve(static_cast<size_t>(gid) + 1);
  }
  SecEdgarQ6Agg &slot = (*data_)[gid];
  if (log_) {
    log_->emplace_back(gid, slot);
  }
  slot = value;
}

void SecEdgarQ6AggStore::Writer::Reserve(size_t n) {
  if (n > data_->size()) {
    if (n > data_->capacity()) {
      data_->reserve(std::max(n, data_->capacity() + data_->capacity() / 2));
    }
    data_->resize(n);
  }
}

SecEdgarQ6Agg *SecEdgarQ6AggStore::Writer::MutableData(size_t n) {
  Reserve(n);
  return data_->data();
}

// ---------------------------------------------------------------------------
// String pool
// ---------------------------------------------------------------------------
uint64_t SecEdgarQ6StringPool::Hash(const char *p, size_t n) noexcept {
  constexpr uint64_t kMul = 0x9FB21C651E98DF25ULL;
  uint64_t h = 0x243F6A8885A308D3ULL ^ (n * 0x9E3779B97F4A7C15ULL);
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    h = (h ^ w) * kMul;
    h ^= h >> 29;
    p += 8;
    n -= 8;
  }
  if (n) {
    uint64_t w = 0;
    std::memcpy(&w, p, n);
    h = (h ^ w) * kMul;
    h ^= h >> 29;
  }
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ULL;
  h ^= h >> 32;
  return h;
}

uint32_t SecEdgarQ6StringPool::FindHashed(uint64_t hash,
                                          std::string_view s) const {
  const auto &index = data_->index;
  for (uint64_t key = hash;; ++key) {
    const uint32_t id = index.Find(key, key);
    if (id == SecEdgarQ6CowIndex<uint64_t>::kAbsent) {
      return 0;
    }
    if (View(id) == s) {
      return id;
    }
  }
}

uint32_t SecEdgarQ6StringPool::Intern(std::string_view s) {
  const uint64_t hash = Hash(s);
  uint64_t key = hash;
  for (;; ++key) {
    const uint32_t id = data_->index.Find(key, key);
    if (id == SecEdgarQ6CowIndex<uint64_t>::kAbsent) {
      break;
    }
    if (View(id) == s) {
      return id;
    }
  }
  Data &d = data_.Mutable();
  if (d.count >= 0xFFFFFFFEu) {
    throw std::length_error("SEC EDGAR Q6 string pool id space exhausted");
  }
  const uint32_t id = ++d.count;
  d.strings.Set(id, s);
  d.index.Insert(key, key, id);
  return id;
}

size_t SecEdgarQ6StringPool::Bytes() const {
  return data_->strings.MemoryBytes() + data_->index.memory_bytes();
}

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------
uint32_t SecEdgarQ6Structure::FindTv(std::string_view tag,
                                     std::string_view version) const {
  const uint32_t tag_id = tags.Find(tag);
  if (tag_id == 0) {
    return 0;
  }
  const uint32_t version_id = versions.Find(version);
  if (version_id == 0) {
    return 0;
  }
  return FindTvIds(tag_id, version_id);
}

SecEdgarQ6Structure::SecEdgarQ6Structure() {
  adsh_keys.SetHasher(&U64Hash);
  tv_index.SetHasher(&U64Hash);
  group_index.SetHasher(&SecEdgarQ6GroupKeyHash);
}

uint32_t SecEdgarQ6Structure::InternTv(uint32_t tag_id, uint32_t version_id) {
  const uint32_t found = FindTvIds(tag_id, version_id);
  if (found != 0) {
    return found;
  }
  if (tv_count >= 0xFFFFFFFEu) {
    throw std::length_error("SEC EDGAR Q6 tag/version id space exhausted");
  }
  const uint32_t tv = ++tv_count;
  const uint64_t key = static_cast<uint64_t>(TvKey(tag_id, version_id));
  tv_index.Insert(key, U64Hash(key), tv);
  tv_tag.Set(tv, tag_id);
  return tv;
}

namespace {
// "dddddddddd-dd-dddddd" -> its 18 digits as an integer (exact for the
// canonical form); false for any other string.
bool AdshKey(std::string_view s, uint64_t &key) {
  if (s.size() != 20 || s[10] != '-' || s[13] != '-') {
    return false;
  }
  uint64_t v = 0;
  for (size_t i = 0; i < 20; ++i) {
    if (i == 10 || i == 13) {
      continue;
    }
    const unsigned d = static_cast<unsigned char>(s[i]) - static_cast<unsigned>('0');
    if (d > 9) {
      return false;
    }
    v = v * 10 + d;
  }
  key = v;
  return true;
}
} // namespace

uint32_t SecEdgarQ6Structure::FindAdshHashed(uint64_t pool_hash,
                                             std::string_view adsh) const {
  uint64_t key;
  if (AdshKey(adsh, key)) {
    const uint32_t id = adsh_keys.Find(key, U64Hash(key));
    return id == SecEdgarQ6CowIndex<uint64_t>::kAbsent ? 0 : id;
  }
  return adshs.FindHashed(pool_hash, adsh);
}

uint32_t SecEdgarQ6Structure::FindAdsh(std::string_view adsh) const {
  uint64_t key;
  if (AdshKey(adsh, key)) {
    const uint32_t id = adsh_keys.Find(key, U64Hash(key));
    return id == SecEdgarQ6CowIndex<uint64_t>::kAbsent ? 0 : id;
  }
  return adshs.Find(adsh);
}

uint32_t SecEdgarQ6Structure::InternAdsh(std::string_view adsh) {
  const uint32_t before = adshs.Count();
  const uint32_t id = adshs.Intern(adsh);
  uint64_t key;
  if (id > before && AdshKey(adsh, key)) {
    adsh_keys.Insert(key, U64Hash(key), id);
  }
  return id;
}

SecEdgarQ6Hit SecEdgarQ6Structure::Lookup(uint32_t adsh_id, uint32_t tv_id) const {
  std::string_view run;
  if (!runs.Get(adsh_id, run)) {
    return {};
  }
  SecEdgarQ6Hit hit = HitOfRun(run, tv_id);
  hit.adsh_id = adsh_id;
  hit.tag_id = tv_tag.Get(tv_id);
  return hit;
}

uint32_t SecEdgarQ6Structure::FindCombo(int32_t fy, uint32_t stmt, uint32_t uom) const {
  const auto it = combos->index.find({fy, stmt, uom});
  return it == combos->index.end() ? kNoPayload : it->second;
}

uint32_t SecEdgarQ6Structure::FindOrAddCombo(int32_t fy, uint32_t stmt, uint32_t uom) {
  const uint32_t found = FindCombo(fy, stmt, uom);
  if (found != kNoPayload) {
    return found;
  }
  auto next = std::make_shared<SecEdgarQ6Combos>(*combos);
  const uint32_t id = static_cast<uint32_t>(next->keys.size());
  next->keys.push_back({fy, stmt, uom});
  next->index.emplace(SecEdgarQ6ComboKey{fy, stmt, uom}, id);
  combos = std::move(next);
  return id;
}

uint64_t SecEdgarQ6GroupKeyHash(const SecEdgarQ6GroupKey &k) noexcept {
  return GroupHash(k.name, k.tag, k.plabel, k.combo);
}

uint32_t SecEdgarQ6Structure::FindGroup(uint32_t name_id, uint32_t tag_id,
                                        uint32_t plabel_id, uint32_t combo) const {
  const SecEdgarQ6GroupKey key{name_id, tag_id, plabel_id, combo};
  const uint32_t gid = group_index.Find(key, SecEdgarQ6GroupKeyHash(key));
  return gid == SecEdgarQ6CowIndex<SecEdgarQ6GroupKey>::kAbsent ? kNoPayload : gid;
}

uint32_t SecEdgarQ6Structure::FindOrAddGroup(uint32_t name_id, uint32_t tag_id,
                                             uint32_t plabel_id, uint32_t combo) {
  const uint32_t found = FindGroup(name_id, tag_id, plabel_id, combo);
  if (found != kNoPayload) {
    return found;
  }
  return AddNewGroup(name_id, tag_id, plabel_id, combo);
}

uint32_t SecEdgarQ6Structure::AddNewGroup(uint32_t name_id, uint32_t tag_id,
                                          uint32_t plabel_id, uint32_t combo) {
  if (group_count >= kMaxGroups) {
    throw std::length_error("SEC EDGAR Q6 group id space exhausted");
  }
  const SecEdgarQ6GroupKey key{name_id, tag_id, plabel_id, combo};
  const uint32_t gid = group_count++;
  groups.Mutable(gid) = {name_id, tag_id, plabel_id};
  group_combo.Mutable(gid) = combo;
  group_index.Insert(key, SecEdgarQ6GroupKeyHash(key), gid);
  return gid;
}

size_t SecEdgarQ6Structure::Bytes() const {
  return adshs.Bytes() + names.Bytes() + tags.Bytes() + versions.Bytes() +
         plabels.Bytes() + stmts.Bytes() + uoms.Bytes() + adsh_keys.memory_bytes() +
         adsh_name.MemoryBytes() +
         adsh_fy.MemoryBytes() + tv_index.memory_bytes() + tv_tag.MemoryBytes() +
         runs.MemoryBytes() + groups.MemoryBytes() + group_combo.MemoryBytes() +
         group_index.memory_bytes() + sizeof(*this);
}

void SecEdgarQ6StructureBuilder::AddCohort(std::string_view adsh, int32_t fy,
                                           std::string_view name,
                                           bool name_is_null) {
  const uint32_t before = s_.adshs.Count();
  const uint32_t id = s_.InternAdsh(adsh);
  if (id <= before) {
    return; // duplicate adsh: keep the first
  }
  s_.adsh_name.Set(id, name_is_null ? 0 : s_.names.Intern(name));
  s_.adsh_fy.Set(id, static_cast<uint32_t>(fy));
}

void SecEdgarQ6StructureBuilder::AddPre(std::string_view adsh, std::string_view tag,
                                        std::string_view version,
                                        std::string_view stmt,
                                        std::string_view plabel,
                                        bool plabel_is_null) {
  const uint32_t adsh_id = s_.FindAdsh(adsh);
  if (adsh_id == 0) {
    return;
  }
  const uint32_t tag_id = s_.tags.Intern(tag);
  const uint32_t tv_id = s_.InternTv(tag_id, s_.versions.Intern(version));
  const uint32_t stmt_id = s_.stmts.Intern(stmt);
  const uint32_t plabel_id = plabel_is_null ? 0 : s_.plabels.Intern(plabel);
  entries_.push_back({adsh_id, tv_id, stmt_id, plabel_id});
}

void SecEdgarQ6WriteRuns(SecEdgarQ6Structure &s, const uint32_t (*entries)[4],
                         size_t n) {
  std::string record;
  std::vector<uint32_t> tvs, stmts, pls;
  size_t i = 0;
  while (i < n) {
    const uint32_t adsh = entries[i][0];
    tvs.clear();
    stmts.clear();
    pls.clear();
    for (; i < n && entries[i][0] == adsh; ++i) {
      tvs.push_back(entries[i][1]);
      stmts.push_back(entries[i][2]);
      pls.push_back(entries[i][3]);
    }
    AppendRun(record, tvs, stmts, pls);
    s.runs.Set(adsh, record);
  }
}

SecEdgarQ6Structure SecEdgarQ6StructureBuilder::Finish() {
  auto &s = s_;
  std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
    if (a.adsh != b.adsh) return a.adsh < b.adsh;
    if (a.tv != b.tv) return a.tv < b.tv;
    if (a.stmt != b.stmt) return a.stmt < b.stmt;
    return a.plabel < b.plabel;
  });
  static_assert(sizeof(Entry) == 4 * sizeof(uint32_t), "entry layout");
  SecEdgarQ6WriteRuns(s, reinterpret_cast<const uint32_t(*)[4]>(entries_.data()),
                      entries_.size());
  entries_.clear();
  entries_.shrink_to_fit();
  s.adshs.Shrink();
  s.names.Shrink();
  s.tags.Shrink();
  s.versions.Shrink();
  s.plabels.Shrink();
  s.stmts.Shrink();
  s.uoms.Shrink();
  s.runs.Shrink();
  return std::move(s_);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
size_t SecEdgarQ6State::Bytes() const {
  size_t n = sizeof(*this) + structure.Bytes() + aggs.Bytes() +
             tops.size() * sizeof(SecEdgarQ6Tops::Ptr);
  for (size_t c = 0; c < tops.size(); ++c) {
    const auto &t = tops[c];
    n += sizeof(*t) + t->gids.capacity() * sizeof(uint32_t) +
         t->sums.capacity() * sizeof(double) + t->cnts.capacity() * sizeof(int64_t);
  }
  return n;
}

bool SecEdgarQ6State::TopUsable(uint32_t combo) const noexcept {
  return combo < tops.size() &&
         (tops[combo]->complete || tops[combo]->gids.size() >= kQ6OutputLimit);
}

namespace {
std::shared_ptr<const SecEdgarQ6TopCache> EmptyTop() {
  static const std::shared_ptr<const SecEdgarQ6TopCache> empty = [] {
    auto t = std::make_shared<SecEdgarQ6TopCache>();
    t->lower_bound = -std::numeric_limits<double>::infinity();
    t->complete = true;
    return t;
  }();
  return empty;
}
void SyncTops(SecEdgarQ6State &state) {
  while (state.tops.size() < state.structure.ComboCount()) {
    state.tops.push_back(EmptyTop());
  }
}
} // namespace

SecEdgarQ6State SecEdgarQ6MakeEmptyState(SecEdgarQ6Structure structure) {
  SecEdgarQ6State state;
  state.structure = std::move(structure);
  SyncTops(state);
  return state;
}

void SecEdgarQ6SetGroup(SecEdgarQ6State &state, uint32_t gid, double sum,
                        int64_t cnt) {
  SecEdgarQ6AggStore::Writer writer(state.aggs);
  writer.Set(gid, {sum, 0.0, cnt});
}

namespace {
// Output order within a combo: sum DESC, name, tag, plabel (NULL name / plabel
// last). The sums are given, the strings come from `s`.
bool GroupBeforeWith(const SecEdgarQ6Structure &s, double sa, double sb,
                     uint32_t a, uint32_t b) {
  if (a == b) {
    return false;
  }
  const int c = CmpSum(sa, sb);
  if (c != 0) {
    return c > 0;
  }
  const SecEdgarQ6Group ga = s.groups.Get(a);
  const SecEdgarQ6Group gb = s.groups.Get(b);
  if (ga.name != gb.name) {
    if (ga.name == 0) return false; // NULL sorts last
    if (gb.name == 0) return true;
    return s.names.View(ga.name) < s.names.View(gb.name);
  }
  if (ga.tag != gb.tag) {
    return s.tags.View(ga.tag) < s.tags.View(gb.tag);
  }
  if (ga.plabel != gb.plabel) {
    if (ga.plabel == 0) return false;
    if (gb.plabel == 0) return true;
    return s.plabels.View(ga.plabel) < s.plabels.View(gb.plabel);
  }
  return false;
}
} // namespace

bool SecEdgarQ6GroupBefore(const SecEdgarQ6State &state, uint32_t a, uint32_t b) {
  return GroupBeforeWith(state.structure, state.Sum(a), state.Sum(b), a, b);
}

void SecEdgarQ6CollectAtLeast(const SecEdgarQ6State &state, uint32_t combo,
                              double lb, std::vector<uint32_t> &out) {
  state.aggs.Read([&](const SecEdgarQ6Agg *data, size_t size) {
    Collect(state.structure, data, size, combo, lb, out);
  });
}

bool SecEdgarQ6UsesAvx2() { return UseAvx2(); }
void SecEdgarQ6ForceScalar(bool force) {
  g_force_scalar.store(force, std::memory_order_relaxed);
}

namespace {

// Builds the cache of one combo from its active group ids (exact selection).
std::shared_ptr<const SecEdgarQ6TopCache>
MakeTop(const SecEdgarQ6Structure &s, const SecEdgarQ6Agg *data,
        std::vector<uint32_t> ids) {
  auto top = std::make_shared<SecEdgarQ6TopCache>();
  if (ids.size() <= kTopTarget) {
    top->lower_bound = -std::numeric_limits<double>::infinity();
    top->complete = true;
    top->gids = std::move(ids);
  } else {
    std::vector<double> sums;
    sums.reserve(ids.size());
    for (const uint32_t g : ids) {
      sums.push_back(data[g].hi);
    }
    std::nth_element(sums.begin(), sums.begin() + static_cast<ptrdiff_t>(kTopTarget),
                     sums.end(), [](double a, double b) { return CmpSum(a, b) > 0; });
    const double lb = sums[kTopTarget];
    top->lower_bound = lb;
    top->complete = false;
    for (const uint32_t g : ids) {
      if (AtLeast(data[g].hi, lb)) {
        top->gids.push_back(g);
      }
    }
  }
  std::sort(top->gids.begin(), top->gids.end(), [&](uint32_t a, uint32_t b) {
    return GroupBeforeWith(s, data[a].hi, data[b].hi, a, b);
  });
  top->sums.reserve(top->gids.size());
  top->cnts.reserve(top->gids.size());
  for (const uint32_t g : top->gids) {
    top->sums.push_back(data[g].hi);
    top->cnts.push_back(data[g].cnt);
  }
  top->base_size = static_cast<uint32_t>(top->gids.size());
  return top;
}

uint64_t CountActive(const SecEdgarQ6Structure &s, const SecEdgarQ6Agg *data,
                     size_t size) {
  const size_t groups = std::min<size_t>(s.GroupCount(), size);
  uint64_t active = 0;
  for (size_t g = 0; g < groups; ++g) {
    active += data[g].cnt != 0;
  }
  return active;
}

} // namespace

void SecEdgarQ6RescanCombo(SecEdgarQ6State &state, uint32_t combo) {
  SyncTops(state);
  state.aggs.Read([&](const SecEdgarQ6Agg *data, size_t size) {
    std::vector<uint32_t> ids;
    Collect(state.structure, data, size, combo,
            -std::numeric_limits<double>::infinity(), ids);
    state.tops.Set(combo, MakeTop(state.structure, data, std::move(ids)));
  });
}

void SecEdgarQ6Rescan(SecEdgarQ6State &state) {
  SyncTops(state);
  const auto &s = state.structure;
  state.aggs.Read([&](const SecEdgarQ6Agg *data, size_t size) {
    state.active_groups = CountActive(s, data, size);
    std::vector<std::vector<uint32_t>> buckets(s.ComboCount());
    const uint32_t groups =
        static_cast<uint32_t>(std::min<size_t>(s.GroupCount(), size));
    constexpr uint32_t kChunk = 1u << kQ6GroupShift;
    for (uint32_t base = 0; base < groups; base += kChunk) {
      const auto *cmb = s.group_combo.ChunkAt(base >> kQ6GroupShift);
      if (!cmb) {
        continue;
      }
      const uint32_t n = std::min<uint32_t>(kChunk, groups - base);
      for (uint32_t i = 0; i < n; ++i) {
        if (data[base + i].cnt != 0) {
          buckets[(*cmb)[i]].push_back(base + i);
        }
      }
    }
    for (uint32_t c = 0; c < buckets.size(); ++c) {
      state.tops.Set(c, MakeTop(s, data, std::move(buckets[c])));
    }
  });
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------
SecEdgarQ6Hit SecEdgarQ6Resolve(const SecEdgarQ6Structure &s, std::string_view adsh,
                                std::string_view tag, std::string_view version) {
  const uint32_t adsh_id = s.FindAdsh(adsh);
  if (adsh_id == 0 || s.adsh_name.Get(adsh_id) == SecEdgarQ6Structure::kNotCohort) {
    return {};
  }
  const uint32_t tv_id = s.FindTv(tag, version);
  if (tv_id == 0) {
    return {};
  }
  return s.Lookup(adsh_id, tv_id);
}

void SecEdgarQ6ExpandHit(SecEdgarQ6Structure &s, const SecEdgarQ6Hit &hit,
                         uint32_t uom_id, SecEdgarQ6Sum sum, int64_t cnt,
                         std::vector<SecEdgarQ6GroupDelta> &out) {
  if (!hit.Found()) {
    return;
  }
  const uint32_t name = s.adsh_name.Get(hit.adsh_id);
  const int32_t fy = static_cast<int32_t>(s.adsh_fy.Get(hit.adsh_id));
  for (uint32_t i = 0; i < hit.count; ++i) {
    const uint32_t combo = s.FindOrAddCombo(fy, hit.Stmt(i), uom_id);
    out.push_back({s.FindOrAddGroup(name, hit.tag_id, hit.Plabel(i), combo), sum, cnt});
  }
}

namespace {

// Batch-local memo string -> id (the strings outlive the batch).
class LocalStrMemo {
public:
  template <typename F> uint32_t Get(std::string_view v, F &&compute) {
    const uint64_t h = SecEdgarQ6StringPool::Hash(v.data(), v.size());
    if (2 * (size_ + 1) > slots_.size()) {
      Grow();
    }
    size_t i = h & (slots_.size() - 1);
    for (;; i = (i + 1) & (slots_.size() - 1)) {
      Slot &s = slots_[i];
      if (!s.used) {
        break;
      }
      if (s.hash == h && s.len == v.size() &&
          (v.empty() || std::memcmp(keys_.data() + s.offset, v.data(), v.size()) == 0)) {
        return s.value;
      }
    }
    const uint32_t value = compute(h, v);
    // A private copy of the key: comparisons stay in this (small) buffer
    // instead of touching the caller's scattered strings again.
    slots_[i] = {h, static_cast<uint32_t>(keys_.size()), static_cast<uint32_t>(v.size()),
                 value, true};
    keys_.insert(keys_.end(), v.begin(), v.end());
    ++size_;
    return value;
  }

private:
  struct Slot {
    uint64_t hash = 0;
    uint32_t offset = 0;
    uint32_t len = 0;
    uint32_t value = 0;
    bool used = false;
  };
  std::vector<char> keys_;
  void Grow() {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(old.empty() ? 64 : old.size() * 2, Slot{});
    for (const Slot &s : old) {
      if (!s.used) {
        continue;
      }
      size_t i = s.hash & (slots_.size() - 1);
      while (slots_[i].used) {
        i = (i + 1) & (slots_.size() - 1);
      }
      slots_[i] = s;
    }
  }
  std::vector<Slot> slots_;
public:
  size_t size_ = 0;
};

} // namespace

void SecEdgarQ6ResolveBatch(SecEdgarQ6Structure &s, const SecEdgarQ6NumRef *refs,
                            size_t n, std::vector<SecEdgarQ6GroupDelta> &out) {
  constexpr uint32_t kNone = SecEdgarQ6Structure::kNoPayload;
  // Rows of one batch share adsh, tags, versions, uoms and (adsh, tag,
  // version, uom) keys: resolve each distinct value once, in phases whose
  // lookups are independent (sorted / prefetched) instead of one dependent
  // chain of cache misses per row.
  LocalStrMemo adsh_memo, tag_memo, version_memo, uom_memo;
  core::FlatHashMap<uint32_t> adsh_key_memo(64); // canonical adsh -> id / kNone
  core::FlatHashMap<uint32_t> tv_memo(64);       // (tag, version) -> tv (0 = none)
  core::FlatHashMap<uint32_t> span_memo(2 * n);  // (tv, adsh) / uom -> span slot
  struct Span {
    uint32_t adsh, tv, uom;
    uint32_t first = 0, count = 0; // into `pending` / `gids`
  };
  std::vector<Span> spans;
  std::vector<uint32_t> ref_span(n, kNone);

  // 1. strings -> ids, (adsh, tv, uom) -> span
  for (size_t i = 0; i < n; ++i) {
    const auto &r = refs[i];
    const auto adsh_of = [&](uint64_t h, std::string_view v) {
      const uint32_t id = s.FindAdshHashed(h, v);
      return id != 0 && s.adsh_name.Get(id) != SecEdgarQ6Structure::kNotCohort ? id : kNone;
    };
    uint32_t adsh_id;
    uint64_t adsh_key;
    if (AdshKey(r.adsh, adsh_key)) {
      const auto ins = adsh_key_memo.Insert(static_cast<int64_t>(adsh_key), 0);
      if (ins.second) {
        *ins.first = adsh_of(0, r.adsh);
      }
      adsh_id = *ins.first;
    } else {
      adsh_id = adsh_memo.Get(r.adsh, adsh_of);
    }
    if (adsh_id == kNone) {
      continue;
    }
    const uint32_t tag_id = tag_memo.Get(
        r.tag, [&](uint64_t h, std::string_view v) { return s.tags.FindHashed(h, v); });
    if (tag_id == 0) {
      continue;
    }
    const uint32_t version_id = version_memo.Get(
        r.version, [&](uint64_t h, std::string_view v) { return s.versions.FindHashed(h, v); });
    if (version_id == 0) {
      continue;
    }
    const int64_t tv_key = SecEdgarQ6Structure::TvKey(tag_id, version_id);
    uint32_t tv;
    if (const uint32_t *found = tv_memo.Find(tv_key)) {
      tv = *found;
    } else {
      tv = s.FindTvIds(tag_id, version_id);
      tv_memo.Upsert(tv_key, tv);
    }
    if (tv == 0) {
      continue;
    }
    const uint32_t uom_id = uom_memo.Get(
        r.uom, [&](uint64_t, std::string_view v) { return s.uoms.Intern(v); });
    // (tv, adsh) is exact in 64 bits; the uom is folded in with a check.
    const uint64_t hit_key = (static_cast<uint64_t>(tv) << 32) | adsh_id;
    uint64_t probe = SecEdgarQ6Mix64(hit_key ^ (static_cast<uint64_t>(uom_id) * 0x9E3779B97F4A7C15ULL));
    for (;; ++probe) {
      const auto ins = span_memo.Insert(static_cast<int64_t>(probe),
                                        static_cast<uint32_t>(spans.size()));
      if (ins.second) {
        spans.push_back({adsh_id, tv, uom_id});
        ref_span[i] = *ins.first;
        break;
      }
      const Span &sp = spans[*ins.first];
      if (sp.adsh == adsh_id && sp.tv == tv && sp.uom == uom_id) {
        ref_span[i] = *ins.first;
        break;
      }
    }
  }

  // 2. bridge runs adsh by adsh (one record read per adsh) -> group keys
  struct Pending {
    SecEdgarQ6GroupKey key;
    uint64_t hash;
  };
  std::vector<Pending> pending;
  pending.reserve(spans.size() + spans.size() / 4);
  {
    // Group the spans by adsh (any order inside an adsh).
    std::vector<uint32_t> order(spans.size());
    {
      std::vector<uint64_t> packed(spans.size());
      for (uint32_t k = 0; k < spans.size(); ++k) {
        packed[k] = (static_cast<uint64_t>(spans[k].adsh) << 32) | k;
      }
      std::sort(packed.begin(), packed.end());
      for (uint32_t k = 0; k < spans.size(); ++k) {
        order[k] = static_cast<uint32_t>(packed[k]);
      }
    }
    core::FlatHashMap<uint32_t> combo_memo(64); // (fy, stmt, uom) packed -> combo
    const auto combo_of = [&](int32_t fy, uint32_t stmt, uint32_t uom) {
      if (stmt >= (1u << 16) || uom >= (1u << 16)) {
        return s.FindOrAddCombo(fy, stmt, uom);
      }
      const int64_t key = static_cast<int64_t>(
          (static_cast<uint64_t>(static_cast<uint32_t>(fy)) << 32) | (stmt << 16) | uom);
      if (const uint32_t *c = combo_memo.Find(key)) {
        return *c;
      }
      const uint32_t c = s.FindOrAddCombo(fy, stmt, uom);
      combo_memo.Upsert(key, c);
      return c;
    };
    // Prefetch the run records a few adsh ahead (their tv arrays are searched).
    std::vector<uint32_t> adsh_starts;
    for (size_t k = 0; k < order.size(); ++k) {
      if (k == 0 || spans[order[k]].adsh != spans[order[k - 1]].adsh) {
        adsh_starts.push_back(static_cast<uint32_t>(k));
      }
    }
    size_t next_prefetch = 0;
    const auto prefetch_run = [&](size_t group) {
      std::string_view r;
      if (group < adsh_starts.size() && s.runs.Get(spans[order[adsh_starts[group]]].adsh, r)) {
        const size_t bytes = std::min<size_t>(r.size(), 4 + 4 * static_cast<size_t>(Load32(r.data())));
        for (size_t off = 0; off < bytes; off += 64) {
          __builtin_prefetch(r.data() + off);
        }
      }
    };
    constexpr size_t kRunsAhead = 4;
    for (; next_prefetch < std::min(kRunsAhead, adsh_starts.size()); ++next_prefetch) {
      prefetch_run(next_prefetch);
    }
    uint32_t cur_adsh = kNone;
    std::string_view run;
    bool has_run = false;
    uint32_t name = 0;
    int32_t fy = 0;
    uint32_t cur_tv = 0, cur_tag = 0;
    for (const uint32_t k : order) {
      Span &sp = spans[k];
      if (sp.adsh != cur_adsh) {
        cur_adsh = sp.adsh;
        if (next_prefetch < adsh_starts.size()) {
          prefetch_run(next_prefetch++);
        }
        has_run = s.runs.Get(cur_adsh, run);
        name = s.adsh_name.Get(cur_adsh);
        fy = static_cast<int32_t>(s.adsh_fy.Get(cur_adsh));
      }
      sp.first = static_cast<uint32_t>(pending.size());
      if (!has_run) {
        continue;
      }
      const SecEdgarQ6Hit hit = HitOfRun(run, sp.tv);
      if (!hit.Found()) {
        continue;
      }
      if (sp.tv != cur_tv) {
        cur_tv = sp.tv;
        cur_tag = s.tv_tag.Get(sp.tv);
      }
      for (uint32_t e = 0; e < hit.count; ++e) {
        Pending p;
        p.key = {name, cur_tag, hit.Plabel(e), combo_of(fy, hit.Stmt(e), sp.uom)};
        p.hash = SecEdgarQ6GroupKeyHash(p.key);
        pending.push_back(p);
      }
      sp.count = hit.count;
    }
  }

  // 3. group ids (prefetched lookups; new groups are added in order)
  std::vector<uint32_t> gids(pending.size());
  constexpr size_t kAhead = 8;
  for (size_t j = 0; j < std::min(kAhead, pending.size()); ++j) {
    s.group_index.Prefetch(pending[j].hash);
  }
  for (size_t j = 0; j < pending.size(); ++j) {
    if (j + kAhead < pending.size()) {
      s.group_index.Prefetch(pending[j + kAhead].hash);
    }
    const Pending &p = pending[j];
    uint32_t gid = s.group_index.Find(p.key, p.hash);
    if (gid == SecEdgarQ6CowIndex<SecEdgarQ6GroupKey>::kAbsent) {
      gid = s.AddNewGroup(p.key.name, p.key.tag, p.key.plabel, p.key.combo);
    }
    gids[j] = gid;
  }

  // 4. deltas
  for (size_t i = 0; i < n; ++i) {
    if (ref_span[i] == kNone) {
      continue;
    }
    const Span &sp = spans[ref_span[i]];
    if (sp.count == 0) {
      continue;
    }
    const auto &r = refs[i];
    SecEdgarQ6Sum sum;
    int64_t cnt;
    if (r.pair) {
      sum = SecEdgarQ6SumAdd({r.value, 0.0}, {-r.old_value, 0.0});
      cnt = 0;
    } else {
      sum = {static_cast<double>(r.sign) * r.value, 0.0};
      cnt = r.sign;
    }
    for (uint32_t k = 0; k < sp.count; ++k) {
      out.push_back({gids[sp.first + k], sum, cnt});
    }
  }
}

// ---------------------------------------------------------------------------
// Delta application
// ---------------------------------------------------------------------------
namespace {

struct Touch {
  uint32_t combo;
  uint32_t gid;
  SecEdgarQ6Agg before;
  SecEdgarQ6Agg after;
};

// Index of the first cache entry not before (sum, gid) in output order.
size_t CachePosition(const SecEdgarQ6Structure &s, const SecEdgarQ6TopCache &t,
                     double sum, uint32_t gid) {
  size_t lo = 0, hi = t.gids.size();
  while (lo < hi) {
    const size_t mid = lo + ((hi - lo) >> 1);
    if (GroupBeforeWith(s, t.sums[mid], sum, t.gids[mid], gid)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// Raise the lower bound so that about kTopTarget entries remain (the cache
// stays the exact >= lb set because it is sorted by sum).
void Trim(SecEdgarQ6TopCache &t) {
  const double lb = t.sums[kTopTarget];
  if (std::isnan(lb) || lb == -std::numeric_limits<double>::infinity()) {
    return;
  }
  size_t end = kTopTarget + 1;
  while (end < t.sums.size() && AtLeast(t.sums[end], lb)) {
    ++end;
  }
  t.gids.resize(end);
  t.sums.resize(end);
  t.cnts.resize(end);
  t.lower_bound = lb;
  t.complete = false;
  t.base_size = static_cast<uint32_t>(end);
}

// The combo's next cache after the touched groups [first, last) changed, or
// nullptr when it must be rescanned. O(cache + touched log touched).
std::shared_ptr<SecEdgarQ6TopCache>
UpdateTop(const SecEdgarQ6Structure &s, const SecEdgarQ6TopCache &old,
          const Touch *first, const Touch *last) {
  const double lb = old.lower_bound;
  const size_t n = old.gids.size();
  std::vector<uint32_t> drop; // cache positions of touched members
  std::vector<uint32_t> add;  // indexes into [first, last)
  for (const Touch *t = first; t != last; ++t) {
    if (t->before.cnt != 0 && AtLeast(t->before.hi, lb)) {
      const size_t at = CachePosition(s, old, t->before.hi, t->gid);
      if (at >= n || old.gids[at] != t->gid) {
        return nullptr; // invariant violated: be safe
      }
      drop.push_back(static_cast<uint32_t>(at));
    }
    if (t->after.cnt != 0 && AtLeast(t->after.hi, lb)) {
      add.push_back(static_cast<uint32_t>(t - first));
    }
  }
  std::sort(drop.begin(), drop.end());
  std::sort(add.begin(), add.end(), [&](uint32_t a, uint32_t b) {
    return GroupBeforeWith(s, first[a].after.hi, first[b].after.hi, first[a].gid,
                           first[b].gid);
  });
  auto top = std::make_shared<SecEdgarQ6TopCache>();
  top->lower_bound = lb;
  top->complete = old.complete;
  top->base_size = old.base_size;
  const size_t size = n - drop.size() + add.size();
  top->gids.reserve(size);
  top->sums.reserve(size);
  top->cnts.reserve(size);
  size_t i = 0, d = 0, a = 0;
  const auto push_old = [&] {
    top->gids.push_back(old.gids[i]);
    top->sums.push_back(old.sums[i]);
    top->cnts.push_back(old.cnts[i]);
  };
  while (i < n || a < add.size()) {
    if (i < n && d < drop.size() && drop[d] == i) {
      ++d;
      ++i;
      continue;
    }
    if (a < add.size() &&
        (i == n || GroupBeforeWith(s, first[add[a]].after.hi, old.sums[i],
                                   first[add[a]].gid, old.gids[i]))) {
      const Touch &t = first[add[a++]];
      top->gids.push_back(t.gid);
      top->sums.push_back(t.after.hi);
      top->cnts.push_back(t.after.cnt);
    } else {
      push_old();
      ++i;
    }
  }
  if (!top->complete && top->gids.size() < kQ6OutputLimit) {
    return nullptr;
  }
  if (top->gids.size() > kTopFloorMax && top->gids.size() > 2ull * top->base_size) {
    Trim(*top);
  }
  return top;
}

// Applies `deltas` (any gids of next's structure) onto `next`, a copy of the
// old state whose structure may have gained groups / combos.
bool ApplyInto(SecEdgarQ6State &next, std::vector<SecEdgarQ6GroupDelta> &deltas,
               bool *overflow) {
  std::sort(deltas.begin(), deltas.end(),
            [](const SecEdgarQ6GroupDelta &a, const SecEdgarQ6GroupDelta &b) {
              return a.gid < b.gid;
            });
  size_t m = 0;
  for (size_t i = 0; i < deltas.size();) {
    SecEdgarQ6GroupDelta merged = deltas[i];
    size_t j = i + 1;
    for (; j < deltas.size() && deltas[j].gid == merged.gid; ++j) {
      merged.sum = SecEdgarQ6SumAdd(merged.sum, deltas[j].sum);
      merged.cnt += deltas[j].cnt;
    }
    if (merged.sum.hi != 0.0 || merged.sum.lo != 0.0 || merged.cnt != 0) {
      deltas[m++] = merged;
    }
    i = j;
  }
  deltas.resize(m);
  if (deltas.empty()) {
    return false;
  }
  SyncTops(next);
  const SecEdgarQ6Structure &structure = next.structure;

  std::vector<Touch> touched;
  touched.reserve(deltas.size());
  {
    SecEdgarQ6AggStore::Writer writer(next.aggs);
    writer.Reserve(structure.GroupCount());
    for (const auto &d : deltas) {
      const SecEdgarQ6Agg before = writer.Get(d.gid);
      SecEdgarQ6Agg after = before;
      after.cnt += d.cnt;
      if (after.cnt == 0) {
        after.hi = after.lo = 0.0;
      } else {
        const SecEdgarQ6Sum r = SecEdgarQ6SumAdd({before.hi, before.lo}, d.sum);
        after.hi = r.hi;
        after.lo = r.lo;
        if (overflow && std::isfinite(before.hi) && std::isfinite(d.sum.hi) &&
            !std::isfinite(after.hi)) {
          *overflow = true;
        }
      }
      writer.Set(d.gid, after);
      next.active_groups += static_cast<uint64_t>(after.cnt != 0) -
                            static_cast<uint64_t>(before.cnt != 0);
      touched.push_back({structure.group_combo.Get(d.gid), d.gid, before, after});
    }
  }

  std::sort(touched.begin(), touched.end(), [](const Touch &a, const Touch &b) {
    return a.combo != b.combo ? a.combo < b.combo : a.gid < b.gid;
  });
  std::vector<uint32_t> rescan;
  for (size_t i = 0; i < touched.size();) {
    const uint32_t combo = touched[i].combo;
    size_t j = i + 1;
    while (j < touched.size() && touched[j].combo == combo) {
      ++j;
    }
    auto top = UpdateTop(structure, *next.tops[combo], touched.data() + i,
                         touched.data() + j);
    if (top) {
      next.tops.Set(combo, std::move(top));
    } else {
      rescan.push_back(combo);
    }
    i = j;
  }
  for (const uint32_t combo : rescan) {
    SecEdgarQ6RescanCombo(next, combo);
  }
  return true;
}

bool TooManyGroups(const SecEdgarQ6Structure &s) {
  return s.group_count > s.base_group_count + s.base_group_count / 2 + 4096;
}

} // namespace

std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyDeltas(const SecEdgarQ6State &old_state,
                      std::vector<SecEdgarQ6GroupDelta> &deltas, bool *overflow) {
  auto next = std::make_shared<SecEdgarQ6State>(old_state); // shares chunks
  if (!ApplyInto(*next, deltas, overflow)) {
    return nullptr;
  }
  return next;
}

std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyNums(const SecEdgarQ6State &old_state, const SecEdgarQ6NumRef *refs,
                    size_t n, bool *overflow) {
  auto next = std::make_shared<SecEdgarQ6State>(old_state);
  std::vector<SecEdgarQ6GroupDelta> deltas;
  deltas.reserve(n + n / 2);
  SecEdgarQ6ResolveBatch(next->structure, refs, n, deltas);
  if (TooManyGroups(next->structure) && overflow) {
    *overflow = true;
  }
  if (!ApplyInto(*next, deltas, overflow)) {
    return nullptr;
  }
  return next;
}

uint32_t SecEdgarQ6FindComboFor(const SecEdgarQ6Structure &s, int64_t fy,
                                std::string_view stmt, std::string_view uom) {
  if (fy < std::numeric_limits<int32_t>::min() ||
      fy > std::numeric_limits<int32_t>::max()) {
    return SecEdgarQ6Structure::kNoPayload;
  }
  const uint32_t stmt_id = s.stmts.Find(stmt);
  const uint32_t uom_id = s.uoms.Find(uom);
  if (stmt_id == 0 || uom_id == 0) {
    return SecEdgarQ6Structure::kNoPayload;
  }
  return s.FindCombo(static_cast<int32_t>(fy), stmt_id, uom_id);
}

std::vector<SecEdgarQ6OutputRow>
SecEdgarQ6TopRows(const SecEdgarQ6State &state, int64_t fy, std::string_view stmt,
                  std::string_view uom, size_t limit) {
  const uint32_t combo = SecEdgarQ6FindComboFor(state.structure, fy, stmt, uom);
  if (combo == SecEdgarQ6Structure::kNoPayload) {
    return {};
  }
  return SecEdgarQ6TopRowsOfCombo(state, combo, stmt, limit);
}

std::vector<SecEdgarQ6OutputRow>
SecEdgarQ6TopRowsOfCombo(const SecEdgarQ6State &state, uint32_t combo,
                         std::string_view stmt, size_t limit) {
  if (combo >= state.tops.size()) {
    return {};
  }
  std::shared_ptr<const SecEdgarQ6TopCache> top = state.tops[combo];
  if (!state.TopUsable(combo)) {
    SecEdgarQ6State scratch = state;
    SecEdgarQ6RescanCombo(scratch, combo);
    top = scratch.tops[combo];
  }
  const auto &s = state.structure;
  const size_t n = std::min(limit, top->gids.size());
  std::vector<SecEdgarQ6OutputRow> rows;
  rows.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t gid = top->gids[i];
    const SecEdgarQ6Group g = s.groups.Get(gid);
    SecEdgarQ6OutputRow row;
    if (g.name == 0) {
      row.name_is_null = true;
    } else {
      row.name = std::string(s.names.View(g.name));
    }
    row.stmt = std::string(stmt);
    row.tag = std::string(s.tags.View(g.tag));
    if (g.plabel == 0) {
      row.plabel_is_null = true;
    } else {
      row.plabel = std::string(s.plabels.View(g.plabel));
    }
    row.total_value = top->sums[i];
    row.cnt = top->cnts[i];
    rows.push_back(std::move(row));
  }
  return rows;
}

// ---------------------------------------------------------------------------
// Structural edits
// ---------------------------------------------------------------------------
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyEdits(const SecEdgarQ6State &old_state,
                     const std::vector<SecEdgarQ6AdshEdit> &edits,
                     const std::vector<SecEdgarQ6NumRef> &other_refs,
                     bool *overflow) {
  const SecEdgarQ6Structure &os = old_state.structure;
  auto base = std::make_shared<SecEdgarQ6State>(old_state);
  SecEdgarQ6Structure &s = base->structure;
  std::vector<SecEdgarQ6GroupDelta> deltas;
  SecEdgarQ6ResolveBatch(s, other_refs.data(), other_refs.size(), deltas);

  struct Entry {
    uint32_t tv, stmt, plabel;
  };
  std::string record;

  for (const auto &edit : edits) {
    // Old contributions (old structure, pre-transaction sums).
    const uint32_t old_id = os.FindAdsh(edit.adsh);
    if (old_id != 0 && os.adsh_name.Get(old_id) != SecEdgarQ6Structure::kNotCohort) {
      const uint32_t old_name = os.adsh_name.Get(old_id);
      const int32_t old_fy = static_cast<int32_t>(os.adsh_fy.Get(old_id));
      for (const auto &n : edit.old_nums) {
        const uint32_t tv = os.FindTv(n.tag, n.version);
        const uint32_t uom = os.uoms.Find(n.uom);
        if (tv == 0 || uom == 0) continue;
        const SecEdgarQ6Hit hit = os.Lookup(old_id, tv);
        for (uint32_t i = 0; i < hit.count; ++i) {
          const uint32_t combo = os.FindCombo(old_fy, hit.Stmt(i), uom);
          const uint32_t gid = combo == SecEdgarQ6Structure::kNoPayload
                                   ? SecEdgarQ6Structure::kNoPayload
                                   : os.FindGroup(old_name, hit.tag_id, hit.Plabel(i), combo);
          if (gid == SecEdgarQ6Structure::kNoPayload) {
            return nullptr; // inconsistent old state: rebuild
          }
          deltas.push_back({gid, SecEdgarQ6Sum{-n.sum, -n.sum_lo}, -n.cnt});
        }
      }
    }
    uint32_t id = s.FindAdsh(edit.adsh);
    if (!edit.new_cohort) {
      if (id != 0) {
        s.adsh_name.Set(id, SecEdgarQ6Structure::kNotCohort);
        s.runs.Erase(id);
      }
      continue;
    }
    if (id == 0) {
      id = s.InternAdsh(edit.adsh);
    }
    const uint32_t name_id = edit.name_is_null ? 0 : s.names.Intern(edit.name);
    s.adsh_name.Set(id, name_id);
    s.adsh_fy.Set(id, static_cast<uint32_t>(edit.fy));

    std::vector<Entry> es;
    for (const auto &pe : edit.pre) {
      const uint32_t tag_id = s.tags.Intern(pe.tag);
      const uint32_t tv = s.InternTv(tag_id, s.versions.Intern(pe.version));
      const uint32_t stmt = s.stmts.Intern(pe.stmt);
      const uint32_t plabel_id = pe.plabel_is_null ? 0 : s.plabels.Intern(pe.plabel);
      for (uint32_t k = 0; k < pe.mult; ++k) {
        es.push_back({tv, stmt, plabel_id});
      }
    }
    std::sort(es.begin(), es.end(), [](const Entry &a, const Entry &b) {
      return std::tie(a.tv, a.stmt, a.plabel) < std::tie(b.tv, b.stmt, b.plabel);
    });
    std::vector<uint32_t> tvs, stmts, pls;
    for (const auto &e : es) {
      tvs.push_back(e.tv);
      stmts.push_back(e.stmt);
      pls.push_back(e.plabel);
    }
    AppendRun(record, tvs, stmts, pls);
    // New contributions from the run being installed.
    for (const auto &n : edit.new_nums) {
      const uint32_t tv = s.FindTv(n.tag, n.version);
      if (tv == 0) continue;
      SecEdgarQ6Hit hit = HitOfRun(record, tv);
      if (!hit.Found()) continue;
      hit.adsh_id = id;
      hit.tag_id = s.tv_tag.Get(tv);
      SecEdgarQ6ExpandHit(s, hit, s.uoms.Intern(n.uom),
                          SecEdgarQ6Sum{n.sum, n.sum_lo}, n.cnt, deltas);
    }
    if (tvs.empty()) {
      s.runs.Erase(id);
    } else {
      s.runs.Set(id, record);
    }
  }

  if (TooManyGroups(s)) {
    return nullptr; // too many unused groups accumulated: let the caller rebuild
  }
  ApplyInto(*base, deltas, overflow); // groups / combos may have been added
  return base;
}

} // namespace accelerator_codegen::aux_state::query
