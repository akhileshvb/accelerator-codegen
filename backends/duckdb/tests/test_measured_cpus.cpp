#include "eval/bench/driver/measured_cpus.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

using accelerator_codegen::benchmarks::driver::ParseCpuList;
using accelerator_codegen::benchmarks::driver::PinToMeasuredCpus;

int main() {
  if (ParseCpuList("16,36") != std::vector<int>{16, 36} || ParseCpuList("8-10,20") != std::vector<int>{8, 9, 10, 20}) {
    std::cerr << "ParseCpuList\n";
    return 1;
  }
  unsetenv("ACCEL_MEASURE_CPUS");
  if (PinToMeasuredCpus(3) != 3) {
    std::cerr << "unset must leave the thread count alone\n";
    return 1;
  }
  setenv("ACCEL_MEASURE_CPUS", "0", 1);
  const int threads = PinToMeasuredCpus(3);
  return threads == 1 || threads == 3 ? 0 : 1; // pinned to cpu 0, or cpu 0 is outside this sandbox's mask
}
