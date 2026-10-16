// Unit tests for SEC EDGAR Q1 aux internals: adsh key packing (SIMD vs scalar),
// copy-on-write refcount chunks (incl. counts past the 8-bit escape),
// dictionary compaction, builder merge, state encode/decode round trips, and
// the generality contract: 64-bit ids/counts, non-canonical accession strings
// (exact, copy-on-write, compacting), exact 128-bit line sums, DuckDB-exact AVG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "sec_edgar/q1/common/adsh_key.hpp"
#include "sec_edgar/q1/duckdb/aux/duckdb_adsh_key.hpp"
#include "sec_edgar/q1/common/module.hpp"

using namespace accelerator_codegen::aux_state::query;

namespace {

using accelerator_codegen::aux_state::core::AuxStateStore;
using accelerator_codegen::aux_state::core::VisibilityToken;
using accelerator_codegen::aux_state::core::AuxEngineServices;

// Reports a worker count; the default ParallelFor runs the slices in order.
class StubServices final : public AuxEngineServices {
public:
  explicit StubServices(std::size_t workers) : workers(workers) {}
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { std::abort(); }
  std::size_t WorkerCount() const override { return workers; }

private:
  std::size_t workers;
};

const StubServices kSerial(1);

void TestAdshKey() {
  std::mt19937_64 rng(7);
  std::vector<std::string> inputs;
  for (int i = 0; i < 20000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010llu-%02d-%06d", static_cast<unsigned long long>(rng() % 10000000000ULL),
                  static_cast<int>(rng() % 100), static_cast<int>(rng() % 1000000));
    std::string s = buf;
    switch (rng() % 12) {
    case 0: s[rng() % 20] = static_cast<char>(rng() % 256); break; // arbitrary corruption
    case 1: s[10] = '_'; break;
    case 2: s[13] = '0'; break;
    case 3: s.pop_back(); break;
    case 4: s += "0"; break;
    case 5: s[18] = ':'; break;  // '9' + 1
    case 6: s[0] = '/'; break;   // '0' - 1
    case 7: s[19] = ' '; break;
    default: break;
    }
    inputs.push_back(std::move(s));
  }
  inputs.push_back("0000000000-00-000000");
  inputs.push_back("9999999999-99-999999");
  inputs.push_back("");
  // batch (via string_t) vs scalar
  std::vector<duckdb::string_t> values;
  for (const auto &s : inputs) {
    values.emplace_back(s.data(), static_cast<uint32_t>(s.size()));
  }
  std::vector<uint64_t> simd(values.size()), scalar(values.size());
  q1::ParseAdshKeyBatch(values.data(), nullptr, values.size(), simd.data());
  q1::ParseAdshKeyBatchScalar(values.data(), nullptr, values.size(), scalar.data());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    uint64_t k = 0;
    const bool ok = q1::ParseAdshKey(inputs[i].data(), inputs[i].size(), k);
    uint64_t ks = 0;
    const bool oks = q1::ParseAdshKeyScalar(inputs[i].data(), inputs[i].size(), ks);
    assert(ok == oks);
    assert(!ok || k == ks);
    assert(simd[i] == scalar[i]);
    assert((simd[i] == q1::kInvalidAdshKey) == !oks);
  }
  // selection vector variant
  std::vector<uint32_t> sel = {5, 3, 1, 100};
  std::vector<uint64_t> a(4), b(4);
  q1::ParseAdshKeyBatch(values.data(), sel.data(), sel.size(), a.data());
  q1::ParseAdshKeyBatchScalar(values.data(), sel.data(), sel.size(), b.data());
  assert(a == b);
  std::cout << "adsh key: simd=" << q1::AdshKeySimdAvailable() << " ok\n";
}

SecEdgarQ1TxnBuffer::Delta Ref(uint32_t group, uint64_t key, int32_t d) {
  SecEdgarQ1TxnBuffer::Delta delta;
  delta.group = group;
  delta.key = key;
  delta.d_cnt = d;
  delta.d_refc = d;
  return delta;
}

void TestCowPagesAndBigCounts() {
  SecEdgarQ1State state;
  {
    SecEdgarQ1TxnBuffer buffer;
    const auto g = buffer.InternGroup("BS", "H", false);
    for (uint64_t k = 1; k <= 5000; ++k) { // forces dictionary compaction (>2048 ids)
      buffer.AddDelta(Ref(g, k * 7919, 1));
    }
    assert(ApplySecEdgarQ1Deltas(kSerial, state, buffer));
  }
  assert(state.groups.size() == 1 && state.groups[0].cnt == 5000 && state.groups[0].distinct == 5000);
  assert(state.dict && state.dict->keys.size() >= 2048);
  for (uint64_t k = 1; k <= 5000; ++k) {
    assert(state.FindId(k * 7919) != q1::kNoId);
  }
  assert(state.FindId(3) == q1::kNoId);

  // copy-on-write: a published copy must not observe later changes
  const SecEdgarQ1State snapshot = state;
  {
    SecEdgarQ1TxnBuffer buffer;
    const auto g = buffer.InternGroup("BS", "H", false);
    SecEdgarQ1TxnBuffer::Delta big = Ref(g, 7919, 70000); // pushes a refcount past the 8-bit escape
    buffer.AddDelta(big);
    assert(ApplySecEdgarQ1Deltas(kSerial, state, buffer));
  }
  assert(snapshot.groups[0].cnt == 5000);
  assert(state.groups[0].cnt == 75000);
  assert(state.groups[0].distinct == 5000);
  const auto id = state.FindId(7919);
  assert(state.groups[0].refs->Get(id) == 70001);
  assert(snapshot.groups[0].refs->Get(id) == 1);
  {
    SecEdgarQ1TxnBuffer buffer;
    const auto g = buffer.InternGroup("BS", "H", false);
    buffer.AddDelta(Ref(g, 7919, -70000));
    buffer.AddDelta(Ref(g, 7919 * 2, -1)); // another key down to zero
    assert(ApplySecEdgarQ1Deltas(kSerial, state, buffer));
  }
  assert(state.groups[0].refs->Get(id) == 1);
  assert(state.groups[0].distinct == 4999);
  assert(state.groups[0].refs->low.Get(static_cast<int64_t>(id)) == 1); // back below the escape
  // underflow / unknown key -> rejected
  {
    SecEdgarQ1State copy = state;
    SecEdgarQ1TxnBuffer buffer;
    const auto g = buffer.InternGroup("BS", "H", false);
    buffer.AddDelta(Ref(g, 7919, -2));
    assert(!ApplySecEdgarQ1Deltas(kSerial, copy, buffer));
  }
  {
    SecEdgarQ1State copy = state;
    SecEdgarQ1TxnBuffer buffer;
    const auto g = buffer.InternGroup("BS", "H", false);
    buffer.AddDelta(Ref(g, 42, -1));
    assert(!ApplySecEdgarQ1Deltas(kSerial, copy, buffer));
  }
  // encode / decode
  const auto bytes = EncodeSecEdgarQ1State(state);
  SecEdgarQ1State decoded;
  assert(DecodeSecEdgarQ1State(bytes, decoded));
  assert(EncodeSecEdgarQ1State(decoded) == bytes);
  assert(decoded.groups[0].distinct == state.groups[0].distinct);
  std::cout << "cow/big/encode ok (encoded " << bytes.size() << " bytes)\n";
}

void TestBuilderMerge() {
  std::mt19937_64 rng(11);
  q1::Builder whole, a, b;
  for (int i = 0; i < 20000; ++i) {
    SecEdgarQ1PreInput in;
    in.stmt = (rng() % 3 == 0) ? "BS" : "IS";
    in.stmt_is_null = rng() % 50 == 0;
    in.rfile = (rng() % 4 == 0) ? "X" : "H";
    in.rfile_is_null = rng() % 40 == 0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010d-22-%06d", static_cast<int>(rng() % 500), static_cast<int>(rng() % 3));
    in.adsh = rng() % 30 == 0 ? "weird-" + std::to_string(rng() % 5) : std::string(buf);
    in.adsh_is_null = rng() % 60 == 0;
    in.line = static_cast<int32_t>(rng() % 100);
    in.line_is_null = rng() % 20 == 0;
    whole.ConsumeRow(in);
    (i % 2 ? a : b).ConsumeRow(in);
  }
  a.Merge(std::move(b));
  const auto s1 = whole.Finish();
  const auto s2 = a.Finish();
  const auto o1 = BuildSecEdgarQ1Output(s1);
  const auto o2 = BuildSecEdgarQ1Output(s2);
  assert(o1.size() == o2.size());
  for (std::size_t i = 0; i < o1.size(); ++i) {
    assert(o1[i].stmt == o2[i].stmt && o1[i].rfile == o2[i].rfile && o1[i].rfile_is_null == o2[i].rfile_is_null);
    assert(o1[i].cnt == o2[i].cnt && o1[i].num_filings == o2[i].num_filings);
    assert(o1[i].avg_is_null == o2[i].avg_is_null && o1[i].avg_line_num == o2[i].avg_line_num);
  }
  std::cout << "builder merge ok\n";
}

// Non-canonical accession strings take the string-dictionary path: they must be
// exact (distinct strings stay distinct), copy-on-write, and survive overlay
// compaction (> 1024 strings) and delete.
void TestOtherAccessions() {
  SecEdgarQ1State state;
  auto add = [&](SecEdgarQ1State &st, int from, int to, int sign) {
    SecEdgarQ1TxnBuffer buffer;
    const auto g = buffer.InternGroup("IS", "", true); // NULL rfile group
    for (int i = from; i < to; ++i) {
      // Same digits as a canonical accession with one char changed: must not alias.
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%010d-22-%06dx", i % 977, i);
      SecEdgarQ1TxnBuffer::Delta d = Ref(g, buffer.InternAdsh(buf), sign);
      buffer.AddDelta(d);
    }
    return ApplySecEdgarQ1Deltas(kSerial, st, buffer);
  };
  assert(add(state, 0, 3000, 1));
  assert(state.groups.size() == 1 && state.groups[0].distinct == 3000 && state.others.size() == 3000);
  const SecEdgarQ1State snapshot = state;
  assert(add(state, 3000, 3100, 1));
  assert(state.groups[0].distinct == 3100 && snapshot.groups[0].distinct == 3000);
  assert(snapshot.others.size() == 3000 && state.others.size() == 3100);
  assert(snapshot.others.Find("0000000999-22-000999x") == q1::OtherIds::kNone);
  assert(add(state, 0, 500, -1));
  assert(state.groups[0].distinct == 2600 && state.groups[0].cnt == 2600);
  // deleting a string never inserted is rejected
  {
    SecEdgarQ1State copy = state;
    SecEdgarQ1TxnBuffer buffer;
    const auto g = buffer.InternGroup("IS", "", true);
    buffer.AddDelta(Ref(g, buffer.InternAdsh("never-seen"), -1));
    assert(!ApplySecEdgarQ1Deltas(kSerial, copy, buffer));
  }
  const auto bytes = EncodeSecEdgarQ1State(state);
  SecEdgarQ1State decoded;
  assert(DecodeSecEdgarQ1State(bytes, decoded));
  assert(EncodeSecEdgarQ1State(decoded) == bytes);
  assert(decoded.others.size() == state.others.size());
  std::cout << "non-canonical accessions ok\n";
}

// Keys across the whole canonical range and counts past 32 bits are exact. Ids
// are 64-bit (a unit test cannot afford the ~16 B per 512 ids directory that
// ids beyond 2^32 would need, so that width is checked statically).
void TestWideKeysAndIds() {
  static_assert(sizeof(q1::Id) == 8 && sizeof(q1::GroupData::cnt) == 8, "ids and counts are 64-bit");
  SecEdgarQ1State state;
  SecEdgarQ1TxnBuffer buffer;
  const auto g = buffer.InternGroup("BS", "H", false);
  const uint64_t keys[] = {0, 1, 999999999999999999ULL, 999999999999999998ULL, 500000000000000000ULL};
  for (const auto key : keys) {
    buffer.AddDelta(Ref(g, key, 1));
    buffer.AddDelta(Ref(g, key, 1));
  }
  assert(ApplySecEdgarQ1Deltas(kSerial, state, buffer));
  assert(state.groups[0].distinct == 5 && state.groups[0].cnt == 10);
  std::vector<q1::Id> ids;
  for (const auto key : keys) {
    const auto id = state.FindId(key);
    assert(id != q1::kNoId);
    assert(state.groups[0].refs->Get(id) == 2);
    ids.push_back(id);
  }
  assert(state.FindId(2) == q1::kNoId);
  // a count past the 8-bit escape and past 32 bits stays exact
  SecEdgarQ1TxnBuffer big;
  const auto g2 = big.InternGroup("BS", "H", false);
  SecEdgarQ1TxnBuffer::Delta d = Ref(g2, 1, 1);
  d.d_cnt = 0;
  d.d_refc = 2147483000;
  big.AddDelta(d);
  big.AddDelta(d);
  big.AddDelta(d);
  assert(ApplySecEdgarQ1Deltas(kSerial, state, big));
  assert(state.groups[0].refs->Get(ids[1]) == 2 + 3ULL * 2147483000ULL);
  assert(state.groups[0].refs->low.Get(static_cast<int64_t>(ids[1])) == q1::kEscape);
  const auto bytes = EncodeSecEdgarQ1State(state);
  SecEdgarQ1State decoded;
  assert(DecodeSecEdgarQ1State(bytes, decoded));
  assert(decoded.groups[0].refs->Get(ids[1]) == 2 + 3ULL * 2147483000ULL);
  assert(EncodeSecEdgarQ1State(decoded) == bytes);
  std::cout << "wide keys/ids ok\n";
}

// SUM(line) is accumulated in 128 bits (DuckDB uses a HUGEINT) and AVG is
// finalized exactly as DuckDB does: long double sum / count rounded to double.
void TestExactLineSum() {
  SecEdgarQ1State state;
  SecEdgarQ1TxnBuffer buffer;
  const auto g = buffer.InternGroup("BS", "H", false);
  for (int i = 0; i < 3; ++i) {
    SecEdgarQ1TxnBuffer::Delta d;
    d.group = g;
    d.d_cnt = 1;
    d.d_line_cnt = 1;
    d.d_line_sum = 4000000000000000000LL; // 3 x 4e18 overflows int64
    buffer.AddDelta(d);
  }
  assert(ApplySecEdgarQ1Deltas(kSerial, state, buffer));
  const q1::Int128 expected = static_cast<q1::Int128>(4000000000000000000LL) * 3;
  assert(state.groups[0].line_sum == expected);
  const auto bytes = EncodeSecEdgarQ1State(state);
  SecEdgarQ1State decoded;
  assert(DecodeSecEdgarQ1State(bytes, decoded));
  assert(decoded.groups[0].line_sum == expected);
  const auto out = BuildSecEdgarQ1Output(decoded);
  assert(out.size() == 1 && out[0].avg_line_num == 4e18);

  // AvgLine == long double division; plain double division can differ.
  std::mt19937_64 rng(5);
  int differs = 0;
  for (int i = 0; i < 200000; ++i) {
    const int64_t sum = static_cast<int64_t>(rng() % 100000000000ULL) - 50000000000LL;
    const int64_t count = 1 + static_cast<int64_t>(rng() % 1000000);
    const double exact = static_cast<double>(static_cast<long double>(sum) / static_cast<long double>(count));
    assert(q1::AvgLine(sum, count) == exact);
    differs += exact != static_cast<double>(sum) / static_cast<double>(count);
  }
  std::cout << "exact line sum ok (double division differs from DuckDB's in " << differs << " of 200000)\n";
}

} // namespace

int main() {
  TestAdshKey();
  TestCowPagesAndBigCounts();
  TestBuilderMerge();
  TestOtherAccessions();
  TestWideKeysAndIds();
  TestExactLineSum();
  std::cout << "test_sec_edgar_q1_units OK\n";
  return 0;
}
