// MySQL cross-accelerator isolation: two aux kernels share mutation
// capture; a write against one must not corrupt the other, and both rematch
// SQL. Builds: test_mysql_cross_accel

#include "eval/tests/mysql/mysql_tpch_harness.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

using namespace accel::mysql::tpch_harness;

int PeerQuery(int target) { return target == 1 ? 3 : 1; }

} // namespace

int main() {
  try {
    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);

    const int target = ParseQueryList("ACCEL_CROSS_ACCEL_TEST_QUERIES", {1}).front();
    auto queries = ParseQueryList("ACCEL_CROSS_ACCEL_TEST_QUERIES",
                                  {target, PeerQuery(target)});
    if (queries.size() == 1)
      queries.push_back(PeerQuery(queries.front()));

    std::cerr << "mysql cross-accel:";
    for (int q : queries)
      std::cerr << " Q" << q;
    std::cerr << '\n';

    std::ostringstream warm;
    for (int q : queries) {
      const auto &entry = LookupMysqlQuery("tpch", q);
      warm << ConcreteAccelSql(q, entry, /*prefer_aux=*/true) << ";\n";
    }
    RunMysqlScript(tools, warm.str());

    for (int q : queries)
      RequireQueryMatch(tools, q, /*prefer_aux=*/true,
                        "baseline_Q" + std::to_string(q));

    for (int writer : queries) {
      Require(RunSQL(tools, InvalidatingUpdateSql(writer)),
              "invalidating writer failed for Q" + std::to_string(writer));
      for (int reader : queries) {
        RequireQueryMatch(tools, reader, /*prefer_aux=*/true,
                          "after_Q" + std::to_string(writer) + "_write_check_Q" +
                              std::to_string(reader));
      }
    }

    for (int writer : queries) {
      std::string sql = InvalidatingUpdateSql(writer);
      const auto plus = sql.find("+ 0.01");
      if (plus != std::string::npos) {
        sql.replace(plus, 6, "- 0.01");
        RunSQL(tools, sql);
      }
    }

    std::cout << "test_mysql_cross_accel passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "test_mysql_cross_accel failed: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
