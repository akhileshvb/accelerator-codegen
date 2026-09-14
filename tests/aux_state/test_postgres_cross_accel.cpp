// PostgreSQL cross-accelerator isolation: two aux kernels share mutation
// capture; a write against one must not corrupt the other, and both rematch
// SQL. Builds: test_postgres_cross_accel
//
// Env: ACCEL_CROSS_ACCEL_TEST_QUERIES (comma list, default target+peer),
//      TPCH_PG_DB, ACCEL_PG_DOCKER=1

#include "aux_state/postgres_tpch_harness.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

using namespace accel::postgres::tpch_harness;

int PeerQuery(int target) { return target == 1 ? 3 : 1; }

void Warm(const Tools &tools, int query_num) {
  const auto &entry = LookupPostgresQuery("tpch", query_num);
  const std::string sql = ConcreteAccelSql(query_num, entry, /*prefer_aux=*/true);
  auto r = QuerySQL(tools, sql);
  Require(r.first, "warm aux Q" + std::to_string(query_num) + " failed");
}

} // namespace

int main() {
  try {
    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);

    const int target = ParseQueryList("ACCEL_CROSS_ACCEL_TEST_QUERIES", {1}).front();
    auto queries = ParseQueryList("ACCEL_CROSS_ACCEL_TEST_QUERIES", {target, PeerQuery(target)});
    if (queries.size() == 1)
      queries.push_back(PeerQuery(queries.front()));

    std::cerr << "postgres cross-accel:";
    for (int q : queries)
      std::cerr << " Q" << q;
    std::cerr << '\n';

    // Same backend session: register both Runtimes so mutation fan-out hits both.
    std::ostringstream warm;
    warm << "LOAD '" << tools.extension_so << "';\n";
    for (int q : queries) {
      const auto &entry = LookupPostgresQuery("tpch", q);
      warm << ConcreteAccelSql(q, entry, /*prefer_aux=*/true) << ";\n";
    }
    RunPsqlScript(tools, warm.str());

    for (int q : queries)
      RequireQueryMatch(tools, q, /*prefer_aux=*/true, "baseline_Q" + std::to_string(q));

    for (int writer : queries) {
      Require(RunSQL(tools, InvalidatingUpdateSql(writer)),
              "invalidating writer failed for Q" + std::to_string(writer));
      for (int reader : queries) {
        RequireQueryMatch(tools, reader, /*prefer_aux=*/true,
                          "after_Q" + std::to_string(writer) + "_write_check_Q" +
                              std::to_string(reader));
      }
    }

    // Compensating restore so the shared TPC-H db is not left dirtied.
    for (int writer : queries) {
      std::string sql = InvalidatingUpdateSql(writer);
      const auto plus = sql.find("+ 0.01");
      if (plus != std::string::npos) {
        sql.replace(plus, 6, "- 0.01");
        RunSQL(tools, sql);
      }
    }

    std::cout << "test_postgres_cross_accel passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "test_postgres_cross_accel failed: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
