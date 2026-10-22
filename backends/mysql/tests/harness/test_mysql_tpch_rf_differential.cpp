// TPC-H refresh-function differential on MySQL: RF1 (insert orders with their
// lineitems under fresh keys) and RF2 (delete them again), each one explicit
// transaction of many statements committed through the trigger capture. After
// each refresh every query's accelerator must equal its reference SQL, the
// state having been maintained from the captured writes.
//
// Usage: test_mysql_tpch_rf_differential [orders_per_refresh=50] [cycles=1]
// Env:   TPCH_MYSQL_DB (default accel_mysql_test)
//        ACCEL_MYSQL_RF_QUERIES (comma list; default 1..22)
//        ACCEL_FORCE_MAINTENANCE_ACTION (default: the cost model chooses)
#include "backends/mysql/tests/harness/mysql_session_client.hpp"
#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace accel::mysql::tpch_harness;

std::string InsertOrder(const std::string &fresh, const std::string &source) {
  return "INSERT INTO orders SELECT " + fresh +
         ", o_custkey, o_orderstatus, o_totalprice, o_orderdate, o_orderpriority, o_clerk, "
         "o_shippriority, o_comment FROM orders WHERE o_orderkey = " + source;
}

std::string InsertLineitems(const std::string &fresh, const std::string &source) {
  return "INSERT INTO lineitem SELECT " + fresh +
         ", l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax, "
         "l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, "
         "l_shipmode, l_comment FROM lineitem WHERE l_orderkey = " + source;
}

std::string InList(const std::vector<std::string> &keys) {
  std::string list;
  for (const auto &key : keys) {
    list += (list.empty() ? "" : ",") + key;
  }
  return list;
}

void MatchAll(const Tools &tools, const std::vector<int> &queries, const std::string &label) {
  for (const int query : queries) {
    RequireQueryMatch(tools, query, /*prefer_aux=*/true, label + " Q" + std::to_string(query));
  }
  std::cerr << "  ok: " << label << " (" << queries.size() << " queries)\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const int orders = argc > 1 ? std::atoi(argv[1]) : 50;
    const int cycles = argc > 2 ? std::atoi(argv[2]) : 1;
    std::vector<int> all(22);
    for (int q = 0; q < 22; ++q) all[static_cast<std::size_t>(q)] = q + 1;
    const auto queries = ParseQueryList("ACCEL_MYSQL_RF_QUERIES", all);

    auto tools = ConnectTpch();
    EnsureMutationTriggers(tools);
    EnsureUdf(tools, "accel_invalidate_all", "STRING");
    EnsureUdf(tools, "mysql_accel_set_scan_database", "STRING");
    EnsureUdf(tools, "accel_set_force_maintenance_action", "STRING");
    Require(RunSQL(tools, "SELECT mysql_accel_set_scan_database(" + SqlLiteral(tools.database) + ")"),
            "set scan database failed");
    Require(RunSQL(tools, "SELECT accel_invalidate_all()"), "invalidate failed");
    const char *action = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
    Require(RunSQL(tools, "SELECT accel_set_force_maintenance_action(" +
                              SqlLiteral(action ? action : "") + ")"),
            "force maintenance action failed");

    MatchAll(tools, queries, "initial");
    Conn refresh(tools);
    for (int cycle = 0; cycle < cycles; ++cycle) {
      const std::string label = "cycle " + std::to_string(cycle);
      const auto sources = SplitLines(TrimLines(
          QuerySQL(tools, "SELECT o_orderkey FROM orders ORDER BY o_totalprice DESC, o_orderkey LIMIT " +
                              std::to_string(orders)).second));
      Require(static_cast<int>(sources.size()) == orders, "not enough orders to clone");
      const long first_fresh =
          std::stol(TrimLines(QuerySQL(tools, "SELECT MAX(o_orderkey) + 1 FROM orders").second));
      std::vector<std::string> fresh;
      for (int i = 0; i < orders; ++i) fresh.push_back(std::to_string(first_fresh + i));

      refresh.Ok("BEGIN");
      for (int i = 0; i < orders; ++i) {
        refresh.Ok(InsertOrder(fresh[static_cast<std::size_t>(i)], sources[static_cast<std::size_t>(i)]));
        refresh.Ok(InsertLineitems(fresh[static_cast<std::size_t>(i)], sources[static_cast<std::size_t>(i)]));
      }
      refresh.Ok("COMMIT");
      MatchAll(tools, queries, label + " after RF1");

      refresh.Ok("BEGIN");
      refresh.Ok("DELETE FROM lineitem WHERE l_orderkey IN (" + InList(fresh) + ")");
      refresh.Ok("DELETE FROM orders WHERE o_orderkey IN (" + InList(fresh) + ")");
      refresh.Ok("COMMIT");
      MatchAll(tools, queries, label + " after RF2");
    }
    std::cout << "PASS tpch rf differential (" << cycles << " cycles of " << orders << " orders, "
              << queries.size() << " queries)\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    std::cerr << "FAIL tpch rf differential: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
