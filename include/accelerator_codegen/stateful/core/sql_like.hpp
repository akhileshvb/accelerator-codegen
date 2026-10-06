#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace accelerator_codegen::aux_state::core::sql_like {

// ---------------------------------------------------------------------------
// SIMD substring search over (ptr, len). Returns the first match offset or npos.
// First+last byte filter (16 positions/iter with baseline SSE2, 32 with AVX2
// chosen at runtime via __builtin_cpu_supports; no -march flags needed), then
// memcmp of the middle. Scalar fallback on other architectures.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kNpos = ~std::size_t{0};

namespace detail {

inline std::size_t FindScalar(const char *h, std::size_t hn, const char *n,
                              std::size_t nn, std::size_t from) {
  if (hn < nn)
    return kNpos;
  const std::size_t last = hn - nn;
  for (std::size_t i = from; i <= last; ++i) {
    const void *p = std::memchr(h + i, n[0], last - i + 1);
    if (!p)
      return kNpos;
    i = static_cast<std::size_t>(static_cast<const char *>(p) - h);
    if (std::memcmp(h + i, n, nn) == 0)
      return i;
  }
  return kNpos;
}

#if defined(__x86_64__)
inline std::size_t FindSse2(const char *h, std::size_t hn, const char *n, std::size_t nn) {
  const __m128i f = _mm_set1_epi8(n[0]), l = _mm_set1_epi8(n[nn - 1]);
  const std::size_t starts = hn - nn + 1; // candidate start positions
  std::size_t i = 0;
  for (; i + 16 <= starts; i += 16) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(h + i));
    const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(h + i + nn - 1));
    uint32_t m = static_cast<uint32_t>(
        _mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, f), _mm_cmpeq_epi8(b, l))));
    while (m) {
      const std::size_t p = i + static_cast<std::size_t>(__builtin_ctz(m));
      if (nn <= 2 || std::memcmp(h + p + 1, n + 1, nn - 2) == 0)
        return p;
      m &= m - 1;
    }
  }
  const std::size_t r = FindScalar(h, hn, n, nn, i);
  return r;
}
__attribute__((target("avx2"))) inline std::size_t FindAvx2(const char *h, std::size_t hn,
                                                            const char *n, std::size_t nn) {
  const __m256i f = _mm256_set1_epi8(n[0]), l = _mm256_set1_epi8(n[nn - 1]);
  const std::size_t starts = hn - nn + 1;
  std::size_t i = 0;
  for (; i + 32 <= starts; i += 32) {
    const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(h + i));
    const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(h + i + nn - 1));
    uint32_t m = static_cast<uint32_t>(
        _mm256_movemask_epi8(_mm256_and_si256(_mm256_cmpeq_epi8(a, f), _mm256_cmpeq_epi8(b, l))));
    while (m) {
      const std::size_t p = i + static_cast<std::size_t>(__builtin_ctz(m));
      if (nn <= 2 || std::memcmp(h + p + 1, n + 1, nn - 2) == 0)
        return p;
      m &= m - 1;
    }
  }
  return FindScalar(h, hn, n, nn, i);
}
inline bool HasAvx2() {
  static const bool v = __builtin_cpu_supports("avx2");
  return v;
}
#endif

} // namespace detail

// force_level: -1 auto, 0 scalar, 1 SSE2, 2 AVX2 (testing hook).
inline std::size_t FindSubstring(const char *h, std::size_t hn, const char *n,
                                 std::size_t nn, int force_level = -1) {
  if (nn == 0)
    return 0;
  if (hn < nn)
    return kNpos;
  if (nn == 1) {
    const void *p = std::memchr(h, n[0], hn);
    return p ? static_cast<std::size_t>(static_cast<const char *>(p) - h) : kNpos;
  }
#if defined(__x86_64__)
  if ((force_level == 2 || force_level < 0) && detail::HasAvx2())
    return detail::FindAvx2(h, hn, n, nn);
  if (force_level != 0)
    return detail::FindSse2(h, hn, n, nn);
#endif
  return detail::FindScalar(h, hn, n, nn, 0);
}

// ---------------------------------------------------------------------------
// Compiled SQL LIKE matcher.
//
// Semantics: `%` any run (incl. empty), `_` exactly one character, `escape`
// (default backslash, as PostgreSQL; pass -1 for none) makes the next pattern
// byte literal; a pattern ending in the escape char throws std::invalid_argument
// (PostgreSQL raises an error). The empty pattern matches only the empty string.
//
// Comparison is BYTE-WISE: `_` matches one byte unless utf8_underscore=true
// (then it matches one UTF-8 scalar, general path). case_insensitive folds ASCII
// letters only. Callers MUST choose these to mirror the target DBMS and column
// collation: PostgreSQL LIKE is case-sensitive/byte-wise (non-deterministic or
// locale collations differ); MySQL default collations (utf8mb4_0900_ai_ci,
// *_general_ci) are case-insensitive and also accent-insensitive/padding aware,
// which this matcher does NOT model beyond ASCII folding.
//
// Fast paths (chosen at Compile): All (`%`), Exact, Prefix (`X%`), Suffix (`%X`),
// Contains (`%X%`, SIMD search), Segments (`%A%B%`, `A%B`, ... greedy leftmost
// search, SIMD), General (any pattern containing `_`: iterative wildcard match).
// Matching is allocation-free and thread-safe (const).
// ---------------------------------------------------------------------------
class LikeMatcher {
public:
  enum class Kind { kAll, kExact, kPrefix, kSuffix, kContains, kSegments, kGeneral };

  static LikeMatcher Compile(std::string_view pattern, int escape = '\\',
                             bool case_insensitive = false, bool utf8_underscore = false) {
    LikeMatcher m;
    m.ci_ = case_insensitive;
    m.utf8_ = utf8_underscore;
    // Tokenize: kind 0 literal, 1 '_', 2 '%' (runs of % collapsed).
    bool has_underscore = false;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(pattern[i]);
      if (escape >= 0 && c == static_cast<unsigned char>(escape)) {
        if (i + 1 >= pattern.size())
          throw std::invalid_argument("LIKE pattern must not end with escape character");
        m.tok_.push_back({0, m.Fold(pattern[++i])});
      } else if (c == '%') {
        if (m.tok_.empty() || m.tok_.back().type != 2)
          m.tok_.push_back({2, 0});
      } else if (c == '_') {
        m.tok_.push_back({1, 0});
        has_underscore = true;
      } else {
        m.tok_.push_back({0, m.Fold(static_cast<char>(c))});
      }
    }
    if (has_underscore) {
      m.kind_ = Kind::kGeneral;
      return m;
    }
    // Pure literals + '%': split into segments.
    std::string cur;
    bool lead = !m.tok_.empty() && m.tok_.front().type == 2;
    bool trail = !m.tok_.empty() && m.tok_.back().type == 2;
    for (const auto &t : m.tok_) {
      if (t.type == 2) {
        if (!cur.empty())
          m.segs_.push_back(std::move(cur));
        cur.clear();
      } else {
        cur.push_back(t.ch);
      }
    }
    if (!cur.empty() || m.tok_.empty())
      m.segs_.push_back(std::move(cur));
    m.anchor_start_ = !lead;
    m.anchor_end_ = !trail;
    if (m.tok_.size() == 1 && m.tok_[0].type == 2) {
      m.kind_ = Kind::kAll;
      m.segs_.clear();
    } else if (m.segs_.size() == 1) {
      m.kind_ = m.anchor_start_ && m.anchor_end_ ? Kind::kExact
                : m.anchor_start_                ? Kind::kPrefix
                : m.anchor_end_                  ? Kind::kSuffix
                                                 : Kind::kContains;
    } else {
      m.kind_ = Kind::kSegments;
    }
    return m;
  }

  Kind kind() const { return kind_; }
  bool Match(std::string_view s) const { return Match(s.data(), s.size()); }

  bool Match(const char *s, std::size_t n) const {
    switch (kind_) {
    case Kind::kAll:
      return true;
    case Kind::kExact:
      return n == segs_[0].size() && EqAt(s, segs_[0]);
    case Kind::kPrefix:
      return n >= segs_[0].size() && EqAt(s, segs_[0]);
    case Kind::kSuffix:
      return n >= segs_[0].size() && EqAt(s + n - segs_[0].size(), segs_[0]);
    case Kind::kContains:
      return Find(s, n, segs_[0]) != kNpos;
    case Kind::kSegments:
      return MatchSegments(s, n);
    case Kind::kGeneral:
      return MatchGeneral(s, n);
    }
    return false;
  }

private:
  struct Tok {
    int type;
    char ch;
  };
  char Fold(char c) const {
    return ci_ && c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
  }
  bool EqAt(const char *s, const std::string &seg) const {
    if (!ci_)
      return std::memcmp(s, seg.data(), seg.size()) == 0;
    for (std::size_t i = 0; i < seg.size(); ++i)
      if (Fold(s[i]) != seg[i])
        return false;
    return true;
  }
  std::size_t Find(const char *s, std::size_t n, const std::string &seg,
                   std::size_t from = 0) const {
    if (from > n)
      return kNpos;
    if (!ci_) {
      const std::size_t r = FindSubstring(s + from, n - from, seg.data(), seg.size());
      return r == kNpos ? kNpos : r + from;
    }
    if (seg.size() > n)
      return kNpos;
    for (std::size_t i = from; i + seg.size() <= n; ++i)
      if (EqAt(s + i, seg))
        return i;
    return kNpos;
  }
  bool MatchSegments(const char *s, std::size_t n) const {
    std::size_t pos = 0, end = n;
    std::size_t first = 0, last = segs_.size();
    if (anchor_start_) {
      const auto &g = segs_[0];
      if (n < g.size() || !EqAt(s, g))
        return false;
      pos = g.size();
      first = 1;
    }
    if (anchor_end_) {
      const auto &g = segs_[last - 1];
      if (n < g.size() || n - g.size() < pos || !EqAt(s + n - g.size(), g))
        return false;
      end = n - g.size();
      --last;
    }
    for (std::size_t i = first; i < last; ++i) {
      const std::size_t p = Find(s, end, segs_[i], pos);
      if (p == kNpos)
        return false;
      pos = p + segs_[i].size();
    }
    return true;
  }
  std::size_t CharLen(const char *s, std::size_t i, std::size_t n) const {
    if (!utf8_)
      return 1;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    return std::min(l, n - i);
  }
  bool MatchGeneral(const char *s, std::size_t n) const {
    const std::size_t T = tok_.size();
    std::size_t ti = 0, si = 0, star_t = kNpos, star_s = 0;
    while (si < n) {
      if (ti < T && tok_[ti].type == 2) {
        star_t = ti++;
        star_s = si;
        continue;
      }
      if (ti < T && tok_[ti].type == 1) {
        si += CharLen(s, si, n);
        ++ti;
        continue;
      }
      if (ti < T && Fold(s[si]) == tok_[ti].ch) {
        ++si;
        ++ti;
        continue;
      }
      if (star_t != kNpos) {
        ti = star_t + 1;
        star_s += CharLen(s, star_s, n);
        si = star_s;
        continue;
      }
      return false;
    }
    while (ti < T && tok_[ti].type == 2)
      ++ti;
    return ti == T;
  }

  Kind kind_ = Kind::kExact;
  bool ci_ = false, utf8_ = false, anchor_start_ = true, anchor_end_ = true;
  std::vector<Tok> tok_;
  std::vector<std::string> segs_; // literal segments (already case-folded if ci)
};

// ---------------------------------------------------------------------------
// Bulk matching over a contiguous blob: string i is data[offsets[i], offsets[i+1])
// (offsets has n+1 uint64 entries, so blobs > 4 GB are fine). Output bitmap has
// ceil(n/64) words, bit i set iff string i matches. threads > 1 splits the
// 64-string-aligned ranges across std::thread workers (link Threads::Threads).
// ---------------------------------------------------------------------------
inline std::vector<uint64_t> MatchBlobBitmap(const LikeMatcher &m, const uint64_t *offsets,
                                             std::size_t n, const char *data,
                                             unsigned threads = 1) {
  const std::size_t words = (n + 63) / 64;
  std::vector<uint64_t> bits(words, 0);
  auto run = [&](std::size_t w0, std::size_t w1) {
    for (std::size_t w = w0; w < w1; ++w) {
      uint64_t acc = 0;
      const std::size_t lo = w * 64, hi = std::min(n, lo + 64);
      for (std::size_t i = lo; i < hi; ++i)
        if (m.Match(data + offsets[i], static_cast<std::size_t>(offsets[i + 1] - offsets[i])))
          acc |= uint64_t{1} << (i - lo);
      bits[w] = acc;
    }
  };
  if (threads <= 1 || words < 2 * threads) {
    run(0, words);
  } else {
    std::vector<std::thread> pool;
    const std::size_t per = (words + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
      const std::size_t a = t * per, b = std::min(words, a + per);
      if (a < b)
        pool.emplace_back(run, a, b);
    }
    for (auto &th : pool)
      th.join();
  }
  return bits;
}

inline std::vector<uint64_t> MatchBlobIndices(const LikeMatcher &m, const uint64_t *offsets,
                                              std::size_t n, const char *data,
                                              unsigned threads = 1) {
  const auto bits = MatchBlobBitmap(m, offsets, n, data, threads);
  std::vector<uint64_t> out;
  for (std::size_t w = 0; w < bits.size(); ++w) {
    uint64_t b = bits[w];
    while (b) {
      out.push_back(w * 64 + static_cast<std::size_t>(__builtin_ctzll(b)));
      b &= b - 1;
    }
  }
  return out;
}

} // namespace accelerator_codegen::aux_state::core::sql_like
