// SCAFFOLDING: compare a query's engine SQL with the accelerator over N random bindings and print both medians.
// For an agent's own micro-benchmark: the callables take the binding index and run one read (draw the binding from
// your own generator, seeded per index), e.g.
//   CompareReadLatency("q", 200, [&](int i) { RunSql(Bind(i)); }, [&](int i) { RunAccelerator(Bind(i)); });
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace accelerator_codegen::benchmarks::driver {

struct ReadLatency {
  double reference_median_us = 0.0;
  double accelerator_median_us = 0.0;
  double Speedup() const { return accelerator_median_us > 0.0 ? reference_median_us / accelerator_median_us : 0.0; }
};

template <typename Fn> double MedianMicros(int count, Fn &&read) {
  std::vector<double> micros;
  micros.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const auto start = std::chrono::steady_clock::now();
    read(i);
    micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
  }
  std::sort(micros.begin(), micros.end());
  return micros.empty() ? 0.0 : micros[micros.size() / 2];
}

// One untimed warm-up read of each side, then `count` timed reads of each over the same binding indices.
template <typename ReferenceFn, typename AcceleratorFn>
ReadLatency CompareReadLatency(const char *label, int count, ReferenceFn &&reference, AcceleratorFn &&accelerator) {
  reference(0);
  accelerator(0);
  ReadLatency result;
  result.reference_median_us = MedianMicros(count, reference);
  result.accelerator_median_us = MedianMicros(count, accelerator);
  std::printf("%s: engine SQL median %.1f us, accelerator median %.1f us, speedup %.2fx over %d bindings\n", label,
              result.reference_median_us, result.accelerator_median_us, result.Speedup(), count);
  return result;
}

} // namespace accelerator_codegen::benchmarks::driver
