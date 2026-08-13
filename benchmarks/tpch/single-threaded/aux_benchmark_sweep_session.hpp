#pragma once

#include <string>
#include <vector>

namespace accelerator_codegen::benchmarks::aux_sweep {

void RunMultiFrequencySweep(int query_num, const std::string &db_path,
                            const std::string &suite, const std::string &locality,
                            const std::vector<int> &write_frequencies,
                            int warmup_queries, int measured_queries,
                            int ref_samples, int base_write_samples);

// Wall-clock invalidating writes every N seconds (matches aux workload write-interval sweeps).
void RunMultiWriteIntervalSweep(int query_num, const std::string &db_path,
                                const std::string &suite,
                                const std::string &locality,
                                const std::vector<double> &write_intervals_sec,
                                int warmup_queries, double base_duration_sec,
                                int min_writes, int ref_samples,
                                int base_write_samples);

std::vector<int> ParseWriteFrequencyList(const std::string &spec);
std::vector<double> ParseWriteIntervalList(const std::string &spec);

// Dispatches to wall-clock interval or query-count frequency sweep based on env.
void RunConfiguredMultiSweep(int query_num, const std::string &db_path,
                             const std::string &suite, const std::string &locality,
                             int warmup_queries, int measured_queries,
                             int ref_samples, int base_write_samples);

} // namespace accelerator_codegen::benchmarks::aux_sweep
