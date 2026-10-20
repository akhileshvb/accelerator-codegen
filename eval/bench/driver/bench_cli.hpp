// SCAFFOLDING: engine-neutral pieces of the shared bench/calibrator drivers
// (benchmark_<engine>_<bench>_aux, calibrate_<engine>_<bench>_aux_cost_model).
// An engine's driver adds only its adapter: how to connect, run SQL and apply writes.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace accelerator_codegen::benchmarks::driver {

using Clock = std::chrono::steady_clock;

// Command line shared by every engine's driver: `--query N` plus positional
// arguments (db path, write percentage, output path, ...). A leading positional
// integer is read as the query when `--query` is absent.
struct Args {
  int query = 0;
  std::vector<std::string> positional;

  Args(int argc, char **argv, int default_query = 0) : query(default_query) {
    bool query_given = false;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--query") {
        if (i + 1 >= argc) {
          throw std::invalid_argument("--query needs a number");
        }
        query = std::stoi(argv[++i]);
        query_given = true;
      } else {
        positional.push_back(arg);
      }
    }
    if (!query_given && !positional.empty() && positional[0].find_first_not_of("0123456789") == std::string::npos) {
      query = std::stoi(positional[0]);
      positional.erase(positional.begin());
    }
    if (query <= 0) {
      throw std::invalid_argument("usage: --query N (the TPC-H query number)");
    }
  }

  std::string Positional(std::size_t index, const std::string &fallback = "") const {
    return index < positional.size() ? positional[index] : fallback;
  }
};

inline int EnvInt(const char *name, int fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stoi(value) : fallback;
}

inline double EnvDouble(const char *name, double fallback) {
  const char *value = std::getenv(name);
  return value && *value ? std::stod(value) : fallback;
}

inline std::string EnvString(const char *name, const std::string &fallback = "") {
  const char *value = std::getenv(name);
  return value ? value : fallback;
}

// Per-query knobs are ACCEL_Q<N>_<SUFFIX>.
inline std::string QueryEnvName(int query, const char *suffix) {
  return "ACCEL_Q" + std::to_string(query) + "_" + suffix;
}

inline double ElapsedUs(const Clock::time_point &start, const Clock::time_point &end) {
  return std::chrono::duration<double, std::micro>(end - start).count();
}

inline double Median(std::vector<double> samples) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const auto middle = samples.size() / 2;
  return samples.size() % 2 == 0 ? (samples[middle - 1] + samples[middle]) / 2.0 : samples[middle];
}

inline double Mean(const std::vector<double> &samples) {
  double sum = 0.0;
  for (const double sample : samples) {
    sum += sample;
  }
  return samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
}

} // namespace accelerator_codegen::benchmarks::driver
