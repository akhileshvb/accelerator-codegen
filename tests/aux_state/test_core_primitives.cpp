// Unit tests for the generic aux-state core primitives. No DBMS required.
#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <unordered_map>

#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/dense_id_array.hpp"
#include "accelerator_codegen/stateful/core/flat_hash_map.hpp"
#include "accelerator_codegen/stateful/core/int128_acc.hpp"
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

int main() {
  TestDenseIdArray();
  TestCow();
  TestFlatHashMap();
  TestDate();
  TestLike();
  TestStrings();
  TestInt128();
  std::puts("test_core_primitives: PASS");
  return 0;
}
