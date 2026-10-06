// Unit tests for the generic aux-state core primitives. No DBMS required.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/any_key_cow_array.hpp"
#include "accelerator_codegen/stateful/core/chunked_blob_map.hpp"
#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/cow_fenwick.hpp"
#include "accelerator_codegen/stateful/core/cow_record_store.hpp"
#include "accelerator_codegen/stateful/core/cow_sorted_blocks.hpp"
#include "accelerator_codegen/stateful/core/decimal_text.hpp"
#include "accelerator_codegen/stateful/core/dense_id_array.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"
#include "accelerator_codegen/stateful/core/int128_acc.hpp"
#include "accelerator_codegen/stateful/core/keyed_string_column.hpp"
#include "accelerator_codegen/stateful/core/pg_numeric_div.hpp"
#include "accelerator_codegen/stateful/core/radix_range_tree.hpp"
#include "accelerator_codegen/stateful/core/sorted_count_sum.hpp"
#include "accelerator_codegen/stateful/core/sql_date.hpp"
#include "accelerator_codegen/stateful/core/sql_like.hpp"
#include "accelerator_codegen/stateful/core/string_blob.hpp"

using namespace accelerator_codegen::aux_state::core;
namespace sd = accelerator_codegen::aux_state::core::sql_date;
namespace sl = accelerator_codegen::aux_state::core::sql_like;

#define CHECK(c)                                                                         \
  do {                                                                                   \
    if (!(c)) {                                                                          \
      std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);          \
      std::abort();                                                                      \
    }                                                                                    \
  } while (0)

static void TestDenseIdArray() {
  DenseIdArray<uint8_t> a;
  CHECK(!a.Set(-1, 1));
  CHECK(a.Get(-5, 9) == 9);
  CHECK(a.Get(7, 9) == 9);
  const int64_t big = 6'000'000'000LL; // beyond old 2^28 cap
  CHECK(a.Set(big, 42, 255));
  CHECK(a.Get(big) == 42);
  CHECK(a.Get(big - 1, 7) == 255);   // filled prefix of the touched chunk
  CHECK(a.Get(big + 1, 7) == 7);     // beyond chunk size reads as absent
  CHECK(a.Has(big, 0));
  CHECK(a.Find(big) && *a.Find(big) == 42);
  CHECK(a.Set(INT64_C(1) << 36, 1));
  CHECK(a.Get(INT64_C(1) << 36) == 1);
  // Sparse: 1 in 4 used over 4M ids, ~1 byte/slot.
  DenseIdArray<uint8_t> sp;
  for (int64_t k = 0; k < (1 << 22); k += 4)
    sp.Set(k, 1);
  const double per_slot = double(sp.MemoryBytes()) / (1 << 22);
  CHECK(per_slot < 1.1);
  // Chunks stay unallocated when untouched.
  DenseIdArray<uint32_t> far;
  far.Set(INT64_C(5) << 30, 3);
  CHECK(far.SlotCount() <= DenseIdArray<uint32_t>::kSize);
}

static void TestCow() {
  ChunkedCowArray<uint32_t> a(0xFFFFFFFFu);
  CHECK(!a.Set(-1, 1));
  CHECK(a.Get(123456789012LL) == 0xFFFFFFFFu);
  for (int64_t i = 0; i < 100000; ++i)
    a.Set(i * 3, uint32_t(i));
  ChunkedCowArray<uint32_t> b = a; // O(#chunks)
  CHECK(b.SharedChunks() == b.AllocatedChunks());
  b.Set(30, 777); // touches one chunk
  CHECK(a.Get(30) == 10 && b.Get(30) == 777);
  CHECK(b.SharedChunks() == b.AllocatedChunks() - 1);
  CHECK(!a.SharesChunkWith(b, 0));
  CHECK(a.SharesChunkWith(b, 5));
  // new chunk in b invisible to a
  b.Set(INT64_C(1) << 33, 5);
  CHECK(a.Get(INT64_C(1) << 33) == 0xFFFFFFFFu && b.Get(INT64_C(1) << 33) == 5);
  // chain of versions
  ChunkedCowArray<uint32_t> c = b;
  c.Set(30, 1);
  CHECK(a.Get(30) == 10 && b.Get(30) == 777 && c.Get(30) == 1);
  // writing to a again after copies are dropped is in place
  { ChunkedCowArray<uint32_t> tmp = a; }
  std::size_t shared_before = a.SharedChunks();
  a.Set(60, 2);
  CHECK(a.SharedChunks() <= shared_before);
  CHECK(b.Get(60) == 20);

  ShardedCowHashMap<int64_t, 6> m;
  for (int64_t i = 0; i < 20000; ++i)
    m.Upsert(i * 7919, i);
  auto m2 = m;
  CHECK(m2.SharedShards() == 64);
  m2.Upsert(7919, -1);
  m2.Erase(2 * 7919);
  CHECK(m2.SharedShards() == 62);
  CHECK(*m.Find(7919) == 1 && *m2.Find(7919) == -1);
  CHECK(m.Find(2 * 7919) && !m2.Find(2 * 7919));
  CHECK(m.size() == 20000 && m2.size() == 19999);
  std::size_t cnt = 0;
  m2.ForEach([&](int64_t, const int64_t &) { ++cnt; });
  CHECK(cnt == 19999);
}

static void TestFlatHashMap() {
  FlatHashMap<int64_t> m;
  std::unordered_map<int64_t, int64_t> ref;
  std::mt19937_64 rng(1);
  for (int step = 0; step < 400000; ++step) {
    const int64_t k = int64_t(rng() % 5000) - 100 + (rng() % 7 == 0 ? (INT64_C(1) << 40) : 0);
    const int op = int(rng() % 4);
    if (op == 0) {
      m.Upsert(k, step);
      ref[k] = step;
    } else if (op == 1) {
      CHECK(m.Erase(k) == (ref.erase(k) == 1));
    } else if (op == 2) {
      auto r = m.Insert(k, step);
      auto rr = ref.emplace(k, step);
      CHECK(r.second == rr.second && *r.first == rr.first->second);
    } else {
      const auto *p = m.Find(k);
      auto it = ref.find(k);
      CHECK((p != nullptr) == (it != ref.end()));
      if (p)
        CHECK(*p == it->second);
    }
    CHECK(m.size() == ref.size());
  }
  std::size_t n = 0;
  for (auto e : m) {
    CHECK(ref.at(e.first) == e.second);
    ++n;
  }
  CHECK(n == ref.size());
  FlatHashMap<uint32_t> big;
  big.Reserve(1000000);
  const std::size_t cap0 = big.capacity();
  for (int64_t i = 0; i < 1000000; ++i)
    big[i * 6'000'001LL] = uint32_t(i);
  CHECK(big.capacity() == cap0);
  CHECK(big.size() == 1000000);
  for (int64_t i = 0; i < 1000000; i += 1000)
    CHECK(*big.Find(i * 6'000'001LL) == uint32_t(i));
  std::printf("flat_hash_map: %.1f B/entry (u32 values, 1M)\n",
              double(big.memory_bytes()) / big.size());
  // heavy churn keeps capacity bounded (tombstone reclamation)
  FlatHashMap<int> ch;
  for (int64_t i = 0; i < 2000000; ++i) {
    ch.Upsert(i, 1);
    if (i >= 100)
      ch.Erase(i - 100);
  }
  CHECK(ch.size() == 100 && ch.capacity() <= 1024);
}

static void TestDate() {
  // Round trip + monotone day-by-day over a huge range.
  int64_t prev = INT64_MIN;
  for (int64_t d = -2000000; d <= 3000000; ++d) {
    auto c = sd::CivilFromDays(d);
    CHECK(sd::DaysFromCivil(c.year, c.month, c.day) == d);
    CHECK(c.day >= 1 && c.day <= sd::DaysInMonth(c.year, c.month));
    if (prev != INT64_MIN) {
      auto p = sd::CivilFromDays(prev);
      const bool next_day = c.year == p.year && c.month == p.month && c.day == p.day + 1;
      const bool next_month = c.day == 1 && p.day == sd::DaysInMonth(p.year, p.month) &&
                              ((c.year == p.year && c.month == p.month + 1) ||
                               (c.year == p.year + 1 && c.month == 1 && p.month == 12));
      CHECK(next_day || next_month);
    }
    prev = d;
  }
  CHECK(sd::DaysFromCivil(1970, 1, 1) == 0);
  CHECK(sd::DaysFromCivil(2000, 3, 1) == 11017);
  CHECK(sd::DaysFromCivil(1900, 3, 1) == -25508);
  CHECK(sd::DaysFromCivil(1, 1, 1) == -719162);
  auto D = [](int y, int m, int d) { return sd::DaysFromCivil(y, m, d); };
  CHECK(sd::AddMonths(D(2024, 1, 31), 1) == D(2024, 2, 29));
  CHECK(sd::AddMonths(D(2023, 1, 31), 1) == D(2023, 2, 28));
  CHECK(sd::AddMonths(D(2024, 3, 31), -1) == D(2024, 2, 29));
  CHECK(sd::AddMonths(D(2024, 1, 31), 13) == D(2025, 2, 28));
  CHECK(sd::AddMonths(D(2024, 5, 31), 1) == D(2024, 6, 30));
  CHECK(sd::AddMonths(D(2024, 1, 15), -13) == D(2022, 12, 15));
  CHECK(sd::AddMonths(D(1969, 12, 31), 2) == D(1970, 2, 28));
  CHECK(sd::AddMonths(D(1600, 1, 31), 1) == D(1600, 2, 29));
  CHECK(sd::AddMonths(D(1900, 1, 31), 1) == D(1900, 2, 28));
  CHECK(sd::AddYears(D(2024, 2, 29), 1) == D(2025, 2, 28));
  CHECK(sd::AddYears(D(2024, 2, 29), 4) == D(2028, 2, 29));
  CHECK(sd::AddYears(D(2024, 2, 29), -4) == D(2020, 2, 29));
  CHECK(sd::AddYears(D(2100, 2, 28), 100) == D(2200, 2, 28));
  CHECK(sd::AddYears(D(1998, 12, 1), -3 * 0 + 0) == D(1998, 12, 1));
  CHECK(sd::MonthStart(D(2024, 2, 17)) == D(2024, 2, 1));
  CHECK(sd::IsMonthStart(D(2024, 2, 1)) && !sd::IsMonthStart(D(2024, 2, 2)));
  CHECK(sd::YearOf(D(-5, 6, 1)) == -5 && sd::MonthOf(D(2024, 2, 17)) == 2);
  // AddMonths vs independent reference over many days/offsets.
  for (int64_t d = -800000; d <= 800000; d += 37)
    for (int n = -30; n <= 30; n += 7) {
      auto c = sd::CivilFromDays(d);
      int y = c.year, mo = c.month - 1 + n;
      while (mo < 0) { mo += 12; --y; }
      while (mo >= 12) { mo -= 12; ++y; }
      int dd = std::min(c.day, sd::DaysInMonth(y, mo + 1));
      CHECK(sd::AddMonths(d, n) == D(y, mo + 1, dd));
    }
}

// Reference recursive LIKE matcher (byte-wise, optional ci).
static bool RefLike(const std::string &p, std::size_t pi, const std::string &s, std::size_t si,
                    bool ci) {
  if (pi == p.size())
    return si == s.size();
  char c = p[pi];
  if (c == '%') {
    for (std::size_t k = si; k <= s.size(); ++k)
      if (RefLike(p, pi + 1, s, k, ci))
        return true;
    return false;
  }
  if (c == '\\') {
    if (pi + 1 >= p.size())
      return false;
    c = p[++pi];
    auto f = [&](char x) { return ci && x >= 'A' && x <= 'Z' ? char(x + 32) : x; };
    return si < s.size() && f(s[si]) == f(c) && RefLike(p, pi + 1, s, si + 1, ci);
  }
  auto f = [&](char x) { return ci && x >= 'A' && x <= 'Z' ? char(x + 32) : x; };
  if (c == '_')
    return si < s.size() && RefLike(p, pi + 1, s, si + 1, ci);
  return si < s.size() && f(s[si]) == f(c) && RefLike(p, pi + 1, s, si + 1, ci);
}

static void TestLike() {
  using K = sl::LikeMatcher::Kind;
  auto M = [](const char *p, const char *s, bool ci = false) {
    return sl::LikeMatcher::Compile(p, '\\', ci).Match(s);
  };
  CHECK(M("", "") && !M("", "a"));
  CHECK(M("%", "") && M("%%", "abc"));
  CHECK(M("a%", "abc") && !M("a%", "ba"));
  CHECK(M("%c", "abc") && !M("%c", "abd"));
  CHECK(M("%b%", "abc") && !M("%x%", "abc"));
  CHECK(M("a%c", "ac") && !M("a%c", "a"));
  CHECK(M("%special%requests%", "xx special yy requests zz"));
  CHECK(!M("%special%requests%", "requests special"));
  CHECK(!M("a%ab", "ab")); // overlap must not count
  CHECK(M("a_c", "abc") && !M("a_c", "ac"));
  CHECK(M("100\\%", "100%") && !M("100\\%", "1000"));
  CHECK(M("a\\_c", "a_c") && !M("a\\_c", "abc"));
  CHECK(M("ABC%", "abcdef", true) && !M("ABC%", "abcdef", false));
  CHECK(sl::LikeMatcher::Compile("%").kind() == K::kAll);
  CHECK(sl::LikeMatcher::Compile("ab").kind() == K::kExact);
  CHECK(sl::LikeMatcher::Compile("ab%").kind() == K::kPrefix);
  CHECK(sl::LikeMatcher::Compile("%ab").kind() == K::kSuffix);
  CHECK(sl::LikeMatcher::Compile("%ab%").kind() == K::kContains);
  CHECK(sl::LikeMatcher::Compile("%a%b%").kind() == K::kSegments);
  CHECK(sl::LikeMatcher::Compile("a_b").kind() == K::kGeneral);
  bool threw = false;
  try { sl::LikeMatcher::Compile("ab\\"); } catch (const std::invalid_argument &) { threw = true; }
  CHECK(threw);
  CHECK(sl::LikeMatcher::Compile("é_", '\\', false, true).Match("éa"));
  CHECK(sl::LikeMatcher::Compile("_", '\\', false, true).Match("é"));
  CHECK(!sl::LikeMatcher::Compile("_", '\\', false, false).Match("é"));

  // Fuzz vs reference.
  std::mt19937 rng(7);
  const char alpha[] = "abAB%_\\";
  const char salpha[] = "abAB%_";
  for (int it = 0; it < 300000; ++it) {
    std::string p, s;
    for (int i = rng() % 8; i > 0; --i) p += alpha[rng() % 7];
    for (int i = rng() % 10; i > 0; --i) s += salpha[rng() % 6];
    if (!p.empty() && p.back() == '\\') {
      bool odd = true; // trailing escape handling: count escapes
      (void)odd;
      std::size_t k = 0;
      while (k < p.size() && p[p.size() - 1 - k] == '\\') ++k;
      if (k % 2 == 1) p.pop_back();
    }
    for (bool ci : {false, true}) {
      auto m = sl::LikeMatcher::Compile(p, '\\', ci);
      CHECK(m.Match(s) == RefLike(p, 0, s, 0, ci));
    }
  }
  // Substring search at all levels vs std::string::find.
  std::string h;
  for (int i = 0; i < 5000; ++i) h += "abcab"[rng() % 5];
  for (int it = 0; it < 20000; ++it) {
    std::string n;
    for (int i = 1 + rng() % 6; i > 0; --i) n += "abcab"[rng() % 5];
    std::size_t len = rng() % h.size();
    std::size_t exp = std::string_view(h.data(), len).find(n);
    if (exp == std::string::npos) exp = sl::kNpos;
    for (int lvl : {-1, 0, 1, 2})
      CHECK(sl::FindSubstring(h.data(), len, n.data(), n.size(), lvl) == exp);
  }
  // Blob matching, > 32-bit offsets are plain uint64 (checked by type) + threads.
  StringBlob blob;
  std::vector<std::string> strs;
  for (int i = 0; i < 100003; ++i) {
    std::string s = "row" + std::to_string(i) + (i % 5 == 0 ? " special req" : " other");
    strs.push_back(s);
    blob.Append(s);
  }
  auto m = sl::LikeMatcher::Compile("%special%req%");
  auto bm1 = sl::MatchBlobBitmap(m, blob.offsets(), blob.size(), blob.data(), 1);
  auto bm4 = sl::MatchBlobBitmap(m, blob.offsets(), blob.size(), blob.data(), 4);
  CHECK(bm1 == bm4);
  auto idx = sl::MatchBlobIndices(m, blob.offsets(), blob.size(), blob.data(), 4);
  CHECK(idx.size() == (strs.size() + 4) / 5);
  for (auto i : idx) CHECK(i % 5 == 0);
}

static void TestStrings() {
  StringBlob b;
  CHECK(b.size() == 0);
  CHECK(b.Append("hello") == 0 && b.Append("") == 1 && b.Append("wor") == 2);
  CHECK(b.View(0) == "hello" && b.View(1).empty() && b.View(2) == "wor");
  CHECK(sizeof(*b.offsets()) == 8);
  StringDict d;
  std::vector<std::string> v;
  for (int i = 0; i < 50000; ++i) v.push_back("k" + std::to_string((i * 7919) % 20011));
  std::unordered_map<std::string, uint32_t> ref;
  for (auto &s : v) {
    uint32_t id = d.Intern(s);
    auto it = ref.emplace(s, id);
    CHECK(it.first->second == id);
    CHECK(d.Get(id) == s);
  }
  CHECK(d.size() == ref.size());
  CHECK(d.Find("nope") == StringDict::kNone);
  CHECK(d.Intern("") != StringDict::kNone && d.Get(d.Find("")).empty());
  auto rank = d.SortedRanks();
  for (uint32_t i = 0; i < d.size(); ++i)
    for (uint32_t j : {0u, 17u, 1234u})
      if (j < d.size()) CHECK((rank[i] < rank[j]) == (d.Get(i) < d.Get(j)));
}

static void TestInt128() {
  using pg = accelerator_codegen::pg_numeric::Int128;
  pg acc = 0;
  i128::AddChecked(acc, i128::Mul64(INT64_MAX, INT64_MAX));
  i128::AddChecked(acc, i128::Mul64(INT64_MAX, INT64_MAX));
  CHECK(acc > 0);
  CHECK(i128::ToString(12345) == "12345");
  bool threw = false;
  pg big = ~(pg(1) << 127);
  try { i128::AddChecked(big, 1); } catch (const std::overflow_error &) { threw = true; }
  CHECK(threw);
  CHECK(accelerator_codegen::pg_numeric::ToText(-12345, 2) == "-123.45");
}

// ---------------------------------------------------------------------------
// Randomized reference tests (std::map / std::set oracles, COW isolation,
// negative / boundary / huge keys). Run under ASan+UBSan on the host.
// ---------------------------------------------------------------------------

// Key pool that mixes small keys of both signs, the dense/sparse boundaries
// (+-2^35, +-2^36) and extreme int64 values.
static std::vector<int64_t> KeyPool(std::mt19937_64 &rng, std::size_t n) {
  std::vector<int64_t> pool = {0, 1, -1, 2, -2, 4095, 4096, -4096, -4097,
                               (INT64_C(1) << 35) - 1, INT64_C(1) << 35, -(INT64_C(1) << 35),
                               -(INT64_C(1) << 35) - 1, (INT64_C(1) << 36), -(INT64_C(1) << 36),
                               (INT64_C(1) << 40) + 5, -(INT64_C(1) << 40) - 5,
                               INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1};
  while (pool.size() < n) {
    switch (rng() % 4) {
    case 0: pool.push_back(int64_t(rng() % 20000) - 10000); break;
    case 1: pool.push_back(int64_t(rng() % 100000000)); break;
    case 2: pool.push_back(-int64_t(rng() % 100000000)); break;
    default: pool.push_back(int64_t(rng())); break;
    }
  }
  return pool;
}

static void TestAnyKeyCowArray() {
  CHECK(AnyKeyCowArray<int32_t>::Unzig(AnyKeyCowArray<int32_t>::Zig(INT64_C(-5))) == -5);
  for (int64_t k : {int64_t(0), int64_t(-1), int64_t(1), -(INT64_C(1) << 35),
                    (INT64_C(1) << 35) - 1}) {
    using A = AnyKeyCowArray<int32_t>;
    CHECK(A::IsDense(k));
    CHECK(A::Unzig(A::Zig(k)) == k);
    CHECK(A::Zig(k) < uint64_t(A::kDenseLimit));
  }
  CHECK(!AnyKeyCowArray<int32_t>::IsDense(INT64_C(1) << 35));
  CHECK(!AnyKeyCowArray<int32_t>::IsDense(-(INT64_C(1) << 35) - 1));

  std::mt19937_64 rng(12345);
  const auto pool = KeyPool(rng, 3000);
  const int32_t kFill = -777;
  // Small chunks and a small shard count exercise directory pages and shards.
  using Arr = AnyKeyCowArray<int32_t, 4, 3>;
  Arr cur(kFill);
  std::map<int64_t, int32_t> ref; // keys whose value != fill
  std::vector<std::pair<Arr, std::map<int64_t, int32_t>>> snaps;
  auto verify = [&](const Arr &a, const std::map<int64_t, int32_t> &r) {
    for (int64_t k : pool) {
      auto it = r.find(k);
      CHECK(a.Get(k) == (it == r.end() ? kFill : it->second));
      CHECK(a.Has(k, kFill) == (it != r.end()));
      CHECK(a.Get(k, 12345) == (a.Find(k) ? a.Get(k) : 12345));
    }
    // ForEach / ForEachOrdered report every non-fill entry exactly once, and
    // ForEachOrdered is strictly ascending.
    std::map<int64_t, int32_t> seen;
    a.ForEach([&](int64_t k, const int32_t &v) {
      if (v != kFill)
        CHECK(seen.emplace(k, v).second);
    });
    CHECK(seen == r);
    std::map<int64_t, int32_t> ordered;
    bool first = true;
    int64_t prev = 0;
    a.ForEachOrdered([&](int64_t k, const int32_t &v) {
      CHECK(first || k > prev);
      first = false;
      prev = k;
      if (v != kFill)
        ordered.emplace(k, v);
    });
    CHECK(ordered == r);
    // The dense scan API (NumChunks/ChunkAt/KeyAt) plus ForEachSparse cover
    // exactly the same entries.
    std::map<int64_t, int32_t> scanned;
    a.ForEachChunk([&](std::size_t ci, const Arr::Chunk &c) {
      CHECK(a.ChunkAt(ci) == &c);
      for (std::size_t i = 0; i < c.size(); ++i)
        if (c[i] != kFill)
          scanned.emplace(Arr::KeyAt(ci, i), c[i]);
    });
    a.ForEachSparse([&](int64_t k, const int32_t &v) {
      if (v != kFill)
        scanned.emplace(k, v);
    });
    CHECK(scanned == r);
  };
  for (int step = 0; step < 40000; ++step) {
    const int64_t k = pool[rng() % pool.size()];
    switch (rng() % 6) {
    case 0:
    case 1: {
      const int32_t v = int32_t(rng() % 50);
      cur.Set(k, v);
      ref[k] = v;
      if (v == kFill) ref.erase(k);
      break;
    }
    case 2: { // restore to absent
      cur.Set(k, kFill);
      ref.erase(k);
      break;
    }
    case 3: {
      const int32_t by = int32_t(rng() % 7);
      cur.Add(k, by);
      const int32_t nv = (ref.count(k) ? ref[k] : kFill) + by;
      if (nv == kFill) ref.erase(k); else ref[k] = nv;
      break;
    }
    case 4: {
      cur.Mutable(k) = 9;
      ref[k] = 9;
      break;
    }
    default:
      if (rng() % 40 == 0 && snaps.size() < 12) {
        snaps.emplace_back(cur, ref); // O(directory) copy; later writes must not leak
      }
      break;
    }
    if (step % 5000 == 4999) {
      verify(cur, ref);
      for (auto &s : snaps)
        verify(s.first, s.second); // snapshots are immutable
    }
  }
  verify(cur, ref);
  CHECK(cur.MemoryBytes() > 0);
  CHECK(cur.SharedChunks() <= cur.AllocatedChunks());

  // COW sharing: a copy shares every chunk; one write unshares exactly one.
  AnyKeyCowArray<uint8_t, 6, 3> a(0);
  for (int64_t k = -3000; k < 3000; ++k) a.Set(k, uint8_t(k & 0x7f) | 1);
  auto b = a;
  CHECK(b.SharedChunks() == b.AllocatedChunks() && b.AllocatedChunks() > 4);
  b.Set(5, 200);
  CHECK(b.SharedChunks() == b.AllocatedChunks() - 1);
  CHECK(a.Get(5) == (5 | 1) && b.Get(5) == 200);
  b.Set(5, 200); // identical write: no clone, no change
  CHECK(b.SharedChunks() == b.AllocatedChunks() - 1);
  auto c = a;
  c.Set(7, a.Get(7)); // no-op write must not clone
  CHECK(c.SharedChunks() == c.AllocatedChunks());
  c.Set(INT64_C(1) << 50, 3); // far key: only the hash shard is touched
  CHECK(c.SharedChunks() == c.AllocatedChunks() && c.Get(INT64_C(1) << 50) == 3);
  CHECK(a.Get(INT64_C(1) << 50) == 0);
  c.Set(INT64_C(1) << 50, 0); // restoring fill removes the far entry
  std::size_t far = 0;
  c.ForEachSparse([&](int64_t, const uint8_t &) { ++far; });
  CHECK(far == 0);

  // A key far from the others must not allocate a directory proportional to
  // its magnitude: key 2^34 with 64-slot chunks.
  AnyKeyCowArray<int64_t, 6, 3> sparse(0);
  sparse.Set(INT64_C(1) << 34, 1);
  sparse.Set(-(INT64_C(1) << 34), 2);
  CHECK(sparse.MemoryBytes() < (INT64_C(1) << 26));
  CHECK(sparse.Get(INT64_C(1) << 34) == 1 && sparse.Get(-(INT64_C(1) << 34)) == 2);

  // Value types without operator== (Mutable path) and heap-owning types.
  struct Rec { int a = 0; std::string s; };
  AnyKeyCowArray<Rec, 3, 3> recs;
  recs.Set(-9, Rec{4, "neg"});
  recs.Mutable(INT64_C(1) << 45).s = "far";
  auto r2 = recs;
  r2.Mutable(-9).s = "changed";
  CHECK(recs.Get(-9).s == "neg" && r2.Get(-9).s == "changed");
  CHECK(recs.Get(INT64_C(1) << 45).s == "far" && recs.Get(77).a == 0);
  AnyKeyCowArray<std::shared_ptr<int>, 4, 3> ptrs;
  ptrs.Set(-5, std::make_shared<int>(5));
  CHECK(*ptrs.Get(-5) == 5 && !ptrs.Get(6));
}

static void TestChunkedCowArrayRandom() {
  std::mt19937_64 rng(7);
  ChunkedCowArray<int32_t, 5> cur(-1);
  std::map<int64_t, int32_t> ref;
  std::vector<std::pair<ChunkedCowArray<int32_t, 5>, std::map<int64_t, int32_t>>> snaps;
  for (int step = 0; step < 20000; ++step) {
    const int64_t k = (rng() % 3 == 0) ? int64_t(rng() % (INT64_C(1) << 24)) : int64_t(rng() % 2000);
    if (rng() % 2000 == 0) CHECK(!cur.Set(-1 - int64_t(rng() % 100), 1));
    if (rng() % 3) { const auto v = int32_t(rng() % 100); cur.Set(k, v); ref[k] = v; }
    else if (rng() % 100 == 0 && snaps.size() < 8) snaps.emplace_back(cur, ref);
    if (step % 4000 == 3999) {
      for (auto &[kk, vv] : ref) CHECK(cur.Get(kk) == vv);
      for (auto &s : snaps)
        for (auto &[kk, vv] : s.second) CHECK(s.first.Get(kk) == vv);
      CHECK(cur.Get(-5) == -1 && cur.Get(INT64_C(1) << 40) == -1);
    }
  }
}

static void TestChunkedBlobMap() {
  std::mt19937_64 rng(99);
  const auto pool = KeyPool(rng, 1500);
  ChunkedBlobMap cur;
  std::map<int64_t, std::string> ref;
  std::vector<std::pair<ChunkedBlobMap, std::map<int64_t, std::string>>> snaps;
  auto rnd_str = [&] {
    std::string s(rng() % 40, 'a');
    for (auto &c : s) c = char(rng());
    if (rng() % 50 == 0) s.assign(300 + rng() % 300, 'z'); // multi-byte varint length
    return s;
  };
  auto verify = [&](const ChunkedBlobMap &m, const std::map<int64_t, std::string> &r) {
    CHECK(m.size() == r.size());
    for (int64_t k : pool) {
      std::string_view v;
      auto it = r.find(k);
      CHECK(m.Get(k, v) == (it != r.end()));
      CHECK(m.Contains(k) == (it != r.end()));
      if (it != r.end()) CHECK(v == it->second);
    }
    std::map<int64_t, std::string> seen;
    m.ForEach([&](int64_t k, std::string_view v) { CHECK(seen.emplace(k, std::string(v)).second); });
    CHECK(seen == r);
    std::map<int64_t, std::string> by_chunk;
    for (std::size_t ci = 0; ci < m.NumChunkSlots(); ++ci)
      m.ForEachInChunk(ci, [&](int64_t k, std::string_view v) { by_chunk.emplace(k, std::string(v)); });
    for (auto &[k, v] : by_chunk) { auto it = r.find(k); CHECK(it != r.end() && it->second == v); }
  };
  for (int step = 0; step < 30000; ++step) {
    const int64_t k = pool[rng() % pool.size()];
    switch (rng() % 5) {
    case 0: case 1: case 2: { std::string v = rnd_str(); cur.Set(k, v); ref[k] = v; break; }
    case 3: cur.Erase(k); ref.erase(k); break;
    default:
      if (rng() % 30 == 0 && snaps.size() < 10) snaps.emplace_back(cur, ref);
    }
    if (step % 6000 == 5999) {
      verify(cur, ref);
      for (auto &s : snaps) verify(s.first, s.second);
    }
  }
  verify(cur, ref);
  // Empty string is a present value, distinct from absent.
  cur.Set(-42, std::string_view());
  std::string_view v = "x";
  CHECK(cur.Get(-42, v) && v.empty());
  cur.Shrink();
  CHECK(cur.Get(-42, v) && v.empty());
  // Heavy overwrite of one chunk compacts the arena instead of growing it.
  ChunkedBlobMap hot;
  for (int i = 0; i < 20000; ++i) hot.Set(3, std::string(100, char('a' + i % 26)));
  CHECK(hot.MemoryBytes() < (1u << 20));
  // Varint helpers.
  std::vector<uint8_t> buf;
  for (int64_t x : {int64_t(0), int64_t(-1), int64_t(63), int64_t(-64), INT64_MAX, INT64_MIN}) PutSignedVarint(buf, x);
  const uint8_t *p = buf.data();
  for (int64_t x : {int64_t(0), int64_t(-1), int64_t(63), int64_t(-64), INT64_MAX, INT64_MIN}) {
    int64_t got = 0;
    CHECK(GetSignedVarint(p, buf.data() + buf.size(), got) && got == x);
  }
  CHECK(p == buf.data() + buf.size());
  const uint8_t trunc[] = {0x80};
  const uint8_t *tp = trunc;
  uint64_t tv = 0;
  CHECK(!GetVarint(tp, trunc + 1, tv));
}

static void TestRecordStore() {
  std::mt19937_64 rng(5);
  const auto pool = KeyPool(rng, 1000);
  ShardedCowRecordStore<4> cur;
  std::map<int64_t, std::string> ref;
  std::vector<std::pair<ShardedCowRecordStore<4>, std::map<int64_t, std::string>>> snaps;
  auto verify = [&](const ShardedCowRecordStore<4> &m, const std::map<int64_t, std::string> &r) {
    CHECK(m.size() == r.size() && m.empty() == r.empty());
    for (int64_t k : pool) {
      ShardedCowRecordStore<4>::View v;
      auto it = r.find(k);
      CHECK(m.Find(k, v) == (it != r.end()) && m.Contains(k) == (it != r.end()));
      if (it != r.end()) CHECK(std::string(reinterpret_cast<const char *>(v.data), v.size) == it->second);
    }
    std::map<int64_t, std::string> seen;
    m.ForEach([&](int64_t k, ShardedCowRecordStore<4>::View v) {
      CHECK(seen.emplace(k, std::string(reinterpret_cast<const char *>(v.data), v.size)).second);
    });
    CHECK(seen == r);
  };
  for (int step = 0; step < 30000; ++step) {
    const int64_t k = pool[rng() % pool.size()];
    switch (rng() % 5) {
    case 0: case 1: case 2: {
      std::string v(rng() % 200, 'q');
      for (auto &c : v) c = char(rng());
      cur.Put(k, v.data(), v.size());
      ref[k] = v;
      break;
    }
    case 3: CHECK(cur.Erase(k) == (ref.erase(k) == 1)); break;
    default:
      if (rng() % 30 == 0 && snaps.size() < 10) snaps.emplace_back(cur, ref);
    }
    if (step % 6000 == 5999) {
      verify(cur, ref);
      for (auto &s : snaps) verify(s.first, s.second);
    }
  }
  verify(cur, ref);
  for (auto &[k, v] : std::map<int64_t, std::string>(ref)) { cur.Erase(k); }
  CHECK(cur.empty() && cur.size() == 0);
}

static void TestKeyedStringColumn() {
  std::mt19937_64 rng(31);
  std::vector<int64_t> pool = KeyPool(rng, 200);
  for (int64_t k = 0; k < 20000; k += 3) pool.push_back(k); // fill several segments
  KeyedStringColumn cur;
  std::map<int64_t, std::string> ref;
  std::vector<std::pair<KeyedStringColumn, std::map<int64_t, std::string>>> snaps;
  const char *words[] = {"alpha", "beta", "gamma", "delta", "forest", "frost", "", "o"};
  auto rnd_str = [&] {
    std::string s;
    for (int i = rng() % 4; i > 0; --i) { s += words[rng() % 8]; s += ' '; }
    return s;
  };
  auto verify = [&](const KeyedStringColumn &c, const std::map<int64_t, std::string> &r,
                    const char *pattern, const std::string &needle) {
    CHECK(c.size() == r.size());
    for (int64_t k : pool) {
      std::string_view v;
      auto it = r.find(k);
      CHECK(c.Get(k, v) == (it != r.end()) && c.Contains(k) == (it != r.end()));
      if (it != r.end()) CHECK(v == it->second);
    }
    const auto m = sl::LikeMatcher::Compile(pattern);
    for (unsigned threads : {1u, 4u}) {
      std::vector<std::vector<int64_t>> per(8);
      c.ForEachMatch(m, threads, [&](unsigned w, int64_t k) { per[w].push_back(k); });
      std::set<int64_t> got;
      for (auto &p : per) for (int64_t k : p) CHECK(got.insert(k).second);
      std::set<int64_t> want;
      for (auto &[k, v] : r) if (v.find(needle) != std::string::npos) want.insert(k);
      CHECK(got == want);
    }
  };
  for (int step = 0; step < 12000; ++step) {
    const int64_t k = pool[rng() % pool.size()];
    switch (rng() % 5) {
    case 0: case 1: case 2: { auto v = rnd_str(); cur.Set(k, v); ref[k] = v; break; }
    case 3: CHECK(cur.Erase(k) == (ref.erase(k) == 1)); break;
    default:
      if (rng() % 100 == 0 && snaps.size() < 6) snaps.emplace_back(cur, ref);
    }
    if (step % 4000 == 3999) {
      verify(cur, ref, "%fo%", "fo");
      for (auto &s : snaps) verify(s.first, s.second, "%ta%", "ta");
    }
  }
  // Bulk AppendNew on a fresh column, enough keys for the multi-threaded path.
  KeyedStringColumn bulk;
  std::map<int64_t, std::string> bref;
  for (int64_t k = -40000; k < 60000; ++k) {
    std::string v = (k % 7 == 0) ? "needle " + std::to_string(k) : "hay " + std::to_string(k);
    bulk.AppendNew(k, v);
    bref[k] = v;
  }
  bulk.AppendNew(INT64_C(1) << 50, "needle far");
  bref[INT64_C(1) << 50] = "needle far";
  verify(bulk, bref, "%needle%", "needle");
}

struct IntLess { bool operator()(int64_t a, int64_t b) const { return a < b; } };
static void TestSortedBlocks() {
  std::mt19937_64 rng(77);
  using Set = CowSortedBlocks<int64_t, IntLess, 8>; // tiny blocks: splits and merges
  Set cur;
  std::set<int64_t> ref;
  std::vector<std::pair<Set, std::set<int64_t>>> snaps;
  auto verify = [&](const Set &s, const std::set<int64_t> &r) {
    CHECK(s.size() == r.size() && s.empty() == r.empty());
    std::vector<int64_t> got;
    s.ForEach([&](int64_t k) { got.push_back(k); return true; });
    CHECK(got == std::vector<int64_t>(r.begin(), r.end()));
    for (int i = 0; i < 50; ++i) {
      const int64_t k = int64_t(rng() % 600) - 300;
      CHECK(s.Contains(k) == (r.count(k) == 1));
    }
    std::size_t n = 0;
    s.ForEach([&](int64_t) { return ++n < 3; }); // early exit
    CHECK(n == std::min<std::size_t>(3, r.size()));
  };
  for (int step = 0; step < 20000; ++step) {
    const int64_t k = int64_t(rng() % 600) - 300;
    const bool grow = (step / 3000) % 2 == 0 ? rng() % 4 != 0 : rng() % 4 == 0;
    if (grow) { cur.Insert(k); ref.insert(k); cur.Insert(k); }
    else CHECK(cur.Erase(k) == (ref.erase(k) == 1));
    if (rng() % 500 == 0 && snaps.size() < 10) snaps.emplace_back(cur, ref);
    if (step % 2500 == 2499) {
      verify(cur, ref);
      for (auto &s : snaps) verify(s.first, s.second);
    }
  }
  // FromSorted, then versions diverge.
  std::vector<int64_t> keys;
  for (int64_t k = -5000; k < 5000; k += 3) keys.push_back(k);
  std::set<int64_t> kref(keys.begin(), keys.end());
  Set built = Set::FromSorted(std::move(keys));
  verify(built, kref);
  Set copy = built;
  CHECK(copy.SharedBlocks() == copy.NumBlocks());
  copy.Insert(2);
  CHECK(copy.SharedBlocks() == copy.NumBlocks() - 1);
  CHECK(!built.Contains(2) && copy.Contains(2));
  CHECK(built.MemoryBytes() > 0);
}

static void TestSortedCountSum() {
  std::mt19937_64 rng(3);
  SortedCountSum cur;
  std::map<int64_t, int64_t> ref;
  std::vector<std::pair<SortedCountSum, std::map<int64_t, int64_t>>> snaps;
  auto above = [](const std::map<int64_t, int64_t> &r, int64_t x) {
    SortedCountSum::Totals t;
    for (auto it = r.upper_bound(x); it != r.end(); ++it) {
      t.count += it->second;
      t.sum += static_cast<SortedCountSum::Int128>(it->first) * it->second;
    }
    return t;
  };
  auto verify = [&](const SortedCountSum &s, const std::map<int64_t, int64_t> &r) {
    for (int i = 0; i < 80; ++i) {
      const int64_t x = (i % 5 == 0) ? (rng() % 2 ? INT64_MIN : INT64_MAX) : int64_t(rng() % 4000) - 2000;
      const auto got = s.SuffixAbove(x), want = above(r, x);
      CHECK(got.count == want.count && got.sum == want.sum);
    }
    const auto tot = s.Total(), want = above(r, INT64_MIN);
    const int64_t min_count = r.count(INT64_MIN) ? r.at(INT64_MIN) : 0;
    CHECK(tot.count == want.count + min_count);
  };
  for (int step = 0; step < 30000; ++step) {
    const int64_t k = (rng() % 50 == 0) ? (rng() % 2 ? INT64_MAX : INT64_MIN + 1) : int64_t(rng() % 3000) - 1500;
    int64_t n = int64_t(rng() % 5) + 1;
    if (rng() % 3 == 0) n = -n;
    const int64_t have = ref.count(k) ? ref[k] : 0;
    const bool ok = cur.Add(k, n);
    CHECK(ok == (have + n >= 0));
    if (ok) { if (have + n == 0) ref.erase(k); else ref[k] = have + n; }
    if (rng() % 1000 == 0 && snaps.size() < 8) snaps.emplace_back(cur, ref);
    if (step % 5000 == 4999) {
      verify(cur, ref);
      for (auto &s : snaps) verify(s.first, s.second);
    }
  }
  CHECK(cur.Add(5, 0));
  CHECK(cur.NumBlocks() >= 1 && cur.MemoryBytes() > 0);
}

static void TestRadixRangeTree() {
  std::mt19937_64 rng(11);
  RadixRangeTree<int64_t> cur;
  const int64_t span = RadixRangeTree<int64_t>::kKeySpan;
  std::vector<std::pair<int64_t, int64_t>> adds; // (key, by)
  RadixRangeTree<int64_t> snap;
  std::vector<std::pair<int64_t, int64_t>> snap_adds;
  auto brute = [&](const std::vector<std::pair<int64_t, int64_t>> &v, int64_t a, int64_t b) {
    int64_t s = 0;
    for (auto &[k, by] : v) if (k >= std::max<int64_t>(a, 0) && k < std::min(b, span)) s += by;
    return s;
  };
  for (int step = 0; step < 4000; ++step) {
    const int64_t k = (rng() % 2) ? int64_t(rng() % 5000) : int64_t(rng() % uint64_t(span));
    const int64_t by = int64_t(rng() % 9) - 3;
    CHECK(RadixRangeTree<int64_t>::InRange(k));
    cur.Add(k, by);
    adds.emplace_back(k, by);
    if (step == 2000) { snap = cur; snap_adds = adds; }
    if (step % 200 == 0) {
      for (int i = 0; i < 20; ++i) {
        int64_t a = int64_t(rng() % uint64_t(span + 200)) - 100, b = int64_t(rng() % uint64_t(span + 200)) - 100;
        if (i % 4 == 0) { a = int64_t(rng() % 6000); b = a + int64_t(rng() % 300); }
        if (a > b) std::swap(a, b);
        CHECK(cur.SumRange(a, b) == brute(adds, a, b));
      }
      CHECK(cur.SumRange(-5, span + 5) == brute(adds, 0, span));
      CHECK(cur.SumRange(10, 10) == 0 && cur.SumRange(20, 10) == 0);
    }
  }
  for (int i = 0; i < 50; ++i) {
    int64_t a = int64_t(rng() % uint64_t(span)), b = int64_t(rng() % uint64_t(span));
    if (a > b) std::swap(a, b);
    CHECK(snap.SumRange(a, b) == brute(snap_adds, a, b));
  }
  RadixRangeTree<__int128> wide;
  wide.Add(5, __int128(INT64_MAX) * 4);
  CHECK(wide.SumRange(0, 6) == __int128(INT64_MAX) * 4);
  CHECK(cur.MemoryBytes() > 0);
}

struct FenCell { int64_t n = 0; int64_t sum = 0; };
static void TestCowFenwick() {
  std::mt19937_64 rng(17);
  CowFenwick<FenCell> cur;
  std::map<int32_t, FenCell> ref;
  CowFenwick<FenCell> snap;
  std::map<int32_t, FenCell> snap_ref;
  auto query = [](const CowFenwick<FenCell> &f, int64_t pos) {
    FenCell t;
    f.VisitPrefix(pos, [&](const FenCell &c) { t.n += c.n; t.sum += c.sum; });
    return t;
  };
  auto brute = [](const std::map<int32_t, FenCell> &r, int64_t pos) {
    FenCell t;
    for (auto &[p, c] : r) if (p <= pos) { t.n += c.n; t.sum += c.sum; }
    return t;
  };
  for (int step = 0; step < 6000; ++step) {
    int32_t pos;
    switch (rng() % 4) {
    case 0: pos = int32_t(rng() % 3000) - 1500; break;
    case 1: pos = rng() % 2 ? INT32_MIN : INT32_MAX; break;
    case 2: pos = INT32_MIN + int32_t(rng() % 50); break;
    default: pos = int32_t(rng()); break;
    }
    const int64_t d = int64_t(rng() % 11) - 4;
    cur.Update(pos, [&](FenCell &c) { c.n += 1; c.sum += d; });
    ref[pos].n += 1; ref[pos].sum += d;
    if (step == 3000) { snap = cur; snap_ref = ref; }
    if (step % 300 == 0) {
      for (int i = 0; i < 25; ++i) {
        const int64_t q = (i == 0) ? int64_t(INT32_MIN) - 1 : (i == 1) ? int64_t(INT32_MAX) + 5
                          : (i == 2) ? INT64_MAX : (i == 3) ? INT64_MIN : int64_t(int32_t(rng() % 4000) - 2000);
        const auto got = query(cur, q), want = brute(ref, q);
        CHECK(got.n == want.n && got.sum == want.sum);
      }
    }
  }
  for (int i = 0; i < 100; ++i) {
    const int64_t q = int32_t(rng());
    const auto got = query(snap, q), want = brute(snap_ref, q);
    CHECK(got.n == want.n && got.sum == want.sum);
  }
  CHECK(cur.NodeCount() > 0 && cur.MemoryBytes(sizeof(FenCell)) > 0);
}

static void TestDecimalText() {
  using accelerator_codegen::aux_state::core::decimal_text::Parse;
  using I = __int128;
  I v = 0;
  CHECK(Parse("123.45", 2, v) && v == 12345);
  CHECK(Parse("-0.5", 2, v) && v == -50);
  CHECK(Parse("+7", 0, v) && v == 7);
  CHECK(Parse("7", 3, v) && v == 7000);
  CHECK(Parse(".5", 1, v) && v == 5);
  CHECK(Parse("1.005", 2, v) && v == 101);   // half away from zero
  CHECK(Parse("-1.005", 2, v) && v == -101);
  CHECK(Parse("1.004999", 2, v) && v == 100);
  CHECK(Parse("0.00", 2, v) && v == 0);
  { I p36 = 1; for (int i = 0; i < 36; ++i) p36 *= 10;
    CHECK(Parse("1000000000000000000000000000000000000", 0, v) && v == p36); }
  CHECK(!Parse("", 2, v));
  CHECK(!Parse("-", 2, v));
  CHECK(!Parse(".", 2, v));
  CHECK(!Parse("1a", 2, v));
  CHECK(!Parse("1.2.3", 2, v));
  CHECK(!Parse("999999999999999999999999999999999999999", 0, v)); // > 127 bits
}

static void TestPgNumericDiv() {
  using namespace accelerator_codegen::pg_numeric;
  Decimal q = DivRound(6, 0, 3, 0);
  CHECK(q.scale >= 0 && q.coeff == Int128(2) * [&] { Int128 p = 1; for (int i = 0; i < q.scale; ++i) p *= 10; return p; }());
  q = DivRound(1, 0, 8, 0);   // 0.125 exactly
  { Int128 p = 1; for (int i = 0; i < q.scale - 3; ++i) p *= 10; CHECK(q.scale >= 3 && q.coeff == 125 * p); }
  q = DivRound(2, 0, 3, 0);   // 0.666..67, rounded half away from zero
  CHECK(q.coeff % 10 == 7);
  q = DivRound(-2, 0, 3, 0);
  CHECK(q.coeff % 10 == -7 && q.coeff < 0);
  q = DivRound(12345, 2, 5, 0); // 123.45 / 5 = 24.69
  { Int128 p = 1; for (int i = 0; i < q.scale - 2; ++i) p *= 10; CHECK(q.scale >= 2 && q.coeff == 2469 * p); }
  q = DivRound(0, 0, 7, 0);
  CHECK(q.coeff == 0);
  bool threw = false;
  try { DivRound(1, 0, 0, 0); } catch (const std::domain_error &) { threw = true; }
  CHECK(threw);
}

int main() {
  TestDenseIdArray();
  TestCow();
  TestFlatHashMap();
  TestDate();
  TestLike();
  TestStrings();
  TestInt128();
  TestAnyKeyCowArray();
  TestChunkedCowArrayRandom();
  TestChunkedBlobMap();
  TestRecordStore();
  TestKeyedStringColumn();
  TestSortedBlocks();
  TestSortedCountSum();
  TestRadixRangeTree();
  TestCowFenwick();
  TestDecimalText();
  TestPgNumericDiv();
  std::puts("test_core_primitives: PASS");
  return 0;
}
