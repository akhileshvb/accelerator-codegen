// SCAFFOLDING
// C12: one thread budget per backend; every parallel step draws from it.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace accelerator_codegen::aux_state::runtime {

// CPUs in this process's affinity mask (sched_getaffinity), so `taskset`
// limits apply; at least 1. Never std::thread::hardware_concurrency.
std::size_t AffinityCpuCount();

// The budget: `ACCEL_AUX_THREADS` if set (> 0), else `engine_setting` if > 0
// (DuckDB `threads`, Postgres `accel.threads`, MySQL `accel_threads`), else
// AffinityCpuCount().
std::size_t ResolveThreadBudget(std::size_t engine_setting);

// Per-phase caps within the budget: `ACCEL_AUX_READ_THREADS` /
// `ACCEL_AUX_BUILD_THREADS` / `ACCEL_AUX_MAINT_THREADS` if set (> 0), else the
// engine's own setting if > 0, else the whole budget for reads and builds and
// 1 for maintenance; never more than `budget`.
std::size_t ResolveReadThreads(std::size_t engine_setting, std::size_t budget);
std::size_t ResolveBuildThreads(std::size_t engine_setting, std::size_t budget);
std::size_t ResolveMaintenanceThreads(std::size_t engine_setting, std::size_t budget);

// Cross-process token source (Postgres: one shared pool across a database's
// shard workers, in shared memory). In-process engines use none.
class TokenSource {
public:
  virtual ~TokenSource() = default;
  // Take up to `wanted` tokens without blocking; returns how many were taken
  // (0..wanted). Each must be returned with Release.
  virtual std::size_t TryAcquire(std::size_t wanted) = 0;
  virtual void Release(std::size_t count) = 0;
};

// A fixed pool of `budget` worker threads (created lazily), plus the calling
// thread. ParallelFor never uses more than `budget` threads in total across
// all concurrent callers of this pool (and, with a TokenSource, across
// processes): a caller that cannot get extra workers runs its tasks itself.
class ThreadBudget {
public:
  explicit ThreadBudget(std::size_t budget, std::shared_ptr<TokenSource> tokens = nullptr);
  ~ThreadBudget();
  ThreadBudget(const ThreadBudget &) = delete;
  ThreadBudget &operator=(const ThreadBudget &) = delete;

  std::size_t Budget() const;
  void SetBudget(std::size_t budget); // takes effect for later ParallelFor calls

  // Caps for module reads, builds and maintenance, each at most Budget().
  // Defaults: the budget for reads and builds, 1 (serial) for maintenance.
  std::size_t ReadLimit() const;
  std::size_t BuildLimit() const;
  std::size_t MaintenanceLimit() const;
  void SetPhaseLimits(std::size_t read, std::size_t build, std::size_t maintenance = 1);

  // Runs fn(i) for i in [0, n), on the calling thread plus up to
  // min(n, max_workers, Budget()) - 1 pool workers. Returns when all are done;
  // rethrows the first exception (remaining tasks are skipped).
  void ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn,
                   std::size_t max_workers = static_cast<std::size_t>(-1));

  // Runs `fn` on a pool worker in the background (builds), counted against
  // the budget while it runs. The returned handle's Wait() blocks until done
  // and rethrows its exception.
  class Task {
  public:
    virtual ~Task() = default;
    virtual void Wait() = 0;
    virtual bool Done() const = 0;
  };
  std::shared_ptr<Task> Spawn(std::function<void()> fn);

  // Highest number of threads (callers + workers) running pool work at once
  // since construction or the last ResetPeak (eval reporting).
  std::size_t PeakThreads() const;
  void ResetPeak();

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace accelerator_codegen::aux_state::runtime
