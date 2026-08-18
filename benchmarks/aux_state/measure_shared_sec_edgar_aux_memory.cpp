#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "catalog/query_catalog.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "duckdb.hpp"

namespace catalog = accelerator_codegen::aux_state::engine;
using catalog::duckdb::DuckdbRuntime;

namespace {

int64_t ReadRssKb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream in(line.substr(6));
      int64_t kb = 0;
      in >> kb;
      return kb;
    }
  }
  return 0;
}

void Require(bool ok, const std::string &message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 3,
            "usage: measure_shared_sec_edgar_aux_memory <db_path> <query>...");
    const std::string db_path = argv[1];
    std::cout << "query,rss_delta_mb,encoded_footprint_mb,rows\n";
    for (int i = 2; i < argc; ++i) {
      const int query = std::stoi(argv[i]);
      const auto &entry = catalog::LookupSharedQuery("sec_edgar", query);
      duckdb::DuckDB database(db_path.c_str());
      duckdb::Connection connection(database);
      auto pragma = connection.Query("PRAGMA threads=1");
      Require(pragma && !pragma->HasError(), "failed to set threads");

      const int64_t rss_before = ReadRssKb();
      DuckdbRuntime runtime(connection, entry, 10.0);
      runtime.Build();
      const int64_t rss_after = ReadRssKb();
      const auto rows = runtime.ExecuteCollect().size();
      const double rss_mb =
          static_cast<double>(std::max<int64_t>(0, rss_after - rss_before)) /
          1024.0;
      const double encoded_mb =
          static_cast<double>(runtime.AuxStateFootprintBytes()) /
          (1024.0 * 1024.0);
      std::cout << "Q" << query << ',' << std::fixed << std::setprecision(3)
                << rss_mb << ',' << encoded_mb << ',' << rows << '\n';
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "measure_shared_sec_edgar_aux_memory failed: " << ex.what()
              << '\n';
    return 1;
  }
}
