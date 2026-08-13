#pragma once

#include <string>

namespace accelerator_codegen::benchmarks {

struct AuxBenchmarkBaseline {
  double duckdb_ref_query_median_us = 0.0;
  double base_kernel_median_us = 0.0;
  bool valid = false;
};

std::string DefaultAuxBenchmarkBaselinePath(int query_num);

AuxBenchmarkBaseline LoadAuxBenchmarkBaseline(const std::string &path, int query_num,
                                              const std::string &cache_key);

void SaveAuxBenchmarkBaseline(const std::string &path, int query_num,
                            const std::string &cache_key,
                            const AuxBenchmarkBaseline &baseline);

} // namespace accelerator_codegen::benchmarks
