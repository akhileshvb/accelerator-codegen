// The (fy, stmt, uom) values the Q6 tests draw their data and random parameters around: those of
// the drawn parameter sets (eval/harness/param_sets.hpp).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::q6_tests {

using Rng = std::mt19937_64;

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
  return s;
}

// The values the data and the random parameters are drawn around: the (fy,
// stmt, uom) of the drawn sets.
struct Domain {
  explicit Domain(std::vector<core::QueryArguments> drawn) : sets(std::move(drawn)) {
    for (const auto &set : sets) {
      AddUnique(fys, FyOf(set));
      AddUnique(stmts, StmtOf(set));
      AddUnique(uoms, UomOf(set));
    }
  }
  static int64_t FyOf(const core::QueryArguments &a) { return std::get<int64_t>(a.values.at(0).payload); }
  static const std::string &StmtOf(const core::QueryArguments &a) {
    return std::get<std::string>(a.values.at(1).payload);
  }
  static const std::string &UomOf(const core::QueryArguments &a) {
    return std::get<std::string>(a.values.at(2).payload);
  }

  // Around `focus`: it, another drawn value, or a look-alike / out-of-domain one.
  int64_t Fy(Rng &rng, int64_t focus) const {
    const auto roll = rng() % 10;
    if (roll < 3) return focus;
    if (roll < 8) return fys[rng() % fys.size()];
    const std::vector<int64_t> outside{0, *std::min_element(fys.begin(), fys.end()) - 2,
                                       *std::max_element(fys.begin(), fys.end()) + 1};
    return outside[rng() % outside.size()];
  }
  std::string Text(Rng &rng, const std::vector<std::string> &pool, const std::string &focus) const {
    const auto roll = rng() % 10;
    if (roll < 3) return focus;
    if (roll < 8) return pool[rng() % pool.size()];
    const std::vector<std::string> lookalikes{Lower(focus), focus + " ", ""};
    return lookalikes[rng() % lookalikes.size()];
  }

  std::vector<core::QueryArguments> sets;
  std::vector<int64_t> fys;
  std::vector<std::string> stmts, uoms;

private:
  template <typename T> static void AddUnique(std::vector<T> &pool, const T &value) {
    if (std::find(pool.begin(), pool.end(), value) == pool.end()) pool.push_back(value);
  }
};

} // namespace accelerator_codegen::aux_state::q6_tests
