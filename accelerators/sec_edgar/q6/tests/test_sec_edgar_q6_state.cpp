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

#include "catalog/query_catalog.hpp"
#include "eval/harness/param_sets.hpp"
#include "sec_edgar/q6/duckdb/aux/state.hpp"
#include "sec_edgar/q6/tests/q6_domain.hpp"

using namespace accelerator_codegen::aux_state::query;

namespace {

using accelerator_codegen::aux_state::core::AuxEngineServices;
using accelerator_codegen::aux_state::core::AuxStateStore;
using accelerator_codegen::aux_state::core::VisibilityToken;

// Reports several workers; the default ParallelFor runs the slices in order.
class StubServices final : public AuxEngineServices {
public:
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { std::abort(); }
  std::size_t WorkerCount() const override { return 4; }
};

const StubServices kServices;

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
  struct Cohort {
    std::string name; // kNull == NULL
    int32_t fy;
  };
  std::map<std::string, Cohort> cohort;
  struct PreRow {
    std::string adsh, tag, version, stmt, plabel; // "\x01NULL" plabel == NULL
  };
  std::vector<PreRow> pre;
  struct NumRow {
    std::string adsh, tag, version, uom;
    double value;
  };
  std::vector<NumRow> nums;
};
const std::string kNull = "\x01NULL";

struct Expected {
  double sum = 0;
  int64_t cnt = 0;
};
using Combo = std::tuple<int32_t, std::string, std::string>; // fy, stmt, uom
using GroupKey = std::tuple<std::string, std::string, std::string>; // name, tag, plabel

std::map<Combo, std::map<GroupKey, Expected>> Naive(const Model &m) {
  std::map<std::tuple<std::string, std::string, std::string>,
           std::vector<std::pair<std::string, std::string>>> bridge;
  for (const auto &p : m.pre) {
    if (m.cohort.count(p.adsh)) bridge[{p.adsh, p.tag, p.version}].emplace_back(p.stmt, p.plabel);
  }
  std::map<Combo, std::map<GroupKey, Expected>> out;
  for (const auto &n : m.nums) {
    auto it = bridge.find({n.adsh, n.tag, n.version});
    if (it == bridge.end()) continue;
    const auto &c = m.cohort.at(n.adsh);
    for (const auto &[stmt, plabel] : it->second) {
      auto &e = out[{c.fy, stmt, n.uom}][{c.name, n.tag, plabel}];
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

std::vector<std::tuple<GroupKey, Expected>> NaiveTop(const std::map<GroupKey, Expected> &agg) {
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

using accelerator_codegen::aux_state::q6_tests::Domain;

// The values the scenarios draw from: those of the drawn parameter sets; `cell` is the first set.
const Domain &Drawn() {
  static const Domain domain(accelerator_codegen::aux_state::harness::ArgumentSets(
      accelerator_codegen::aux_state::engine::LookupSharedQuery("sec_edgar", 6)));
  return domain;
}
struct Cell {
  int32_t fy;
  std::string stmt, uom;
};
Cell FirstCell() {
  const auto &set = Drawn().sets.front();
  return {static_cast<int32_t>(Domain::FyOf(set)), Domain::StmtOf(set),
          Domain::UomOf(set)};
}

void CheckCombo(const SecEdgarQ6State &state, const std::map<GroupKey, Expected> *agg,
                int32_t fy, const std::string &stmt, const std::string &uom, const std::string &ctx) {
  const auto expected = agg ? NaiveTop(*agg) : std::vector<std::tuple<GroupKey, Expected>>{};
  const auto rows = SecEdgarQ6TopRows(state, fy, stmt, uom);
  if (rows.size() != expected.size()) {
    for (const auto &[key, e] : expected) {
      bool found = false;
      for (const auto &r : rows)
        found |= (r.name_is_null ? kNull : r.name) == std::get<0>(key) && r.tag == std::get<1>(key) &&
                 (r.plabel_is_null ? kNull : r.plabel) == std::get<2>(key);
      if (!found) {
        const auto &st = state.structure;
        for (uint32_t c = 0; c < st.ComboCount(); ++c) {
          const auto g = st.FindGroup(st.names.Find(std::get<0>(key)), st.tags.Find(std::get<1>(key)), 0, c);
          if (g != SecEdgarQ6Structure::kNoPayload)
            std::cerr << "  group in combo " << c << " fy=" << st.combos->keys[c].fy << " stmt="
                      << st.stmts.View(st.combos->keys[c].stmt) << " uom=" << st.uoms.View(st.combos->keys[c].uom)
                      << " gid=" << g << " sum=" << state.Sum(g) << " cnt=" << state.Count(g) << "\n";
        }
      }
      if (!found)
        std::cerr << "missing (" << std::get<0>(key) << "," << std::get<1>(key) << "," << std::get<2>(key)
                  << ") sum=" << e.sum << " cnt=" << e.cnt << "\n";
    }
  }
  Require(rows.size() == expected.size(),
          ctx + " (" + std::to_string(fy) + "," + stmt + "," + uom + "): row count " + std::to_string(rows.size()) + " vs " + std::to_string(expected.size()));
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto &[key, e] = expected[i];
    const std::string name = rows[i].name_is_null ? kNull : rows[i].name;
    const std::string plabel = rows[i].plabel_is_null ? kNull : rows[i].plabel;
    Require(name == std::get<0>(key) && rows[i].tag == std::get<1>(key) && rows[i].stmt == stmt &&
                plabel == std::get<2>(key) && rows[i].cnt == e.cnt && rows[i].total_value == e.sum,
            ctx + ": mismatch at row " + std::to_string(i) + " got(" + name + "," + rows[i].tag + "," + plabel + "," +
                std::to_string(rows[i].cnt) + "," + std::to_string(rows[i].total_value) + ") want(" +
                std::get<0>(key) + "," + std::get<1>(key) + "," + std::get<2>(key) + "," + std::to_string(e.cnt) + "," +
                std::to_string(e.sum) + ")");
  }
}

void CheckState(const SecEdgarQ6State &state, const Model &m, const std::string &ctx) {
  const auto naive = Naive(m);
  // every populated combo plus a few absent ones, with random-ish edge values
  for (const auto &[combo, agg] : naive)
    CheckCombo(state, &agg, std::get<0>(combo), std::get<1>(combo), std::get<2>(combo), ctx);
  const auto cell = FirstCell();
  const auto outside_fy = *std::min_element(Drawn().fys.begin(), Drawn().fys.end()) - 4;
  CheckCombo(state, nullptr, static_cast<int32_t>(outside_fy), cell.stmt, cell.uom, ctx);
  CheckCombo(state, nullptr, cell.fy, cell.stmt + "~", cell.uom, ctx);
  CheckCombo(state, nullptr, cell.fy, cell.stmt, cell.uom + "~", ctx);
  CheckCombo(state, nullptr, int64_t(1) << 40, cell.stmt, cell.uom, ctx);
  // Cache invariant per combo: exactly the active groups with !(sum < lb), sorted.
  for (uint32_t c = 0; c < state.structure.ComboCount(); ++c) {
    const auto &top = state.Top(c);
    std::vector<uint32_t> all;
    SecEdgarQ6CollectAtLeast(state, c, top.lower_bound, all);
    std::vector<uint32_t> cached = top.gids;
    std::sort(cached.begin(), cached.end());
    std::sort(all.begin(), all.end());
    Require(cached == all, ctx + ": top cache is not the exact >= lb set");
    for (size_t i = 1; i < top.gids.size(); ++i) {
      Require(!SecEdgarQ6GroupBefore(state, top.gids[i], top.gids[i - 1]), ctx + ": top cache order");
    }
  }
  uint64_t active = 0;
  for (uint32_t g = 0; g < state.structure.GroupCount(); ++g) active += state.Count(g) != 0;
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
  const auto &drawn = Drawn();
  const auto rnd_stmt = [&]() { return drawn.stmts[rng() % drawn.stmts.size()]; };
  const auto rnd_uom = [&]() { return drawn.uoms[rng() % drawn.uoms.size()]; };
  const auto rnd_fy = [&]() { return static_cast<int32_t>(drawn.fys[rng() % drawn.fys.size()]); };

  SecEdgarQ6StructureBuilder builder;
  for (int i = 0; i < adsh_count; ++i) {
    const std::string adsh = "ADSH" + std::to_string(i);
    const std::string &name = name_pool[rng() % name_pool.size()];
    const int32_t fy = rnd_fy();
    m.cohort.emplace(adsh, Model::Cohort{name, fy});
    builder.AddCohort(adsh, fy, name == kNull ? "" : name, name == kNull);
    if (rng() % 7 == 0) builder.AddCohort(adsh, 1999, "other", false); // duplicate: ignored
    const int pre_rows = 1 + static_cast<int>(rng() % 25);
    for (int k = 0; k < pre_rows; ++k) {
      Model::PreRow p{adsh, tag_pool[rng() % tag_pool.size()], version_pool[rng() % version_pool.size()],
                      rnd_stmt(), plabel_pool[rng() % plabel_pool.size()]};
      m.pre.push_back(p);
      builder.AddPre(p.adsh, p.tag, p.version, p.stmt, p.plabel == kNull ? "" : p.plabel, p.plabel == kNull);
      if (rng() % 5 == 0) { // duplicate row (multiplicity 2)
        m.pre.push_back(p);
        builder.AddPre(p.adsh, p.tag, p.version, p.stmt, p.plabel == kNull ? "" : p.plabel, p.plabel == kNull);
      }
    }
  }
  // orphan pre rows (not in cohort) are ignored by the builder.
  builder.AddPre("ORPHAN", "Tag1", "v1", rnd_stmt(), "x", false);
  auto structure = builder.Finish();
  {
    std::map<std::tuple<std::string, std::string, std::string>, int> keys;
    for (auto &p : m.pre) keys[{p.adsh, p.tag, p.version}]++;
    for (auto &[k, n] : keys) {
      const auto hit = SecEdgarQ6Resolve(structure, std::get<0>(k), std::get<1>(k), std::get<2>(k));
      Require(hit.Found() && static_cast<int>(hit.count) == n, "bridge hit expected");
    }
    Require(!SecEdgarQ6Resolve(structure, "ORPHAN", "Tag1", "v1").Found(), "orphan must not resolve");
    Require(!SecEdgarQ6Resolve(structure, "ADSH0", "nope", "v1").Found(), "unknown tag");
  }

  auto state = std::make_shared<SecEdgarQ6State>(SecEdgarQ6MakeEmptyState(structure));
  SecEdgarQ6Rescan(*state);
  CheckState(*state, m, "empty");

  auto random_num = [&]() {
    Model::NumRow n;
    if (!m.pre.empty() && rng() % 10 < 8) {
      const auto &p = m.pre[rng() % m.pre.size()];
      n = {p.adsh, p.tag, p.version, rnd_uom(), 0};
    } else {
      n = {"ADSH" + std::to_string(rng() % (adsh_count + 5)), tag_pool[rng() % tag_pool.size()],
           version_pool[rng() % version_pool.size()], rnd_uom(), 0};
    }
    static const double kv[] = {0, 1, 2, 3, 5, 10, 100, 1000, -1, -5, 0.5, 1e9, -1e9, 7};
    // dyadic rationals keep every sum exact, so ordering never depends on the
    // summation order (twin groups with identical contributions tie exactly).
    n.value = rng() % 10 == 0 ? static_cast<double>(static_cast<int64_t>(rng() % 6400000) - 64000) / 64.0
                              : kv[rng() % (sizeof kv / sizeof *kv)];
    return n;
  };

  auto apply = [&](const std::vector<Model::NumRow> &rows, int sign, const std::string &ctx) {
    std::vector<SecEdgarQ6NumRef> refs;
    for (auto &r : rows) refs.push_back({r.adsh, r.tag, r.version, r.uom, r.value, sign});
    bool overflow = false;
    auto next = SecEdgarQ6ApplyNums(kServices, *state, refs.data(), refs.size(), &overflow);
    if (next) state = std::make_shared<SecEdgarQ6State>(*next);
    if (sign > 0) {
      for (auto &r : rows) m.nums.push_back(r);
    } else {
      for (auto &r : rows) {
        auto it = std::find_if(m.nums.begin(), m.nums.end(), [&](const Model::NumRow &x) {
          return x.adsh == r.adsh && x.tag == r.tag && x.version == r.version && x.uom == r.uom &&
                 x.value == r.value;
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
    state->structure.base_group_count = state->structure.group_count; // as a full build does
  }
  const auto key_of = [](const Model::NumRow &a) {
    return std::tie(a.adsh, a.tag, a.version, a.uom, a.value);
  };
  for (int round = 0; round < rounds; ++round) {
    const int kind = static_cast<int>(rng() % 11);
    std::vector<Model::NumRow> rows;
    if (kind <= 3) { // small insert
      for (size_t i = 0, n = 1 + rng() % 3; i < n; ++i) rows.push_back(random_num());
      apply(rows, +1, "round " + std::to_string(round) + " small insert");
    } else if (kind <= 5 && !m.nums.empty()) { // small delete
      for (size_t i = 0, n = 1 + rng() % 3; i < n && !m.nums.empty(); ++i)
        rows.push_back(m.nums[rng() % m.nums.size()]);
      std::sort(rows.begin(), rows.end(), [&](const auto &a, const auto &b) { return key_of(a) < key_of(b); });
      rows.erase(std::unique(rows.begin(), rows.end(),
                             [&](const auto &a, const auto &b) { return key_of(a) == key_of(b); }),
                 rows.end());
      apply(rows, -1, "round " + std::to_string(round) + " small delete");
    } else if (kind == 6) { // value bump near the head of some combo
      if (!m.pre.empty()) {
        auto n = random_num();
        n.value = 1e10 * static_cast<double>(1 + rng() % 3);
        rows.push_back(n);
        apply(rows, +1, "round " + std::to_string(round) + " head bump");
      }
    } else if (kind >= 8) { // structural edit of 1-3 adsh (sub / pre change)
      std::vector<SecEdgarQ6AdshEdit> edits;
      std::set<std::string> picked;
      for (size_t i = 0, n = 1 + rng() % 3; i < n; ++i) {
        const std::string adsh = "ADSH" + std::to_string(rng() % (adsh_count + 3));
        if (!picked.insert(adsh).second) continue;
        // 0 rename, 1 exit / enter cohort, 2 add pre row, 3 drop pre rows, 4 relabel, 5 fy move, 6 restmt
        const int op = static_cast<int>(rng() % 7);
        const bool in = m.cohort.count(adsh) != 0;
        const std::string new_name = name_pool[rng() % name_pool.size()];
        if (op == 0 || (op == 1 && !in)) {
          m.cohort[adsh] = {new_name, rnd_fy()};
        } else if (op == 1) {
          m.cohort.erase(adsh);
        } else if (op == 2) {
          m.pre.push_back({adsh, tag_pool[rng() % tag_pool.size()], version_pool[rng() % version_pool.size()],
                           rnd_stmt(), plabel_pool[rng() % plabel_pool.size()]});
        } else if (op == 3) {
          const auto cut = std::remove_if(m.pre.begin(), m.pre.end(), [&](const Model::PreRow &p) {
            return p.adsh == adsh && rng() % 2;
          });
          m.pre.erase(cut, m.pre.end());
        } else if (op == 4) {
          for (auto &p : m.pre)
            if (p.adsh == adsh && rng() % 2) p.plabel = plabel_pool[rng() % plabel_pool.size()];
        } else if (op == 5) {
          if (in) m.cohort[adsh].fy = rnd_fy();
        } else {
          for (auto &p : m.pre)
            if (p.adsh == adsh && rng() % 2) p.stmt = rnd_stmt();
        }
        SecEdgarQ6AdshEdit edit;
        edit.adsh = adsh;
        edit.new_cohort = m.cohort.count(adsh) != 0;
        if (edit.new_cohort) {
          const auto &c = m.cohort.at(adsh);
          edit.fy = c.fy;
          edit.name_is_null = c.name == kNull;
          if (!edit.name_is_null) edit.name = c.name;
        }
        std::map<std::tuple<std::string, std::string, std::string, std::string>, uint32_t> pm;
        for (const auto &p : m.pre)
          if (p.adsh == adsh) pm[{p.tag, p.version, p.stmt, p.plabel}]++;
        for (const auto &[k, mult] : pm) {
          SecEdgarQ6PreEntry pe;
          pe.tag = std::get<0>(k);
          pe.version = std::get<1>(k);
          pe.stmt = std::get<2>(k);
          pe.plabel_is_null = std::get<3>(k) == kNull;
          if (!pe.plabel_is_null) pe.plabel = std::get<3>(k);
          pe.mult = mult;
          edit.pre.push_back(std::move(pe));
        }
        std::map<std::tuple<std::string, std::string, std::string>, SecEdgarQ6NumSum> ns;
        for (const auto &nrow : m.nums) {
          if (nrow.adsh != adsh) continue;
          auto &e = ns[{nrow.tag, nrow.version, nrow.uom}];
          e.tag = nrow.tag;
          e.version = nrow.version;
          e.uom = nrow.uom;
          e.sum += nrow.value;
          e.cnt += 1;
        }
        for (auto &[k, v] : ns) edit.new_nums.push_back(v);
        edits.push_back(std::move(edit));
      }
      // old nums are the same sums, but they were filed under the OLD bridge.
      for (auto &e : edits) e.old_nums = e.new_nums;
      // a few num writes of other adsh ride along
      std::vector<SecEdgarQ6NumRef> other;
      std::vector<Model::NumRow> extra;
      extra.reserve(4); // the refs hold views into these rows
      for (size_t i = 0, n = rng() % 3; i < n; ++i) {
        auto nrow = random_num();
        if (picked.count(nrow.adsh)) continue;
        extra.push_back(nrow);
        other.push_back({extra.back().adsh, extra.back().tag, extra.back().version, extra.back().uom,
                         extra.back().value, 1});
      }
      // old_nums must describe the state BEFORE the extra rows (they never touch picked adsh)
      for (auto &x : extra) m.nums.push_back(x);
      bool overflow = false;
      auto next = SecEdgarQ6ApplyEdits(kServices, *state, edits, other, &overflow);
      Require(next && !overflow, "edit applied");
      state = std::make_shared<SecEdgarQ6State>(*next);
      CheckState(*state, m, "round " + std::to_string(round) + " structural edit");
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
  // The same writes through both dispatch modes must yield identical caches.
  const auto cell = FirstCell();
  SecEdgarQ6StructureBuilder builder;
  for (int i = 0; i < 500; ++i) {
    builder.AddCohort("A" + std::to_string(i), cell.fy + i % 2, "N" + std::to_string(i % 40), false);
    for (int k = 0; k < 30; ++k)
      builder.AddPre("A" + std::to_string(i), "T" + std::to_string(k), "v", k % 4 ? cell.stmt : cell.stmt + "~",
                     "P" + std::to_string(k % 5), false);
  }
  auto structure = builder.Finish();
  auto make = [&](bool scalar) {
    SecEdgarQ6ForceScalar(scalar);
    Rng r(42);
    auto state = std::make_shared<SecEdgarQ6State>(SecEdgarQ6MakeEmptyState(structure));
    std::vector<std::string> keep;
    std::vector<SecEdgarQ6NumRef> refs;
    std::vector<std::string> store;
    store.reserve(500 * 30 * 3);
    for (int i = 0; i < 500; ++i)
      for (int k = 0; k < 30; ++k)
        if (r() % 3) {
          store.push_back("A" + std::to_string(i));
          store.push_back("T" + std::to_string(k));
          refs.push_back({store[store.size() - 2], store.back(), "v", cell.uom,
                          static_cast<double>(r() % 1000) - 200.0, 1});
          // refs hold views into `store`: fix up after the loop
        }
    // re-point views (vector growth moved the strings)
    size_t at = 0;
    for (auto &ref : refs) {
      ref.adsh = store[at];
      ref.tag = store[at + 1];
      at += 2;
    }
    auto next = SecEdgarQ6ApplyNums(kServices, *state, refs.data(), refs.size());
    Require(next != nullptr, "simd test applied");
    auto out = std::make_shared<SecEdgarQ6State>(*next);
    SecEdgarQ6Rescan(*out);
    return out;
  };
  const auto a = make(true);
  const auto b = make(false);
  SecEdgarQ6ForceScalar(false);
  Require(a->active_groups == b->active_groups && a->tops.size() == b->tops.size(), "scalar/simd shape");
  for (size_t c = 0; c < a->tops.size(); ++c)
    Require(a->tops[c]->gids == b->tops[c]->gids, "scalar/simd rescan equal");
  (void)rng;
}

// Non-dyadic values: sums must not depend on the order writes arrive in, and
// adding then removing a value must restore the sum bit for bit.
void TestSumExactness() {
  const auto cell = FirstCell();
  SecEdgarQ6StructureBuilder builder;
  for (const char *a : {"A", "B"}) {
    builder.AddCohort(a, cell.fy, std::string("N") + a, false);
    builder.AddPre(a, "T", "v", cell.stmt, "P", false);
  }
  auto state = std::make_shared<SecEdgarQ6State>(SecEdgarQ6MakeEmptyState(builder.Finish()));
  SecEdgarQ6Rescan(*state);
  const double vals[] = {0.1, 0.2, 0.3, 1e-3, 1e12 + 0.7, 3.14159, -2.718281828};
  const auto apply = [&](const char *adsh, double v, int sign) {
    const SecEdgarQ6NumRef ref{adsh, "T", "v", cell.uom, v, sign};
    bool overflow = false;
    auto next = SecEdgarQ6ApplyNums(kServices, *state, &ref, 1, &overflow);
    Require(!overflow, "no overflow");
    if (next) state = std::make_shared<SecEdgarQ6State>(*next);
  };
  const size_t n = sizeof vals / sizeof *vals;
  for (size_t i = 0; i < n; ++i) apply("A", vals[i], +1);
  for (size_t i = n; i-- > 0;) apply("B", vals[i], +1);
  auto rows = SecEdgarQ6TopRows(*state, cell.fy, cell.stmt, cell.uom);
  Require(rows.size() == 2, "twin rows");
  Require(rows[0].total_value == rows[1].total_value, "twin groups tie bit for bit");
  Require(rows[0].name == "NA" && rows[1].name == "NB", "tie broken by name");
  const double before = rows[0].total_value;
  apply("A", 0.7, +1);
  apply("A", 0.7, -1);
  rows = SecEdgarQ6TopRows(*state, cell.fy, cell.stmt, cell.uom);
  Require(rows[0].total_value == before && rows[0].name == "NA", "add/remove restores the sum");
  for (size_t i = 0; i < n; ++i) apply("A", vals[i], -1);
  rows = SecEdgarQ6TopRows(*state, cell.fy, cell.stmt, cell.uom);
  Require(rows.size() == 1 && rows[0].name == "NB", "A emptied");
  Require(SecEdgarQ6TopRows(*state, cell.fy + 1, cell.stmt, cell.uom).empty() &&
              SecEdgarQ6TopRows(*state, cell.fy, cell.stmt + "~", cell.uom).empty() &&
              SecEdgarQ6TopRows(*state, cell.fy, cell.stmt, cell.uom + "~").empty(),
          "other parameters are empty");
}

} // namespace

int main() {
  Rng rng(0xC0FFEE);
  std::cout << "avx2=" << SecEdgarQ6UsesAvx2() << "\n";
  TestStringPool(rng);
  TestSimdMatchesScalar(rng);
  TestSumExactness();
  // small: complete-mode cache; large: sampled rescan + incremental maintenance
  RunScenario(rng, 30, 6, 5, 4, 300, 150, false);
  RunScenario(rng, 30, 6, 5, 4, 300, 150, true);
  RunScenario(rng, 1500, 200, 40, 8, 30000, 60, false);
  RunScenario(rng, 1500, 200, 40, 8, 30000, 30, true);
  std::cout << "PASS test_sec_edgar_q6_state\n";
  return 0;
}
