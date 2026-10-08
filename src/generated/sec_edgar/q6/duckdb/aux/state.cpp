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
// tv-run search
// ---------------------------------------------------------------------------
size_t FindTvScalar(const uint32_t *a, size_t n, uint32_t key) noexcept {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = lo + ((hi - lo) >> 1);
    if (a[mid] < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return (lo < n && a[lo] == key) ? lo : n;
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) size_t FindTvAvx2(const uint32_t *a, size_t n,
                                                   uint32_t key) noexcept {
  const __m256i k = _mm256_set1_epi32(static_cast<int>(key));
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256i v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i));
    const int m = _mm256_movemask_ps(
        _mm256_castsi256_ps(_mm256_cmpeq_epi32(v, k)));
    if (m) {
      return i + static_cast<size_t>(__builtin_ctz(static_cast<unsigned>(m)));
    }
  }
  for (; i < n; ++i) {
    if (a[i] == key) {
      return i;
    }
  }
  return n;
}
#endif

inline size_t FindTv(const uint32_t *a, size_t n, uint32_t key) noexcept {
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
  const uint32_t groups = state.structure->GroupCount();
  for (uint32_t c = 0; c < state.chunks.size(); ++c) {
    const auto &chunk = *state.chunks[c];
    const uint32_t base = c << kQ6ChunkShift;
    const uint32_t n = std::min<uint32_t>(kQ6ChunkSize, groups - base);
    for (uint32_t i = 0; i < n; ++i) {
      if (chunk.cnt[i] != 0 && AtLeast(chunk.sum[i], lb)) {
        out.push_back(base + i);
      }
    }
  }
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) void CollectAvx2(const SecEdgarQ6State &state,
                                                  double lb,
                                                  std::vector<uint32_t> &out) {
  const uint32_t groups = state.structure->GroupCount();
  const __m256d lbv = _mm256_set1_pd(lb);
  const __m256i zero = _mm256_setzero_si256();
  for (uint32_t c = 0; c < state.chunks.size(); ++c) {
    const auto &chunk = *state.chunks[c];
    const uint32_t base = c << kQ6ChunkShift;
    const uint32_t n = std::min<uint32_t>(kQ6ChunkSize, groups - base);
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
      const __m256d s = _mm256_loadu_pd(chunk.sum + i);
      // _CMP_NLT_UQ: !(s < lb), true for NaN
      const int keep_sum =
          _mm256_movemask_pd(_mm256_cmp_pd(s, lbv, _CMP_NLT_UQ));
      const __m256i cnt =
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(chunk.cnt + i));
      const int inactive = _mm256_movemask_pd(
          _mm256_castsi256_pd(_mm256_cmpeq_epi64(cnt, zero)));
      int mask = keep_sum & ~inactive & 0xF;
      while (mask) {
        const int j = __builtin_ctz(static_cast<unsigned>(mask));
        out.push_back(base + i + static_cast<uint32_t>(j));
        mask &= mask - 1;
      }
    }
    for (; i < n; ++i) {
      if (chunk.cnt[i] != 0 && AtLeast(chunk.sum[i], lb)) {
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
  for (const auto &chunk : state.chunks) {
    for (uint32_t i = 0; i < kQ6ChunkSize; ++i) {
      active += chunk->cnt[i] != 0;
    }
  }
  return active;
}

#ifdef ACCEL_Q6_X86
__attribute__((target("avx2"))) uint64_t CountActiveAvx2(
    const SecEdgarQ6State &state) {
  uint64_t inactive = 0;
  const __m256i zero = _mm256_setzero_si256();
  for (const auto &chunk : state.chunks) {
    for (uint32_t i = 0; i < kQ6ChunkSize; i += 4) {
      const __m256i cnt =
          _mm256_load_si256(reinterpret_cast<const __m256i *>(chunk->cnt + i));
      inactive += static_cast<uint64_t>(__builtin_popcount(
          static_cast<unsigned>(_mm256_movemask_pd(
              _mm256_castsi256_pd(_mm256_cmpeq_epi64(cnt, zero))))));
    }
  }
  const uint64_t total =
      static_cast<uint64_t>(state.chunks.size()) * kQ6ChunkSize;
  return total - inactive;
}
#endif

uint64_t CountActive(const SecEdgarQ6State &state) {
  // Padding slots past GroupCount() always have cnt == 0.
#ifdef ACCEL_Q6_X86
  if (UseAvx2()) {
    return CountActiveAvx2(state);
  }
#endif
  return CountActiveScalar(state);
}

std::shared_ptr<SecEdgarQ6AggChunk> NewChunk() {
  return std::shared_ptr<SecEdgarQ6AggChunk>(new SecEdgarQ6AggChunk());
}

} // namespace

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
                                          std::string_view s) const noexcept {
  if (table_.empty()) {
    return 0;
  }
  const uint32_t tag = static_cast<uint32_t>(hash >> 32);
  for (uint64_t idx = hash & mask_;; idx = (idx + 1) & mask_) {
    const uint64_t e = table_[idx];
    if (e == 0) {
      return 0;
    }
    if (static_cast<uint32_t>(e >> 32) == tag) {
      const uint32_t id = static_cast<uint32_t>(e);
      if (View(id) == s) {
        return id;
      }
    }
  }
}

void SecEdgarQ6StringPool::Grow() {
  const size_t new_size = table_.empty() ? 64 : table_.size() * 2;
  table_.assign(new_size, 0);
  mask_ = new_size - 1;
  for (uint32_t id = 1; id <= Count(); ++id) {
    const uint64_t h = Hash(View(id));
    uint64_t idx = h & mask_;
    while (table_[idx] != 0) {
      idx = (idx + 1) & mask_;
    }
    table_[idx] = (h & 0xFFFFFFFF00000000ULL) | id;
  }
}

uint32_t SecEdgarQ6StringPool::Intern(std::string_view s) {
  const uint64_t h = Hash(s);
  if (const uint32_t found = FindHashed(h, s)) {
    return found;
  }
  if ((static_cast<size_t>(Count()) + 1) * 2 > table_.size()) {
    Grow();
  }
  if (data_.size() + s.size() > 0xFFFFFFF0ULL) {
    throw std::length_error("SEC EDGAR Q6 string pool overflow");
  }
  data_.insert(data_.end(), s.begin(), s.end());
  offs_.push_back(static_cast<uint32_t>(data_.size()));
  const uint32_t id = Count();
  uint64_t idx = h & mask_;
  while (table_[idx] != 0) {
    idx = (idx + 1) & mask_;
  }
  table_[idx] = (h & 0xFFFFFFFF00000000ULL) | id;
  return id;
}

size_t SecEdgarQ6StringPool::Bytes() const noexcept {
  return data_.capacity() + offs_.capacity() * sizeof(uint32_t) +
         table_.capacity() * sizeof(uint64_t);
}

void SecEdgarQ6StringPool::ShrinkToFit() {
  data_.shrink_to_fit();
  offs_.shrink_to_fit();
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
uint32_t SecEdgarQ6Structure::Lookup(uint32_t adsh_id,
                                     uint32_t tv_id) const noexcept {
  const uint32_t begin = csr_off[adsh_id];
  const uint32_t end = csr_off[adsh_id + 1];
  const size_t n = end - begin;
  const size_t idx = FindTv(csr_tv.data() + begin, n, tv_id);
  return idx == n ? kNoPayload : csr_pay[begin + idx];
}

namespace {
inline uint64_t GroupHash(uint32_t n, uint32_t t, uint32_t p) noexcept {
  uint64_t h = (static_cast<uint64_t>(n) + 1) * 0x9E3779B97F4A7C15ULL ^
               (static_cast<uint64_t>(t) + 0x7F4A7C15ULL) * 0xC2B2AE3D27D4EB4FULL ^
               (static_cast<uint64_t>(p) + 1) * 0x165667B19E3779F9ULL;
  h ^= h >> 32;
  h *= 0xD6E8FEB86659FD93ULL;
  h ^= h >> 32;
  return h;
}
} // namespace

void SecEdgarQ6Structure::RebuildGroupIndex() {
  size_t size = 16;
  while (size < static_cast<size_t>(g_name.size()) * 2 + 2) size <<= 1;
  group_index.assign(size, 0);
  const size_t mask = size - 1;
  for (uint32_t g = 0; g < g_name.size(); ++g) {
    size_t idx = GroupHash(g_name[g], g_tag[g], g_plabel[g]) & mask;
    while (group_index[idx] != 0) idx = (idx + 1) & mask;
    group_index[idx] = g + 1;
  }
}

uint32_t SecEdgarQ6Structure::FindGroup(uint32_t name_id, uint32_t tag_id,
                                        uint32_t plabel_id) const noexcept {
  if (group_index.empty()) return kNoPayload;
  const size_t mask = group_index.size() - 1;
  for (size_t idx = GroupHash(name_id, tag_id, plabel_id) & mask;;
       idx = (idx + 1) & mask) {
    const uint32_t e = group_index[idx];
    if (e == 0) return kNoPayload;
    const uint32_t g = e - 1;
    if (g_name[g] == name_id && g_tag[g] == tag_id && g_plabel[g] == plabel_id) return g;
  }
}

uint32_t SecEdgarQ6Structure::FindOrAddGroup(uint32_t name_id, uint32_t tag_id,
                                             uint32_t plabel_id) {
  if (const uint32_t g = FindGroup(name_id, tag_id, plabel_id); g != kNoPayload) return g;
  if (g_name.size() + 1 >= kMultiFlag) throw std::length_error("SEC EDGAR Q6 too many groups");
  g_name.push_back(name_id);
  g_tag.push_back(tag_id);
  g_plabel.push_back(plabel_id);
  const uint32_t gid = static_cast<uint32_t>(g_name.size() - 1);
  if ((g_name.size() + 1) * 2 > group_index.size()) {
    RebuildGroupIndex();
  } else {
    const size_t mask = group_index.size() - 1;
    size_t idx = GroupHash(name_id, tag_id, plabel_id) & mask;
    while (group_index[idx] != 0) idx = (idx + 1) & mask;
    group_index[idx] = gid + 1;
  }
  return gid;
}

size_t SecEdgarQ6Structure::Bytes() const noexcept {
  size_t total = adshs.Bytes() + names.Bytes() + tags.Bytes() + tvs.Bytes() +
                 plabels.Bytes();
  const auto vec = [](const std::vector<uint32_t> &v) {
    return v.capacity() * sizeof(uint32_t);
  };
  total += vec(adsh_name) + vec(tv_tag) + vec(csr_off) + vec(csr_tv) +
           vec(csr_pay) + vec(multi) + vec(g_name) + vec(g_tag) + vec(g_plabel) + vec(group_index);
  return total + sizeof(*this);
}

void SecEdgarQ6StructureBuilder::AddCohort(std::string_view adsh,
                                           std::string_view name,
                                           bool name_is_null) {
  const uint32_t before = s_->adshs.Count();
  const uint32_t id = s_->adshs.Intern(adsh);
  if (id <= before) {
    return; // duplicate adsh: keep the first
  }
  if (s_->adsh_name.empty()) {
    s_->adsh_name.push_back(0);
  }
  s_->adsh_name.resize(static_cast<size_t>(id) + 1);
  s_->adsh_name[id] = name_is_null ? 0 : s_->names.Intern(name);
}

void SecEdgarQ6StructureBuilder::AddPre(std::string_view adsh,
                                        std::string_view tag,
                                        std::string_view version,
                                        std::string_view plabel,
                                        bool plabel_is_null) {
  const uint32_t adsh_id = s_->adshs.Find(adsh);
  if (adsh_id == 0) {
    return;
  }
  const uint32_t tag_id = s_->tags.Intern(tag);
  const SecEdgarQ6TagVersionKey key(tag, version);
  const uint32_t before = s_->tvs.Count();
  const uint32_t tv_id = s_->tvs.Intern(key.View());
  if (s_->tv_tag.empty()) {
    s_->tv_tag.push_back(0);
  }
  if (tv_id > before) {
    s_->tv_tag.resize(static_cast<size_t>(tv_id) + 1);
    s_->tv_tag[tv_id] = tag_id;
  }
  const uint32_t plabel_id = plabel_is_null ? 0 : s_->plabels.Intern(plabel);
  entries_.push_back({adsh_id, tv_id, plabel_id});
}

std::shared_ptr<const SecEdgarQ6Structure> SecEdgarQ6StructureBuilder::Finish() {
  auto &s = *s_;
  if (s.adsh_name.empty()) {
    s.adsh_name.push_back(0);
  }
  if (s.tv_tag.empty()) {
    s.tv_tag.push_back(0);
  }
  std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
    if (a.adsh != b.adsh) return a.adsh < b.adsh;
    if (a.tv != b.tv) return a.tv < b.tv;
    return a.plabel < b.plabel;
  });

  struct Unique {
    uint32_t adsh, tv, plabel, mult, gid;
  };
  std::vector<Unique> uniq;
  uniq.reserve(entries_.size());
  for (const auto &e : entries_) {
    if (!uniq.empty() && uniq.back().adsh == e.adsh && uniq.back().tv == e.tv &&
        uniq.back().plabel == e.plabel) {
      ++uniq.back().mult;
    } else {
      uniq.push_back({e.adsh, e.tv, e.plabel, 1, 0});
    }
  }
  entries_.clear();
  entries_.shrink_to_fit();

  struct GroupKey {
    uint32_t name, tag, plabel, uidx;
  };
  std::vector<GroupKey> keys(uniq.size());
  for (uint32_t i = 0; i < uniq.size(); ++i) {
    keys[i] = {s.adsh_name[uniq[i].adsh], s.tv_tag[uniq[i].tv], uniq[i].plabel, i};
  }
  std::sort(keys.begin(), keys.end(), [](const GroupKey &a, const GroupKey &b) {
    if (a.name != b.name) return a.name < b.name;
    if (a.tag != b.tag) return a.tag < b.tag;
    return a.plabel < b.plabel;
  });
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i == 0 || keys[i].name != keys[i - 1].name ||
        keys[i].tag != keys[i - 1].tag || keys[i].plabel != keys[i - 1].plabel) {
      s.g_name.push_back(keys[i].name);
      s.g_tag.push_back(keys[i].tag);
      s.g_plabel.push_back(keys[i].plabel);
    }
    uniq[keys[i].uidx].gid = static_cast<uint32_t>(s.g_name.size() - 1);
  }
  keys.clear();
  keys.shrink_to_fit();
  if (s.g_name.size() >= SecEdgarQ6Structure::kMultiFlag) {
    throw std::length_error("SEC EDGAR Q6 too many groups");
  }

  const uint32_t adsh_count = s.adshs.Count();
  s.csr_off.assign(static_cast<size_t>(adsh_count) + 2, 0);
  s.csr_tv.reserve(uniq.size());
  s.csr_pay.reserve(uniq.size());
  size_t i = 0;
  while (i < uniq.size()) {
    size_t j = i + 1;
    while (j < uniq.size() && uniq[j].adsh == uniq[i].adsh &&
           uniq[j].tv == uniq[i].tv) {
      ++j;
    }
    uint32_t payload;
    if (j - i == 1 && uniq[i].mult == 1) {
      payload = uniq[i].gid;
    } else {
      if (s.multi.size() >= SecEdgarQ6Structure::kMultiFlag) {
        throw std::length_error("SEC EDGAR Q6 multi table overflow");
      }
      payload = SecEdgarQ6Structure::kMultiFlag |
                static_cast<uint32_t>(s.multi.size());
      s.multi.push_back(static_cast<uint32_t>(j - i));
      for (size_t k = i; k < j; ++k) {
        s.multi.push_back(uniq[k].gid);
        s.multi.push_back(uniq[k].mult);
      }
    }
    s.csr_tv.push_back(uniq[i].tv);
    s.csr_pay.push_back(payload);
    ++s.csr_off[uniq[i].adsh + 1];
    i = j;
  }
  // csr_off[a + 1] currently holds the count of adsh a; prefix-sum into starts.
  for (uint32_t a = 1; a <= adsh_count + 1; ++a) {
    s.csr_off[a] += s.csr_off[a - 1];
  }
  // After the prefix sum csr_off[a] is the start of adsh a (csr_off[1] == 0)
  // and csr_off[a + 1] its end.

  s.RebuildGroupIndex();
  s.base_group_count = static_cast<uint32_t>(s.g_name.size());
  s.base_multi_size = static_cast<uint32_t>(s.multi.size());
  s.adshs.ShrinkToFit();
  s.names.ShrinkToFit();
  s.tags.ShrinkToFit();
  s.tvs.ShrinkToFit();
  s.plabels.ShrinkToFit();
  s.adsh_name.shrink_to_fit();
  s.tv_tag.shrink_to_fit();
  s.csr_tv.shrink_to_fit();
  s.csr_pay.shrink_to_fit();
  s.multi.shrink_to_fit();
  s.g_name.shrink_to_fit();
  s.g_tag.shrink_to_fit();
  s.g_plabel.shrink_to_fit();
  return std::shared_ptr<const SecEdgarQ6Structure>(std::move(s_));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
size_t SecEdgarQ6State::Bytes() const noexcept {
  size_t total = sizeof(*this);
  if (structure) {
    total += structure->Bytes();
  }
  total += chunks.capacity() * sizeof(chunks[0]);
  total += chunks.size() * sizeof(SecEdgarQ6AggChunk);
  total += top.gids.capacity() * sizeof(uint32_t);
  return total;
}

bool SecEdgarQ6State::TopUsable() const noexcept {
  return top.complete || top.gids.size() >= kQ6OutputLimit;
}

SecEdgarQ6State SecEdgarQ6MakeEmptyState(
    std::shared_ptr<const SecEdgarQ6Structure> structure) {
  SecEdgarQ6State state;
  const uint32_t groups = structure->GroupCount();
  state.structure = std::move(structure);
  const size_t chunks = (static_cast<size_t>(groups) + kQ6ChunkMask) >> kQ6ChunkShift;
  state.chunks.reserve(chunks);
  for (size_t i = 0; i < chunks; ++i) {
    state.chunks.push_back(NewChunk());
  }
  state.top.complete = true;
  state.top.lower_bound = -std::numeric_limits<double>::infinity();
  return state;
}

void SecEdgarQ6SetGroup(SecEdgarQ6State &state, uint32_t gid, double sum,
                        int64_t cnt) {
  // The chunk was created by SecEdgarQ6MakeEmptyState and is uniquely owned
  // while the state is being built.
  auto *chunk = const_cast<SecEdgarQ6AggChunk *>(
      state.chunks[gid >> kQ6ChunkShift].get());
  chunk->sum[gid & kQ6ChunkMask] = sum;
  chunk->cnt[gid & kQ6ChunkMask] = cnt;
}

bool SecEdgarQ6GroupBefore(const SecEdgarQ6State &state, uint32_t a, uint32_t b) {
  if (a == b) {
    return false;
  }
  const int c = CmpSum(state.Sum(a), state.Sum(b));
  if (c != 0) {
    return c > 0;
  }
  const auto &s = *state.structure;
  const uint32_t na = s.g_name[a], nb = s.g_name[b];
  if (na != nb) {
    if (na == 0) return false; // NULL sorts last
    if (nb == 0) return true;
    return s.names.View(na) < s.names.View(nb);
  }
  const uint32_t ta = s.g_tag[a], tb = s.g_tag[b];
  if (ta != tb) {
    return s.tags.View(ta) < s.tags.View(tb);
  }
  const uint32_t pa = s.g_plabel[a], pb = s.g_plabel[b];
  if (pa != pb) {
    if (pa == 0) return false;
    if (pb == 0) return true;
    return s.plabels.View(pa) < s.plabels.View(pb);
  }
  return false;
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
  const uint32_t groups = state.structure->GroupCount();
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
uint32_t SecEdgarQ6Resolve(const SecEdgarQ6Structure &s, std::string_view adsh,
                           std::string_view tag, std::string_view version) {
  const uint32_t adsh_id = s.adshs.Find(adsh);
  if (adsh_id == 0) {
    return SecEdgarQ6Structure::kNoPayload;
  }
  const SecEdgarQ6TagVersionKey key(tag, version);
  const uint32_t tv_id = s.tvs.Find(key.View());
  if (tv_id == 0) {
    return SecEdgarQ6Structure::kNoPayload;
  }
  return s.Lookup(adsh_id, tv_id);
}

void SecEdgarQ6ExpandPayload(const SecEdgarQ6Structure &s, uint32_t payload,
                             double value, int64_t sign,
                             std::vector<SecEdgarQ6GroupDelta> &out) {
  if (payload == SecEdgarQ6Structure::kNoPayload) {
    return;
  }
  const double signed_value = static_cast<double>(sign) * value;
  if ((payload & SecEdgarQ6Structure::kMultiFlag) == 0) {
    out.push_back({payload, signed_value, sign});
    return;
  }
  const uint32_t *block = s.multi.data() + (payload & ~SecEdgarQ6Structure::kMultiFlag);
  const uint32_t k = block[0];
  for (uint32_t i = 0; i < k; ++i) {
    const uint32_t gid = block[1 + 2 * i];
    const uint32_t mult = block[2 + 2 * i];
    out.push_back({gid, signed_value * static_cast<double>(mult),
                   sign * static_cast<int64_t>(mult)});
  }
}

void SecEdgarQ6ResolveBatch(const SecEdgarQ6Structure &s,
                            const SecEdgarQ6NumRef *refs, size_t n,
                            std::vector<SecEdgarQ6GroupDelta> &out) {
  constexpr size_t kBlock = 32;
  uint64_t ha[kBlock], ht[kBlock];
  uint32_t adsh_id[kBlock], tv_id[kBlock], begin[kBlock], end[kBlock];
  for (size_t base = 0; base < n; base += kBlock) {
    const size_t m = std::min(kBlock, n - base);
    // A: hash + prefetch pool slots.
    for (size_t i = 0; i < m; ++i) {
      const auto &r = refs[base + i];
      ha[i] = SecEdgarQ6StringPool::Hash(r.adsh);
      s.adshs.Prefetch(ha[i]);
      ht[i] = SecEdgarQ6TagVersionKey(r.tag, r.version).Hash();
      s.tvs.Prefetch(ht[i]);
    }
    // B: resolve ids; prefetch the CSR offsets.
    for (size_t i = 0; i < m; ++i) {
      const auto &r = refs[base + i];
      adsh_id[i] = s.adshs.FindHashed(ha[i], r.adsh);
      if (adsh_id[i] == 0) {
        tv_id[i] = 0;
        continue;
      }
      const SecEdgarQ6TagVersionKey key(r.tag, r.version);
      tv_id[i] = s.tvs.FindHashed(ht[i], key.View());
      if (tv_id[i] != 0) {
        __builtin_prefetch(&s.csr_off[adsh_id[i]]);
      }
    }
    // C: read run bounds; prefetch the run.
    for (size_t i = 0; i < m; ++i) {
      if (adsh_id[i] == 0 || tv_id[i] == 0) {
        begin[i] = end[i] = 0;
        continue;
      }
      begin[i] = s.csr_off[adsh_id[i]];
      end[i] = s.csr_off[adsh_id[i] + 1];
      __builtin_prefetch(&s.csr_tv[begin[i]]);
      __builtin_prefetch(&s.csr_pay[begin[i]]);
    }
    // D: find tv in the run, expand.
    for (size_t i = 0; i < m; ++i) {
      if (begin[i] == end[i]) {
        continue;
      }
      const size_t len = end[i] - begin[i];
      const size_t idx = FindTv(s.csr_tv.data() + begin[i], len, tv_id[i]);
      if (idx == len) {
        continue;
      }
      SecEdgarQ6ExpandPayload(s, s.csr_pay[begin[i] + idx], refs[base + i].value,
                              refs[base + i].sign, out);
    }
  }
}

// ---------------------------------------------------------------------------
// Delta application
// ---------------------------------------------------------------------------
std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyDeltas(const SecEdgarQ6State &old_state,
                      std::vector<SecEdgarQ6GroupDelta> &deltas) {
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
      merged.sum += deltas[j].sum;
      merged.cnt += deltas[j].cnt;
    }
    if (merged.sum != 0.0 || merged.cnt != 0) {
      deltas[m++] = merged;
    }
    i = j;
  }
  deltas.resize(m);
  if (deltas.empty()) {
    return nullptr;
  }

  auto next = std::make_shared<SecEdgarQ6State>();
  next->structure = old_state.structure;
  next->chunks = old_state.chunks;
  next->active_groups = old_state.active_groups;
  next->top = old_state.top;

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
              return SecEdgarQ6GroupBefore(old_state, x, y);
            });
        if (it == gids.end() || *it != d.gid) {
          rescan = true; // invariant violated: be safe
          break;
        }
        gids.erase(it);
      }
    }
  }

  // Apply phase: clone each touched chunk once.
  SecEdgarQ6AggChunk *cur = nullptr;
  uint32_t cur_index = std::numeric_limits<uint32_t>::max();
  for (const auto &d : deltas) {
    const uint32_t ci = d.gid >> kQ6ChunkShift;
    if (ci != cur_index) {
      auto clone = std::shared_ptr<SecEdgarQ6AggChunk>(
          new SecEdgarQ6AggChunk(*old_state.chunks[ci]));
      cur = clone.get();
      next->chunks[ci] = std::move(clone);
      cur_index = ci;
    }
    const uint32_t slot = d.gid & kQ6ChunkMask;
    const bool was_active = cur->cnt[slot] != 0;
    cur->cnt[slot] += d.cnt;
    cur->sum[slot] = cur->cnt[slot] == 0 ? 0.0 : cur->sum[slot] + d.sum;
    const bool is_active = cur->cnt[slot] != 0;
    next->active_groups += static_cast<uint64_t>(is_active) -
                           static_cast<uint64_t>(was_active);
  }

  if (!rescan) {
    for (const auto &d : deltas) {
      if (next->Count(d.gid) != 0 && AtLeast(next->Sum(d.gid), lb)) {
        const auto it = std::lower_bound(
            gids.begin(), gids.end(), d.gid, [&](uint32_t x, uint32_t y) {
              return SecEdgarQ6GroupBefore(*next, x, y);
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
  const auto &s = *src->structure;
  const size_t n = std::min(limit, src->top.gids.size());
  std::vector<SecEdgarQ6OutputRow> rows;
  rows.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t g = src->top.gids[i];
    SecEdgarQ6OutputRow row;
    if (s.g_name[g] == 0) {
      row.name_is_null = true;
    } else {
      row.name = std::string(s.names.View(s.g_name[g]));
    }
    row.tag = std::string(s.tags.View(s.g_tag[g]));
    if (s.g_plabel[g] == 0) {
      row.plabel_is_null = true;
    } else {
      row.plabel = std::string(s.plabels.View(s.g_plabel[g]));
    }
    row.total_value = src->Sum(g);
    row.cnt = src->Count(g);
    rows.push_back(std::move(row));
  }
  return rows;
}


// ---------------------------------------------------------------------------
// Structural edits
// ---------------------------------------------------------------------------
namespace {
void ExpandAgg(const SecEdgarQ6Structure &s, uint32_t payload, double sum, int64_t cnt,
               int sign, std::vector<SecEdgarQ6GroupDelta> &out) {
  if (payload == SecEdgarQ6Structure::kNoPayload) return;
  const double ss = static_cast<double>(sign) * sum;
  const int64_t sc = static_cast<int64_t>(sign) * cnt;
  if ((payload & SecEdgarQ6Structure::kMultiFlag) == 0) {
    out.push_back({payload, ss, sc});
    return;
  }
  const uint32_t *block = s.multi.data() + (payload & ~SecEdgarQ6Structure::kMultiFlag);
  for (uint32_t i = 0; i < block[0]; ++i) {
    const double mult = block[2 + 2 * i];
    out.push_back({block[1 + 2 * i], ss * mult, sc * static_cast<int64_t>(block[2 + 2 * i])});
  }
}
} // namespace

std::shared_ptr<const SecEdgarQ6State>
SecEdgarQ6ApplyEdits(const SecEdgarQ6State &old_state,
                     const std::vector<SecEdgarQ6AdshEdit> &edits,
                     std::vector<SecEdgarQ6GroupDelta> &extra) {
  const SecEdgarQ6Structure &os = *old_state.structure;
  auto s = std::make_shared<SecEdgarQ6Structure>(os);
  std::vector<SecEdgarQ6GroupDelta> deltas = std::move(extra);
  using Run = std::vector<std::pair<uint32_t, uint32_t>>; // (tv, payload)
  std::vector<std::pair<uint32_t, Run>> runs;

  for (const auto &edit : edits) {
    // Old contributions (old structure, pre-transaction sums).
    const uint32_t old_id = os.adshs.Find(edit.adsh);
    if (old_id != 0 && os.adsh_name[old_id] != SecEdgarQ6Structure::kNotCohort) {
      for (const auto &n : edit.old_nums) {
        const uint32_t tv = os.tvs.Find(SecEdgarQ6TagVersionKey(n.tag, n.version).View());
        if (tv == 0) continue;
        ExpandAgg(os, os.Lookup(old_id, tv), n.sum, n.cnt, -1, deltas);
      }
    }
    uint32_t id = s->adshs.Find(edit.adsh);
    Run run;
    if (edit.new_cohort) {
      if (id == 0) {
        id = s->adshs.Intern(edit.adsh);
        s->adsh_name.resize(static_cast<size_t>(id) + 1, 0);
      }
      const uint32_t name_id = edit.name_is_null ? 0 : s->names.Intern(edit.name);
      s->adsh_name[id] = name_id;
      struct E { uint32_t tv, gid, mult; };
      std::vector<E> es;
      for (const auto &pe : edit.pre) {
        const uint32_t tag_id = s->tags.Intern(pe.tag);
        const SecEdgarQ6TagVersionKey key(pe.tag, pe.version);
        const uint32_t before = s->tvs.Count();
        const uint32_t tv = s->tvs.Intern(key.View());
        if (tv > before) {
          s->tv_tag.resize(static_cast<size_t>(tv) + 1, 0);
          s->tv_tag[tv] = tag_id;
        }
        const uint32_t plabel_id = pe.plabel_is_null ? 0 : s->plabels.Intern(pe.plabel);
        es.push_back({tv, s->FindOrAddGroup(name_id, tag_id, plabel_id), pe.mult});
      }
      std::sort(es.begin(), es.end(), [](const E &a, const E &b) {
        return a.tv != b.tv ? a.tv < b.tv : a.gid < b.gid;
      });
      for (size_t i = 0; i < es.size();) {
        size_t j = i;
        std::vector<std::pair<uint32_t, uint32_t>> gm; // (gid, mult)
        while (j < es.size() && es[j].tv == es[i].tv) {
          if (!gm.empty() && gm.back().first == es[j].gid) gm.back().second += es[j].mult;
          else gm.emplace_back(es[j].gid, es[j].mult);
          ++j;
        }
        uint32_t payload;
        if (gm.size() == 1 && gm[0].second == 1) {
          payload = gm[0].first;
        } else {
          payload = SecEdgarQ6Structure::kMultiFlag | static_cast<uint32_t>(s->multi.size());
          s->multi.push_back(static_cast<uint32_t>(gm.size()));
          for (auto &[g, m] : gm) {
            s->multi.push_back(g);
            s->multi.push_back(m);
          }
        }
        run.emplace_back(es[i].tv, payload);
        i = j;
      }
      for (const auto &n : edit.new_nums) {
        const uint32_t tv = s->tvs.Find(SecEdgarQ6TagVersionKey(n.tag, n.version).View());
        if (tv == 0) continue;
        const auto it = std::lower_bound(run.begin(), run.end(), tv,
                                         [](const auto &r, uint32_t v) { return r.first < v; });
        if (it == run.end() || it->first != tv) continue;
        ExpandAgg(*s, it->second, n.sum, n.cnt, +1, deltas);
      }
    } else if (id != 0) {
      s->adsh_name[id] = SecEdgarQ6Structure::kNotCohort;
    }
    if (id != 0) runs.emplace_back(id, std::move(run));
  }

  if (s->GroupCount() > s->base_group_count + s->base_group_count / 2 + 4096 ||
      s->multi.size() > static_cast<size_t>(s->base_multi_size) * 2 + 4096) {
    return nullptr; // too much accumulated garbage: let the caller rebuild
  }

  // Rebuild the CSR in one pass.
  std::sort(runs.begin(), runs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  const uint32_t n_adsh = s->adshs.Count();
  const uint32_t old_n = os.adshs.Count();
  std::vector<uint32_t> off(static_cast<size_t>(n_adsh) + 2, 0), tvv, pay;
  tvv.reserve(os.csr_tv.size() + 64);
  pay.reserve(os.csr_pay.size() + 64);
  size_t ri = 0;
  for (uint32_t a = 1; a <= n_adsh; ++a) {
    off[a] = static_cast<uint32_t>(tvv.size());
    if (ri < runs.size() && runs[ri].first == a) {
      for (const auto &[tv, p] : runs[ri].second) {
        tvv.push_back(tv);
        pay.push_back(p);
      }
      ++ri;
    } else if (a <= old_n) {
      tvv.insert(tvv.end(), os.csr_tv.begin() + os.csr_off[a], os.csr_tv.begin() + os.csr_off[a + 1]);
      pay.insert(pay.end(), os.csr_pay.begin() + os.csr_off[a], os.csr_pay.begin() + os.csr_off[a + 1]);
    }
  }
  off[n_adsh + 1] = static_cast<uint32_t>(tvv.size());
  s->csr_off = std::move(off);
  s->csr_tv = std::move(tvv);
  s->csr_pay = std::move(pay);

  auto base = std::make_shared<SecEdgarQ6State>();
  base->structure = s;
  base->chunks = old_state.chunks;
  const size_t want = (static_cast<size_t>(s->GroupCount()) + kQ6ChunkMask) >> kQ6ChunkShift;
  while (base->chunks.size() < want) base->chunks.push_back(NewChunk());
  base->active_groups = old_state.active_groups;
  base->top = old_state.top;
  auto next = SecEdgarQ6ApplyDeltas(*base, deltas);
  return next ? next : std::shared_ptr<const SecEdgarQ6State>(base);
}

} // namespace accelerator_codegen::aux_state::query
