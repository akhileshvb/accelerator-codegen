// End-to-end differential test for the SEC-EDGAR Q4 aux module on PostgreSQL.
//
//   test_postgres_sec_edgar_q4_differential synthetic [seeds] [steps]
//   test_postgres_sec_edgar_q4_differential file [rounds]
//
// Talks to a running server through psql (PGHOST/PGPORT/PGUSER/PGPASSWORD/
// PGDATABASE), exactly like the TPC-H PostgreSQL differential: one long-lived
// psql session per run, so the backend-local aux state is built once and then
// maintained incrementally across the statements of the script.
//
// synthetic: a scratch database (Q4_SYNTH_DB, default accel_q4_synth) is created
//   with the extension functions and the SEC-EDGAR capture triggers.  Random
//   multi-mutation transactions (INSERT/DELETE/UPDATE on num, sub, tag and pre,
//   incl. key and filter columns that move rows in and out of the result,
//   NULL keys, boundary sics, duplicate pre rows, +-inf/NaN values), with
//   commits and rollbacks, come from the same generator the DuckDB test uses.
//   After every transaction (and for some, inside it) accel_sec_edgar_q4_aux()
//   must equal benchmarks/sec_edgar/queries/q4.sql, run on the same session.
// file: the same invariants on the database named by PGDATABASE (e.g. the SF3
//   load) with hand-picked high-impact writes, each committed then undone.
//
// The reference is q4.sql with a total ORDER BY (ties on total_value broken by
// sic, tlabel as the module does) so that LIMIT 500 is deterministic.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support/sec_edgar_q4_txn_gen.hpp"

#ifndef ACCEL_SRC_DIR
#define ACCEL_SRC_DIR "."
#endif

namespace {

[[noreturn]] void Fail(const std::string &what) {
  std::cerr << "FAIL: " << what << '\n';
  std::exit(1);
}
void Require(bool ok, const std::string &what) {
  if (!ok) Fail(what);
}

std::string Env(const char *k, const char *fallback) {
  const char *v = std::getenv(k);
  return v && *v ? v : fallback;
}

std::string ShellQuote(const std::string &s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

std::string Psql(const std::string &db, const std::string &args) {
  return "PGPASSWORD=" + ShellQuote(Env("PGPASSWORD", "postgres")) + " psql -X -h " +
         ShellQuote(Env("PGHOST", "127.0.0.1")) + " -p " + Env("PGPORT", "5432") + " -U " +
         ShellQuote(Env("PGUSER", "postgres")) + " -d " + ShellQuote(db) + " " + args;
}

std::string Run(const std::string &cmd, bool must_succeed = true) {
  FILE *fp = popen((cmd + " 2>&1").c_str(), "r");
  Require(fp != nullptr, "popen failed");
  std::string out;
  char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
  const int rc = pclose(fp);
  if (must_succeed && (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0)) {
    const size_t keep = 4000;
    Fail("command failed: " + cmd.substr(0, 300) + "\n...\n" +
         (out.size() > keep ? out.substr(out.size() - keep) : out));
  }
  return out;
}

std::string RunScript(const std::string &db, const std::string &script) {
  const std::string path = "/tmp/accel_q4_pg_" + std::to_string(::getpid()) + ".sql";
  {
    std::ofstream f(path);
    f << script;
  }
  const auto out = Run(Psql(db, "-v ON_ERROR_STOP=1 -q -t -A -f " + ShellQuote(path)));
  std::remove(path.c_str());
  return out;
}

std::string ReadFile(const std::string &path) {
  std::ifstream f(path);
  Require(f.good(), "cannot read " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// q4.sql with a total order (deterministic LIMIT 500).
std::string ReferenceSql() {
  std::string sql = ReadFile(std::string(ACCEL_SRC_DIR) + "/benchmarks/sec_edgar/queries/q4.sql");
  const std::string from = "ORDER BY total_value DESC";
  const auto pos = sql.rfind(from);
  Require(pos != std::string::npos, "q4.sql: ORDER BY not found");
  sql.replace(pos, from.size(), "ORDER BY total_value DESC, s.sic, t.tlabel COLLATE \"C\" NULLS FIRST");
  while (!sql.empty() && (sql.back() == '\n' || sql.back() == ';' || sql.back() == ' ')) sql.pop_back();
  return sql + ";\n";
}

const char *kAccelSql = "SELECT * FROM accel_sec_edgar_q4_aux();\n";

// ---------------------------------------------------------------- compare
std::vector<std::string> Split(const std::string &line, char sep) {
  std::vector<std::string> cells;
  std::string cur;
  for (char c : line) {
    if (c == sep) {
      cells.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  cells.push_back(cur);
  return cells;
}

bool CellsEqual(const std::string &a, const std::string &b) {
  if (a == b) return true;
  try {
    const double da = std::stod(a), db = std::stod(b);
    if (std::isnan(da) && std::isnan(db)) return true;
    const double scale = std::max({1.0, std::fabs(da), std::fabs(db)});
    return std::fabs(da - db) <= 1e-6 * scale;
  } catch (...) {
    return false;
  }
}

void CompareRows(const std::vector<std::string> &ref, const std::vector<std::string> &accel,
                 const std::string &label) {
  auto dump = [&] {
    std::string s;
    for (size_t i = 0; i < std::max(ref.size(), accel.size()) && i < 12; ++i) {
      s += "  ref:   " + (i < ref.size() ? ref[i] : std::string("-")) + "\n  accel: " +
           (i < accel.size() ? accel[i] : std::string("-")) + "\n";
    }
    return s;
  };
  if (ref.size() != accel.size()) {
    Fail(label + ": row count ref=" + std::to_string(ref.size()) +
         " accel=" + std::to_string(accel.size()) + "\n" + dump());
  }
  for (size_t i = 0; i < ref.size(); ++i) {
    const auto r = Split(ref[i], '|'), a = Split(accel[i], '|');
    bool ok = r.size() == a.size();
    for (size_t j = 0; ok && j < r.size(); ++j) ok = CellsEqual(r[j], a[j]);
    if (!ok) Fail(label + ": mismatch at row " + std::to_string(i) + "\n  ref:   " + ref[i] +
                  "\n  accel: " + accel[i]);
  }
}

// Output blocks: "@@<label>" lines open a block, all other lines belong to it.
std::map<std::string, std::vector<std::string>> Blocks(const std::string &out) {
  std::map<std::string, std::vector<std::string>> blocks;
  std::vector<std::string> *cur = nullptr;
  std::istringstream in(out);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("@@", 0) == 0) {
      cur = &blocks[line.substr(2)];
    } else if (cur && !line.empty()) {
      cur->push_back(line);
    }
  }
  return blocks;
}

std::string Mark(const std::string &label) { return "\\echo @@" + label + "\n"; }

void EmitCompare(std::string &script, const std::string &label, const std::string &ref_sql) {
  script += Mark("A " + label) + kAccelSql + Mark("R " + label) + ref_sql;
}

void CheckLabel(const std::map<std::string, std::vector<std::string>> &blocks,
                const std::string &label) {
  const auto a = blocks.find("A " + label);
  const auto r = blocks.find("R " + label);
  Require(a != blocks.end() && r != blocks.end(), label + ": output missing (session aborted?)");
  CompareRows(r->second, a->second, label);
}

// -------------------------------------------------------------- bootstrap
// Scratch database: the extension (installed by pg/build_pg_ext.sh) and the
// generic capture-trigger helper.
void InstallExtension(const std::string &db) {
  Run(Psql(db, "-v ON_ERROR_STOP=1 -q -c " +
                   ShellQuote("CREATE EXTENSION IF NOT EXISTS accelerator_codegen_postgres")));
  Run(Psql(db, "-v ON_ERROR_STOP=1 -q -f " +
                   ShellQuote(std::string(ACCEL_SRC_DIR) + "/scripts/postgres_accel_mutation_triggers.sql")));
}

std::string PgSql(std::string sql) {
  // DuckDB-dialect text of the shared generator -> PostgreSQL.
  for (const std::pair<const char *, const char *> &r :
       {std::pair<const char *, const char *>{" AS DOUBLE)", " AS DOUBLE PRECISION)"},
        {"value DOUBLE,", "value DOUBLE PRECISION,"}}) {
    size_t pos = 0;
    while ((pos = sql.find(r.first, pos)) != std::string::npos) {
      sql.replace(pos, std::string(r.first).size(), r.second);
      pos += std::string(r.second).size();
    }
  }
  return sql;
}

// -------------------------------------------------------------- synthetic
int RunSynthetic(int seeds, int steps) {
  const std::string admin_db = Env("PGDATABASE", "postgres");
  const std::string db = Env("Q4_SYNTH_DB", "accel_q4_synth");
  const std::string ref_sql = ReferenceSql();
  long total_checks = 0;
  for (int seed = 1; seed <= seeds + 1; ++seed) {
    const bool limit_case = seed == seeds + 1;
    auto cfg = q4gen::MakeConfig(seed, limit_case);
    cfg.big_values = false;
    cfg.unique_sums = true;
    cfg.wide_keys = seed % 2 == 1;
    cfg.nonfinite = !limit_case && seed % 3 != 0;
    cfg.huge_values = false; // PostgreSQL's SUM raises "overflow" where the module would return inf
    q4gen::Model model(static_cast<uint64_t>(seed) * 104729, cfg);

    Run(Psql(admin_db, "-v ON_ERROR_STOP=1 -q -c " +
                           ShellQuote("DROP DATABASE IF EXISTS " + db + " WITH (FORCE)")));
    Run(Psql(admin_db, "-v ON_ERROR_STOP=1 -q -c " +
                           ShellQuote("CREATE DATABASE " + db +
                                      " TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C'")));
    InstallExtension(db);

    std::string script = "SET client_min_messages = warning;\n";
    for (const auto &sql : model.Init()) script += PgSql(sql) + ";\n";
    script += "ANALYZE;\n";
    // Fixed SEC-EDGAR capture ids (as in the template database): sub=1 num=2 pre=3 tag=4.
    script += "SELECT create_accel_mutation_triggers('sub','1','accel_codegen_sec_edgar'),"
              " create_accel_mutation_triggers('num','2','accel_codegen_sec_edgar'),"
              " create_accel_mutation_triggers('pre','3','accel_codegen_sec_edgar'),"
              " create_accel_mutation_triggers('tag','4','accel_codegen_sec_edgar');\n";
    EmitCompare(script, "initial", ref_sql);
    std::vector<std::string> labels{"initial"};
    int rollbacks = 0, in_txn_reads = 0;
    for (int step = 1; step <= steps; ++step) {
      const q4gen::Db snapshot = model.db;
      const int64_t rid = model.next_rid;
      const bool rollback = model.Coin(0.1);
      const bool read_inside = model.Coin(0.25);
      auto txn = model.NextTxn();
      script += "BEGIN;\n";
      for (const auto &m : txn) {
        for (const auto &sql : m.sql) script += PgSql(sql) + ";\n";
      }
      if (read_inside) {
        EmitCompare(script, "in-txn " + std::to_string(step), ref_sql);
        labels.push_back("in-txn " + std::to_string(step));
        ++in_txn_reads;
      }
      if (rollback) {
        script += "ROLLBACK;\n";
        model.db = snapshot;
        model.next_rid = rid;
        ++rollbacks;
      } else {
        script += "COMMIT;\n";
      }
      const std::string label = "step " + std::to_string(step) + " " +
                                (rollback ? "rollback" : model.last_what);
      EmitCompare(script, label, ref_sql);
      labels.push_back(label);
    }
    script += Mark("counts") + "SELECT accel_postgres_get_execute_counts();\n";
    const auto out = RunScript(db, script);
    const auto blocks = Blocks(out);
    for (const auto &label : labels) {
      CheckLabel(blocks, label);
      ++total_checks;
    }
    if (limit_case) {
      Require(blocks.at("A initial").size() == 500, "LIMIT 500 must truncate");
    }
    const auto counts = blocks.at("counts");
    Require(!counts.empty(), "no execute counts");
    int hits = 0;
    std::sscanf(counts[0].c_str(), "hit=%d", &hits);
    std::cerr << "synthetic seed " << seed << ": " << steps << " txns ok (rollbacks=" << rollbacks
              << " in_txn_reads=" << in_txn_reads << ") " << counts[0] << '\n';
    // Most post-commit reads must be served from the maintained state, not by rescans.
    Require(hits >= steps / 2, "too few aux hits: " + counts[0]);
  }
  std::cerr << "total checks: " << total_checks << '\n';
  return 0;
}

// ------------------------------------------------------------------- file
int RunFile(int rounds) {
  const std::string db = Env("PGDATABASE", "postgres");
  const std::string ref_sql = ReferenceSql();
  std::string script = "SET client_min_messages = warning;\n";
  std::vector<std::string> labels;
  auto check = [&](const std::string &label) {
    EmitCompare(script, label, ref_sql);
    labels.push_back(label);
  };
  auto txn = [&](const std::string &label, const std::vector<std::string> &stmts, bool commit) {
    script += "BEGIN;\n";
    for (const auto &s : stmts) script += s + ";\n";
    script += commit ? "COMMIT;\n" : "ROLLBACK;\n";
    check(label);
  };
  check("initial");
  // Hand-picked, high-impact writes on rows that are part of the result.  Each is
  // committed (or rolled back) and then undone; the result is checked after each.
  const std::string cohort_adsh =
      "(SELECT s.adsh FROM sub s JOIN pre p ON p.adsh = s.adsh AND p.stmt = 'EQ' "
      "WHERE s.sic BETWEEN 4000 AND 4999 ORDER BY s.adsh LIMIT 1 OFFSET %OFF%)";
  const std::string cohort_tag =
      "(SELECT p.tag FROM pre p JOIN sub s ON s.adsh = p.adsh AND s.sic BETWEEN 4000 AND 4999 "
      "JOIN tag t ON t.tag = p.tag AND t.version = p.version AND t.abstract = 0 "
      "WHERE p.stmt = 'EQ' ORDER BY p.adsh, p.tag LIMIT 1 OFFSET %OFF%)";
  auto at = [](std::string s, int off) {
    s.replace(s.find("%OFF%"), 5, std::to_string(off));
    return s;
  };
  for (int round = 0; round < rounds; ++round) {
    const int off = round * 997;
    const std::string r = "round " + std::to_string(round) + " ";
    const std::string adsh = at(cohort_adsh, off), tagsel = at(cohort_tag, off);
    // sub: leave the sic range, come back; change cik
    txn(r + "sub sic out", {"UPDATE sub SET sic = 5000 WHERE adsh = " + adsh}, true);
    txn(r + "sub sic back", {"UPDATE sub SET sic = 4999 WHERE adsh = " + adsh}, true);
    txn(r + "sub cik rollback", {"UPDATE sub SET cik = cik + 1000000 WHERE adsh = " + adsh}, false);
    // tag: abstract flip and relabel
    txn(r + "tag abstract", {"UPDATE tag SET abstract = 1 WHERE tag = " + tagsel}, true);
    txn(r + "tag restore", {"UPDATE tag SET abstract = 0 WHERE tag = " + tagsel}, true);
    // pre: drop EQ rows of one filing (structural), then put them back
    txn(r + "pre stmt out", {"UPDATE pre SET stmt = 'XX' WHERE stmt = 'EQ' AND adsh = " + adsh}, true);
    txn(r + "pre stmt back", {"UPDATE pre SET stmt = 'EQ' WHERE stmt = 'XX' AND adsh = " + adsh}, true);
    // num: scale values of one filing's USD facts (delta path), then undo; plus a scratch row
    txn(r + "num value", {"UPDATE num SET value = value * 2 + 1 WHERE uom = 'USD' AND adsh = " + adsh}, true);
    txn(r + "num value undo", {"UPDATE num SET value = (value - 1) / 2 WHERE uom = 'USD' AND adsh = " + adsh}, true);
    txn(r + "num insert",
        {"INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value) SELECT adsh, tag, version, "
         "ddate, qtrs, uom, 'q4scratch', value + 12345 FROM num WHERE adsh = " + adsh +
         " AND uom = 'USD' LIMIT 50"},
        true);
    txn(r + "num delete", {"DELETE FROM num WHERE coreg = 'q4scratch' AND adsh = " + adsh}, true);
    txn(r + "num insert+delete rollback",
        {"INSERT INTO num (adsh, tag, version, ddate, qtrs, uom, coreg, value) SELECT adsh, tag, version, "
         "ddate, qtrs, uom, 'q4scratch', value + 999 FROM num WHERE adsh = " + adsh +
         " AND uom = 'USD' LIMIT 50",
         "DELETE FROM num WHERE adsh = " + adsh + " AND uom = 'USD' AND coreg = ''"},
        false);
  }
  script += Mark("counts") + "SELECT accel_postgres_get_execute_counts();\n";
  const auto out = RunScript(db, script);
  const auto blocks = Blocks(out);
  for (const auto &label : labels) CheckLabel(blocks, label);
  std::cerr << "file differential ok (" << labels.size() << " checks) " << blocks.at("counts")[0] << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "synthetic";
  try {
    if (mode == "synthetic") {
      RunSynthetic(argc > 2 ? std::atoi(argv[2]) : 4, argc > 3 ? std::atoi(argv[3]) : 200);
    } else if (mode == "file") {
      RunFile(argc > 2 ? std::atoi(argv[2]) : 1);
    } else {
      Fail("unknown mode " + mode);
    }
  } catch (const std::exception &e) {
    std::cerr << "test_postgres_sec_edgar_q4_differential failed: " << e.what() << '\n';
    return 1;
  }
  std::cout << "test_postgres_sec_edgar_q4_differential " << mode << " passed\n";
  return 0;
}
