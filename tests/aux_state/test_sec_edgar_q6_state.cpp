// Unit/property test for the Q6 aux state primitives (no DuckDB needed):
// string pool, bridge structure, copy-on-write aggregates, top cache
// maintenance (incremental + rescan), SIMD vs scalar paths.

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "sec_edgar/q6/duckdb/aux/state.hpp"

using namespace accelerator_codegen::aux_state::query;

namespace {

using Rng = std::mt19937_64;

void Require(bool ok, const std::string &msg) {
  if (!ok) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
  }
}

void TestStringPool(Rng &rng) {
  SecEdgarQ6StringPool pool;
  std::map<std::string, uint32_t> model;
  for (int i = 0; i < 20000; ++i) {
    std::string s;
    const size_t len = rng() % 40;
    for (size_t k = 0; k < len; ++k) s.push_back(static_cast<char>(rng() % 256));
    const uint32_t id = pool.Intern(s);
    auto [it, inserted] = model.emplace(s, id);
    Require(inserted || it->second == id, "pool id stable");
    Require(pool.View(id) == s, "pool view");
  }
  for (auto &[s, id] : model) {
    Require(pool.Find(s) == id, "pool find");
  }
  Require(pool.Find("definitely-not-there-\xff\xfe") == 0, "pool miss");
  Require(pool.Intern("") != 0 && pool.Find("") == pool.Intern(""), "empty string");
  Require(pool.Count() == model.size() + (model.count("") ? 0 : 1), "pool count");
}

struct Model {
  std::map<std::string, std::string> cohort; // adsh -> name ("\x01NULL" == NULL)
  struct PreRow {
    std::string adsh, tag, version, plabel; // "\x01NULL" plabel == NULL
  };
  std::vector<PreRow> pre;
  struct NumRow {
    std::string adsh, tag, version;
    double value;
  };
  std::vector<NumRow> nums;
};
const std::string kNull = "\x01NULL";

struct Expected {
  double sum = 0;
  int64_t cnt = 0;
};
using GroupKey = std::tuple<std::string, std::string, std::string>; // name, tag, plabel

std::map<GroupKey, Expected> Naive(const Model &m) {
  std::map<std::tuple<std::string, std::string, std::string>, std::vector<std::string>> bridge;
  for (const auto &p : m.pre) {
    if (m.cohort.count(p.adsh)) bridge[{p.adsh, p.tag, p.version}].push_back(p.plabel);
  }
  std::map<GroupKey, Expected> out;
  for (const auto &n : m.nums) {
    auto it = bridge.find({n.adsh, n.tag, n.version});
    if (it == bridge.end()) continue;
    for (const auto &plabel : it->second) {
      auto &e = out[{m.cohort.at(n.adsh), n.tag, plabel}];
      e.sum += n.value;
      e.cnt += 1;
    }
  }
  return out;
}

bool NameLess(const std::string &a, const std::string &b) {
  if (a == kNull) return false;
  if (b == kNull) return true;
  return a < b;
}

std::vector<std::tuple<GroupKey, Expected>> NaiveTop(const Model &m) {
  auto agg = Naive(m);
  std::vector<std::tuple<GroupKey, Expected>> rows;
  for (auto &[k, e] : agg)
    if (e.cnt != 0) rows.emplace_back(k, e);
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    const auto &ea = std::get<1>(a), &eb = std::get<1>(b);
    if (ea.sum != eb.sum) return ea.sum > eb.sum;
    const auto &ka = std::get<0>(a), &kb = std::get<0>(b);
    if (std::get<0>(ka) != std::get<0>(kb)) return NameLess(std::get<0>(ka), std::get<0>(kb));
    if (std::get<1>(ka) != std::get<1>(kb)) return std::get<1>(ka) < std::get<1>(kb);
    return NameLess(std::get<2>(ka), std::get<2>(kb));
  });
  if (rows.size() > 200) rows.resize(200);
  return rows;
}

void CheckState(const SecEdgarQ6State &state, const Model &m, const std::string &ctx) {
  const auto expected = NaiveTop(m);
  const auto rows = SecEdgarQ6TopRows(state);
  Require(rows.size() == expected.size(),
          ctx + ": row count " + std::to_string(rows.size()) + " vs " + std::to_string(expected.size()));
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto &[key, e] = expected[i];
    const std::string name = rows[i].name_is_null ? kNull : rows[i].name;
    const std::string plabel = rows[i].plabel_is_null ? kNull : rows[i].plabel;
    const double tol = 0.0;
    Require(name == std::get<0>(key) && rows[i].tag == std::get<1>(key) &&
                plabel == std::get<2>(key) && rows[i].cnt == e.cnt &&
                std::fabs(rows[i].total_value - e.sum) <= tol,
            ctx + ": mismatch at row " + std::to_string(i) + " got(" + name + "," + rows[i].tag + "," + plabel + "," +
                std::to_string(rows[i].cnt) + "," + std::to_string(rows[i].total_value) + ") want(" +
                std::get<0>(key) + "," + std::get<1>(key) + "," + std::get<2>(key) + "," + std::to_string(e.cnt) + "," +
                std::to_string(e.sum) + ")");
  }
  // Cache invariant: exactly the active groups with !(sum < lb), sorted.
  std::vector<uint32_t> all;
  SecEdgarQ6CollectAtLeast(state, state.top.lower_bound, all);
  std::vector<uint32_t> cached = state.top.gids;
  std::sort(cached.begin(), cached.end());
  std::sort(all.begin(), all.end());
  Require(cached == all, ctx + ": top cache is not the exact >= lb set");
  for (size_t i = 1; i < state.top.gids.size(); ++i) {
    Require(!SecEdgarQ6GroupBefore(state, state.top.gids[i], state.top.gids[i - 1]),
            ctx + ": top cache order");
  }
  uint64_t active = 0;
  for (uint32_t g = 0; g < state.structure->GroupCount(); ++g) active += state.Count(g) != 0;
  Require(active == state.active_groups, ctx + ": active_groups");
}

void RunScenario(Rng &rng, int adsh_count, int names, int tags, int plabels, int nums, int rounds,
                 bool force_scalar) {
  SecEdgarQ6ForceScalar(force_scalar);
  Model m;
  std::vector<std::string> name_pool, tag_pool, version_pool = {"v1", "v2", "", "us-gaap/2023"},
                                       plabel_pool;
  for (int i = 0; i < names; ++i) name_pool.push_back(i == 0 ? "" : i == 1 ? kNull : "Name" + std::to_string(i));
  for (int i = 0; i < tags; ++i) tag_pool.push_back(i == 0 ? "" : "Tag" + std::to_string(i));
  for (int i = 0; i < plabels; ++i) plabel_pool.push_back(i == 0 ? kNull : i == 1 ? "" : "PL" + std::to_string(i));

  SecEdgarQ6StructureBuilder builder;
  for (int i = 0; i < adsh_count; ++i) {
    const std::string adsh = "ADSH" + std::to_string(i);
    const std::string &name = name_pool[rng() % name_pool.size()];
    m.cohort.emplace(adsh, name);
    builder.AddCohort(adsh, name == kNull ? "" : name, name == kNull);
    if (rng() % 7 == 0) builder.AddCohort(adsh, "other", false); // duplicate: ignored
    const int pre_rows = 1 + static_cast<int>(rng() % 25);
    for (int k = 0; k < pre_rows; ++k) {
      Model::PreRow p{adsh, tag_pool[rng() % tag_pool.size()], version_pool[rng() % version_pool.size()],
                      plabel_pool[rng() % plabel_pool.size()]};
      m.pre.push_back(p);
      builder.AddPre(p.adsh, p.tag, p.version, p.plabel == kNull ? "" : p.plabel, p.plabel == kNull);
      if (rng() % 5 == 0) { // duplicate row (multiplicity 2)
        m.pre.push_back(p);
        builder.AddPre(p.adsh, p.tag, p.version, p.plabel == kNull ? "" : p.plabel, p.plabel == kNull);
      }
    }
  }
  // orphan pre rows (not in cohort) are ignored by the builder.
  builder.AddPre("ORPHAN", "Tag1", "v1", "x", false);
  auto structure = builder.Finish();
  // payload check against the naive bridge
  {
    std::map<std::tuple<std::string, std::string, std::string>, int> keys;
    for (auto &p : m.pre) keys[{p.adsh, p.tag, p.version}]++;
    for (auto &[k, n] : keys) {
      Require(SecEdgarQ6Resolve(*structure, std::get<0>(k), std::get<1>(k), std::get<2>(k)) !=
                  SecEdgarQ6Structure::kNoPayload,
              "bridge hit expected");
    }
    Require(SecEdgarQ6Resolve(*structure, "ORPHAN", "Tag1", "v1") == SecEdgarQ6Structure::kNoPayload,
            "orphan must not resolve");
    Require(SecEdgarQ6Resolve(*structure, "ADSH0", "nope", "v1") == SecEdgarQ6Structure::kNoPayload,
            "unknown tag");
  }

  auto state = std::make_shared<SecEdgarQ6State>(SecEdgarQ6MakeEmptyState(structure));
  SecEdgarQ6Rescan(*state);
  CheckState(*state, m, "empty");

  auto random_num = [&]() {
    Model::NumRow n;
    if (!m.pre.empty() && rng() % 10 < 8) {
      const auto &p = m.pre[rng() % m.pre.size()];
      n = {p.adsh, p.tag, p.version, 0};
    } else {
      n = {"ADSH" + std::to_string(rng() % (adsh_count + 5)), tag_pool[rng() % tag_pool.size()],
           version_pool[rng() % version_pool.size()], 0};
    }
    static const double kv[] = {0, 1, 2, 3, 5, 10, 100, 1000, -1, -5, 0.5, 1e9, -1e9, 7};
    // dyadic rationals keep every sum exact, so ordering never depends on the
    // summation order (twin groups with identical contributions tie exactly).
    n.value = rng() % 10 == 0 ? static_cast<double>(static_cast<int64_t>(rng() % 6400000) - 64000) / 64.0
                              : kv[rng() % (sizeof kv / sizeof *kv)];
    return n;
  };

  // initial load as one big batch
  auto apply = [&](const std::vector<Model::NumRow> &rows, int sign, const std::string &ctx) {
    std::vector<SecEdgarQ6NumRef> refs;
    for (auto &r : rows) refs.push_back({r.adsh, r.tag, r.version, r.value, sign});
    std::vector<SecEdgarQ6GroupDelta> deltas;
    if (refs.size() == 1) {
      SecEdgarQ6ExpandPayload(*state->structure,
                              SecEdgarQ6Resolve(*state->structure, refs[0].adsh, refs[0].tag, refs[0].version),
                              refs[0].value, sign, deltas);
    } else {
      SecEdgarQ6ResolveBatch(*state->structure, refs.data(), refs.size(), deltas);
    }
    auto next = SecEdgarQ6ApplyDeltas(*state, deltas);
    if (next) state = std::make_shared<SecEdgarQ6State>(*next);
    if (sign > 0) {
      for (auto &r : rows) m.nums.push_back(r);
    } else {
      for (auto &r : rows) {
        auto it = std::find_if(m.nums.begin(), m.nums.end(), [&](const Model::NumRow &x) {
          return x.adsh == r.adsh && x.tag == r.tag && x.version == r.version && x.value == r.value;
        });
        Require(it != m.nums.end(), "model delete");
        m.nums.erase(it);
      }
    }
    CheckState(*state, m, ctx);
  };

  {
    std::vector<Model::NumRow> rows;
    for (int i = 0; i < nums; ++i) rows.push_back(random_num());
    apply(rows, +1, "initial load");
  }
  for (int round = 0; round < rounds; ++round) {
    const int kind = static_cast<int>(rng() % 8);
    std::vector<Model::NumRow> rows;
    if (kind <= 3) { // small insert
      for (size_t i = 0, n = 1 + rng() % 3; i < n; ++i) rows.push_back(random_num());
      apply(rows, +1, "round " + std::to_string(round) + " small insert");
    } else if (kind <= 5 && !m.nums.empty()) { // small delete
      for (size_t i = 0, n = 1 + rng() % 3; i < n && !m.nums.empty(); ++i)
        rows.push_back(m.nums[rng() % m.nums.size()]);
      // dedupe so each model row is removed at most once
      std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return std::tie(a.adsh, a.tag, a.version, a.value) < std::tie(b.adsh, b.tag, b.version, b.value);
      });
      rows.erase(std::unique(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
                   return std::tie(a.adsh, a.tag, a.version, a.value) == std::tie(b.adsh, b.tag, b.version, b.value);
                 }),
                 rows.end());
      apply(rows, -1, "round " + std::to_string(round) + " small delete");
    } else if (kind == 6) { // value bump on the current best group (stress cache head)
      const auto top = SecEdgarQ6TopRows(*state, 1);
      if (!top.empty() && !m.pre.empty()) {
        // add a big row on a random existing key (changes ordering near the head)
        auto n = random_num();
        n.value = 1e10 * static_cast<double>(1 + rng() % 3);
        rows.push_back(n);
        apply(rows, +1, "round " + std::to_string(round) + " head bump");
      }
    } else { // big batch (> kIncrementalTouchedMax groups) or big delete
      const bool del = rng() % 2 && m.nums.size() > 3000;
      if (del) {
        std::shuffle(m.nums.begin(), m.nums.end(), rng);
        rows.assign(m.nums.begin(), m.nums.begin() + 2500);
        apply(rows, -1, "round " + std::to_string(round) + " big delete");
      } else {
        for (int i = 0; i < 3000; ++i) rows.push_back(random_num());
        apply(rows, +1, "round " + std::to_string(round) + " big insert");
      }
    }
  }
  SecEdgarQ6ForceScalar(false);
}

void TestSimdMatchesScalar(Rng &rng) {
  // Same deltas through both dispatch modes must yield identical caches.
  SecEdgarQ6StructureBuilder builder;
  for (int i = 0; i < 500; ++i) {
    builder.AddCohort("A" + std::to_string(i), "N" + std::to_string(i % 40), false);
    for (int k = 0; k < 30; ++k)
      builder.AddPre("A" + std::to_string(i), "T" + std::to_string(k), "v", "P" + std::to_string(k % 5), false);
  }
  auto structure = builder.Finish();
  auto make = [&](bool scalar) {
    SecEdgarQ6ForceScalar(scalar);
    Rng r(42);
    auto state = SecEdgarQ6MakeEmptyState(structure);
    for (uint32_t g = 0; g < structure->GroupCount(); ++g)
      if (r() % 3) SecEdgarQ6SetGroup(state, g, static_cast<double>(r() % 1000) - 200.0, 1 + r() % 5);
    SecEdgarQ6Rescan(state);
    return state;
  };
  const auto a = make(true);
  const auto b = make(false);
  SecEdgarQ6ForceScalar(false);
  Require(a.top.gids == b.top.gids && a.active_groups == b.active_groups, "scalar/simd rescan equal");
  (void)rng;
}

} // namespace

int main() {
  Rng rng(0xC0FFEE);
  std::cout << "avx2=" << SecEdgarQ6UsesAvx2() << "\n";
  TestStringPool(rng);
  TestSimdMatchesScalar(rng);
  // small: complete-mode cache; large: sampled rescan + incremental maintenance
  RunScenario(rng, 30, 6, 5, 4, 300, 150, false);
  RunScenario(rng, 30, 6, 5, 4, 300, 150, true);
  RunScenario(rng, 1500, 200, 40, 8, 30000, 60, false);
  RunScenario(rng, 1500, 200, 40, 8, 30000, 30, true);
  std::cout << "PASS test_sec_edgar_q6_state\n";
  return 0;
}
