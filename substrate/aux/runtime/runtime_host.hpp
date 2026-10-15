// SCAFFOLDING
// C1: the per-database host (per Postgres shard worker): accelerators, writer
// transactions, the thread budget.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "substrate/aux/runtime/accelerator_runtime.hpp"
#include "substrate/aux/runtime/columnar_batch.hpp"
#include "substrate/aux/runtime/engine_slots.hpp"
#include "substrate/aux/runtime/thread_budget.hpp"

namespace accelerator_codegen::aux_state::runtime {

class RuntimeHost {
public:
  RuntimeHost(EngineSlots &slots, std::size_t thread_budget,
              std::shared_ptr<TokenSource> tokens = nullptr);
  ~RuntimeHost();

  // ---- registration ----
  AcceleratorRuntime &Register(std::shared_ptr<core::AuxQueryModule> module,
                               StoreFactory make_store = nullptr);
  AcceleratorRuntime *Find(const core::AcceleratorId &id);
  std::vector<AcceleratorRuntime *> All();

  // S1: the attnums to capture on `table` (union over every registered
  // accelerator's dependencies, sorted; empty optional: no accelerator
  // watches the table; empty vector: capture every column).
  std::optional<std::vector<int32_t>> CaptureColumns(core::TableId table) const;
  // Bumped whenever CaptureColumns may have changed (registration).
  uint64_t CaptureVersion() const;

  // ---- in-process writer transactions (S1 + S2) ----
  // `txn` is the engine's transaction identity; `depth` the subtransaction
  // depth (0 = top level). Batches are kept, not interpreted, until commit.
  void OnCapture(uint64_t txn, std::size_t depth, ColumnarBatchRef batch);
  void CommitSubtransaction(uint64_t txn, std::size_t depth);
  void AbortSubtransaction(uint64_t txn, std::size_t depth);
  // Whether `txn` wrote any table `accelerator` depends on (its own reads
  // must fall back: its writes are in no shared state).
  bool TxnWroteDependency(uint64_t txn, const AcceleratorRuntime &accelerator) const;
  // Pre-commit: every affected accelerator takes its place in commit order
  // (accelerator-id order, so concurrent commits cannot deadlock).
  void PreCommit(uint64_t txn);
  // The engine committed at `epoch` (S2): each affected accelerator
  // maintains and publishes.
  void Commit(uint64_t txn, Epoch epoch);
  void Abort(uint64_t txn);

  // ---- builds ----
  // Builds every registered accelerator that is not synced, in parallel on
  // the thread budget (in-process), and waits.
  void BuildAll();
  // Host-thread placement (S6: Postgres): runs queued engine work (builds,
  // shipped applies) on the calling thread; returns whether anything ran.
  bool RunEngineWork();

  ThreadBudget &Threads();
  EngineSlots &Slots() { return slots; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
  EngineSlots &slots;
};

} // namespace accelerator_codegen::aux_state::runtime
