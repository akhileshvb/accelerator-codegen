// SCAFFOLDING: pins a bench's warmup and measured loop to the measurement lease's cores.
// A bench builds its state on the run's whole core budget, then calls PinToMeasuredCpus once before its warmup:
// every thread of the process, the engine's pool included, moves to ACCEL_MEASURE_CPUS (a cpu list such as `16,36`
// or `8-11`), and threads created later inherit it. Unset, nothing moves.
#pragma once

#include <dirent.h>
#include <sched.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace accelerator_codegen::benchmarks::driver {

inline std::vector<int> ParseCpuList(const std::string &list) {
  std::vector<int> cpus;
  std::stringstream stream(list);
  std::string part;
  while (std::getline(stream, part, ',')) {
    const auto dash = part.find('-');
    const int first = std::stoi(part.substr(0, dash));
    const int last = dash == std::string::npos ? first : std::stoi(part.substr(dash + 1));
    for (int cpu = first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
}

// The cores this process may run on: the run's whole core budget.
inline int RunCoreBudget() {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  return sched_getaffinity(0, sizeof(mask), &mask) == 0 ? CPU_COUNT(&mask) : 1;
}

// Returns how many cores the measured run has: the pinned list's size, else `unpinned_threads`.
inline int PinToMeasuredCpus(int unpinned_threads) {
  const char *list = std::getenv("ACCEL_MEASURE_CPUS");
  if (list == nullptr || *list == '\0') {
    return unpinned_threads;
  }
  const std::vector<int> cpus = ParseCpuList(list);
  cpu_set_t mask;
  CPU_ZERO(&mask);
  for (const int cpu : cpus) {
    CPU_SET(cpu, &mask);
  }
  DIR *tasks = opendir("/proc/self/task");
  bool pinned = tasks != nullptr;
  while (tasks != nullptr) {
    const dirent *task = readdir(tasks);
    if (task == nullptr) {
      break;
    }
    const int tid = std::atoi(task->d_name);
    pinned = pinned && (tid == 0 || sched_setaffinity(tid, sizeof(mask), &mask) == 0);
  }
  if (tasks != nullptr) {
    closedir(tasks);
  }
  if (!pinned) {
    std::cerr << "[measure] cannot pin to cores " << list << ": measuring unpinned\n";
    return unpinned_threads;
  }
  std::cerr << "[measure] measured run pinned to cores " << list << '\n';
  return static_cast<int>(cpus.size());
}

} // namespace accelerator_codegen::benchmarks::driver
