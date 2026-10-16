// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/state/shard_gucs.hpp"
#include "backends/postgres/state/shard_worker.hpp"

extern "C" {
#include <utils/guc.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {
namespace {

int g_read_timeout_ms = 60000;
int g_read_wait_ms = 5000;
int g_read_threads = 4;
int g_idle_exit_s = 0;
int g_phase_read_threads = 0;
int g_phase_build_threads = 0;
int g_phase_maint_threads = 0;
bool g_build_tuning = true;

} // namespace

int ReadTimeoutMs() { return g_read_timeout_ms; }
int ReadWaitMs() { return g_read_wait_ms; }
int ReadThreads() { return g_read_threads; }
int PhaseReadThreads() { return g_phase_read_threads; }
int PhaseBuildThreads() { return g_phase_build_threads; }
int PhaseMaintenanceThreads() { return g_phase_maint_threads; }
int IdleExitSeconds() { return g_idle_exit_s; }
bool BuildTuning() { return g_build_tuning; }

void DefineGucs() {
  DefineCustomIntVariable("accel.read_timeout_ms",
                          "How long a query waits for an accelerator's shard worker before it "
                          "answers from SQL.",
                          nullptr, &g_read_timeout_ms, 60000, 100, 3600000, PGC_USERSET, 0, nullptr,
                          nullptr, nullptr);
  DefineCustomIntVariable("accel.read_wait_ms",
                          "How long a shard worker makes a read wait for commits in flight, a "
                          "build or a catch-up before declining it (the query then answers from "
                          "SQL).",
                          nullptr, &g_read_wait_ms, 5000, 0, 3600000, PGC_USERSET, 0, nullptr, nullptr,
                          nullptr);
  DefineCustomIntVariable("accel.shard_read_threads", "Threads serving reads in each shard worker.",
                          nullptr, &g_read_threads, 4, 1, 64, PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
  DefineCustomIntVariable("accel.read_threads", "Threads a read may use (0: the accel.threads budget).",
                          nullptr, &g_phase_read_threads, 0, 0, 4096, PGC_SIGHUP, 0, nullptr, nullptr,
                          nullptr);
  DefineCustomIntVariable("accel.build_threads", "Threads a build may use (0: the accel.threads budget).",
                          nullptr, &g_phase_build_threads, 0, 0, 4096, PGC_SIGHUP, 0, nullptr, nullptr,
                          nullptr);
  DefineCustomIntVariable("accel.maint_threads", "Threads a maintenance pass may use (0: one).", nullptr,
                          &g_phase_maint_threads, 0, 0, 4096, PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
  DefineCustomIntVariable("accel.shard_idle_exit_s",
                          "A shard worker exits after this many idle seconds (0: never).", nullptr,
                          &g_idle_exit_s, 0, 0, 86400, PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
  DefineCustomBoolVariable("accel.build_tuning",
                           "Plan the one-off state build with in-memory hash joins (work_mem, no "
                           "nested loop / merge joins, no JIT) instead of the session settings.",
                           nullptr, &g_build_tuning, true, PGC_SIGHUP, 0, nullptr, nullptr, nullptr);
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

#endif
