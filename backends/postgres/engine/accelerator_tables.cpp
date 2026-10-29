// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/engine/accelerator_tables.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/state/shared.hpp"

extern "C" {
#include <postgres.h>
#include <catalog/namespace.h>
}

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

struct Tables {
  uint64_t generation = ~uint64_t{0};
  std::unordered_map<core::TableId, WatchedTable> watched;
  std::map<int, std::unordered_set<core::TableId>> of_accelerator; // by index
};

// The oid of every table of `accelerator`, by the module's number; the tables
// that do not exist (nothing to capture on them) are left out.
runtime::TableMap ResolveTables(const PostgresAccelerator &accelerator, bool must_exist) {
  runtime::TableMap map;
  for (const auto &table : accelerator.Tables()) {
    const Oid oid = RelnameGetRelid(table.relation);
    if (!OidIsValid(oid)) {
      if (must_exist) {
        ereport(ERROR, (errmsg("accelerator %s: relation \"%s\" not found", accelerator.Id().c_str(), table.relation)));
      }
      ereport(WARNING, (errmsg("accelerator %s: relation \"%s\" not found; its writes are not captured",
                               accelerator.Id().c_str(), table.relation)));
      continue;
    }
    map[table.module_id] = oid;
  }
  return map;
}

const Tables &Current() {
  static Tables tables;
  const uint64_t generation = shared::Generation();
  if (tables.generation == generation) {
    return tables;
  }
  tables = Tables{};
  tables.generation = generation;
  for (const int index : shared::Active()) {
    auto &mine = tables.of_accelerator[index];
    // activated by a build that has other accelerators compiled in: no members
    for (auto *accelerator : ShardMembers(shared::IdOf(index))) {
      const auto host = ResolveTables(*accelerator, /*must_exist=*/false);
      for (const auto &dependency : accelerator->Dependencies()) {
        const auto oid = host.find(dependency.table);
        if (oid == host.end()) { // its relation was reported missing by ResolveTables, or is not named at all
          const auto named = accelerator->Tables();
          if (std::none_of(named.begin(), named.end(),
                           [&](const AcceleratorTable &t) { return t.module_id == dependency.table; })) {
            ereport(WARNING, (errmsg("accelerator %s: table %llu is not named; its writes are not captured",
                                     accelerator->Id().c_str(), static_cast<unsigned long long>(dependency.table))));
          }
          continue;
        }
        mine.insert(oid->second);
        auto &watched = tables.watched[oid->second];
        watched.whole_row |= dependency.mutation_attnums.empty();
        watched.attnums.insert(watched.attnums.end(), dependency.mutation_attnums.begin(),
                               dependency.mutation_attnums.end());
      }
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

runtime::TableMap HostTableMap(const PostgresAccelerator &accelerator) {
  return ResolveTables(accelerator, /*must_exist=*/true);
}

std::vector<core::TableId> HostTables(int index) {
  const auto &of_accelerator = Current().of_accelerator;
  const auto it = of_accelerator.find(index);
  return it == of_accelerator.end() ? std::vector<core::TableId>{}
                                    : std::vector<core::TableId>(it->second.begin(), it->second.end());
}

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
