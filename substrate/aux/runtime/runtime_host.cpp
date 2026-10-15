// SCAFFOLDING
#include "substrate/aux/runtime/runtime_host.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <stdexcept>

namespace accelerator_codegen::aux_state::runtime {

struct RuntimeHost::Impl {
  explicit Impl(std::size_t budget, std::shared_ptr<TokenSource> tokens)
      : threads(budget, std::move(tokens)) {}

  struct Txn {
    // frames[d]: batches captured at subtransaction depth d (0 = top level).
    std::vector<std::vector<ColumnarBatchRef>> frames;
    // Accelerators staged at pre-commit, in id order.
    std::vector<AcceleratorRuntime *> staged;
  };

  // Declared before `threads`: the pool (and any build still running on it)
  // is torn down first.
  std::map<core::AcceleratorId, std::unique_ptr<AcceleratorRuntime>> runtimes; // id order
  ThreadBudget threads;
  mutable std::mutex mutex; // runtimes, by_table, txns
  std::unordered_map<core::TableId, std::vector<AcceleratorRuntime *>> by_table;
  std::unordered_map<uint64_t, Txn> txns;
  std::atomic<uint64_t> capture_version{0};

  std::mutex work_mutex; // host-thread engine work (S6)
  std::deque<std::function<void()>> work;

  Txn TakeTxn(uint64_t txn) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = txns.find(txn);
    if (it == txns.end()) {
      return {};
    }
    Txn taken = std::move(it->second);
    txns.erase(it);
    return taken;
  }
};

RuntimeHost::RuntimeHost(EngineSlots &slots_p, std::size_t thread_budget,
                         std::shared_ptr<TokenSource> tokens)
    : impl(std::make_unique<Impl>(thread_budget, std::move(tokens))), slots(slots_p) {}

RuntimeHost::~RuntimeHost() = default;

// ---- registration ----

AcceleratorRuntime &RuntimeHost::Register(std::shared_ptr<core::AuxQueryModule> module,
                                          StoreFactory make_store, const TableMap &tables) {
  AcceleratorRuntime::BuildScheduler schedule;
  if (slots.EngineWorkOnHostThread()) {
    schedule = [this](std::function<void()> work) {
      std::lock_guard<std::mutex> lock(impl->work_mutex);
      impl->work.push_back(std::move(work));
    };
  }
  auto runtime =
      std::make_unique<AcceleratorRuntime>(std::move(module), slots, impl->threads, schedule,
                                           std::move(make_store), tables);
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto &slot = impl->runtimes[runtime->Id()];
  if (slot) {
    throw std::invalid_argument("duplicate accelerator: " + runtime->Id());
  }
  slot = std::move(runtime);
  for (const auto table : slot->DependencyTables()) {
    impl->by_table[table].push_back(slot.get());
  }
  impl->capture_version++;
  return *slot;
}

AcceleratorRuntime *RuntimeHost::Find(const core::AcceleratorId &id) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto it = impl->runtimes.find(id);
  return it == impl->runtimes.end() ? nullptr : it->second.get();
}

std::vector<AcceleratorRuntime *> RuntimeHost::All() {
  std::lock_guard<std::mutex> lock(impl->mutex);
  std::vector<AcceleratorRuntime *> all;
  for (auto &entry : impl->runtimes) {
    all.push_back(entry.second.get());
  }
  return all;
}

std::optional<std::vector<int32_t>> RuntimeHost::CaptureColumns(core::TableId table) const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto it = impl->by_table.find(table);
  if (it == impl->by_table.end()) {
    return std::nullopt;
  }
  std::vector<int32_t> columns;
  for (auto *runtime : it->second) {
    const auto &watched = *runtime->Projection(table);
    if (watched.empty()) {
      return std::vector<int32_t>{}; // someone needs the full row
    }
    columns.insert(columns.end(), watched.begin(), watched.end());
  }
  std::sort(columns.begin(), columns.end());
  columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
  return columns;
}

uint64_t RuntimeHost::CaptureVersion() const { return impl->capture_version.load(); }

// ---- writer transactions ----

void RuntimeHost::OnCapture(uint64_t txn, std::size_t depth, ColumnarBatchRef batch) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto &frames = impl->txns[txn].frames;
  if (frames.size() > depth + 1) {
    throw std::logic_error("capture below an unfinished subtransaction");
  }
  frames.resize(depth + 1);
  frames[depth].push_back(std::move(batch));
}

void RuntimeHost::CommitSubtransaction(uint64_t txn, std::size_t depth) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto it = impl->txns.find(txn);
  if (depth == 0 || it == impl->txns.end() || it->second.frames.size() <= depth) {
    return; // nothing captured at this depth
  }
  auto &frames = it->second.frames;
  auto &parent = frames[depth - 1];
  for (std::size_t d = depth; d < frames.size(); d++) {
    parent.insert(parent.end(), frames[d].begin(), frames[d].end());
  }
  frames.resize(depth);
}

void RuntimeHost::AbortSubtransaction(uint64_t txn, std::size_t depth) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto it = impl->txns.find(txn);
  if (depth > 0 && it != impl->txns.end() && it->second.frames.size() > depth) {
    it->second.frames.resize(depth);
  }
}

bool RuntimeHost::TxnWroteDependency(uint64_t txn, const AcceleratorRuntime &accelerator) const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto it = impl->txns.find(txn);
  if (it == impl->txns.end()) {
    return false;
  }
  const auto tables = accelerator.DependencyTables();
  for (const auto &frame : it->second.frames) {
    for (const auto &batch : frame) {
      if (std::find(tables.begin(), tables.end(), batch->table) != tables.end()) {
        return true;
      }
    }
  }
  return false;
}

void RuntimeHost::PreCommit(uint64_t txn) {
  // Per accelerator (id order, so concurrent commits cannot deadlock): the
  // transaction's batches on its tables.
  std::unordered_map<AcceleratorRuntime *, std::vector<ColumnarBatchRef>> per_runtime;
  std::vector<AcceleratorRuntime *> order;
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto it = impl->txns.find(txn);
    if (it == impl->txns.end() || it->second.frames.empty()) {
      return;
    }
    if (it->second.frames.size() > 1) {
      throw std::logic_error("pre-commit with an open subtransaction");
    }
    for (const auto &batch : it->second.frames[0]) {
      auto runtimes = impl->by_table.find(batch->table);
      if (runtimes == impl->by_table.end()) {
        continue;
      }
      for (auto *runtime : runtimes->second) {
        per_runtime[runtime].push_back(batch);
      }
    }
    for (auto &entry : impl->runtimes) {
      if (per_runtime.count(entry.second.get())) {
        order.push_back(entry.second.get());
      }
    }
  }
  std::vector<AcceleratorRuntime *> staged;
  try {
    for (auto *runtime : order) {
      runtime->BeginCommit(txn, std::move(per_runtime[runtime]));
      staged.push_back(runtime);
    }
  } catch (...) {
    for (auto *runtime : staged) {
      runtime->AbortCommit();
    }
    impl->TakeTxn(txn);
    throw;
  }
  std::lock_guard<std::mutex> lock(impl->mutex);
  impl->txns[txn].staged = std::move(staged);
}

void RuntimeHost::Commit(uint64_t txn, Epoch epoch) {
  for (auto *runtime : impl->TakeTxn(txn).staged) {
    runtime->PublishCommit(epoch);
  }
}

void RuntimeHost::Abort(uint64_t txn) {
  for (auto *runtime : impl->TakeTxn(txn).staged) {
    runtime->AbortCommit();
  }
}

// ---- builds ----

void RuntimeHost::BuildAll() {
  std::vector<AcceleratorRuntime *> stale;
  for (auto *runtime : All()) {
    if (!runtime->IsSynced()) {
      stale.push_back(runtime);
    }
  }
  if (slots.EngineWorkOnHostThread()) {
    for (auto *runtime : stale) {
      runtime->BuildNow();
    }
    return;
  }
  impl->threads.ParallelFor(stale.size(), [&](std::size_t i) { stale[i]->BuildNow(); });
}

bool RuntimeHost::RunEngineWork() {
  std::deque<std::function<void()>> work;
  {
    std::lock_guard<std::mutex> lock(impl->work_mutex);
    work.swap(impl->work);
  }
  for (auto &item : work) {
    item();
  }
  return !work.empty();
}

ThreadBudget &RuntimeHost::Threads() { return impl->threads; }

} // namespace accelerator_codegen::aux_state::runtime
