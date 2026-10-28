// The DuckDB host and its aux state live as long as the database, not as long
// as a statement: plain SQL on fresh connections builds the state once, and
// closing the database frees the host.
//
// usage: test_host_lifetime <tpch.duckdb> <query>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "backends/duckdb/api/duckdb_host.hpp"
#include "backends/duckdb/api/register.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/tests/test_support/db_paths.hpp"

namespace {

namespace engine = accelerator_codegen::aux_state::engine;
namespace ts = accelerator_codegen::test_support;
using engine::duckdb::DuckdbHost;

void Require(bool ok, const std::string &message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

void Run(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  Require(result && !result->HasError(), sql.substr(0, 120) + ": " + (result ? result->GetError() : "null"));
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: test_host_lifetime <tpch.duckdb> <query>\n";
    return 2;
  }
  try {
    const auto &entry = engine::LookupSharedQuery("tpch", std::stoi(argv[2]));
    ts::ScratchDir scratch("host_lifetime");
    duckdb::weak_ptr<duckdb::DatabaseInstance> instance;
    {
      auto db = std::make_unique<duckdb::DuckDB>(ts::PrivateDbCopy(argv[1], scratch));
      for (int statement = 0; statement < 3; ++statement) {
        duckdb::Connection connection(*db);
        accelerator_codegen::RegisterAcceleratorFunctions(connection);
        Run(connection, entry.duckdb_aux_sql);
      }
      auto *host = DuckdbHost::Existing(*db->instance);
      Require(host != nullptr, "no host after three statements");
      Require(host->Find(entry.accelerator_id)->runtime->Stats().builds == 1,
              "the state was built more than once across statements");
      instance = db->instance;
    }
    // The host holds connections to the database: closing it must not leave a cycle.
    Require(instance.expired(), "closing the database left it, and the host, alive");
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "OK\n";
  return 0;
}
