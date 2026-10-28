// Shared row model + randomized write generator for the Q24 aux tests.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace q24test {
using namespace accelerator_codegen::aux_state::core;

using OptStr = std::optional<std::string>;

struct NumRow {
  OptStr adsh, tag, version;
  std::optional<int64_t> ddate;
  int64_t qtrs = 0;
  OptStr uom;
  std::string coreg;
  std::optional<double> value;
  bool operator==(const NumRow &o) const {
    return std::tie(adsh, tag, version, ddate, qtrs, uom, coreg, value) ==
           std::tie(o.adsh, o.tag, o.version, o.ddate, o.qtrs, o.uom, o.coreg,
                    o.value);
  }
};

struct PreRow {
  OptStr adsh, tag, version;
  bool operator==(const PreRow &o) const {
    return std::tie(adsh, tag, version) == std::tie(o.adsh, o.tag, o.version);
  }
};

inline AuxValue SV(const OptStr &s) {
  return s ? AuxValue::Of(AuxType::String, *s) : AuxValue::Null(AuxType::String);
}

inline AuxRow ToAux(const NumRow &r) {
  return {{SV(r.adsh), SV(r.tag), SV(r.version),
           r.ddate ? AuxValue::Of(AuxType::Int64, *r.ddate)
                   : AuxValue::Null(AuxType::Int64),
           AuxValue::Of(AuxType::Int64, r.qtrs), SV(r.uom),
           AuxValue::Of(AuxType::String, r.coreg),
           r.value ? AuxValue::Of(AuxType::Double, *r.value)
                   : AuxValue::Null(AuxType::Double)}};
}

inline AuxRow ToAux(const PreRow &r) { return {{SV(r.adsh), SV(r.tag), SV(r.version)}}; }


constexpr std::array<const char *, 12> kAdsh = {
    "a00", "a01", "a02", "a03", "a04", "a05", "a06", "a07", "a08", "a09", "a10", "a11"};
constexpr std::array<const char *, 5> kTags = {"Rev", "Cost", "Assets", "Cash", "Debt"};
constexpr std::array<const char *, 3> kVers = {"us-gaap/2023", "us-gaap/2024", "custom"};

struct Gen {
  std::mt19937_64 rng;
  bool nulls;
  explicit Gen(uint64_t seed, bool nulls_p) : rng(seed), nulls(nulls_p) {}
  int R(int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); }
  OptStr Pick(const char *const *pool, int n) {
    if (nulls && R(40) == 0) return std::nullopt;
    return std::string(pool[R(n)]);
  }
  // ddate: mostly 2023/2024, some other years, a few month/day values that lie
  // in no year's window (exactly the rows the state must keep out of buckets).
  int64_t Date() {
    static const int64_t md[] = {101, 615, 1231, 731, 301, 1031};
    const int p = R(100);
    if (p < 3) {
      static const int64_t odd[] = {20230100, 20231232, 20230000, 20240000, 20221300, 0, 99991231, 201, -9000};
      return odd[R(9)];
    }
    int64_t y;
    const int q = R(100);
    if (q < 50) y = 2023;
    else if (q < 75) y = 2024;
    else if (q < 85) y = 2022;
    else if (q < 93) y = 2019 + R(3);
    else y = 2025 + R(2);
    return y * 10000 + md[R(6)];
  }
  OptStr Uom() {
    const int p = R(100);
    if (p < 55) return std::string("USD");
    if (p < 70) return std::string("EUR");
    if (p < 80) return std::string("shares");
    if (p < 88) return std::string("pure");
    if (p < 94) return std::string("CAD");
    if (p < 97) return std::string("usd");
    return std::string("XYZ");
  }
  // A (year, uom) cell the generated rows concentrate in: HAVING > 10 groups form here.
  std::pair<int64_t, std::string> DenseCell() {
    return {R(2) ? 2023 : 2024, R(3) ? "USD" : "EUR"};
  }
  // Arguments outside the query's domain: NULLs, unknown uom, years no ddate can match.
  std::pair<std::optional<int64_t>, OptStr> EdgeParams() {
    static const int64_t years[] = {2017, 2025, 0, -1, 4000000000000000000LL};
    std::optional<int64_t> y = R(5) == 0 ? std::nullopt : std::optional<int64_t>(years[R(5)]);
    OptStr u = R(5) == 0 ? std::nullopt : OptStr(std::string("unknown_uom"));
    return {y, u};
  }
  NumRow Num() {
    NumRow r;
    r.adsh = Pick(kAdsh.data(), kAdsh.size());
    r.tag = Pick(kTags.data(), 3 + R(3));
    r.version = Pick(kVers.data(), 2 + R(2));
    r.ddate = Date();
    r.qtrs = R(4);
    r.uom = Uom();
    if (nulls && R(30) == 0) r.uom = std::nullopt;
    if (nulls && R(30) == 0) r.ddate = std::nullopt;
    r.coreg = R(2) ? "" : "x";
    if (R(12) == 0 && nulls) r.value = std::nullopt;
    else r.value = (R(3) == 0) ? static_cast<double>(R(1000)) / 7.0 : static_cast<double>(R(100000));
    return r;
  }
  PreRow Pre() {
    return {Pick(kAdsh.data(), kAdsh.size()), Pick(kTags.data(), 3 + R(3)),
            Pick(kVers.data(), 2 + R(2))};
  }
};

// Typed copy of what a generated batch changed (for mirroring into SQL).
struct Typed {
  std::vector<NumRow> n_old, n_new;
  std::vector<PreRow> p_old, p_new;
};

inline MutationBatch NumBatch(MutationKind kind, std::vector<NumRow> olds,
                       std::vector<NumRow> news, std::vector<ColumnId> cols = {}) {
  MutationBatch m;
  m.table = 101;
  m.kind = kind;
  m.updated_columns = std::move(cols);
  for (const auto &r : olds) m.old_rows.push_back(ToAux(r));
  for (const auto &r : news) m.new_rows.push_back(ToAux(r));
  return m;
}

inline MutationBatch PreBatch(MutationKind kind, std::vector<PreRow> olds,
                       std::vector<PreRow> news, std::vector<ColumnId> cols = {}) {
  MutationBatch m;
  m.table = 102;
  m.kind = kind;
  m.updated_columns = std::move(cols);
  for (const auto &r : olds) m.old_rows.push_back(ToAux(r));
  for (const auto &r : news) m.new_rows.push_back(ToAux(r));
  return m;
}

// Applies a random write to both the oracle tables and a mutation batch.
// Returns the batch (possibly empty rows when nothing was applicable).
// `expect_incremental` is cleared for actions that must rebuild.
inline MutationBatch RandomWrite(Gen &g, std::vector<NumRow> &num_tbl, std::vector<PreRow> &pre_tbl, bool &expect_incremental,
                          int &kind_out, Typed &typed) {
  auto NB = [&](MutationKind k, std::vector<NumRow> o, std::vector<NumRow> n, std::vector<ColumnId> c = {}) {
    typed.n_old = o; typed.n_new = n;
    return NumBatch(k, std::move(o), std::move(n), std::move(c));
  };
  auto PB = [&](MutationKind k, std::vector<PreRow> o, std::vector<PreRow> n, std::vector<ColumnId> c = {}) {
    typed.p_old = o; typed.p_new = n;
    return PreBatch(k, std::move(o), std::move(n), std::move(c));
  };

  const int kind = g.R(14);
  kind_out = kind;
  auto pick_num = [&]() -> int {
    return num_tbl.empty() ? -1 : g.R(static_cast<int>(num_tbl.size()));
  };
  switch (kind) {
  case 0: case 1: { // num insert
    std::vector<NumRow> rows;
    const int n = 1 + g.R(5);
    for (int i = 0; i < n; ++i) rows.push_back(g.Num());
    for (auto &r : rows) num_tbl.push_back(r);
    return NB(MutationKind::Insert, {}, rows);
  }
  case 2: { // num delete
    std::vector<NumRow> rows;
    for (int i = 0, n = 1 + g.R(4); i < n; ++i) {
      const int idx = pick_num();
      if (idx < 0) break;
      rows.push_back(num_tbl[idx]);
      num_tbl.erase(num_tbl.begin() + idx);
    }
    return NB(MutationKind::Delete, rows, {});
  }
  case 3: case 4: case 5: { // num value update (the benchmark's write)
    std::vector<NumRow> olds, news;
    for (int i = 0, n = 1 + g.R(3); i < n; ++i) {
      const int idx = pick_num();
      if (idx < 0) break;
      olds.push_back(num_tbl[idx]);
      if (g.R(8) == 0) num_tbl[idx].value = std::nullopt;
      else num_tbl[idx].value = static_cast<double>(g.R(500000)) / 3.0;
      news.push_back(num_tbl[idx]);
    }
    return NB(MutationKind::Update, olds, news, {7});
  }
  case 6: { // join-key move (adsh / tag / version)
    std::vector<NumRow> olds, news;
    const int idx = pick_num();
    if (idx >= 0) {
      olds.push_back(num_tbl[idx]);
      Gen tmp(g.rng(), g.nulls);
      const int which = g.R(3);
      if (which == 0) num_tbl[idx].adsh = tmp.Pick(kAdsh.data(), kAdsh.size());
      if (which == 1) num_tbl[idx].tag = tmp.Pick(kTags.data(), 5);
      if (which == 2) num_tbl[idx].version = tmp.Pick(kVers.data(), 3);
      news.push_back(num_tbl[idx]);
      return NB(MutationKind::Update, olds, news, {static_cast<ColumnId>(which)});
    }
    return NB(MutationKind::Update, {}, {}, {0});
  }
  case 7: { // filter-column move (ddate / uom) in or out of the result
    std::vector<NumRow> olds, news;
    const int idx = pick_num();
    if (idx >= 0) {
      olds.push_back(num_tbl[idx]);
      ColumnId col;
      if (g.R(2)) {
        num_tbl[idx].ddate = g.Date();
        col = 3;
      } else {
        num_tbl[idx].uom = g.Uom();
        col = 5;
      }
      news.push_back(num_tbl[idx]);
      return NB(MutationKind::Update, olds, news, {col});
    }
    return NB(MutationKind::Update, {}, {}, {3});
  }
  case 8: { // irrelevant column update (qtrs / coreg)
    std::vector<NumRow> olds, news;
    const int idx = pick_num();
    if (idx >= 0) {
      olds.push_back(num_tbl[idx]);
      num_tbl[idx].qtrs += 1;
      news.push_back(num_tbl[idx]);
      return NB(MutationKind::Update, olds, news, {4});
    }
    return NB(MutationKind::Update, {}, {}, {4});
  }
  case 9: case 10: { // pre insert (incl. duplicates of existing triples)
    std::vector<PreRow> rows;
    for (int i = 0, n = 1 + g.R(3); i < n; ++i) {
      PreRow r = (!pre_tbl.empty() && g.R(4) == 0) ? pre_tbl[g.R(static_cast<int>(pre_tbl.size()))] : g.Pre();
      rows.push_back(r);
      pre_tbl.push_back(r);
    }
    return PB(MutationKind::Insert, {}, rows);
  }
  case 11: { // pre delete (incremental: touched triples resolved from the tables)
    std::vector<PreRow> rows;
    if (!pre_tbl.empty()) {
      const int idx = g.R(static_cast<int>(pre_tbl.size()));
      rows.push_back(pre_tbl[idx]);
      pre_tbl.erase(pre_tbl.begin() + idx);
    }
    (void)expect_incremental;
    return PB(MutationKind::Delete, rows, {});
  }
  case 12: { // pre key update (incremental, as delete)
    std::vector<PreRow> olds, news;
    if (!pre_tbl.empty()) {
      const int idx = g.R(static_cast<int>(pre_tbl.size()));
      olds.push_back(pre_tbl[idx]);
      pre_tbl[idx] = g.Pre();
      news.push_back(pre_tbl[idx]);
    }
    (void)expect_incremental;
    return PB(MutationKind::Update, olds, news, {1});
  }
  default: { // insert rows copying an existing key to hit the same group
    std::vector<NumRow> rows;
    if (!num_tbl.empty()) {
      NumRow r = num_tbl[g.R(static_cast<int>(num_tbl.size()))];
      r.value = static_cast<double>(g.R(1000));
      if (g.R(2)) { r.uom = "USD"; r.ddate = 20230601; }
      rows.push_back(r);
      num_tbl.push_back(r);
    }
    return NB(MutationKind::Insert, {}, rows);
  }
  }
}


} // namespace q24test
