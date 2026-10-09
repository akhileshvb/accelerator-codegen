#include "sec_edgar/q6/common/state.hpp"

#include <algorithm>
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

constexpr size_t kTopTarget = 512;     // desired cache size after a rescan
constexpr size_t kTopFloorMax = 4096;  // rescan when the cache grows past this
constexpr size_t kIncrementalTouchedMax = 1024;

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
// Collect ids of active groups of a combo with !(sum < lb)
// ---------------------------------------------------------------------------
void CollectScalar(const SecEdgarQ6State &state, uint32_t combo, double lb,
                   std::vector<uint32_t> &out) {
  const auto &s = state.structure;
  const uint32_t groups = s.GroupCount();
  for (uint32_t c = 0; c < state.cnt.NumChunks(); ++c) {
    const uint32_t base = c << kQ6ChunkShift;
    if (base >= groups) {
      break;
    }
    const auto *cnt = state.cnt.ChunkAt(c);
    const auto *sum = state.sum_hi.ChunkAt(c);
    const auto *cmb = s.group_combo.ChunkAt(c);
    if (!cnt || !sum || !cmb) {
      continue;
    }
    const uint32_t n = std::min<uint32_t>(kQ6ChunkSize, groups - base);
    for (uint32_t i = 0; i < n; ++i) {
      if ((*cmb)[i] == combo && (*cnt)[i] != 0 && AtLeast((*sum)[i], lb)) {
        out.push_back(base + i);
      }
    }
  }
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) void CollectAvx2(const SecEdgarQ6State &state,
                                                  uint32_t combo, double lb,
                                                  std::vector<uint32_t> &out) {
  const auto &s = state.structure;
  const uint32_t groups = s.GroupCount();
  const __m256d lbv = _mm256_set1_pd(lb);
  const __m256i zero = _mm256_setzero_si256();
  const __m128i cv = _mm_set1_epi32(static_cast<int>(combo));
  for (uint32_t c = 0; c < state.cnt.NumChunks(); ++c) {
    const uint32_t base = c << kQ6ChunkShift;
    if (base >= groups) {
      break;
    }
    const auto *cnt_chunk = state.cnt.ChunkAt(c);
    const auto *sum_chunk = state.sum_hi.ChunkAt(c);
    const auto *cmb_chunk = s.group_combo.ChunkAt(c);
    if (!cnt_chunk || !sum_chunk || !cmb_chunk) {
      continue;
    }
    const double *sum = sum_chunk->data();
    const int64_t *cnt = cnt_chunk->data();
    const uint32_t *cmb = cmb_chunk->data();
    const uint32_t n = std::min<uint32_t>(kQ6ChunkSize, groups - base);
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
      const int same = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(
          _mm_loadu_si128(reinterpret_cast<const __m128i *>(cmb + i)), cv)));
      if (!same) {
        continue;
      }
      const __m256d sv = _mm256_loadu_pd(sum + i);
      // _CMP_NLT_UQ: !(s < lb), true for NaN
      const int keep_sum = _mm256_movemask_pd(_mm256_cmp_pd(sv, lbv, _CMP_NLT_UQ));
      const __m256i cn =
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(cnt + i));
      const int inactive = _mm256_movemask_pd(
          _mm256_castsi256_pd(_mm256_cmpeq_epi64(cn, zero)));
      int mask = keep_sum & ~inactive & same & 0xF;
      while (mask) {
        const int j = __builtin_ctz(static_cast<unsigned>(mask));
        out.push_back(base + i + static_cast<uint32_t>(j));
        mask &= mask - 1;
      }
    }
    for (; i < n; ++i) {
      if (cmb[i] == combo && cnt[i] != 0 && AtLeast(sum[i], lb)) {
        out.push_back(base + i);
      }
    }
  }
}
#endif

void Collect(const SecEdgarQ6State &state, uint32_t combo, double lb,
             std::vector<uint32_t> &out) {
#ifdef ACCEL_Q6_X86
  if (UseAvx2()) {
    CollectAvx2(state, combo, lb, out);
    return;
  }
#endif
  CollectScalar(state, combo, lb, out);
}

uint64_t CountActiveScalar(const SecEdgarQ6State &state) {
  uint64_t active = 0;
  for (uint32_t c = 0; c < state.cnt.NumChunks(); ++c) {
    if (const auto *chunk = state.cnt.ChunkAt(c)) {
      for (const int64_t v : *chunk) {
        active += v != 0;
      }
    }
  }
  return active;
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) uint64_t CountActiveAvx2(
    const SecEdgarQ6State &state) {
  uint64_t active = 0;
  const __m256i zero = _mm256_setzero_si256();
  for (uint32_t c = 0; c < state.cnt.NumChunks(); ++c) {
    const auto *chunk = state.cnt.ChunkAt(c);
    if (!chunk) {
      continue;
    }
    const int64_t *cnt = chunk->data();
    uint64_t inactive = 0;
    for (uint32_t i = 0; i < kQ6ChunkSize; i += 4) {
      const __m256i v =
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(cnt + i));
      inactive += static_cast<uint64_t>(__builtin_popcount(
          static_cast<unsigned>(_mm256_movemask_pd(
              _mm256_castsi256_pd(_mm256_cmpeq_epi64(v, zero))))));
    }
    active += kQ6ChunkSize - inactive;
  }
  return active;
}
#endif

uint64_t CountActive(const SecEdgarQ6State &state) {
  // Slots past GroupCount() always have cnt == 0.
#ifdef ACCEL_Q6_X86
  if (UseAvx2()) {
    return CountActiveAvx2(state);
  }
#endif
  return CountActiveScalar(state);
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
  for (uint64_t key = hash;; ++key) {
    const uint32_t *id = index_.Find(static_cast<int64_t>(key));
    if (!id) {
      return 0;
    }
    if (View(*id) == s) {
      return *id;
    }
  }
}

uint32_t SecEdgarQ6StringPool::Intern(std::string_view s) {
  const uint64_t hash = Hash(s);
  uint64_t key = hash;
  for (;; ++key) {
    const uint32_t *id = index_.Find(static_cast<int64_t>(key));
    if (!id) {
      break;
    }
    if (View(*id) == s) {
      return *id;
    }
  }
  if (count_ >= 0xFFFFFFFEu) {
    throw std::length_error("SEC EDGAR Q6 string pool id space exhausted");
  }
  const uint32_t id = ++count_;
  strings_.Set(id, s);
  index_.Upsert(static_cast<int64_t>(key), id);
  return id;
}

size_t SecEdgarQ6StringPool::Bytes() const {
  return strings_.MemoryBytes() + index_.memory_bytes();
}

SecEdgarQ6TagVersionKey::SecEdgarQ6TagVersionKey(std::string_view tag,
                                                 std::string_view version) {
  const size_t total = 4 + tag.size() + version.size();
  char *dst;
  if (total <= sizeof(stack_)) {
    dst = stack_;
    ptr_ = stack_;
  } else {
    heap_.resize(total);
    dst = heap_.data();
    ptr_ = heap_.data();
  }
  const uint32_t tag_len = static_cast<uint32_t>(tag.size());
  std::memcpy(dst, &tag_len, 4);
  std::memcpy(dst + 4, tag.data(), tag.size());
  std::memcpy(dst + 4 + tag.size(), version.data(), version.size());
  size_ = total;
}

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------
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

uint32_t SecEdgarQ6Structure::FindGroup(uint32_t name_id, uint32_t tag_id,
                                        uint32_t plabel_id, uint32_t combo) const {
  for (uint64_t key = GroupHash(name_id, tag_id, plabel_id, combo);; ++key) {
    const uint32_t *gid = group_index.Find(static_cast<int64_t>(key));
    if (!gid) {
      return kNoPayload;
    }
    const SecEdgarQ6Group g = groups.Get(*gid);
    if (g.name == name_id && g.tag == tag_id && g.plabel == plabel_id &&
        group_combo.Get(*gid) == combo) {
      return *gid;
    }
  }
}

uint32_t SecEdgarQ6Structure::FindOrAddGroup(uint32_t name_id, uint32_t tag_id,
                                             uint32_t plabel_id, uint32_t combo) {
  uint64_t key = GroupHash(name_id, tag_id, plabel_id, combo);
  for (;; ++key) {
    const uint32_t *gid = group_index.Find(static_cast<int64_t>(key));
    if (!gid) {
      break;
    }
    const SecEdgarQ6Group g = groups.Get(*gid);
    if (g.name == name_id && g.tag == tag_id && g.plabel == plabel_id &&
        group_combo.Get(*gid) == combo) {
      return *gid;
    }
  }
  if (group_count >= kMaxGroups) {
    throw std::length_error("SEC EDGAR Q6 group id space exhausted");
  }
  const uint32_t gid = group_count++;
  groups.Mutable(gid) = {name_id, tag_id, plabel_id};
  group_combo.Mutable(gid) = combo;
  group_index.Upsert(static_cast<int64_t>(key), gid);
  return gid;
}

size_t SecEdgarQ6Structure::Bytes() const {
  return adshs.Bytes() + names.Bytes() + tags.Bytes() + tvs.Bytes() +
         plabels.Bytes() + stmts.Bytes() + uoms.Bytes() + adsh_name.MemoryBytes() +
         adsh_fy.MemoryBytes() + tv_tag.MemoryBytes() + runs.MemoryBytes() +
         groups.MemoryBytes() + group_combo.MemoryBytes() +
         group_index.memory_bytes() + sizeof(*this);
}

void SecEdgarQ6StructureBuilder::AddCohort(std::string_view adsh, int32_t fy,
                                           std::string_view name,
                                           bool name_is_null) {
  const uint32_t before = s_.adshs.Count();
  const uint32_t id = s_.adshs.Intern(adsh);
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
  const uint32_t adsh_id = s_.adshs.Find(adsh);
  if (adsh_id == 0) {
    return;
  }
  const uint32_t tag_id = s_.tags.Intern(tag);
  const SecEdgarQ6TagVersionKey key(tag, version);
  const uint32_t before = s_.tvs.Count();
  const uint32_t tv_id = s_.tvs.Intern(key.View());
  if (tv_id > before) {
    s_.tv_tag.Set(tv_id, tag_id);
  }
  const uint32_t stmt_id = s_.stmts.Intern(stmt);
  const uint32_t plabel_id = plabel_is_null ? 0 : s_.plabels.Intern(plabel);
  entries_.push_back({adsh_id, tv_id, stmt_id, plabel_id});
}

SecEdgarQ6Structure SecEdgarQ6StructureBuilder::Finish() {
  auto &s = s_;
  std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
    if (a.adsh != b.adsh) return a.adsh < b.adsh;
    if (a.tv != b.tv) return a.tv < b.tv;
    if (a.stmt != b.stmt) return a.stmt < b.stmt;
    return a.plabel < b.plabel;
  });
  std::string record;
  std::vector<uint32_t> tvs, stmts, pls;
  size_t i = 0;
  while (i < entries_.size()) {
    const uint32_t adsh = entries_[i].adsh;
    tvs.clear();
    stmts.clear();
    pls.clear();
    for (; i < entries_.size() && entries_[i].adsh == adsh; ++i) {
      tvs.push_back(entries_[i].tv);
      stmts.push_back(entries_[i].stmt);
      pls.push_back(entries_[i].plabel);
    }
    AppendRun(record, tvs, stmts, pls);
    s.runs.Set(adsh, record);
  }
  entries_.clear();
  entries_.shrink_to_fit();
  s.adshs.Shrink();
  s.names.Shrink();
  s.tags.Shrink();
  s.tvs.Shrink();
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
  size_t n = sizeof(*this) + structure.Bytes() + sum_hi.MemoryBytes() +
             sum_lo.MemoryBytes() + cnt.MemoryBytes() +
             tops.capacity() * sizeof(tops[0]);
  for (const auto &t : tops) {
    n += sizeof(*t) + t->gids.capacity() * sizeof(uint32_t);
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
  state.sum_hi.Mutable(gid) = sum;
  state.sum_lo.Mutable(gid) = 0.0;
  state.cnt.Mutable(gid) = cnt;
}

namespace {
// Output order within a combo: sum DESC, name, tag, plabel (NULL name / plabel
// last). The sums come from `sa`/`sb`, the strings from `s`.
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
  Collect(state, combo, lb, out);
}

bool SecEdgarQ6UsesAvx2() { return UseAvx2(); }
void SecEdgarQ6ForceScalar(bool force) {
  g_force_scalar.store(force, std::memory_order_relaxed);
}

namespace {

// Builds the cache of one combo from its active group ids (exact selection).
std::shared_ptr<const SecEdgarQ6TopCache>
MakeTop(const SecEdgarQ6State &state, std::vector<uint32_t> ids) {
  auto top = std::make_shared<SecEdgarQ6TopCache>();
  const auto before = [&](uint32_t a, uint32_t b) {
    return SecEdgarQ6GroupBefore(state, a, b);
  };
  if (ids.size() <= kTopTarget) {
    top->lower_bound = -std::numeric_limits<double>::infinity();
    top->complete = true;
    top->gids = std::move(ids);
  } else {
    std::vector<double> sums;
    sums.reserve(ids.size());
    for (const uint32_t g : ids) {
      sums.push_back(state.Sum(g));
    }
    std::nth_element(sums.begin(), sums.begin() + static_cast<ptrdiff_t>(kTopTarget),
                     sums.end(), [](double a, double b) { return CmpSum(a, b) > 0; });
    const double lb = sums[kTopTarget];
    top->lower_bound = lb;
    top->complete = false;
    for (const uint32_t g : ids) {
      if (AtLeast(state.Sum(g), lb)) {
        top->gids.push_back(g);
      }
    }
  }
  std::sort(top->gids.begin(), top->gids.end(), before);
  top->base_size = static_cast<uint32_t>(top->gids.size());
  return top;
}

} // namespace

void SecEdgarQ6RescanCombo(SecEdgarQ6State &state, uint32_t combo) {
  SyncTops(state);
  std::vector<uint32_t> ids;
  Collect(state, combo, -std::numeric_limits<double>::infinity(), ids);
  state.tops[combo] = MakeTop(state, std::move(ids));
}

void SecEdgarQ6Rescan(SecEdgarQ6State &state) {
  state.active_groups = CountActive(state);
  SyncTops(state);
  const auto &s = state.structure;
  std::vector<std::vector<uint32_t>> buckets(s.ComboCount());
  const uint32_t groups = s.GroupCount();
  for (uint32_t c = 0; c < state.cnt.NumChunks(); ++c) {
    const uint32_t base = c << kQ6ChunkShift;
    if (base >= groups) {
      break;
    }
    const auto *cnt = state.cnt.ChunkAt(c);
    const auto *cmb = s.group_combo.ChunkAt(c);
    if (!cnt || !cmb) {
      continue;
    }
    const uint32_t n = std::min<uint32_t>(kQ6ChunkSize, groups - base);
    for (uint32_t i = 0; i < n; ++i) {
      if ((*cnt)[i] != 0) {
        buckets[(*cmb)[i]].push_back(base + i);
      }
    }
  }
  for (uint32_t c = 0; c < buckets.size(); ++c) {
    state.tops[c] = MakeTop(state, std::move(buckets[c]));
  }
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------
SecEdgarQ6Hit SecEdgarQ6Resolve(const SecEdgarQ6Structure &s, std::string_view adsh,
                                std::string_view tag, std::string_view version) {
  const uint32_t adsh_id = s.adshs.Find(adsh);
  if (adsh_id == 0 || s.adsh_name.Get(adsh_id) == SecEdgarQ6Structure::kNotCohort) {
    return {};
  }
  const SecEdgarQ6TagVersionKey key(tag, version);
  const uint32_t tv_id = s.tvs.Find(key.View());
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

void SecEdgarQ6ResolveBatch(SecEdgarQ6Structure &s, const SecEdgarQ6NumRef *refs,
                            size_t n, std::vector<SecEdgarQ6GroupDelta> &out) {
  for (size_t i = 0; i < n; ++i) {
    const auto &r = refs[i];
    const SecEdgarQ6Hit hit = SecEdgarQ6Resolve(s, r.adsh, r.tag, r.version);
    if (!hit.Found()) {
      continue;
    }
    const uint32_t uom_id = s.uoms.Intern(r.uom);
    SecEdgarQ6ExpandHit(s, hit, uom_id,
                        SecEdgarQ6Sum{static_cast<double>(r.sign) * r.value, 0.0},
                        r.sign, out);
  }
}

// ---------------------------------------------------------------------------
// Delta application
// ---------------------------------------------------------------------------
namespace {

// Applies `deltas` (any gids of next's structure) onto `next`, a copy of
// `old_state` whose structure may have gained groups / combos.
bool ApplyInto(const SecEdgarQ6State &old_state, SecEdgarQ6State &next,
               std::vector<SecEdgarQ6GroupDelta> &deltas, bool *overflow) {
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

  // Per touched combo: a private copy of its cache and its touched count.
  struct Work {
    std::shared_ptr<SecEdgarQ6TopCache> top;
    size_t touched = 0;
    bool rescan = false;
  };
  std::map<uint32_t, Work> work;
  for (const auto &d : deltas) {
    const uint32_t combo = structure.group_combo.Get(d.gid);
    auto it = work.find(combo);
    if (it == work.end()) {
      it = work.emplace(combo, Work{}).first;
      it->second.top = std::make_shared<SecEdgarQ6TopCache>(*next.tops[combo]);
    }
    ++it->second.touched;
  }
  for (auto &[combo, w] : work) {
    w.rescan = w.touched > kIncrementalTouchedMax;
  }

  // Removal phase: erase touched groups from the sorted caches using the OLD
  // values (a cache is ordered by them).
  for (const auto &d : deltas) {
    Work &w = work[structure.group_combo.Get(d.gid)];
    if (w.rescan) {
      continue;
    }
    auto &gids = w.top->gids;
    if (old_state.Count(d.gid) != 0 && AtLeast(old_state.Sum(d.gid), w.top->lower_bound)) {
      const auto it = std::lower_bound(
          gids.begin(), gids.end(), d.gid, [&](uint32_t x, uint32_t y) {
            return GroupBeforeWith(structure, old_state.Sum(x), old_state.Sum(y), x, y);
          });
      if (it == gids.end() || *it != d.gid) {
        w.rescan = true; // invariant violated: be safe
        continue;
      }
      gids.erase(it);
    }
  }

  // Apply phase: each touched chunk is cloned once (on first write).
  for (const auto &d : deltas) {
    double &hi = next.sum_hi.Mutable(d.gid);
    double &lo = next.sum_lo.Mutable(d.gid);
    int64_t &cnt = next.cnt.Mutable(d.gid);
    const bool was_active = cnt != 0;
    const bool was_finite = std::isfinite(hi);
    cnt += d.cnt;
    if (cnt == 0) {
      hi = lo = 0.0;
    } else {
      const SecEdgarQ6Sum r = SecEdgarQ6SumAdd({hi, lo}, d.sum);
      hi = r.hi;
      lo = r.lo;
      if (overflow && was_finite && std::isfinite(d.sum.hi) && !std::isfinite(hi)) {
        *overflow = true;
      }
    }
    next.active_groups += static_cast<uint64_t>(cnt != 0) -
                          static_cast<uint64_t>(was_active);
  }

  for (const auto &d : deltas) {
    Work &w = work[structure.group_combo.Get(d.gid)];
    if (w.rescan) {
      continue;
    }
    if (next.Count(d.gid) != 0 && AtLeast(next.Sum(d.gid), w.top->lower_bound)) {
      auto &gids = w.top->gids;
      const auto it = std::lower_bound(
          gids.begin(), gids.end(), d.gid, [&](uint32_t x, uint32_t y) {
            return GroupBeforeWith(structure, next.Sum(x), next.Sum(y), x, y);
          });
      gids.insert(it, d.gid);
    }
  }
  for (auto &[combo, w] : work) {
    if (!w.rescan) {
      const auto &t = *w.top;
      if ((!t.complete && t.gids.size() < kQ6OutputLimit) ||
          (t.gids.size() > kTopFloorMax && t.gids.size() > 2ull * t.base_size)) {
        w.rescan = true;
      }
    }
    if (w.rescan) {
      SecEdgarQ6RescanCombo(next, combo);
    } else {
      next.tops[combo] = std::move(w.top);
    }
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
  if (!ApplyInto(old_state, *next, deltas, overflow)) {
    return nullptr;
  }
  return next;
}

std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyNums(const SecEdgarQ6State &old_state, const SecEdgarQ6NumRef *refs,
                    size_t n, bool *overflow) {
  auto next = std::make_shared<SecEdgarQ6State>(old_state);
  std::vector<SecEdgarQ6GroupDelta> deltas;
  deltas.reserve(n);
  SecEdgarQ6ResolveBatch(next->structure, refs, n, deltas);
  if (TooManyGroups(next->structure) && overflow) {
    *overflow = true;
  }
  if (!ApplyInto(old_state, *next, deltas, overflow)) {
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
  const SecEdgarQ6State *src = &state;
  SecEdgarQ6State scratch;
  if (combo >= state.tops.size()) {
    return {};
  }
  if (!state.TopUsable(combo)) {
    scratch = state;
    SecEdgarQ6RescanCombo(scratch, combo);
    src = &scratch;
  }
  const auto &s = src->structure;
  const auto &top = src->Top(combo);
  const size_t n = std::min(limit, top.gids.size());
  std::vector<SecEdgarQ6OutputRow> rows;
  rows.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t gid = top.gids[i];
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
    row.total_value = src->Sum(gid);
    row.cnt = src->Count(gid);
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
    const uint32_t old_id = os.adshs.Find(edit.adsh);
    if (old_id != 0 && os.adsh_name.Get(old_id) != SecEdgarQ6Structure::kNotCohort) {
      const uint32_t old_name = os.adsh_name.Get(old_id);
      const int32_t old_fy = static_cast<int32_t>(os.adsh_fy.Get(old_id));
      for (const auto &n : edit.old_nums) {
        const uint32_t tv =
            os.tvs.Find(SecEdgarQ6TagVersionKey(n.tag, n.version).View());
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
    uint32_t id = s.adshs.Find(edit.adsh);
    if (!edit.new_cohort) {
      if (id != 0) {
        s.adsh_name.Set(id, SecEdgarQ6Structure::kNotCohort);
        s.runs.Erase(id);
      }
      continue;
    }
    if (id == 0) {
      id = s.adshs.Intern(edit.adsh);
    }
    const uint32_t name_id = edit.name_is_null ? 0 : s.names.Intern(edit.name);
    s.adsh_name.Set(id, name_id);
    s.adsh_fy.Set(id, static_cast<uint32_t>(edit.fy));

    std::vector<Entry> es;
    for (const auto &pe : edit.pre) {
      const uint32_t tag_id = s.tags.Intern(pe.tag);
      const SecEdgarQ6TagVersionKey key(pe.tag, pe.version);
      const uint32_t before = s.tvs.Count();
      const uint32_t tv = s.tvs.Intern(key.View());
      if (tv > before) {
        s.tv_tag.Set(tv, tag_id);
      }
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
      const uint32_t tv =
          s.tvs.Find(SecEdgarQ6TagVersionKey(n.tag, n.version).View());
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
  if (!ApplyInto(old_state, *base, deltas, overflow)) {
    return base; // groups / combos may have been added: still a valid state
  }
  return base;
}

} // namespace accelerator_codegen::aux_state::query
