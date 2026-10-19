// Backend-generic slot gates: scenarios that any engine backend must pass,
// written once against `Engine` (what a backend adapter supplies) and run
// with the registered accelerators of the workload the adapter picks.
//
//   epoch_stress       writers and readers at once; every read is exact at its
//                      snapshot (no torn or stale read), per reader mode
//   hits_under_writes  continuous commits; reads stay exact and most are hits
//   read_after_commit  a read on another connection after a commit returns
//                      includes it, also while other readers keep the apply busy
//   txn_edges          aborts, savepoints and TRUNCATE leave exact answers and
//                      cost no rebuild (TRUNCATE is rebuilt)
//   restart            accelerator workers / processes restarted under load:
//                      answers stay exact and hits resume
//   own_writes         a read inside a transaction that wrote a tracked table
//                      includes its own uncommitted rows (also a statement
//                      prepared before the write: exact or refused, never stale);
//                      rollback leaves no trace
//   ddl_events         replacing, renaming or reshaping a tracked table never
//                      leaves an answer built from the old table
//
// A scenario the engine cannot express (no savepoints, nothing to restart)
// reports `NotApplicable`, never a silent pass.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace accelerator_codegen::slot_gates {

// Canonical result: one string per row, sorted (numbers rounded so the
// accelerator's fixed point and the engine's decimals agree).
using Rows = std::vector<std::string>;

class Session {
public:
  virtual ~Session() = default;
  virtual void Exec(const std::string &sql) = 0; // throws on error
  virtual Rows Query(const std::string &sql) = 0;
  // A statement planned now and run later (after a write); empty: the engine has none.
  virtual std::function<Rows()> Prepare(const std::string &) { return nullptr; }
};

// One accelerated read and the base SQL that defines its answer.
struct ReadCase {
  std::string label, accel_sql, ref_sql;
};

// How a reader transaction pins its snapshot. `per_transaction`: every
// statement of the transaction sees one snapshot, so the accelerator and the
// reference must agree inside it. Otherwise each statement has its own
// snapshot and exactness is only checked while no commit is in flight.
struct ReaderMode {
  std::string label, begin_sql;
  bool per_transaction = true;
};

// Cumulative accelerator-side counters of the workload's accelerators.
struct ReadStats {
  uint64_t hits = 0, fallbacks = 0, builds = 0;
};

// Statements that replace, rename or reshape a tracked table while keeping it
// queryable, with the statements that put the original back. The reference SQL
// reads whatever table the statements leave.
struct DdlCase {
  std::string label;
  std::vector<std::string> statements, restore;
};

// Statements of one write transaction on the tracked tables, from a private
// generator (keys it invents are its own).
class WriteSource {
public:
  virtual ~WriteSource() = default;
  virtual std::vector<std::string> Next() = 0;
};

class Engine {
public:
  virtual ~Engine() = default;
  virtual std::string Name() const = 0;
  virtual std::unique_ptr<Session> Connect() = 0;
  // Accelerated reads over the tracked tables; built (warm) when this returns.
  virtual std::vector<ReadCase> ReadCases() = 0;
  virtual std::vector<ReaderMode> ReaderModes() = 0;
  virtual std::unique_ptr<WriteSource> NewWriteSource(uint64_t seed) = 0;
  virtual ReadStats Stats() = 0;

  // Statements that empty a tracked table inside one transaction and restore
  // its rows (TRUNCATE is not incrementally maintainable).
  virtual std::vector<std::string> TruncateAndRestore() = 0;
  virtual bool SupportsSavepoints() const = 0;
  // DDL on the tracked tables; empty: the engine's DDL scenarios are not expressed.
  virtual std::vector<DdlCase> DdlCases() { return {}; }
  // Restarts every accelerator worker / process; false: the engine has none.
  virtual bool RestartAccelerators() = 0;
};

struct Options {
  int seconds = 4;
  int readers = 4;
  uint64_t seed = 1;
  // Under continuous commits, the share of accelerator reads that must be hits.
  double min_hit_rate = 0.8;
};

enum class Outcome { Pass, Fail, NotApplicable };

struct Report {
  Outcome outcome = Outcome::Pass;
  std::string detail;
};

Report EpochStress(Engine &engine, const Options &options);
Report HitsUnderWrites(Engine &engine, const Options &options);
Report ReadAfterCommit(Engine &engine, const Options &options);
Report TxnEdges(Engine &engine, const Options &options);
Report Restart(Engine &engine, const Options &options);
Report OwnWrites(Engine &engine, const Options &options);
Report DdlEvents(Engine &engine, const Options &options);

// Runs the named scenario ("all" runs every one), prints one line per
// scenario, returns the process exit code: 0 pass, 1 fail, 77 not applicable.
int Run(Engine &engine, const std::string &scenario, const Options &options);

} // namespace accelerator_codegen::slot_gates
