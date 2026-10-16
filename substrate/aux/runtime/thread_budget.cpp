// SCAFFOLDING
// C12: ThreadBudget, the shared worker pool every parallel step draws from.
#include "substrate/aux/runtime/thread_budget.hpp"

#include <pthread.h>
#include <sched.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace accelerator_codegen::aux_state::runtime {

namespace {

// Pool threads never take signals: the host engine's handlers (Postgres)
// expect to run on its own thread. A new thread inherits the creator's mask.
template <typename F>
std::thread StartThread(F &&body) {
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  std::thread thread(std::forward<F>(body));
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  return thread;
}

} // namespace

std::size_t AffinityCpuCount() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    return 1;
  }
  return std::max<std::size_t>(1, static_cast<std::size_t>(CPU_COUNT(&set)));
}

std::size_t ResolveThreadBudget(std::size_t engine_setting) {
  if (const char *env = std::getenv("ACCEL_AUX_THREADS")) {
    const long long value = std::atoll(env);
    if (value > 0) {
      return static_cast<std::size_t>(value);
    }
  }
  return engine_setting > 0 ? engine_setting : AffinityCpuCount();
}

namespace {

std::size_t ResolvePhaseThreads(const char *env_name, std::size_t engine_setting,
                                std::size_t budget, std::size_t fallback) {
  std::size_t wanted = engine_setting;
  if (const char *env = std::getenv(env_name)) {
    const long long value = std::atoll(env);
    wanted = value > 0 ? static_cast<std::size_t>(value) : wanted;
  }
  return std::min(wanted > 0 ? wanted : fallback, budget);
}

} // namespace

std::size_t ResolveReadThreads(std::size_t engine_setting, std::size_t budget) {
  return ResolvePhaseThreads("ACCEL_AUX_READ_THREADS", engine_setting, budget, budget);
}

std::size_t ResolveBuildThreads(std::size_t engine_setting, std::size_t budget) {
  return ResolvePhaseThreads("ACCEL_AUX_BUILD_THREADS", engine_setting, budget, budget);
}

std::size_t ResolveMaintenanceThreads(std::size_t engine_setting, std::size_t budget) {
  return ResolvePhaseThreads("ACCEL_AUX_MAINT_THREADS", engine_setting, budget, 1);
}

namespace {

// The pool whose work the current thread is running; nested ParallelFor
// calls on it are already counted.
thread_local const void *counted_pool = nullptr;

} // namespace

// Every thread running pool work (a ParallelFor caller, a worker running a
// helper or a spawned task) is counted in `in_use`. Extra workers and spawned
// tasks are granted only while in_use < budget. A ParallelFor caller whose
// pool is full waits for a slot while helpers hold some (they yield between
// tasks when a caller waits); with none to yield (only callers and spawned
// tasks fill the pool) it runs anyway, counted, so it is never refused.
// A spawned task that found no budget runs on a dedicated thread, counted.
struct ThreadBudget::Impl {
  std::shared_ptr<TokenSource> tokens;
  // mutex guards every field below except waiting_callers, and is a leaf lock
  // (jobs run with it released). `wake` signals workers that a job or shutdown
  // arrived; `freed` signals ParallelFor callers that in_use dropped.
  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable freed;
  std::size_t budget;
  std::size_t read_limit = 0; // 0: the whole budget
  std::size_t build_limit = 0;
  std::size_t maintenance_limit = 1;
  std::size_t in_use = 0;
  std::size_t peak = 0;
  std::size_t idle = 0;         // workers waiting for a job
  std::size_t helper_slots = 0; // granted ParallelFor helpers not yet finished
  std::atomic<std::size_t> waiting_callers{0};
  bool shutdown = false;
  std::deque<std::function<void()>> jobs;
  std::vector<std::thread> workers;
  std::list<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> dedicated;

  Impl(std::size_t budget_p, std::shared_ptr<TokenSource> tokens_p)
      : tokens(std::move(tokens_p)), budget(std::max<std::size_t>(1, budget_p)) {}

  void AddInUse(std::size_t count) { // mutex held
    in_use += count;
    peak = std::max(peak, in_use);
  }

  void Release(std::size_t count, std::size_t tokens_held, bool helper = false) {
    if (tokens_held > 0) {
      tokens->Release(tokens_held);
    }
    std::lock_guard<std::mutex> lock(mutex);
    in_use -= count;
    helper_slots -= helper ? count : 0;
    freed.notify_all();
  }

  // Takes up to `wanted` extra slots from the budget (and the token source).
  // None once shutting down: callers then run their tasks themselves.
  std::size_t GrantExtra(std::size_t wanted) { // mutex held
    const std::size_t free_slots = !shutdown && budget > in_use ? budget - in_use : 0;
    std::size_t granted = std::min(wanted, free_slots);
    if (granted > 0 && tokens) {
      granted = tokens->TryAcquire(granted);
    }
    AddInUse(granted);
    return granted;
  }

  void Enqueue(std::function<void()> job) { // mutex held
    jobs.push_back(std::move(job));
    if (jobs.size() > idle && workers.size() < budget) {
      workers.push_back(StartThread([this] { WorkerLoop(); }));
    }
    wake.notify_one();
  }

  void WorkerLoop() {
    counted_pool = this;
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      ++idle;
      wake.wait(lock, [&] { return shutdown || !jobs.empty(); });
      --idle;
      if (jobs.empty()) {
        return;
      }
      auto job = std::move(jobs.front());
      jobs.pop_front();
      lock.unlock();
      job();
      lock.lock();
    }
  }

  void ReapDedicated() { // mutex held
    for (auto it = dedicated.begin(); it != dedicated.end();) {
      if (it->second->load()) {
        it->first.join();
        it = dedicated.erase(it);
      } else {
        ++it;
      }
    }
  }
};

namespace {

// State shared by one ParallelFor call and its helpers.
struct ForState {
  const std::function<void(std::size_t)> &fn;
  std::size_t n;
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::mutex mutex;
  std::condition_variable idle;
  std::size_t running = 0;
  bool closed = false;
  std::exception_ptr error;

  // Claims tasks until none are left, a task failed, or `yield` says stop.
  void Run(const std::function<bool()> &yield = nullptr) {
    try {
      for (;;) {
        if (failed || (yield && yield())) {
          return;
        }
        const std::size_t i = next++;
        if (i >= n) {
          return;
        }
        fn(i);
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      if (!failed.exchange(true)) {
        error = std::current_exception();
      }
    }
  }

  // Helper entry: false if the call already returned (fn must not be used).
  bool Enter() {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed) {
      return false;
    }
    ++running;
    return true;
  }
  void Leave() {
    std::lock_guard<std::mutex> lock(mutex);
    --running;
    idle.notify_all();
  }
  void Close() {
    std::unique_lock<std::mutex> lock(mutex);
    closed = true;
    idle.wait(lock, [&] { return running == 0; });
  }
};

class SpawnedTask : public ThreadBudget::Task {
public:
  void Wait() override {
    std::unique_lock<std::mutex> lock(mutex);
    done_cv.wait(lock, [&] { return done; });
    if (error) {
      std::rethrow_exception(error);
    }
  }
  bool Done() const override {
    std::lock_guard<std::mutex> lock(mutex);
    return done;
  }
  void Finish(std::exception_ptr e) {
    std::lock_guard<std::mutex> lock(mutex);
    error = std::move(e);
    done = true;
    done_cv.notify_all();
  }

private:
  mutable std::mutex mutex;
  std::condition_variable done_cv;
  bool done = false;
  std::exception_ptr error;
};

} // namespace

ThreadBudget::ThreadBudget(std::size_t budget, std::shared_ptr<TokenSource> tokens)
    : impl(std::make_unique<Impl>(budget, std::move(tokens))) {}

ThreadBudget::~ThreadBudget() {
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->shutdown = true;
  }
  impl->wake.notify_all();
  for (auto &worker : impl->workers) { // no worker starts after shutdown
    worker.join();
  }
  // Tasks still running may start dedicated threads until they finish.
  for (;;) {
    decltype(impl->dedicated) finishing;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      finishing.swap(impl->dedicated);
    }
    if (finishing.empty()) {
      break;
    }
    for (auto &entry : finishing) {
      entry.first.join();
    }
  }
}

std::size_t ThreadBudget::Budget() const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return impl->budget;
}

void ThreadBudget::SetBudget(std::size_t budget) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  impl->budget = std::max<std::size_t>(1, budget);
}

std::size_t ThreadBudget::ReadLimit() const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return impl->read_limit > 0 ? std::min(impl->read_limit, impl->budget) : impl->budget;
}

std::size_t ThreadBudget::BuildLimit() const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return impl->build_limit > 0 ? std::min(impl->build_limit, impl->budget) : impl->budget;
}

std::size_t ThreadBudget::MaintenanceLimit() const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return std::max<std::size_t>(1, std::min(impl->maintenance_limit, impl->budget));
}

void ThreadBudget::SetPhaseLimits(std::size_t read, std::size_t build, std::size_t maintenance) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  impl->read_limit = read;
  impl->build_limit = build;
  impl->maintenance_limit = maintenance;
}

void ThreadBudget::ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn,
                               std::size_t max_workers) {
  if (n == 0) {
    return;
  }
  const bool counted = counted_pool == impl.get();
  auto state = std::shared_ptr<ForState>(new ForState{fn, n});
  std::size_t helpers = 0;
  {
    std::unique_lock<std::mutex> lock(impl->mutex);
    if (!counted) {
      ++impl->waiting_callers;
      impl->freed.wait(lock, [&] { return impl->in_use < impl->budget || impl->helper_slots == 0; });
      --impl->waiting_callers;
      impl->AddInUse(1);
    }
    const std::size_t wanted = std::min({n, max_workers, impl->budget}) - 1;
    helpers = impl->GrantExtra(wanted);
    impl->helper_slots += helpers;
    for (std::size_t h = 0; h < helpers; ++h) {
      impl->Enqueue([impl = impl.get(), state] {
        if (state->Enter()) {
          state->Run([impl] { return impl->waiting_callers.load() > 0; });
          state->Leave();
        }
        impl->Release(1, impl->tokens ? 1 : 0, true);
      });
    }
  }
  counted_pool = impl.get();
  state->Run();
  counted_pool = counted ? impl.get() : nullptr;
  state->Close();
  if (!counted) {
    impl->Release(1, 0);
  }
  if (state->error) {
    std::rethrow_exception(state->error);
  }
}

std::shared_ptr<ThreadBudget::Task> ThreadBudget::Spawn(std::function<void()> fn) {
  auto task = std::make_shared<SpawnedTask>();
  Impl *pool = impl.get();
  const bool has_tokens = pool->tokens != nullptr;
  auto body = [pool, task, fn = std::move(fn)](std::size_t tokens_held) {
    std::exception_ptr error;
    try {
      fn();
    } catch (...) {
      error = std::current_exception();
    }
    pool->Release(1, tokens_held);
    task->Finish(std::move(error));
  };

  std::lock_guard<std::mutex> lock(pool->mutex);
  pool->ReapDedicated();
  if (pool->GrantExtra(1) == 1) {
    pool->Enqueue([body, has_tokens] { body(has_tokens ? 1 : 0); });
  } else {
    // No budget: still run, on a dedicated thread, counted while it runs.
    pool->AddInUse(1);
    auto finished = std::make_shared<std::atomic<bool>>(false);
    pool->dedicated.emplace_back(
        StartThread([pool, body, finished] {
          counted_pool = pool;
          body(0);
          finished->store(true);
        }),
        finished);
  }
  return task;
}

std::size_t ThreadBudget::PeakThreads() const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return impl->peak;
}

void ThreadBudget::ResetPeak() {
  std::lock_guard<std::mutex> lock(impl->mutex);
  impl->peak = impl->in_use;
}

} // namespace accelerator_codegen::aux_state::runtime
