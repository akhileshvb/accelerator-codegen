// GENERATED BACKEND
//
// The accelerators compiled into this module, by id. Every per-query
// Runtime (engine_runtime.hpp) registers itself here, so the capture path
// can find an active accelerator's dependencies and a shard worker can find
// the accelerator it hosts. Lookups construct the Runtime on first use.
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <memory>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

class PostgresAccelerator {
public:
  virtual ~PostgresAccelerator() = default;
  virtual const core::AcceleratorId &Id() const = 0;
  virtual std::vector<core::TableDependency> Dependencies() const = 0;
  // Worker side: the module the shard worker registers with its runtime host
  // (its scans bind to the worker's SPI connection).
  virtual std::shared_ptr<core::AuxQueryModule> Module() = 0;
};

using PostgresAcceleratorGetter = PostgresAccelerator &(*)();

void RegisterPostgresAccelerator(const char *id, PostgresAcceleratorGetter get);
// nullptr if no accelerator with `id` is compiled in.
PostgresAccelerator *FindPostgresAccelerator(const core::AcceleratorId &id);

struct PostgresAcceleratorRegistrar {
  PostgresAcceleratorRegistrar(const char *id, PostgresAcceleratorGetter get) {
    RegisterPostgresAccelerator(id, get);
  }
};

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
