// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/api/accelerators.hpp"

#include <map>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Function-local storage: safe against static-initialisation order.
std::map<std::string, PostgresAcceleratorGetter> &Accelerators() {
  static std::map<std::string, PostgresAcceleratorGetter> accelerators;
  return accelerators;
}

} // namespace

void RegisterPostgresAccelerator(const char *id, PostgresAcceleratorGetter get) {
  Accelerators()[id] = std::move(get);
}

PostgresAccelerator *FindPostgresAccelerator(const core::AcceleratorId &id) {
  const auto it = Accelerators().find(id);
  return it == Accelerators().end() ? nullptr : &it->second();
}

std::vector<PostgresAccelerator *> ShardMembers(const core::AcceleratorId &shard) {
  if (auto *alone = FindPostgresAccelerator(shard); alone && alone->Shard() == shard) {
    return {alone};
  }
  std::vector<PostgresAccelerator *> members;
  for (const auto &entry : Accelerators()) {
    if (auto &accelerator = entry.second(); accelerator.Shard() == shard) {
      members.push_back(&accelerator);
    }
  }
  return members;
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
