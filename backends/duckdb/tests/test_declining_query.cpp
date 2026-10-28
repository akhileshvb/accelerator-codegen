// The declining registration: with its default arguments it registers as any module does (the parameterized
// call and the arguments in the catalog entry), so the workload harnesses are checked on that shape.
#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "backends/duckdb/api/declining_query.hpp"

namespace engine = accelerator_codegen::aux_state::engine;
namespace core = accelerator_codegen::aux_state::core;

namespace {

void Require(bool condition, const char *what) {
  if (!condition) throw std::runtime_error(what);
}

const engine::duckdb::DecliningQueryRegistrar kBound{engine::duckdb::DecliningQuery{
    7, "declining_test", "declining_test_q7_aux", "accel_declining_test_q7_aux", {"VARCHAR", "DATE"},
    "SELECT 1", "SELECT $1, $2", {"t"}, {"north", "2024-03-01"}}};

} // namespace

int main() {
  try {
    const auto &entry = engine::LookupSharedQuery("declining_test", 7);
    Require(std::string(entry.duckdb_aux_sql) == "SELECT * FROM accel_declining_test_q7_aux($1, $2)", "parameterized call");
    const auto &values = entry.benchmark_arguments.values;
    Require(values.size() == 2, "two default arguments");
    Require(values[0].type == core::AuxType::String && values[1].type == core::AuxType::Date, "typed by the parameters");
    std::cout << "declining query registers its default arguments\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "declining query test failed: " << error.what() << "\n";
    return 1;
  }
}
