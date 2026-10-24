// The canary checks: what an accelerator needs from a backend, under realistic conditions, judged
// against the database's own SQL. Each check has an owning layer (the layer whose session fixes a
// failure) and names the defect class it exists for. Engine-neutral: SQL through Session, the
// canary accelerators through Engine.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include "canary_mirror.hpp"

namespace accelerator_codegen::canary {

struct CheckFailed : std::runtime_error {
  using std::runtime_error::runtime_error;
};

#define CANARY_REQUIRE(cond, message)                                                                     \
  do {                                                                                                    \
    if (!(cond)) {                                                                                        \
      std::ostringstream canary_out_;                                                                     \
      canary_out_ << message;                                                                             \
      throw ::accelerator_codegen::canary::CheckFailed(canary_out_.str());                                \
    }                                                                                                     \
  } while (0)

inline std::string Show(const Cell &cell) {
  if (std::holds_alternative<std::monostate>(cell)) {
    return "NULL";
  }
  if (const auto *i = std::get_if<int64_t>(&cell)) {
    return std::to_string(*i);
  }
  const auto &s = std::get<std::string>(cell);
  return "'" + (s.size() > 24 ? s.substr(0, 24) + "..." : s) + "'(" + std::to_string(s.size()) + ")";
}

inline std::string Show(const Row &row) {
  std::string out = "(";
  for (std::size_t i = 0; i < row.size(); ++i) {
    out += (i ? ", " : "") + Show(row[i]);
  }
  return out + ")";
}

// "" when equal; else the first difference. `ordered`: row order counts, else the multisets.
inline std::string Diff(Rows actual, Rows expected, bool ordered) {
  if (!ordered) {
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
  }
  std::ostringstream out;
  if (actual.size() != expected.size()) {
    out << "row count " << actual.size() << " != expected " << expected.size() << "; ";
  }
  for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i) {
    if (actual[i] != expected[i]) {
      out << "first difference at row " << i << ": got " << Show(actual[i]) << ", expected " << Show(expected[i]);
      return out.str();
    }
  }
  return out.str();
}

inline std::string Lit(const Cell &cell) {
  if (std::holds_alternative<std::monostate>(cell)) {
    return "NULL";
  }
  if (const auto *i = std::get_if<int64_t>(&cell)) {
    return std::to_string(*i);
  }
  std::string out = "'";
  for (const char ch : std::get<std::string>(cell)) {
    out += ch;
    if (ch == '\'') {
      out += '\'';
    }
  }
  return out + "'";
}

// ---- the suite's tables ----

inline const std::vector<CanaryColumn> kItemColumns = {
    {"id", false, false}, {"grp", false, true}, {"label", true, true}, {"note", true, false}, {"amount", false, false}};
inline const std::vector<CanaryColumn> kGroupColumns = {{"grp", false, false}, {"gname", true, false}};
inline const std::vector<CanaryColumn> kScanColumns = {{"id", false, false}, {"s", true, false}};

inline const char *kItemsDdl =
    "CREATE TABLE canary_items (id BIGINT NOT NULL, grp BIGINT, label VARCHAR(400), note VARCHAR(400) NOT NULL, "
    "amount BIGINT NOT NULL)";
inline const char *kGroupsDdl = "CREATE TABLE canary_groups (grp BIGINT NOT NULL, gname VARCHAR(100) NOT NULL)";
inline const char *kScanDdl = "CREATE TABLE canary_scan (id BIGINT NOT NULL, s VARCHAR(400) NOT NULL)";

// Strings of every length class: empty, inlined short (<= 12 bytes), the 12/13 boundary, long, multibyte.
inline std::string Text(int64_t i, int salt) {
  switch ((i + salt) % 7) {
  case 0: return "";
  case 1: return "a";
  case 2: return "k" + std::to_string(i % 100000);
  case 3: return std::string(12, static_cast<char>('a' + i % 26));
  case 4: return std::string(13, static_cast<char>('a' + i % 26));
  case 5: return std::string(40 + i % 150, static_cast<char>('A' + i % 26)) + std::to_string(i);
  default: return "caf\xc3\xa9-" + std::to_string(i);
  }
}

inline Row ItemRow(int64_t id, int salt = 0) {
  const int64_t amount = id % 11 == 0 ? (id % 2 ? (int64_t{1} << 61) : -(int64_t{1} << 61)) : id * 7919 - 5000000;
  return {id, id % 7 == 0 ? Cell(std::monostate{}) : Cell(id % 13), id % 5 == 0 ? Cell(std::monostate{}) : Cell(Text(id, salt)),
          Text(id, salt + 3), amount};
}

inline std::string InsertSql(const std::string &table, const std::vector<Row> &rows) {
  std::string sql = "INSERT INTO " + table + " VALUES ";
  for (std::size_t r = 0; r < rows.size(); ++r) {
    sql += (r ? ", (" : "(");
    for (std::size_t c = 0; c < rows[r].size(); ++c) {
      sql += (c ? ", " : "") + Lit(rows[r][c]);
    }
    sql += ")";
  }
  return sql;
}

// Rows [first, last) in a scrambled order (so the table's physical order is not the sort order), in batches.
inline void Load(Session &session, const std::string &table, int64_t first, int64_t last,
                 const std::function<Row(int64_t)> &make, std::size_t batch = 500) {
  std::vector<int64_t> ids;
  for (int64_t i = first; i < last; ++i) {
    ids.push_back(i);
  }
  std::mt19937_64 rng(static_cast<uint64_t>(first) * 7919 + 17);
  std::shuffle(ids.begin(), ids.end(), rng);
  for (std::size_t at = 0; at < ids.size(); at += batch) {
    std::vector<Row> rows;
    for (std::size_t i = at; i < std::min(ids.size(), at + batch); ++i) {
      rows.push_back(make(ids[i]));
    }
    session.Exec(InsertSql(table, rows));
  }
}

inline const char *kItemsQuery =
    "SELECT id, grp, label, note, amount FROM canary_items ORDER BY grp DESC, id";
inline const char *kGroupsQuery = "SELECT grp, gname FROM canary_groups ORDER BY gname DESC, grp";
inline const char *kBothQuery =
    "SELECT CAST(0 AS BIGINT) AS tag, id, label FROM canary_items UNION ALL "
    "SELECT CAST(1 AS BIGINT), grp, gname FROM canary_groups ORDER BY 1, 2, 3";
inline const char *kScanQuery = "SELECT id, s FROM canary_scan ORDER BY id, s";

inline AcceleratorSpec Items(const std::string &name, MirrorOptions options = {}) {
  return {name, {"canary_items"}, kItemsQuery, [=](ScanFn scan) {
            return std::make_shared<MirrorModule>(name, std::vector<TableShape>{{"canary_items", kItemColumns, {}}},
                                                  std::move(scan), options);
          }};
}
inline AcceleratorSpec Groups(const std::string &name, MirrorOptions options = {}) {
  return {name, {"canary_groups"}, kGroupsQuery, [=](ScanFn scan) {
            return std::make_shared<MirrorModule>(name, std::vector<TableShape>{{"canary_groups", kGroupColumns, {}}},
                                                  std::move(scan), options);
          }};
}
inline AcceleratorSpec Both(const std::string &name) {
  return {name, {"canary_items", "canary_groups"}, kBothQuery, [=](ScanFn scan) {
            MirrorOptions options;
            options.tagged = true;
            return std::make_shared<MirrorModule>(
                name, std::vector<TableShape>{{"canary_items", kItemColumns, {0, 2}},
                                              {"canary_groups", kGroupColumns, {0, 1}}},
                std::move(scan), options);
          }};
}
inline AcceleratorSpec Scan(const std::string &name, std::size_t partitions) {
  MirrorOptions options;
  options.partitions = partitions;
  options.incremental = false;
  return {name, {"canary_scan"}, kScanQuery, [=](ScanFn scan) {
            return std::make_shared<MirrorModule>(name, std::vector<TableShape>{{"canary_scan", kScanColumns, {}}},
                                                  std::move(scan), options);
          }};
}

// ---- reads ----

using Clock = std::chrono::steady_clock;

inline Rows Read(Engine &engine, Session &session, const std::string &name) {
  return session.Query(engine.ReadSql(name));
}

// Reads until one read is a hit (the state was built by reads alone), within `seconds`; returns its rows.
inline Rows ReadUntilHit(Engine &engine, Session &session, const std::string &name, int seconds = 60) {
  const auto deadline = Clock::now() + std::chrono::seconds(seconds);
  while (Clock::now() < deadline) {
    const auto before = engine.ReadCounters(name);
    auto rows = Read(engine, session, name);
    if (engine.ReadCounters(name).hits > before.hits) {
      return rows;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  throw CheckFailed("no read of " + name + " was answered from the state within " + std::to_string(seconds) +
                    "s: repeated reads never built it (builds=" + std::to_string(engine.ReadCounters(name).builds) + ")");
}

// One read that must be a hit, equal (as a multiset) to the SQL answer.
inline void ExpectHit(Engine &engine, Session &session, const std::string &name, const std::string &sql,
                      const std::string &when) {
  const auto before = engine.ReadCounters(name);
  const auto rows = Read(engine, session, name);
  const auto after = engine.ReadCounters(name);
  CANARY_REQUIRE(after.hits == before.hits + 1 && after.fallbacks == before.fallbacks,
                 name << " " << when << ": the read was not a hit (hits +" << after.hits - before.hits << ", fallbacks +"
                      << after.fallbacks - before.fallbacks << ")");
  const auto diff = Diff(rows, session.Query(sql), false);
  CANARY_REQUIRE(diff.empty(), name << " " << when << ": " << diff);
}

// ---- checks ----

struct Check {
  const char *id;
  const char *owner; // the layer whose session fixes a failure
  const char *defect;
  std::function<void(Engine &)> run;
};

inline void TextViewsShortAndLong(Engine &engine) {
  auto db = engine.Connect();
  db->Exec(kItemsDdl);
  Load(*db, "canary_items", 0, 6000, [](int64_t i) { return ItemRow(i); });
  engine.Register(Items("canary_serial", {1, true, false, false}));
  engine.Register(Items("canary_parts", {4, true, false, false}));
  for (const char *name : {"canary_serial", "canary_parts"}) {
    const auto rows = ReadUntilHit(engine, *db, name);
    const auto diff = Diff(rows, db->Query(kItemsQuery), false);
    CANARY_REQUIRE(diff.empty(), name << ": the scanned text of short and long strings differs from SQL: " << diff);
  }
}

inline void DictionaryAndNulls(Engine &engine) {
  auto db = engine.Connect();
  db->Exec(kItemsDdl);
  // Few distinct values over many rows: after the storage settles the columns are dictionary encoded.
  std::vector<std::string> words = {"red", "green", "blue", "a-much-longer-colour-name", ""};
  for (int64_t at = 0; at < 150000; at += 1000) {
    std::vector<Row> rows;
    for (int64_t i = at; i < at + 1000; ++i) {
      rows.push_back({i, i % 9 == 0 ? Cell(std::monostate{}) : Cell(i % 3),
                      i % 4 == 0 ? Cell(std::monostate{}) : Cell(words[i % words.size()]), words[(i / 5) % words.size()],
                      i % 17});
    }
    db->Exec(InsertSql("canary_items", rows));
  }
  engine.Settle();
  engine.Register(Items("canary_serial", {1, true, false, false}));
  engine.Register(Items("canary_parts", {5, true, false, false}));
  for (const char *name : {"canary_serial", "canary_parts"}) {
    const auto diff = Diff(ReadUntilHit(engine, *db, name), db->Query(kItemsQuery), false);
    CANARY_REQUIRE(diff.empty(), name << ": dictionary-encoded and NULL cells differ from SQL: " << diff);
  }
  db->Exec("INSERT INTO canary_items VALUES (900001, NULL, NULL, 'x', 1), (900002, 5, NULL, '', 2)");
  db->Exec("UPDATE canary_items SET label = NULL, grp = NULL WHERE id < 50");
  ExpectHit(engine, *db, "canary_serial", kItemsQuery, "after NULL-producing writes");
}

inline void ScanGrowthAndDelete(Engine &engine) {
  auto db = engine.Connect();
  db->Exec(kScanDdl);
  const auto make = [](int64_t i) { return Row{i, Text(i, 1)}; };
  Load(*db, "canary_scan", 0, 300000, make, 2000);
  engine.Settle();
  engine.Register(Scan("canary_serial", 1));
  engine.Register(Scan("canary_parts", 7));
  const auto check = [&](const std::string &when) {
    const auto golden = db->Query(kScanQuery);
    for (const char *name : {"canary_serial", "canary_parts"}) {
      const auto diff = Diff(ReadUntilHit(engine, *db, name, 120), golden, false);
      CANARY_REQUIRE(diff.empty(), name << " " << when << ": " << diff);
    }
  };
  check("on the first build");
  Load(*db, "canary_scan", 300000, 520000, make, 2000); // the table grows past more row groups
  check("after the table grew");
  db->Exec("DELETE FROM canary_scan WHERE id < 130000");  // whole row groups
  db->Exec("DELETE FROM canary_scan WHERE id % 3 = 0");   // and scattered rows
  engine.Settle();
  check("after multi-row-group DELETEs");
  Load(*db, "canary_scan", 0, 5000, make, 2000);          // rows back into the emptied range
  check("after inserts into the deleted range");
}

inline void HitAfterFirstRead(Engine &engine) {
  auto db = engine.Connect();
  db->Exec(kItemsDdl);
  Load(*db, "canary_items", 0, 3000, [](int64_t i) { return ItemRow(i); });
  engine.Register(Items("canary_rows"));
  CANARY_REQUIRE(engine.ReadCounters("canary_rows").builds == 0, "the state was built before any read");
  ReadUntilHit(engine, *db, "canary_rows"); // reads alone must build it
  CANARY_REQUIRE(engine.ReadCounters("canary_rows").builds >= 1, "reads were served without a build");
  engine.TrapFallback(true);
  for (int i = 0; i < 5; ++i) {
    ExpectHit(engine, *db, "canary_rows", kItemsQuery, "with fallback SQL trapped");
  }
  engine.TrapFallback(false);
}

inline void DmlIncrementalHits(Engine &engine) {
  auto writer = engine.Connect();
  auto reader = engine.Connect();
  writer->Exec(kItemsDdl);
  Load(*writer, "canary_items", 0, 4000, [](int64_t i) { return ItemRow(i); });
  engine.Register(Items("canary_rows"));
  ReadUntilHit(engine, *reader, "canary_rows");
  const auto start = engine.ReadCounters("canary_rows");
  std::mt19937_64 rng(42);
  int64_t next = 100000;
  for (int step = 0; step < 150; ++step) {
    std::string sql;
    switch (rng() % 6) {
    case 0: sql = InsertSql("canary_items", {ItemRow(next++), ItemRow(next++)}); break;
    case 1: sql = "DELETE FROM canary_items WHERE id = " + std::to_string(rng() % 4000); break;
    case 2: sql = "DELETE FROM canary_items WHERE id >= " + std::to_string(rng() % 4000) + " AND id < " +
                  std::to_string(rng() % 4000 + 30); break;
    case 3: sql = "UPDATE canary_items SET amount = amount + 1, label = NULL WHERE id = " + std::to_string(rng() % 4000); break;
    case 4: sql = "UPDATE canary_items SET id = id + 500000 WHERE id = " + std::to_string(rng() % 4000); break;
    default: sql = "INSERT INTO canary_items SELECT id + 700000, grp, label, note, amount FROM canary_items WHERE id < " +
                   std::to_string(rng() % 40); break;
    }
    writer->Exec(sql);
    ExpectHit(engine, *reader, "canary_rows", kItemsQuery, "after `" + sql.substr(0, 60) + "`");
  }
  const auto end = engine.ReadCounters("canary_rows");
  CANARY_REQUIRE(end.builds == start.builds && end.stale_commits == start.stale_commits,
                 "writes forced rebuilds: builds " << start.builds << " -> " << end.builds << ", stale commits "
                                                  << start.stale_commits << " -> " << end.stale_commits);
  CANARY_REQUIRE(end.fallbacks == start.fallbacks && end.hits == start.hits + 150,
                 "hit rate not held: hits +" << end.hits - start.hits << " of 150 reads, fallbacks +"
                                             << end.fallbacks - start.fallbacks);
}

inline void BulkInsertThreads(Engine &engine) {
  auto admin = engine.Connect();
  admin->Exec(kItemsDdl);
  admin->Exec("CREATE TABLE canary_seed (id BIGINT NOT NULL, grp BIGINT, label VARCHAR(400), note VARCHAR(400) NOT NULL, amount BIGINT NOT NULL)");
  Load(*admin, "canary_items", 0, 2000, [](int64_t i) { return ItemRow(i); });
  Load(*admin, "canary_seed", 0, 150000, [](int64_t i) { return ItemRow(i, 2); }, 1000);
  engine.Register(Items("canary_rows"));
  ReadUntilHit(engine, *admin, "canary_rows");
  constexpr int kThreads = 4;
  std::vector<std::thread> threads;
  std::vector<std::string> errors(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      try {
        auto session = engine.Connect();
        session->Exec("INSERT INTO canary_items SELECT id + " + std::to_string(1000000 * (t + 1)) +
                      ", grp, label, note, amount FROM canary_seed");
      } catch (const std::exception &e) {
        errors[t] = e.what();
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (const auto &error : errors) {
    CANARY_REQUIRE(error.empty(), "a bulk INSERT ... SELECT failed: " << error);
  }
  const auto rows = ReadUntilHit(engine, *admin, "canary_rows", 120);
  const auto diff = Diff(rows, admin->Query(kItemsQuery), false);
  CANARY_REQUIRE(diff.empty(), "after " << kThreads << " concurrent bulk inserts: " << diff);
}

inline void MultiTableByName(Engine &engine) {
  auto db = engine.Connect();
  db->Exec(kItemsDdl);
  db->Exec(kGroupsDdl);
  Load(*db, "canary_items", 0, 800, [](int64_t i) { return ItemRow(i); });
  for (int64_t g = 0; g < 13; ++g) {
    db->Exec("INSERT INTO canary_groups VALUES (" + std::to_string(g) + ", 'group-" + std::to_string(g) + "')");
  }
  // Module table id 0 is canary_items for the first, canary_groups for the second: ids collide.
  engine.Register(Items("canary_items_only"));
  engine.Register(Groups("canary_groups_only"));
  engine.Register(Both("canary_both"));
  const auto expect_all = [&](const std::string &when, bool items_hit, bool groups_hit) {
    for (const auto &[name, sql, expect] : std::vector<std::tuple<std::string, std::string, bool>>{
             {"canary_items_only", kItemsQuery, items_hit}, {"canary_groups_only", kGroupsQuery, groups_hit},
             {"canary_both", kBothQuery, true}}) {
      if (expect) {
        ExpectHit(engine, *db, name, sql, when);
      }
    }
  };
  for (const char *name : {"canary_items_only", "canary_groups_only", "canary_both"}) {
    const auto sql = std::string(name) == "canary_items_only" ? kItemsQuery
                     : std::string(name) == "canary_groups_only" ? kGroupsQuery : kBothQuery;
    const auto diff = Diff(ReadUntilHit(engine, *db, name), db->Query(sql), false);
    CANARY_REQUIRE(diff.empty(), name << " on the first build: " << diff);
  }
  const auto items_before = engine.ReadCounters("canary_items_only");
  db->Exec("INSERT INTO canary_groups VALUES (99, 'late'), (100, 'later')");
  db->Exec("DELETE FROM canary_groups WHERE grp = 3");
  expect_all("after writes to canary_groups", true, true);
  CANARY_REQUIRE(engine.ReadCounters("canary_items_only").builds == items_before.builds &&
                     engine.ReadCounters("canary_items_only").stale_commits == items_before.stale_commits,
                 "canary_items_only was disturbed by writes to canary_groups");
  db->Exec("UPDATE canary_items SET label = 'moved' WHERE id < 20");
  db->Exec("DELETE FROM canary_items WHERE id >= 700");
  expect_all("after writes to canary_items", true, true);
  const auto groups_before = engine.ReadCounters("canary_groups_only");
  db->Exec("INSERT INTO canary_items VALUES (5000, 1, 'z', 'n', 1)");
  expect_all("after one more write to canary_items", true, true);
  CANARY_REQUIRE(engine.ReadCounters("canary_groups_only").stale_commits == groups_before.stale_commits,
                 "canary_groups_only was disturbed by writes to canary_items");
}

inline void FallbackSnapshotAndOrder(Engine &engine) {
  auto writer = engine.Connect();
  auto reader = engine.Connect();
  writer->Exec(kItemsDdl);
  Load(*writer, "canary_items", 0, 1500, [](int64_t i) { return ItemRow(i); });
  engine.Register(Items("canary_rows"));
  ReadUntilHit(engine, *reader, "canary_rows");
  for (const bool forced : {true, false}) {
    engine.ForceFallback(forced);
    // The fallback is the query itself: same rows in the same order.
    if (forced) {
      const auto before = engine.ReadCounters("canary_rows");
      const auto rows = Read(engine, *reader, "canary_rows");
      CANARY_REQUIRE(engine.ReadCounters("canary_rows").fallbacks > before.fallbacks, "ForceFallback did not force the fallback");
      const auto diff = Diff(rows, reader->Query(kItemsQuery), true);
      CANARY_REQUIRE(diff.empty(), "the fallback answer is not the query's answer (rows or ORDER BY): " << diff);
    }
    // A reader inside a transaction sees its snapshot, on the fallback and on the state alike.
    reader->Exec("BEGIN");
    const auto snapshot = reader->Query(kItemsQuery);
    writer->Exec("INSERT INTO canary_items VALUES (777001, 1, 'late', 'late', 5)");
    writer->Exec("DELETE FROM canary_items WHERE id < 10");
    const auto again = Read(engine, *reader, "canary_rows");
    const auto diff = Diff(again, snapshot, false);
    reader->Exec("COMMIT");
    CANARY_REQUIRE(diff.empty(), (forced ? "fallback" : "state") << " read inside a transaction is not at its snapshot: " << diff);
    // Own writes are in the answer.
    reader->Exec("BEGIN");
    reader->Exec("INSERT INTO canary_items VALUES (777002, 2, 'mine', 'mine', 6)");
    const auto own = Diff(Read(engine, *reader, "canary_rows"), reader->Query(kItemsQuery), false);
    reader->Exec("ROLLBACK");
    CANARY_REQUIRE(own.empty(), (forced ? "fallback" : "state") << " read does not include the transaction's own writes: " << own);
  }
  engine.ForceFallback(false);
}

inline void DeclineSnapshot(Engine &engine) {
  auto writer = engine.Connect();
  auto reader = engine.Connect();
  writer->Exec(kItemsDdl);
  Load(*writer, "canary_items", 0, 1000, [](int64_t i) { return ItemRow(i); });
  engine.Register(Items("canary_decline", {1, true, true, false}));
  reader->Exec("BEGIN");
  const auto snapshot = reader->Query(kItemsQuery);
  writer->Exec("INSERT INTO canary_items VALUES (888001, 1, 'late', 'late', 5)");
  writer->Exec("UPDATE canary_items SET amount = 0 WHERE id < 100");
  // The module declines, so the runtime answers from SQL: at the reader's snapshot, not the newest.
  const auto rows = Read(engine, *reader, "canary_decline");
  const auto diff = Diff(rows, snapshot, false);
  reader->Exec("COMMIT");
  CANARY_REQUIRE(diff.empty(), "an answer from SQL after the module declined is not at the reader's snapshot: " << diff);
  const auto fresh = Diff(Read(engine, *reader, "canary_decline"), reader->Query(kItemsQuery), false);
  CANARY_REQUIRE(fresh.empty(), "the declined answer outside a transaction differs from SQL: " << fresh);
}

inline void CommitBuildConcurrency(Engine &engine) {
  auto admin = engine.Connect();
  admin->Exec(kItemsDdl);
  Load(*admin, "canary_items", 0, 20000, [](int64_t i) { return ItemRow(i); }, 1000);
  engine.Register(Items("canary_a", {3, true, false, false}));
  engine.Register(Items("canary_b"));
  ReadUntilHit(engine, *admin, "canary_a");
  ReadUntilHit(engine, *admin, "canary_b");
  std::atomic<bool> stop{false};
  std::vector<std::string> errors(8);
  std::vector<std::thread> threads;
  for (int w = 0; w < 3; ++w) {
    threads.emplace_back([&, w] {
      try {
        auto session = engine.Connect();
        for (int64_t i = 0; !stop; ++i) {
          const int64_t id = 2000000 + w * 1000000 + i;
          session->Exec(InsertSql("canary_items", {ItemRow(id)}));
          if (i % 3 == 0) {
            session->Exec("DELETE FROM canary_items WHERE id = " + std::to_string(id));
          }
        }
      } catch (const std::exception &e) {
        errors[w] = e.what();
      }
    });
  }
  for (int r = 0; r < 3; ++r) {
    threads.emplace_back([&, r] {
      try {
        auto session = engine.Connect();
        while (!stop) {
          Read(engine, *session, r % 2 ? "canary_a" : "canary_b");
        }
      } catch (const std::exception &e) {
        errors[3 + r] = e.what();
      }
    });
  }
  threads.emplace_back([&] {
    try {
      while (!stop) {
        engine.Invalidate("canary_a");
        engine.Invalidate("canary_b");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
    } catch (const std::exception &e) {
      errors[6] = e.what();
    }
  });
  std::this_thread::sleep_for(std::chrono::seconds(6));
  stop = true;
  for (auto &thread : threads) {
    thread.join();
  }
  for (const auto &error : errors) {
    CANARY_REQUIRE(error.empty(), "a client failed under concurrent commits, reads and rebuilds: " << error);
  }
  for (const char *name : {"canary_a", "canary_b"}) {
    const auto diff = Diff(ReadUntilHit(engine, *admin, name, 60), admin->Query(kItemsQuery), false);
    CANARY_REQUIRE(diff.empty(), name << " after the concurrent run: " << diff);
  }
}

inline const std::vector<Check> &AllChecks() {
  static const std::vector<Check> checks = {
      {"text_views_short_and_long", "scan", "dangling text view of short strings read after the chunk is released", TextViewsShortAndLong},
      {"dictionary_and_nulls", "scan", "dictionary-encoded vectors and NULL cells mis-scanned", DictionaryAndNulls},
      {"scan_growth_and_delete", "scan", "partitioned scan duplicating or losing rows after growth and DELETE", ScanGrowthAndDelete},
      {"hit_after_first_read", "api", "reads never build the state; a hit that still runs the fallback SQL", HitAfterFirstRead},
      {"dml_incremental_hits", "capture", "a write forcing a full rebuild, or the hit rate dropping under writes", DmlIncrementalHits},
      {"bulk_insert_threads", "capture", "capture racing under a multi-threaded bulk INSERT ... SELECT", BulkInsertThreads},
      {"multi_table_by_name", "runtime", "module table ids not mapped to engine tables by name", MultiTableByName},
      {"fallback_snapshot_and_order", "api", "fallback SQL at the wrong snapshot, without own writes, or without ORDER BY", FallbackSnapshotAndOrder},
      {"decline_snapshot", "runtime", "the answer after a module declines taken at the wrong snapshot", DeclineSnapshot},
      {"commit_build_concurrency", "runtime", "commit/build deadlock or an answer wrong after racing rebuilds", CommitBuildConcurrency},
  };
  return checks;
}

} // namespace accelerator_codegen::canary
