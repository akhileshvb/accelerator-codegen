// The canary suite's view of an engine backend: SQL sessions, the registration of the suite's own
// accelerators by table name, the runtime's counters, and the typed build scan modules use. It
// names no engine type; a backend binds it in backends/<engine>/tests/test_<engine>_canary.cpp
// (pipeline/prompts/engine_backend/canary_contract.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::canary {

namespace core = accelerator_codegen::aux_state::core;

// A SQL value of the suite's tables: NULL, BIGINT or VARCHAR.
using Cell = std::variant<std::monostate, int64_t, std::string>;
using Row = std::vector<Cell>;
using Rows = std::vector<Row>;

// One client connection, with its own transaction state and snapshot. Query runs any statement and
// returns its rows (none for DDL/DML); it throws std::runtime_error with the engine's message on an error.
class Session {
public:
  virtual ~Session() = default;
  virtual Rows Query(const std::string &sql) = 0;
  void Exec(const std::string &sql) { Query(sql); }
};

struct CanaryColumn {
  std::string name;
  bool is_text = false; // else BIGINT
  bool nullable = false;
};

// A batch of rows of a build scan, typed as the module-facing scan contract
// (native_scan_contract.md, "Module-facing build scan"): the backend forwards its own row view.
class ScanBatch {
public:
  virtual ~ScanBatch() = default;
  virtual std::size_t size() const = 0;
  virtual int64_t Int(std::size_t row, int column) const = 0;
  virtual std::string_view TextView(std::size_t row, int column) const = 0;
  virtual bool IsNull(std::size_t row, int column) const = 0;
};

using BatchSink = std::function<void(std::size_t partition, const ScanBatch &)>;
// ScanPartitioned of the backend: `partitions` ways, inside the build snapshot of `services`.
using ScanFn = std::function<void(const core::AuxEngineServices &services, const std::string &table,
                                  const std::vector<CanaryColumn> &columns, std::size_t partitions,
                                  const BatchSink &sink)>;

struct Counters {
  uint64_t hits = 0;          // reads answered from the state
  uint64_t fallbacks = 0;     // reads answered from SQL
  uint64_t builds = 0;        // completed state builds
  uint64_t stale_commits = 0; // commits the module could not maintain (each costs a rebuild)
};

// A canary accelerator: `make_module` builds the pipeline's module around the backend's scan.
// Module table id i is the engine table `tables[i]`: two accelerators may use the same ids for
// different tables, and the backend maps them by name.
struct AcceleratorSpec {
  std::string name;
  std::vector<std::string> tables;
  std::string sql; // its query: what the fallback answers and the module must equal
  std::function<std::shared_ptr<core::AuxQueryModule>(ScanFn)> make_module;
};

class Engine {
public:
  virtual ~Engine() = default;
  virtual std::unique_ptr<Session> Connect() = 0;
  // Registers the accelerator on this engine's database (the tables exist); its reads are
  // ReadSql(name).
  virtual void Register(const AcceleratorSpec &spec) = 0;
  virtual std::string ReadSql(const std::string &name) = 0;
  virtual Counters ReadCounters(const std::string &name) = 0;
  // The accelerator's state is rebuilt before its next hit.
  virtual void Invalidate(const std::string &name) = 0;
  // Every read is answered from SQL (the substrate's forced fallback).
  virtual void ForceFallback(bool forced) = 0;
  // A fallback SQL execution becomes an error (ACCEL_FALLBACK_TRAP, host_contract.md "Hit contract").
  virtual void TrapFallback(bool trapped) = 0;
  // The storage reaches its steady state (checkpoint, compaction), where compressed and
  // dictionary-encoded columns exist.
  virtual void Settle() = 0;
};

using EngineFactory = std::function<std::unique_ptr<Engine>()>;

} // namespace accelerator_codegen::canary
