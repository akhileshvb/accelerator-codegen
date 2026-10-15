// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/api/accelerators.hpp"

#include <map>
#include <string>

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Function-local storage: safe against static-initialisation order.
std::map<std::string, PostgresAcceleratorGetter> &Accelerators() {
  static std::map<std::string, PostgresAcceleratorGetter> accelerators;
  return accelerators;
}

} // namespace

void RegisterPostgresAccelerator(const char *id, PostgresAcceleratorGetter get) {
  Accelerators()[id] = get;
}

PostgresAccelerator *FindPostgresAccelerator(const core::AcceleratorId &id) {
  const auto it = Accelerators().find(id);
  return it == Accelerators().end() ? nullptr : &it->second();
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
