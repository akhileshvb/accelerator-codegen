// SCAFFOLDING
// Opt-in live tracing of commits and per-accelerator maintenance (ACCEL_AUX_TRACE_MAINT=1).
// One flushed stderr line per event, prefixed "MAINT_TRACE t=<unix seconds> rss=<GB>". While a commit overruns, a
// heartbeat names the accelerators still maintaining (ACCEL_AUX_TRACE_HEARTBEAT_AFTER_S, default 60, then every
// ACCEL_AUX_TRACE_HEARTBEAT_S, default 60). Every entry point is a no-op unless enabled.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace accelerator_codegen::aux_state::core::trace {

bool Enabled();

// A begin line and, at destruction, an end line with the duration, around a phase. Open spans feed the heartbeat.
// `name` is "commit" for a whole commit (its end line adds the slowest accelerators) or any engine-side phase.
class Span {
public:
  Span(const char *name, std::string detail = {});
  ~Span();
  void SetEndDetail(std::string detail) { end_detail_ = std::move(detail); }
  Span(const Span &) = delete;
  Span &operator=(const Span &) = delete;

private:
  bool active_;
  uint64_t id_ = 0;
  const char *name_;
  std::string end_detail_;
  std::chrono::steady_clock::time_point start_;
};

// One accelerator's maintenance of one commit, on the calling thread; the hooks below act on the calling thread's
// current Maintenance and do nothing outside one.
class Maintenance {
public:
  Maintenance(const std::string &accelerator, uint64_t rows);
  ~Maintenance();
  Maintenance(const Maintenance &) = delete;
  Maintenance &operator=(const Maintenance &) = delete;

private:
  bool active_;
  uint64_t id_ = 0;
  std::string accelerator_;
  uint64_t rows_;
  std::chrono::steady_clock::time_point start_;
};

// Ends the current phase and starts `name` (a string literal); time credited meanwhile is excluded from it.
void EnterPhase(const char *name);
void SetAction(const char *action);

// Times its scope as a nested phase (e.g. "reclaim") of the current phase.
class CreditScope {
public:
  explicit CreditScope(const char *phase);
  ~CreditScope();

private:
  bool active_;
  const char *phase_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace accelerator_codegen::aux_state::core::trace
