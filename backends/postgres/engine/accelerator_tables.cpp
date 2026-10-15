// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/engine/accelerator_tables.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/state/shared.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

struct Tables {
  uint64_t generation = ~uint64_t{0};
  std::unordered_map<core::TableId, WatchedTable> watched;
  std::map<int, std::unordered_set<core::TableId>> of_accelerator; // by index
};

const Tables &Current() {
  static Tables tables;
  const uint64_t generation = shared::Generation();
  if (tables.generation == generation) {
    return tables;
  }
  tables = Tables{};
  tables.generation = generation;
  for (const int index : shared::Active()) {
    auto *accelerator = FindPostgresAccelerator(shared::IdOf(index));
    if (accelerator == nullptr) {
      continue; // activated by a build that has other accelerators compiled in
    }
    auto &mine = tables.of_accelerator[index];
    for (const auto &dependency : accelerator->Dependencies()) {
      mine.insert(dependency.table);
      auto &watched = tables.watched[dependency.table];
      watched.whole_row |= dependency.mutation_attnums.empty();
      watched.attnums.insert(watched.attnums.end(), dependency.mutation_attnums.begin(),
                             dependency.mutation_attnums.end());
    }
  }
  for (auto &entry : tables.watched) {
    auto &attnums = entry.second.attnums;
    std::sort(attnums.begin(), attnums.end());
    attnums.erase(std::unique(attnums.begin(), attnums.end()), attnums.end());
  }
  return tables;
}

} // namespace

const WatchedTable *WatchedBy(core::TableId table) {
  const auto &watched = Current().watched;
  const auto it = watched.find(table);
  return it == watched.end() ? nullptr : &it->second;
}

bool Depends(int index, core::TableId table) {
  const auto &of_accelerator = Current().of_accelerator;
  const auto it = of_accelerator.find(index);
  return it != of_accelerator.end() && it->second.count(table) != 0;
}

std::vector<int> AcceleratorsOn(const std::set<core::TableId> &tables) {
  std::vector<int> indices;
  for (const auto &[index, mine] : Current().of_accelerator) { // ascending
    if (std::any_of(tables.begin(), tables.end(), [&](core::TableId t) { return mine.count(t) != 0; })) {
      indices.push_back(index);
    }
  }
  return indices;
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
