// Parameter sweep of accel_sec_edgar_q2_aux(fy, uom) against plain SQL on the
// SF3 database of the instance addressed by the PG* environment (default
// 127.0.0.1:55402 / sec_edgar): every unit of measure at fy 2022 (plus case /
// whitespace variants and NULL), the largest units (USD, shares) at every
// fiscal year and edge values, and a write phase on the large units
// (insert / update / delete of the current maxima, commit and rollback; the
// table is restored afterwards).
//
//   test_postgres_sec_edgar_q2_params [large_units=3] [--no-writes] [--max-fy=N]
#include <algorithm>
#include <chrono>

#include "sec_edgar/q2/tests/postgres_sec_edgar_q2_common.hpp"

namespace {
using namespace q2pg;
using Clock = std::chrono::steady_clock;

uint64_t checked = 0, failed = 0;
double plain_s = 0, accel_s = 0;

void Check(Conn &c, const char *fy, const char *uom, const std::string &label = "") {
  ++checked;
  const std::vector<const char *> params{fy, uom};
  auto t0 = Clock::now();
  const Rows want = c.Query(PlainSql(), params);
  auto t1 = Clock::now();
  const Rows got = c.Query(AccelSql(), params);
  auto t2 = Clock::now();
  plain_s += std::chrono::duration<double>(t1 - t0).count();
  accel_s += std::chrono::duration<double>(t2 - t1).count();
  std::string diff;
  if (want.size() != got.size()) {
    diff = "row count accel=" + std::to_string(got.size()) + " sql=" + std::to_string(want.size());
  }
  for (size_t i = 0; diff.empty() && i < want.size(); ++i) {
    if (want[i][0].is_null != got[i][0].is_null || want[i][0].text != got[i][0].text ||
        want[i][1].text != got[i][1].text || !SameDouble(want[i][2].text, got[i][2].text)) {
      diff = "row " + std::to_string(i) + " differs";
    }
  }
  if (!diff.empty()) {
    ++failed;
    std::cerr << "MISMATCH " << label << " fy=" << (fy ? fy : "NULL")
              << " uom=" << (uom ? uom : "NULL") << ": " << diff << "\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  int large_units = 3, max_fy = 1000;
  bool writes = true;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--no-writes") writes = false;
    else if (a.rfind("--max-fy=", 0) == 0) max_fy = std::atoi(a.c_str() + 9);
    else large_units = std::atoi(a.c_str());
  }
  std::cout << std::unitbuf;
  try {
    Conn c(Env("PGDATABASE", "sec_edgar"));
    c.Exec("SET max_parallel_workers_per_gather = 4");
    // Warm: first call builds the state.
    auto t0 = Clock::now();
    c.Query(AccelSql(), {"2022", "pure"});
    std::cout << "first call (build) " << std::chrono::duration<double>(Clock::now() - t0).count()
              << " s\n";

    const Rows uoms = c.Query("SELECT uom, count(*) FROM num WHERE value IS NOT NULL "
                              "GROUP BY uom ORDER BY 2 DESC, 1");
    std::vector<std::string> names, large;
    for (const auto &r : uoms) {
      if (!r[0].is_null) {
        names.push_back(r[0].text);
        if (static_cast<int>(large.size()) < large_units) large.push_back(r[0].text);
      }
    }
    std::cout << names.size() << " units; large:";
    for (auto &l : large) std::cout << " " << l;
    std::cout << "\n";

    // 1. Every unit at fy 2022 (+ variants).
    for (const auto &u : names) {
      Check(c, "2022", u.c_str(), "all-uoms");
      std::string up = u + " ";
      Check(c, "2022", up.c_str(), "padded");
    }
    Check(c, "2022", nullptr, "null uom");
    Check(c, nullptr, "USD", "null fy");
    Check(c, nullptr, nullptr, "null both");
    Check(c, "2022", "", "empty uom");
    Check(c, "2022", "nonexistent", "unknown uom");
    std::cout << "all units: checked=" << checked << " failed=" << failed << "\n";

    // 2. Large units across fiscal years and edge values.
    const Rows fys = c.Query("SELECT DISTINCT fy FROM sub WHERE fy IS NOT NULL ORDER BY fy DESC");
    std::vector<std::string> fy_list;
    for (const auto &r : fys) {
      if (static_cast<int>(fy_list.size()) < max_fy) fy_list.push_back(r[0].text);
    }
    for (const char *edge : {"0", "2147483647", "9223372036854775807"}) {
      fy_list.push_back(edge);
    }
    for (const auto &u : large) {
      for (const auto &fy : fy_list) Check(c, fy.c_str(), u.c_str(), "large-x-fy");
    }
    std::cout << "large units x fy: checked=" << checked << " failed=" << failed << "\n";

    // 3. Writes on the large units.
    if (writes && !large.empty()) {
      const std::string u = large[0];
      const Rows pick = c.Query("SELECT n.adsh, n.tag, n.ctid::text FROM num n JOIN sub s "
                                "ON n.adsh = s.adsh WHERE n.uom = $1::text AND s.fy = 2022 "
                                "AND n.value IS NOT NULL ORDER BY n.value DESC LIMIT 1",
                                {u.c_str()});
      Require(!pick.empty(), "no row to write against");
      const std::string adsh = pick[0][0].text, tag = pick[0][1].text;
      const std::string uq = "'" + u + "'";
      const auto row = [&](const std::string &value, const std::string &tg) {
        return "INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, value) VALUES ('" + adsh +
               "', '" + tg + "', 'w', 20220101, 4, " + uq + ", " + value + ")";
      };
      const std::string baseline_tag = tag;
      for (const char *probe : {"1e30", "'NaN'::float8", "'Infinity'::float8"}) {
        c.Exec("BEGIN");
        c.Exec(row(probe, "q2probe"));
        Check(c, "2022", u.c_str(), "in-txn insert");   // own write: fallback path
        c.Exec("ROLLBACK");
        Check(c, "2022", u.c_str(), "after rollback");
        c.Exec(row(probe, "q2probe"));
        Check(c, "2022", u.c_str(), "after insert");
        Check(c, "2022", "pure", "other unit untouched");
        c.Exec(std::string("UPDATE num SET value = -1 WHERE tag = 'q2probe' AND adsh = '") + adsh + "'");
        Check(c, "2022", u.c_str(), "after update");
        c.Exec("DELETE FROM num WHERE tag = 'q2probe'");
        Check(c, "2022", u.c_str(), "after delete");
      }
      // Delete the current top rows of the unit, then put them back.
      const std::string top =
          "SELECT n.ctid FROM num n JOIN sub s ON n.adsh = s.adsh WHERE n.uom = " + uq +
          " AND s.fy = 2022 AND n.value IS NOT NULL ORDER BY n.value DESC LIMIT 3";
      c.Exec("CREATE TEMP TABLE q2top AS SELECT * FROM num WHERE ctid = ANY(ARRAY(" + top + "))");
      c.Exec("BEGIN");
      c.Exec("DELETE FROM num WHERE ctid = ANY(ARRAY(" + top + "))");
      c.Exec("COMMIT");
      Check(c, "2022", u.c_str(), "after deleting the top rows");
      c.Exec("INSERT INTO num SELECT * FROM q2top");
      Check(c, "2022", u.c_str(), "after restoring the top rows");
      c.Exec("DROP TABLE q2top");
      std::cout << "writes: checked=" << checked << " failed=" << failed << "\n";
    }
    std::cout << "checked=" << checked << " failed=" << failed << " plain_s=" << plain_s
              << " accel_s=" << accel_s << "\n";
    return failed == 0 ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
}
