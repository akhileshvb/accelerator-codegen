// PostgreSQL differential for the SEC-EDGAR Q2 aux accelerator.
//
// Creates a scratch database (C collation) in the instance addressed by the
// PG* environment (default 127.0.0.1:55402), loads a small random SEC-EDGAR
// num/sub data set, installs the extension and the statement-level capture
// triggers, then runs randomized write transactions (INSERT / DELETE /
// UPDATE on num and sub, savepoints, rollbacks, failing statements, reads
// inside the writing transaction) and after each one compares
// accel_sec_edgar_q2_aux(fy, uom) with the plain SQL of workloads/sec_edgar/q2.sql
// for the (fy, uom) sets drawn by eval/harness/param_source (ACCEL_PARAM_SEED=<s>
// reproduces a run; the generated data covers the drawn units and years) and for
// random draws including NULL and edge values.
//
//   test_postgres_sec_edgar_q2_differential [seed=1] [transactions=150] [keep]
// Run from the repository root (reads backends/postgres/host/capture_triggers.sql).
#include <fstream>
#include <memory>
#include <random>
#include <sstream>

#include "sec_edgar/q2/tests/postgres_sec_edgar_q2_common.hpp"

namespace {
using namespace q2pg;

constexpr const char *kScratchDb = "q2diff";

const std::vector<std::string> kValueSql = {
    "0", "0", "'-0'::float8", "1", "1", "1", "2.5", "2.5", "-3", "7", "7", "100",
    "1e18", "1e18", "'NaN'::float8", "'Infinity'::float8", "'-Infinity'::float8", "0.25", "3",
    "1e-300", "-1e300", "123456789.125"};
const std::vector<std::string> kTags = {
    "Assets", "Liabilities", "Revenue", "tag'quote", "EPS", "Shares", "A", "B", "ProfitLoss",
    "caf\xc3\xa9", "back\\slash", "", " lead", "Assets "};
const std::vector<std::string> kNames = {"'Acme'", "'Beta Corp'", "'Acme'", "'zeta'",
                                         "'a\"b''c'", "NULL", "'Delta'", "''",
                                         "'\xc3\x89t\xc3\xa9'", "'back\\\\slash'", "'acme'"};

struct Gen {
  std::mt19937_64 rng;
  std::vector<std::string> uoms; // the data's units: the drawn ones, near misses, edge spellings
  std::vector<std::string> fys;  // SQL literals of sub.fy: the drawn years, their neighbours, NULL
  long long fy_lo = 0, fy_hi = 0;  // range of the drawn years
  explicit Gen(uint64_t seed) : rng(seed) {}
  size_t Pick(size_t n) { return static_cast<size_t>(rng() % n); }
  bool Chance(int pct) { return static_cast<int>(rng() % 100) < pct; }
  std::string Lit(const std::string &s) {
    std::string out = "E'";
    for (char c : s) {
      if (c == '\'') out += "''";
      else if (c == '\\') out += "\\\\";
      else out += c;
    }
    return out + "'";
  }
  std::string Value() { return kValueSql[Pick(kValueSql.size())]; }
  std::string Tag() { return Lit(kTags[Pick(kTags.size())]); }
  std::string Uom() { return Lit(uoms[Pick(uoms.size())]); }

  // Fills the pools from the drawn sets.
  void Cover(const std::vector<Params> &sets) {
    uoms = {"", "x'y", "back\\slash"};
    fy_lo = std::stoll(sets.front().fy);
    fy_hi = fy_lo;
    for (const auto &set : sets) {
      std::string flipped = set.uom;
      flipped[0] = static_cast<char>(std::isupper(static_cast<unsigned char>(flipped[0])) ? std::tolower(flipped[0])
                                                                                           : std::toupper(flipped[0]));
      for (int copies = 0; copies < 3; ++copies) uoms.push_back(set.uom);
      uoms.push_back(flipped);
      uoms.push_back(set.uom + " ");
      fy_lo = std::min(fy_lo, std::stoll(set.fy));
      fy_hi = std::max(fy_hi, std::stoll(set.fy));
    }
    for (long long fy = fy_lo - 1; fy <= fy_hi + 1; ++fy) fys.push_back(std::to_string(fy));
    fys.push_back("NULL");
  }
  std::string Fy() { return fys[Pick(fys.size())]; }
};

std::string NumRow(Gen &g, const std::vector<std::string> &adshs) {
  const std::string adsh = g.Chance(2) ? "NULL" : g.Lit(adshs[g.Pick(adshs.size())]);
  const std::string tag = g.Chance(2) ? "NULL" : g.Tag();
  const std::string uom = g.Chance(2) ? "NULL" : g.Uom();
  const std::string value = g.Chance(5) ? "NULL" : g.Value();
  return "(" + adsh + "," + tag + ",'v',20220101,4," + uom + ",NULL," + value + ",NULL)";
}

struct Test {
  Gen g;
  Conn admin{"postgres"};
  std::unique_ptr<Conn> w, r;
  std::vector<std::string> adshs;
  int next_adsh = 0;
  uint64_t reads = 0, txns = 0, rollbacks = 0, errors = 0, nonempty_drawn_reads = 0;
  bool failed = false;

  explicit Test(uint64_t seed) : g(seed) { g.Cover(DrawnParams(admin)); }

  void Setup() {
    admin.Exec(std::string("DROP DATABASE IF EXISTS ") + kScratchDb + " WITH (FORCE)");
    admin.Exec(std::string("CREATE DATABASE ") + kScratchDb +
               " TEMPLATE template0 ENCODING 'UTF8' LOCALE 'C'");
    w = std::make_unique<Conn>(kScratchDb);
    r = std::make_unique<Conn>(kScratchDb);
    std::ifstream schema_in("workloads/sec_edgar/schema.sql");
    Require(schema_in.good(), "run from the repository root (workloads/sec_edgar/schema.sql)");
    std::stringstream ss;
    ss << schema_in.rdbuf();
    std::string schema = ss.str();
    for (size_t p; (p = schema.find("DOUBLE,")) != std::string::npos;) {
      schema.replace(p, 7, "DOUBLE PRECISION,");
    }
    w->Exec(schema);
    w->Exec("CREATE EXTENSION accelerator_codegen_postgres");
    std::ifstream trig("backends/postgres/host/capture_triggers.sql");
    Require(trig.good(), "backends/postgres/host/capture_triggers.sql");
    std::stringstream ts;
    ts << trig.rdbuf();
    w->Exec(ts.str());
    w->Exec("SELECT create_accel_mutation_triggers('sub', 'accel_codegen_sec_edgar')");
    w->Exec("SELECT create_accel_mutation_triggers('num', 'accel_codegen_sec_edgar')");
    for (int i = 0; i < 70; ++i) {
      AddSub(*w);
    }
    std::string batch;
    for (int i = 0; i < 4000; ++i) {
      batch += (batch.empty() ? "" : ",") + NumRow(g, adshs);
      if (i % 500 == 499) {
        w->Exec("INSERT INTO num VALUES " + batch);
        batch.clear();
      }
    }
    // A group with many equal values (exercises the count paths).
    w->Exec("INSERT INTO num SELECT adsh, tag, version, ddate, qtrs, uom, coreg, value, footnote "
            "FROM num WHERE uom = " + g.Lit(DrawnParams(admin).front().uom) + " AND value IS NOT NULL LIMIT 200");
  }

  std::string AddSub(Conn &c) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%010d-22-%06d", 1000000 + static_cast<int>(g.rng() % 50),
                  next_adsh++);
    adshs.push_back(buf);
    c.Exec("INSERT INTO sub (adsh, cik, name, fy) VALUES ('" + adshs.back() + "', 1, " +
           kNames[g.Pick(kNames.size())] + ", " + g.Fy() + ")");
    return adshs.back();
  }

  // Returns whether the answer has rows.
  bool Check(Conn &c, const char *fy, const char *uom, const std::string &label) {
    ++reads;
    size_t answer_rows = 0;
    const std::string diff = Compare(c, c, fy, uom, &answer_rows);
    if (!diff.empty()) {
      std::cerr << "MISMATCH after " << label << ": " << diff << "\n";
      failed = true;
    }
    return answer_rows > 0;
  }

  void RandomReads(Conn &c, const std::string &label, int draws) {
    for (const auto &set : DrawnParams(admin)) {
      nonempty_drawn_reads += Check(c, set.fy.c_str(), set.uom.c_str(), label) ? 1 : 0;
    }
    std::vector<std::string> fys = {"0", "-1", "2147483647", "-2147483648", "9223372036854775807"};
    for (const auto &fy : g.fys) {
      if (fy != "NULL") fys.push_back(fy);
    }
    for (int d = 0; d < draws; ++d) {
      const char *fy = g.Chance(3) ? nullptr : fys[g.Pick(fys.size())].c_str();
      std::string uom_s;
      const char *uom;
      if (g.Chance(3)) {
        uom = nullptr;
      } else if (g.Chance(8)) {
        uom_s = "nonexistent";
        uom = uom_s.c_str();
      } else {
        uom_s = g.uoms[g.Pick(g.uoms.size())];
        uom = uom_s.c_str();
      }
      Check(c, fy, uom, label);
    }
  }

  void RandomStatement() {
    const auto pick_rows = [&](int max) {
      return "ctid IN (SELECT ctid FROM num ORDER BY random() LIMIT " +
             std::to_string(1 + g.Pick(max)) + ")";
    };
    const std::string stmt = [&]() -> std::string {
      switch (g.Pick(16)) {
      case 0:
      case 1: {
        std::string values;
        const int n = 1 + static_cast<int>(g.Pick(4));
        for (int i = 0; i < n; ++i) values += (i ? "," : "") + NumRow(g, adshs);
        return "INSERT INTO num VALUES " + values;
      }
      case 2: // duplicates of existing rows (counts)
        return "INSERT INTO num SELECT * FROM num WHERE " + pick_rows(5);
      case 3:
      case 4:
        return "DELETE FROM num WHERE " + pick_rows(6);
      case 5: // delete the current maxima of a unit
        return "DELETE FROM num WHERE ctid IN (SELECT ctid FROM num WHERE uom = " + g.Uom() +
               " ORDER BY value DESC NULLS LAST LIMIT " + std::to_string(1 + g.Pick(5)) + ")";
      case 6:
      case 7:
        return "UPDATE num SET value = " + (g.Chance(15) ? std::string("NULL") : g.Value()) +
               " WHERE " + pick_rows(4);
      case 8:
        return "UPDATE num SET value = value + 1 WHERE " + pick_rows(4);
      case 9:
        return "UPDATE num SET tag = " + g.Tag() + " WHERE " + pick_rows(4);
      case 10:
        return "UPDATE num SET uom = " + g.Uom() + " WHERE " + pick_rows(4);
      case 11:
        return "UPDATE num SET adsh = " + g.Lit(adshs[g.Pick(adshs.size())]) + " WHERE " +
               pick_rows(3);
      case 12:
        return "UPDATE num SET version = 'w' WHERE " + pick_rows(5);
      case 13:
        if (g.Chance(40)) { // multi-row sub update (old/new transition rows of several filers)
          return "UPDATE sub SET fy = CASE WHEN fy IS NULL OR fy >= " + std::to_string(g.fy_hi) + " THEN " +
                 std::to_string(g.fy_lo) + " ELSE fy + 1 END, name = CASE WHEN name IS NULL THEN 'n' ELSE NULL END "
                 "WHERE adsh IN (SELECT adsh FROM sub ORDER BY random() LIMIT 6)";
        }
        return "UPDATE sub SET fy = " + g.Fy() + ", name = " +
               kNames[g.Pick(kNames.size())] + " WHERE adsh = " +
               g.Lit(adshs[g.Pick(adshs.size())]);
      case 14: // new filer + rows for it in one statement chain
        return "INSERT INTO num VALUES " + NumRow(g, adshs);
      default: // delete a sub no num row references / rename one
        return g.Chance(50)
                   ? "DELETE FROM sub WHERE adsh IN (SELECT adsh FROM sub s WHERE NOT EXISTS "
                     "(SELECT 1 FROM num n WHERE n.adsh = s.adsh) LIMIT 1)"
                   : "UPDATE sub SET name = " + kNames[g.Pick(kNames.size())] +
                         " WHERE adsh = " + g.Lit(adshs[g.Pick(adshs.size())]);
      }
    }();
    const std::string err = w->TryExec(stmt);
    if (!err.empty()) {
      ++errors; // e.g. a failing statement: the transaction is aborted
      throw std::runtime_error("stmt");
    }
  }

  void Transaction(int step) {
    w->Exec("BEGIN");
    ++txns;
    bool aborted = false;
    try {
      const int statements = 1 + static_cast<int>(g.Pick(3));
      for (int s = 0; s < statements; ++s) {
        if (g.Chance(8)) {
          AddSub(*w);
        }
        if (g.Chance(10)) {
          w->Exec("SAVEPOINT sp");
          RandomStatement();
          if (g.Chance(50)) w->Exec("ROLLBACK TO sp");
          else w->Exec("RELEASE sp");
        } else {
          RandomStatement();
        }
        if (g.Chance(12)) {
          RandomReads(*w, "in-txn " + std::to_string(step), 2);
        }
      }
    } catch (const std::runtime_error &) {
      aborted = true;
    }
    if (aborted || g.Chance(30)) {
      w->Exec("ROLLBACK");
      ++rollbacks;
    } else {
      w->Exec("COMMIT");
    }
  }

  int Run(int transactions) {
    Setup();
    RandomReads(*r, "initial", 12);
    for (int step = 0; step < transactions; ++step) {
      Transaction(step);
      RandomReads(g.Chance(25) ? *w : *r, "step " + std::to_string(step), 3);
      if (failed) break;
    }
    RandomReads(*r, "final", 40);
    // A fresh session must agree too (its own build).
    Conn fresh(kScratchDb);
    RandomReads(fresh, "fresh session", 15);
    const std::string counts = r->Scalar("SELECT accel_get_execute_counts()");
    std::cout << "execute counts (reader): " << counts << "\n";
    Require(nonempty_drawn_reads > 0, "no drawn set had a non-empty answer");
    std::cout << "txns=" << txns << " rollbacks=" << rollbacks << " failing_stmts=" << errors
              << " reads=" << reads << (failed ? " FAILED" : " ok") << "\n";
    return failed ? 1 : 0;
  }
};
} // namespace

int main(int argc, char **argv) {
  const uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  const int txns = argc > 2 ? std::atoi(argv[2]) : 150;
  const bool keep = argc > 3 && std::string(argv[3]) == "keep";
  try {
    Test t(seed);
    const int rc = t.Run(txns);
    if (!keep) {
      t.w.reset();
      t.r.reset();
      t.admin.TryExec(std::string("DROP DATABASE IF EXISTS ") + kScratchDb + " WITH (FORCE)");
    }
    std::cout << (rc == 0 ? "ALL OK\n" : "FAILED\n");
    return rc;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 2;
  }
}
