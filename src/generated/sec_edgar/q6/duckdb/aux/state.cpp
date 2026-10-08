#include "sec_edgar/q6/duckdb/aux/state.hpp"

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
constexpr size_t kTopMinAccept = 320;  // sampled rescan acceptance window
constexpr size_t kTopMaxAccept = 2048;
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
// Collect ids of active groups with !(sum < lb)
// ---------------------------------------------------------------------------
void CollectScalar(const SecEdgarQ6State &state, double lb,
                   std::vector<uint32_t> &out) {
  const uint32_t groups = state.structure.GroupCount();
  for (uint32_t c = 0; c < state.cnt.NumChunks(); ++c) {
    const uint32_t base = c << kQ6ChunkShift;
    if (base >= groups) {
      break;
    }
    const auto *cnt = state.cnt.ChunkAt(c);
    const auto *sum = state.sum_hi.ChunkAt(c);
    if (!cnt || !sum) {
      continue;
    }
    const uint32_t n = std::min<uint32_t>(kQ6ChunkSize, groups - base);
    for (uint32_t i = 0; i < n; ++i) {
      if ((*cnt)[i] != 0 && AtLeast((*sum)[i], lb)) {
        out.push_back(base + i);
      }
    }
  }
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) void CollectAvx2(const SecEdgarQ6State &state,
                                                  double lb,
                                                  std::vector<uint32_t> &out) {
  const uint32_t groups = state.structure.GroupCount();
  const __m256d lbv = _mm256_set1_pd(lb);
  const __m256i zero = _mm256_setzero_si256();
  for (uint32_t c = 0; c < state.cnt.NumChunks(); ++c) {
    const uint32_t base = c << kQ6ChunkShift;
    if (base >= groups) {
      break;
    }
    const auto *cnt_chunk = state.cnt.ChunkAt(c);
    const auto *sum_chunk = state.sum_hi.ChunkAt(c);
    if (!cnt_chunk || !sum_chunk) {
      continue;
    }
    const double *sum = sum_chunk->data();
    const int64_t *cnt = cnt_chunk->data();
    const uint32_t n = std::min<uint32_t>(kQ6ChunkSize, groups - base);
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
      const __m256d s = _mm256_loadu_pd(sum + i);
      // _CMP_NLT_UQ: !(s < lb), true for NaN
      const int keep_sum =
          _mm256_movemask_pd(_mm256_cmp_pd(s, lbv, _CMP_NLT_UQ));
      const __m256i cn =
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(cnt + i));
      const int inactive = _mm256_movemask_pd(
          _mm256_castsi256_pd(_mm256_cmpeq_epi64(cn, zero)));
      int mask = keep_sum & ~inactive & 0xF;
      while (mask) {
        const int j = __builtin_ctz(static_cast<unsigned>(mask));
        out.push_back(base + i + static_cast<uint32_t>(j));
        mask &= mask - 1;
      }
    }
    for (; i < n; ++i) {
      if (cnt[i] != 0 && AtLeast(sum[i], lb)) {
        out.push_back(base + i);
      }
    }
  }
}
#endif

void Collect(const SecEdgarQ6State &state, double lb,
             std::vector<uint32_t> &out) {
#ifdef ACCEL_Q6_X86
  if (UseAvx2()) {
    CollectAvx2(state, lb, out);
    return;
  }
#endif
  CollectScalar(state, lb, out);
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

inline uint64_t GroupHash(uint32_t n, uint32_t t, uint32_t p) noexcept {
  uint64_t h = (static_cast<uint64_t>(n) + 1) * 0x9E3779B97F4A7C15ULL ^
               (static_cast<uint64_t>(t) + 0x7F4A7C15ULL) * 0xC2B2AE3D27D4EB4FULL ^
               (static_cast<uint64_t>(p) + 1) * 0x165667B19E3779F9ULL;
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ULL;
  h ^= h >> 32;
  return h;
}

// value * mult as an exact double-double (mult is a small integer).
inline SecEdgarQ6Sum MulExact(double v, double m) noexcept {
  const double p = v * m;
  if (!std::isfinite(p)) {
    return {p, 0.0};
  }
  return {p, std::fma(v, m, -p)};
}

// Expands a payload against the trailer of its run (words after the entries).
// `sum`/`cnt` are already signed.
void ExpandGeneric(const char *trailer, uint32_t payload, SecEdgarQ6Sum sum,
                   int64_t cnt, std::vector<SecEdgarQ6GroupDelta> &out) {
  if (payload == SecEdgarQ6Structure::kNoPayload) {
    return;
  }
  if ((payload & SecEdgarQ6Structure::kMultiFlag) == 0) {
    out.push_back({payload, sum, cnt});
    return;
  }
  const char *block = trailer + 4ull * (payload & ~SecEdgarQ6Structure::kMultiFlag);
  const uint32_t k = Load32(block);
  for (uint32_t i = 0; i < k; ++i) {
    const uint32_t gid = Load32(block + 4ull * (1 + 2 * i));
    const uint32_t mult = Load32(block + 4ull * (2 + 2 * i));
    const SecEdgarQ6Sum hi_part = MulExact(sum.hi, static_cast<double>(mult));
    out.push_back({gid,
                   SecEdgarQ6SumAdd(hi_part, {sum.lo * static_cast<double>(mult), 0.0}),
                   cnt * static_cast<int64_t>(mult)});
  }
}

inline const char *TrailerOf(std::string_view run) noexcept {
  return run.data() + 4 + 8ull * Load32(run.data());
}

} // namespace

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
SecEdgarQ6Hit SecEdgarQ6Structure::Lookup(uint32_t adsh_id,
                                          uint32_t tv_id) const {
  std::string_view run;
  if (!runs.Get(adsh_id, run) || run.size() < 4) {
    return {};
  }
  const size_t n = Load32(run.data());
  const char *tv = run.data() + 4;
  const size_t idx = FindTv(tv, n, tv_id);
  if (idx == n) {
    return {};
  }
  return {run, Load32(tv + 4 * n + 4 * idx)};
}

uint32_t SecEdgarQ6Structure::FindGroup(uint32_t name_id, uint32_t tag_id,
                                        uint32_t plabel_id) const {
  for (uint64_t key = GroupHash(name_id, tag_id, plabel_id);; ++key) {
    const uint32_t *gid = group_index.Find(static_cast<int64_t>(key));
    if (!gid) {
      return kNoPayload;
    }
    const SecEdgarQ6Group g = groups.Get(*gid);
    if (g.name == name_id && g.tag == tag_id && g.plabel == plabel_id) {
      return *gid;
    }
  }
}

uint32_t SecEdgarQ6Structure::FindOrAddGroup(uint32_t name_id, uint32_t tag_id,
                                             uint32_t plabel_id) {
  uint64_t key = GroupHash(name_id, tag_id, plabel_id);
  for (;; ++key) {
    const uint32_t *gid = group_index.Find(static_cast<int64_t>(key));
    if (!gid) {
      break;
    }
    const SecEdgarQ6Group g = groups.Get(*gid);
    if (g.name == name_id && g.tag == tag_id && g.plabel == plabel_id) {
      return *gid;
    }
  }
  if (group_count >= kMaxGroups) {
    throw std::length_error("SEC EDGAR Q6 group id space exhausted");
  }
  const uint32_t gid = group_count++;
  groups.Mutable(gid) = {name_id, tag_id, plabel_id};
  group_index.Upsert(static_cast<int64_t>(key), gid);
  return gid;
}

size_t SecEdgarQ6Structure::Bytes() const {
  return adshs.Bytes() + names.Bytes() + tags.Bytes() + tvs.Bytes() +
         plabels.Bytes() + adsh_name.MemoryBytes() + tv_tag.MemoryBytes() +
         runs.MemoryBytes() + groups.MemoryBytes() + group_index.memory_bytes() +
         sizeof(*this);
}

void SecEdgarQ6StructureBuilder::AddCohort(std::string_view adsh,
                                           std::string_view name,
                                           bool name_is_null) {
  const uint32_t before = s_.adshs.Count();
  const uint32_t id = s_.adshs.Intern(adsh);
  if (id <= before) {
    return; // duplicate adsh: keep the first
  }
  s_.adsh_name.Set(id, name_is_null ? 0 : s_.names.Intern(name));
}

void SecEdgarQ6StructureBuilder::AddPre(std::string_view adsh,
                                        std::string_view tag,
                                        std::string_view version,
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
  const uint32_t plabel_id = plabel_is_null ? 0 : s_.plabels.Intern(plabel);
  entries_.push_back({adsh_id, tv_id, plabel_id});
}

SecEdgarQ6Structure SecEdgarQ6StructureBuilder::Finish() {
  auto &s = s_;
  std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
    if (a.adsh != b.adsh) return a.adsh < b.adsh;
    if (a.tv != b.tv) return a.tv < b.tv;
    return a.plabel < b.plabel;
  });

  // One run per adsh: entries sorted by tv; a (adsh, tv) with one label and
  // multiplicity 1 stores the group id, anything else a trailer block.
  std::string record;
  std::vector<uint32_t> tvs, pays, trailer;
  size_t i = 0;
  while (i < entries_.size()) {
    const uint32_t adsh = entries_[i].adsh;
    const uint32_t name_id = s.adsh_name.Get(adsh);
    tvs.clear();
    pays.clear();
    trailer.clear();
    while (i < entries_.size() && entries_[i].adsh == adsh) {
      const uint32_t tv = entries_[i].tv;
      // labels of this (adsh, tv) with multiplicities
      size_t j = i;
      size_t distinct = 0;
      uint32_t first_gid = 0, first_mult = 0;
      const size_t trailer_start = trailer.size();
      trailer.push_back(0); // k placeholder (dropped for single payloads)
      while (j < entries_.size() && entries_[j].adsh == adsh &&
             entries_[j].tv == tv) {
        size_t m = j;
        while (m < entries_.size() && entries_[m].adsh == adsh &&
               entries_[m].tv == tv && entries_[m].plabel == entries_[j].plabel) {
          ++m;
        }
        const uint32_t gid = s.FindOrAddGroup(
            name_id, s.tv_tag.Get(tv), entries_[j].plabel);
        const uint32_t mult = static_cast<uint32_t>(m - j);
        if (distinct == 0) {
          first_gid = gid;
          first_mult = mult;
        }
        trailer.push_back(gid);
        trailer.push_back(mult);
        ++distinct;
        j = m;
      }
      tvs.push_back(tv);
      if (distinct == 1 && first_mult == 1) {
        trailer.resize(trailer_start);
        pays.push_back(first_gid);
      } else {
        trailer[trailer_start] = static_cast<uint32_t>(distinct);
        pays.push_back(SecEdgarQ6Structure::kMultiFlag |
                       static_cast<uint32_t>(trailer_start));
      }
      i = j;
    }
    record.clear();
    Append32(record, static_cast<uint32_t>(tvs.size()));
    record.append(reinterpret_cast<const char *>(tvs.data()), 4 * tvs.size());
    record.append(reinterpret_cast<const char *>(pays.data()), 4 * pays.size());
    record.append(reinterpret_cast<const char *>(trailer.data()), 4 * trailer.size());
    s.runs.Set(adsh, record);
  }
  entries_.clear();
  entries_.shrink_to_fit();
  s.base_group_count = s.group_count;
  s.adshs.Shrink();
  s.names.Shrink();
  s.tags.Shrink();
  s.tvs.Shrink();
  s.plabels.Shrink();
  s.runs.Shrink();
  return std::move(s_);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
size_t SecEdgarQ6State::Bytes() const {
  return sizeof(*this) + structure.Bytes() + sum_hi.MemoryBytes() +
         sum_lo.MemoryBytes() + cnt.MemoryBytes() +
         top.gids.capacity() * sizeof(uint32_t);
}

bool SecEdgarQ6State::TopUsable() const noexcept {
  return top.complete || top.gids.size() >= kQ6OutputLimit;
}

SecEdgarQ6State SecEdgarQ6MakeEmptyState(SecEdgarQ6Structure structure) {
  SecEdgarQ6State state;
  state.structure = std::move(structure);
  state.top.complete = true;
  state.top.lower_bound = -std::numeric_limits<double>::infinity();
  return state;
}

void SecEdgarQ6SetGroup(SecEdgarQ6State &state, uint32_t gid, double sum,
                        int64_t cnt) {
  state.sum_hi.Mutable(gid) = sum;
  state.sum_lo.Mutable(gid) = 0.0;
  state.cnt.Mutable(gid) = cnt;
}

namespace {
// Output order: sum DESC, name, tag, plabel (NULL name / plabel last). The
// sums come from `sa`/`sb`, the strings from `s`.
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

void SecEdgarQ6CollectAtLeast(const SecEdgarQ6State &state, double lb,
                              std::vector<uint32_t> &out) {
  Collect(state, lb, out);
}

bool SecEdgarQ6UsesAvx2() { return UseAvx2(); }
void SecEdgarQ6ForceScalar(bool force) {
  g_force_scalar.store(force, std::memory_order_relaxed);
}

namespace {

// Largest-first rank selection over the sums of active groups.
double SelectThreshold(const std::vector<double> &sums_in, size_t rank_from_top) {
  std::vector<double> sums = sums_in;
  const size_t idx = std::min(rank_from_top, sums.size() - 1);
  std::nth_element(sums.begin(), sums.begin() + static_cast<ptrdiff_t>(idx),
                   sums.end(), [](double a, double b) { return CmpSum(a, b) > 0; });
  return sums[idx];
}

} // namespace

void SecEdgarQ6Rescan(SecEdgarQ6State &state) {
  const uint32_t groups = state.structure.GroupCount();
  state.active_groups = CountActive(state);
  auto &top = state.top;
  top.gids.clear();
  const auto before = [&](uint32_t a, uint32_t b) {
    return SecEdgarQ6GroupBefore(state, a, b);
  };
  if (state.active_groups <= kTopTarget) {
    top.lower_bound = -std::numeric_limits<double>::infinity();
    top.complete = true;
    Collect(state, top.lower_bound, top.gids);
    std::sort(top.gids.begin(), top.gids.end(), before);
    return;
  }

  // Sample a quantile estimate, then verify with one SIMD pass. Fall back to an
  // exact selection over all active sums if the sampled bound keeps missing.
  double lb = 0.0;
  bool done = false;
  size_t target = kTopTarget;
  const size_t stride = std::max<size_t>(1, groups / 4096);
  for (int attempt = 0; attempt < 3 && !done; ++attempt) {
    std::vector<double> sample;
    sample.reserve(groups / stride + 1);
    for (uint32_t g = 0; g < groups; g += static_cast<uint32_t>(stride)) {
      if (state.Count(g) != 0) {
        sample.push_back(state.Sum(g));
      }
    }
    if (sample.empty()) {
      break;
    }
    const size_t rank = std::min(
        sample.size() - 1,
        static_cast<size_t>(static_cast<double>(sample.size()) *
                            static_cast<double>(target) /
                            static_cast<double>(state.active_groups)));
    lb = SelectThreshold(sample, rank);
    top.gids.clear();
    Collect(state, lb, top.gids);
    if (top.gids.size() >= kTopMinAccept && top.gids.size() <= kTopMaxAccept) {
      done = true;
    } else if (top.gids.size() < kTopMinAccept) {
      target *= 2;
    } else {
      target = std::max<size_t>(kQ6OutputLimit + 56, target / 2);
    }
  }
  if (!done) {
    std::vector<double> sums;
    sums.reserve(state.active_groups);
    for (uint32_t g = 0; g < groups; ++g) {
      if (state.Count(g) != 0) {
        sums.push_back(state.Sum(g));
      }
    }
    lb = SelectThreshold(sums, kTopTarget);
    top.gids.clear();
    Collect(state, lb, top.gids);
  }
  top.lower_bound = lb;
  top.complete = false;
  std::sort(top.gids.begin(), top.gids.end(), before);
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------
SecEdgarQ6Hit SecEdgarQ6Resolve(const SecEdgarQ6Structure &s, std::string_view adsh,
                                std::string_view tag, std::string_view version) {
  const uint32_t adsh_id = s.adshs.Find(adsh);
  if (adsh_id == 0) {
    return {};
  }
  const SecEdgarQ6TagVersionKey key(tag, version);
  const uint32_t tv_id = s.tvs.Find(key.View());
  if (tv_id == 0) {
    return {};
  }
  return s.Lookup(adsh_id, tv_id);
}

void SecEdgarQ6ExpandPayload(const SecEdgarQ6Hit &hit, double value, int64_t sign,
                             std::vector<SecEdgarQ6GroupDelta> &out) {
  if (!hit.Found()) {
    return;
  }
  ExpandGeneric(TrailerOf(hit.run), hit.payload,
                SecEdgarQ6Sum{static_cast<double>(sign) * value, 0.0}, sign, out);
}

void SecEdgarQ6ResolveBatch(const SecEdgarQ6Structure &s,
                            const SecEdgarQ6NumRef *refs, size_t n,
                            std::vector<SecEdgarQ6GroupDelta> &out) {
  for (size_t i = 0; i < n; ++i) {
    const auto &r = refs[i];
    SecEdgarQ6ExpandPayload(SecEdgarQ6Resolve(s, r.adsh, r.tag, r.version),
                            r.value, r.sign, out);
  }
}

// ---------------------------------------------------------------------------
// Delta application
// ---------------------------------------------------------------------------
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyDeltas(const SecEdgarQ6State &old_state,
                      std::vector<SecEdgarQ6GroupDelta> &deltas,
                      bool *overflow) {
  if (deltas.empty()) {
    return nullptr;
  }
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
    return nullptr;
  }

  auto next = std::make_shared<SecEdgarQ6State>(old_state); // shares chunks
  const SecEdgarQ6Structure &structure = next->structure;

  bool rescan = deltas.size() > kIncrementalTouchedMax;
  auto &gids = next->top.gids;
  const double lb = next->top.lower_bound;

  // Removal phase: erase touched groups from the sorted cache using the OLD
  // values (the cache is ordered by them).
  if (!rescan) {
    for (const auto &d : deltas) {
      if (old_state.Count(d.gid) != 0 && AtLeast(old_state.Sum(d.gid), lb)) {
        const auto it = std::lower_bound(
            gids.begin(), gids.end(), d.gid, [&](uint32_t x, uint32_t y) {
              return GroupBeforeWith(structure, old_state.Sum(x),
                                     old_state.Sum(y), x, y);
            });
        if (it == gids.end() || *it != d.gid) {
          rescan = true; // invariant violated: be safe
          break;
        }
        gids.erase(it);
      }
    }
  }

  // Apply phase: each touched chunk is cloned once (on first write).
  for (const auto &d : deltas) {
    double &hi = next->sum_hi.Mutable(d.gid);
    double &lo = next->sum_lo.Mutable(d.gid);
    int64_t &cnt = next->cnt.Mutable(d.gid);
    const bool was_active = cnt != 0;
    const bool was_finite = std::isfinite(hi);
    cnt += d.cnt;
    if (cnt == 0) {
      hi = lo = 0.0;
    } else {
      const SecEdgarQ6Sum r = SecEdgarQ6SumAdd({hi, lo}, d.sum);
      hi = r.hi;
      lo = r.lo;
      if (overflow && was_finite && std::isfinite(d.sum.hi) &&
          !std::isfinite(hi)) {
        *overflow = true;
      }
    }
    next->active_groups += static_cast<uint64_t>(cnt != 0) -
                           static_cast<uint64_t>(was_active);
  }

  if (!rescan) {
    for (const auto &d : deltas) {
      if (next->Count(d.gid) != 0 && AtLeast(next->Sum(d.gid), lb)) {
        const auto it = std::lower_bound(
            gids.begin(), gids.end(), d.gid, [&](uint32_t x, uint32_t y) {
              return GroupBeforeWith(structure, next->Sum(x), next->Sum(y), x, y);
            });
        gids.insert(it, d.gid);
      }
    }
    if ((!next->top.complete && gids.size() < kQ6OutputLimit) ||
        gids.size() > kTopFloorMax) {
      rescan = true;
    }
  }
  if (rescan) {
    SecEdgarQ6Rescan(*next);
  }
  return next;
}

std::vector<SecEdgarQ6OutputRow> SecEdgarQ6TopRows(const SecEdgarQ6State &state,
                                                   size_t limit) {
  const SecEdgarQ6State *src = &state;
  SecEdgarQ6State scratch;
  if (!state.TopUsable()) {
    scratch = state;
    SecEdgarQ6Rescan(scratch);
    src = &scratch;
  }
  const auto &s = src->structure;
  const size_t n = std::min(limit, src->top.gids.size());
  std::vector<SecEdgarQ6OutputRow> rows;
  rows.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t gid = src->top.gids[i];
    const SecEdgarQ6Group g = s.groups.Get(gid);
    SecEdgarQ6OutputRow row;
    if (g.name == 0) {
      row.name_is_null = true;
    } else {
      row.name = std::string(s.names.View(g.name));
    }
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
                     std::vector<SecEdgarQ6GroupDelta> &extra, bool *overflow) {
  const SecEdgarQ6Structure &os = old_state.structure;
  // A copy shares every chunk / shard with old_state; the edits below clone
  // only what they touch.
  auto base = std::make_shared<SecEdgarQ6State>(old_state);
  SecEdgarQ6Structure &s = base->structure;
  std::vector<SecEdgarQ6GroupDelta> deltas = std::move(extra);

  struct Entry {
    uint32_t tv, gid, mult;
  };
  std::string record;

  for (const auto &edit : edits) {
    // Old contributions (old structure, pre-transaction sums).
    const uint32_t old_id = os.adshs.Find(edit.adsh);
    if (old_id != 0 && os.adsh_name.Get(old_id) != SecEdgarQ6Structure::kNotCohort) {
      for (const auto &n : edit.old_nums) {
        const uint32_t tv =
            os.tvs.Find(SecEdgarQ6TagVersionKey(n.tag, n.version).View());
        if (tv == 0) continue;
        const SecEdgarQ6Hit hit = os.Lookup(old_id, tv);
        if (!hit.Found()) continue;
        ExpandGeneric(TrailerOf(hit.run), hit.payload, SecEdgarQ6Sum{-n.sum, -n.sum_lo},
                      -n.cnt, deltas);
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

    std::vector<Entry> es;
    for (const auto &pe : edit.pre) {
      const uint32_t tag_id = s.tags.Intern(pe.tag);
      const SecEdgarQ6TagVersionKey key(pe.tag, pe.version);
      const uint32_t before = s.tvs.Count();
      const uint32_t tv = s.tvs.Intern(key.View());
      if (tv > before) {
        s.tv_tag.Set(tv, tag_id);
      }
      const uint32_t plabel_id = pe.plabel_is_null ? 0 : s.plabels.Intern(pe.plabel);
      es.push_back({tv, s.FindOrAddGroup(name_id, tag_id, plabel_id), pe.mult});
    }
    std::sort(es.begin(), es.end(), [](const Entry &a, const Entry &b) {
      return a.tv != b.tv ? a.tv < b.tv : a.gid < b.gid;
    });
    std::vector<uint32_t> tvs, pays, trailer;
    for (size_t i = 0; i < es.size();) {
      size_t j = i;
      std::vector<std::pair<uint32_t, uint32_t>> gm; // (gid, mult)
      while (j < es.size() && es[j].tv == es[i].tv) {
        if (!gm.empty() && gm.back().first == es[j].gid) {
          gm.back().second += es[j].mult;
        } else {
          gm.emplace_back(es[j].gid, es[j].mult);
        }
        ++j;
      }
      uint32_t payload;
      if (gm.size() == 1 && gm[0].second == 1) {
        payload = gm[0].first;
      } else {
        payload = SecEdgarQ6Structure::kMultiFlag | static_cast<uint32_t>(trailer.size());
        trailer.push_back(static_cast<uint32_t>(gm.size()));
        for (const auto &[g, m] : gm) {
          trailer.push_back(g);
          trailer.push_back(m);
        }
      }
      tvs.push_back(es[i].tv);
      pays.push_back(payload);
      i = j;
    }
    // New contributions from the run being installed.
    for (const auto &n : edit.new_nums) {
      const uint32_t tv =
          s.tvs.Find(SecEdgarQ6TagVersionKey(n.tag, n.version).View());
      if (tv == 0) continue;
      const auto it = std::lower_bound(tvs.begin(), tvs.end(), tv);
      if (it == tvs.end() || *it != tv) continue;
      const uint32_t payload = pays[static_cast<size_t>(it - tvs.begin())];
      ExpandGeneric(reinterpret_cast<const char *>(trailer.data()), payload,
                    SecEdgarQ6Sum{n.sum, n.sum_lo}, n.cnt, deltas);
    }
    if (tvs.empty()) {
      s.runs.Erase(id);
    } else {
      record.clear();
      Append32(record, static_cast<uint32_t>(tvs.size()));
      record.append(reinterpret_cast<const char *>(tvs.data()), 4 * tvs.size());
      record.append(reinterpret_cast<const char *>(pays.data()), 4 * pays.size());
      record.append(reinterpret_cast<const char *>(trailer.data()), 4 * trailer.size());
      s.runs.Set(id, record);
    }
  }

  if (s.group_count > s.base_group_count + s.base_group_count / 2 + 4096) {
    return nullptr; // too many unused groups accumulated: let the caller rebuild
  }
  auto next = SecEdgarQ6ApplyDeltas(*base, deltas, overflow);
  return next ? next : std::shared_ptr<const SecEdgarQ6State>(base);
}

} // namespace accelerator_codegen::aux_state::query
