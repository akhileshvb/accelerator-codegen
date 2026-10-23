// SCAFFOLDING
#include "substrate/aux/core/maintenance_trace.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace accelerator_codegen::aux_state::core::trace {
namespace {
using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point from, Clock::time_point to = Clock::now()) {
  return std::chrono::duration<double>(to - from).count();
}

double EnvSeconds(const char *name, double fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::atof(value) : fallback;
}

double RssGb() {
  unsigned long long pages = 0, resident = 0;
  if (FILE *statm = std::fopen("/proc/self/statm", "r")) {
    if (std::fscanf(statm, "%llu %llu", &pages, &resident) != 2) {
      resident = 0;
    }
    std::fclose(statm);
  }
  return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / 1e9;
}

// One write(2) per line: whole and immediately visible, whichever thread logs.
void Line(const std::string &text) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  char head[96];
  const int n = std::snprintf(head, sizeof head, "MAINT_TRACE t=%.3f rss=%.1fGB ",
                              std::chrono::duration<double>(now).count(), RssGb());
  std::string line(head, static_cast<std::size_t>(n));
  line += text;
  line += '\n';
  const char *p = line.data();
  for (std::size_t left = line.size(); left > 0;) {
    const ssize_t w = ::write(STDERR_FILENO, p, left);
    if (w <= 0) {
      return;
    }
    p += w;
    left -= static_cast<std::size_t>(w);
  }
}

std::string Fixed(double v, int digits = 3) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.*f", digits, v);
  return buf;
}

// What is running now, for the heartbeat and the commit summary.
class Registry {
public:
  static Registry &Get() {
    static Registry instance;
    return instance;
  }
  struct OpenSpan {
    std::string name;
    Clock::time_point start;
  };
  struct OpenMaintenance {
    std::string accelerator;
    Clock::time_point start;
    const char *phase = "start";
  };

  uint64_t OpenSpanId(const char *name, Clock::time_point start) {
    std::lock_guard<std::mutex> lock(mutex);
    StartHeartbeatLocked();
    const uint64_t id = ++next_span;
    spans[id] = {name, start};
    if (std::string(name) == "commit") {
      finished.clear();
    }
    return id;
  }
  void CloseSpan(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    spans.erase(id);
  }
  uint64_t OpenMaintenanceId(const std::string &accelerator, Clock::time_point start) {
    std::lock_guard<std::mutex> lock(mutex);
    StartHeartbeatLocked();
    const uint64_t id = ++next_maintenance;
    running[id] = {accelerator, start};
    return id;
  }
  void SetPhase(uint64_t id, const char *phase) {
    std::lock_guard<std::mutex> lock(mutex);
    running[id].phase = phase;
  }
  void CloseMaintenance(uint64_t id, double seconds) {
    std::lock_guard<std::mutex> lock(mutex);
    finished.emplace_back(running[id].accelerator, seconds);
    running.erase(id);
  }
  std::string Slowest(std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex);
    auto sorted = finished;
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    std::string out;
    for (std::size_t i = 0; i < std::min(count, sorted.size()); ++i) {
      out += (out.empty() ? "" : ",") + sorted[i].first + ":" + Fixed(sorted[i].second) + "s";
    }
    return out;
  }

private:
  Registry() = default;
  ~Registry() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    wake.notify_all();
    if (heartbeat.joinable()) {
      heartbeat.join();
    }
  }

  void StartHeartbeatLocked() {
    if (!heartbeat.joinable()) {
      heartbeat = std::thread([this] { RunHeartbeat(); });
    }
  }

  void RunHeartbeat() {
    const double after = EnvSeconds("ACCEL_AUX_TRACE_HEARTBEAT_AFTER_S", 60);
    const auto period = std::chrono::duration<double>(EnvSeconds("ACCEL_AUX_TRACE_HEARTBEAT_S", 60));
    std::unique_lock<std::mutex> lock(mutex);
    while (!stop) {
      wake.wait_for(lock, period, [this] { return stop; });
      if (stop) {
        return;
      }
      const auto now = Clock::now();
      std::string text;
      for (const auto &[id, span] : spans) {
        if (Seconds(span.start, now) >= after) {
          text += (text.empty() ? "" : " ") + span.name + "#" + std::to_string(id) + "=" + Fixed(Seconds(span.start, now), 0) + "s";
        }
      }
      if (text.empty()) {
        continue;
      }
      std::string maintaining;
      for (const auto &[id, m] : running) {
        maintaining += (maintaining.empty() ? "" : ", ") + m.accelerator + " " + m.phase + " " + Fixed(Seconds(m.start, now), 0) + "s";
      }
      const auto count = running.size();
      lock.unlock();
      Line("heartbeat open: " + text + " | maintaining " + std::to_string(count) + ": " +
           (maintaining.empty() ? "none" : maintaining));
      lock.lock();
    }
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::thread heartbeat;
  bool stop = false;
  uint64_t next_span = 0, next_maintenance = 0;
  std::map<uint64_t, OpenSpan> spans;
  std::map<uint64_t, OpenMaintenance> running;
  std::vector<std::pair<std::string, double>> finished; // since the last commit began
};

// The calling thread's current maintenance and its phase bookkeeping.
struct ThreadPhases {
  uint64_t id = 0;
  const char *phase = nullptr;
  Clock::time_point phase_start;
  double credited_us = 0;
  std::string action = "none";
  std::string timings; // "stage=0.1s apply=2.3s ..."

  void Close() {
    if (!phase) {
      return;
    }
    const double s = std::max(0.0, Seconds(phase_start) - credited_us / 1e6);
    timings += (timings.empty() ? "" : " ") + std::string(phase) + "=" + Fixed(s) + "s";
    phase = nullptr;
  }
};
thread_local ThreadPhases *t_phases = nullptr;
} // namespace

bool Enabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("ACCEL_AUX_TRACE_MAINT");
    return value && *value && value[0] != '0';
  }();
  return enabled;
}

Span::Span(const char *name, std::string detail) : active_(Enabled()), name_(name) {
  if (!active_) {
    return;
  }
  start_ = Clock::now();
  id_ = Registry::Get().OpenSpanId(name, start_);
  Line(std::string(name_) + "#" + std::to_string(id_) + " begin" + (detail.empty() ? "" : " " + detail));
}

Span::~Span() {
  if (!active_) {
    return;
  }
  Registry::Get().CloseSpan(id_);
  std::string text = std::string(name_) + "#" + std::to_string(id_) + " end took=" + Fixed(Seconds(start_)) + "s";
  if (std::string(name_) == "commit") {
    text += " slowest=" + Registry::Get().Slowest(3);
  }
  Line(text + (end_detail_.empty() ? "" : " " + end_detail_));
}

Maintenance::Maintenance(const std::string &accelerator, uint64_t rows)
    : active_(Enabled()), accelerator_(accelerator), rows_(rows) {
  if (!active_) {
    return;
  }
  start_ = Clock::now();
  id_ = Registry::Get().OpenMaintenanceId(accelerator_, start_);
  auto *phases = new ThreadPhases();
  phases->id = id_;
  t_phases = phases;
  Line("accel " + accelerator_ + " begin rows=" + std::to_string(rows_));
}

Maintenance::~Maintenance() {
  if (!active_) {
    return;
  }
  std::unique_ptr<ThreadPhases> phases(t_phases);
  t_phases = nullptr;
  phases->Close();
  const double seconds = Seconds(start_);
  Registry::Get().CloseMaintenance(id_, seconds);
  Line("accel " + accelerator_ + " end action=" + phases->action + " rows=" + std::to_string(rows_) +
       " took=" + Fixed(seconds) + "s " + phases->timings);
}

void EnterPhase(const char *name) {
  if (!t_phases) {
    return;
  }
  t_phases->Close();
  t_phases->phase = name;
  t_phases->phase_start = Clock::now();
  t_phases->credited_us = 0;
  Registry::Get().SetPhase(t_phases->id, name);
}

void SetAction(const char *action) {
  if (t_phases) {
    t_phases->action = action;
  }
}

void Note(const std::string &text) {
  if (Enabled()) {
    Line(text);
  }
}

CreditScope::CreditScope(const char *phase) : active_(t_phases != nullptr), phase_(phase) {
  if (active_) {
    start_ = Clock::now();
  }
}

CreditScope::~CreditScope() {
  if (!active_ || !t_phases) {
    return;
  }
  const double us = Seconds(start_) * 1e6;
  t_phases->credited_us += us;
  t_phases->timings += (t_phases->timings.empty() ? "" : " ") + std::string(phase_) + "=" + Fixed(us / 1e6) + "s";
}

} // namespace accelerator_codegen::aux_state::core::trace
