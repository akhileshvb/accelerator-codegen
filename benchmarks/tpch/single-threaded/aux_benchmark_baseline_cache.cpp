#include "aux_benchmark_baseline_cache.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace accelerator_codegen::benchmarks {
namespace {

std::string EscapeJson(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    if (ch == '"' || ch == '\\') {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

bool ParseIntField(const std::string &content, const char *key, int &out) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = content.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  const auto start = pos + needle.size();
  out = std::stoi(content.substr(start));
  return true;
}

bool ParseStringField(const std::string &content, const char *key, std::string &out) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const auto pos = content.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  const auto start = pos + needle.size();
  const auto end = content.find('"', start);
  if (end == std::string::npos) {
    return false;
  }
  out = content.substr(start, end - start);
  return true;
}

bool ParseDoubleField(const std::string &content, const char *key, double &out) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = content.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  const auto start = pos + needle.size();
  out = std::stod(content.substr(start));
  return true;
}

std::string ReadFile(const std::string &path) {
  std::ifstream input(path);
  if (!input.good()) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

} // namespace

std::string DefaultAuxBenchmarkBaselinePath(int query_num) {
  return "config/q" + std::to_string(query_num) + "_aux_benchmark_baseline.json";
}

AuxBenchmarkBaseline LoadAuxBenchmarkBaseline(const std::string &path, int query_num,
                                              const std::string &cache_key) {
  AuxBenchmarkBaseline baseline;
  const auto content = ReadFile(path);
  if (content.empty()) {
    return baseline;
  }

  int stored_query = 0;
  std::string stored_key;
  if (!ParseIntField(content, "query", stored_query) || stored_query != query_num) {
    return baseline;
  }
  if (!ParseStringField(content, "cache_key", stored_key) || stored_key != cache_key) {
    return baseline;
  }
  if (!ParseDoubleField(content, "duckdb_ref_query_median_us",
                        baseline.duckdb_ref_query_median_us) ||
      !ParseDoubleField(content, "base_kernel_median_us", baseline.base_kernel_median_us)) {
    return baseline;
  }
  baseline.valid = baseline.duckdb_ref_query_median_us > 0.0 &&
                   baseline.base_kernel_median_us > 0.0;
  return baseline;
}

void SaveAuxBenchmarkBaseline(const std::string &path, int query_num,
                              const std::string &cache_key,
                              const AuxBenchmarkBaseline &baseline) {
  std::ofstream out(path);
  if (!out.good()) {
    throw std::runtime_error("failed to write aux benchmark baseline cache: " + path);
  }
  out << "{\n"
      << "  \"version\": 1,\n"
      << "  \"query\": " << query_num << ",\n"
      << "  \"cache_key\": \"" << EscapeJson(cache_key) << "\",\n"
      << "  \"duckdb_ref_query_median_us\": " << baseline.duckdb_ref_query_median_us
      << ",\n"
      << "  \"base_kernel_median_us\": " << baseline.base_kernel_median_us << "\n"
      << "}\n";
}

} // namespace accelerator_codegen::benchmarks
