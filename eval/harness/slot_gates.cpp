#include "eval/harness/slot_gates.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace accelerator_codegen::slot_gates {
namespace {

using Clock = std::chrono::steady_clock;

void SleepUs(uint64_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

// The first difference, for the report.
std::string Diff(const Rows &accel, const Rows &ref) {
  std::vector<std::string> only_accel, only_ref;
  std::set_difference(accel.begin(), accel.end(), ref.begin(), ref.end(), std::back_inserter(only_accel));
  std::set_difference(ref.begin(), ref.end(), accel.begin(), accel.end(), std::back_inserter(only_ref));
  std::ostringstream out;
  out << "accel_rows=" << accel.size() << " ref_rows=" << ref.size() << " only_accel=" << only_accel.size()
      << (only_accel.empty() ? "" : " e.g. " + only_accel.front()) << " only_ref=" << only_ref.size()
      << (only_ref.empty() ? "" : " e.g. " + only_ref.front());
  return out.str();
}

// Accelerator and reference answers of every case on one session, outside any
// concurrency: the empty string if all agree.
std::string CheckAll(Session &session, const std::vector<ReadCase> &cases, const std::string &when) {
  for (const auto &c : cases) {
    const auto accel = session.Query(c.accel_sql);
    const auto ref = session.Query(c.ref_sql);
    if (accel != ref) {
      return when + " [" + c.label + "]: " + Diff(accel, ref);
    }
  }
  return "";
}

void RunWriteTxn(Session &session, const std::vector<std::string> &statements) {
  session.Exec("BEGIN");
  try {
    for (const auto &sql : statements) {
      session.Exec(sql);
    }
  } catch (...) {
    try {
      session.Exec("ROLLBACK");
    } catch (...) {
    }
    throw;
  }
  session.Exec("COMMIT");
}

struct Load {
  ReaderMode mode;
  uint64_t reader_pause_us = 0; // between the statements of a read transaction
  uint64_t writer_pause_us = 2000;
  // Runs on its own thread while the load is on (a restart).
  std::function<void()> disturb;
};

struct LoadResult {
  long reads = 0, commits = 0, aborted_writes = 0, mismatches = 0, unchecked = 0, errors = 0;
  ReadStats stats;       // accelerator counters during the load
  std::string first_failure;

  bool Exact() const { return mismatches == 0 && errors == 0 && first_failure.empty(); }
  double HitRate() const {
    const auto total = stats.hits + stats.fallbacks;
    return total ? static_cast<double>(stats.hits) / static_cast<double>(total) : 0.0;
  }
  std::string Summary() const {
    std::ostringstream out;
    out << reads << " reads (" << unchecked << " not checkable), " << commits << " commits, " << aborted_writes
        << " aborted writes; hits=" << stats.hits << " fallbacks=" << stats.fallbacks << " builds=" << stats.builds;
    return out.str();
  }
};

// One writer committing continuously and `readers` readers in `load.mode`.
// Every accelerated read must equal the reference at the same snapshot.
LoadResult RunLoad(Engine &engine, const Load &load, const Options &options) {
  const auto cases = engine.ReadCases();
  const auto before = engine.Stats();
  LoadResult result;
  std::mutex mutex;
  auto fail = [&](const std::string &message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (result.first_failure.empty()) {
      result.first_failure = message;
    }
  };
  std::atomic<bool> stop{false};
  std::atomic<long> reads{0}, commits{0}, aborted{0}, mismatches{0}, unchecked{0}, errors{0};
  std::atomic<long> commits_started{0};

  std::vector<std::thread> threads;
  threads.emplace_back([&] {
    try {
      auto session = engine.Connect();
      auto writes = engine.NewWriteSource(options.seed);
      while (!stop) {
        ++commits_started;
        try {
          RunWriteTxn(*session, writes->Next());
          ++commits;
        } catch (const std::exception &) {
          ++aborted; // a conflict or a rejected statement: the transaction rolled back
        }
        SleepUs(load.writer_pause_us);
      }
    } catch (const std::exception &e) {
      fail(std::string("writer: ") + e.what());
    }
  });
  for (int t = 0; t < options.readers; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(options.seed * 1000003 + static_cast<uint64_t>(t));
      try {
        auto session = engine.Connect();
        while (!stop) {
          const auto &c = cases[rng() % cases.size()];
          try {
            session->Exec(load.mode.begin_sql);
            // No commit started or finished while the whole read runs: then the
            // database did not change under a per-statement snapshot.
            const long quiet_at =
                commits_started.load() == commits.load() + aborted.load() ? commits_started.load() : -1;
            const auto accel = session->Query(c.accel_sql);
            SleepUs(load.reader_pause_us ? rng() % load.reader_pause_us : 0);
            const auto ref = session->Query(c.ref_sql);
            SleepUs(load.reader_pause_us ? rng() % load.reader_pause_us : 0);
            const bool second = load.mode.per_transaction && load.reader_pause_us > 0;
            const auto again = second ? session->Query(c.accel_sql) : accel;
            session->Exec("COMMIT");
            ++reads;
            const bool checkable =
                load.mode.per_transaction || (quiet_at >= 0 && commits_started.load() == quiet_at);
            if (!checkable) {
              ++unchecked;
            } else if (accel != ref || again != ref) {
              ++mismatches;
              fail("[" + c.label + "] " +
                   (accel != ref ? "first read: " + Diff(accel, ref) : "second read: " + Diff(again, ref)));
            }
          } catch (const std::exception &e) {
            ++errors;
            fail(std::string("reader error: ") + e.what());
            try {
              session->Exec("ROLLBACK");
            } catch (...) {
            }
          }
        }
      } catch (const std::exception &e) {
        fail(std::string("reader: ") + e.what());
      }
    });
  }
  std::thread disturber;
  if (load.disturb) {
    disturber = std::thread([&] {
      SleepUs(static_cast<uint64_t>(options.seconds) * 500000);
      try {
        load.disturb();
      } catch (const std::exception &e) {
        fail(std::string("disturbance: ") + e.what());
      }
    });
  }
  SleepUs(static_cast<uint64_t>(options.seconds) * 1000000);
  stop = true;
  for (auto &thread : threads) {
    thread.join();
  }
  if (disturber.joinable()) {
    disturber.join();
  }

  const auto after = engine.Stats();
  result.reads = reads;
  result.commits = commits;
  result.aborted_writes = aborted;
  result.mismatches = mismatches;
  result.unchecked = unchecked;
  result.errors = errors;
  result.stats = {after.hits - before.hits, after.fallbacks - before.fallbacks, after.builds - before.builds};
  if (result.first_failure.empty()) {
    auto session = engine.Connect();
    if (auto e = CheckAll(*session, cases, "after the load"); !e.empty()) {
      result.first_failure = e;
    }
  }
  return result;
}

Report Fail(const std::string &detail) { return {Outcome::Fail, detail}; }

// Waits until the accelerator serves a read again (rebuilds are in the
// background), up to `seconds`.
bool WaitForHit(Engine &engine, int seconds) {
  auto session = engine.Connect();
  const auto cases = engine.ReadCases();
  const auto deadline = Clock::now() + std::chrono::seconds(seconds);
  const auto start = engine.Stats().hits;
  while (Clock::now() < deadline) {
    session->Query(cases.front().accel_sql);
    if (engine.Stats().hits > start) {
      return true;
    }
    SleepUs(50000);
  }
  return false;
}

} // namespace

Report EpochStress(Engine &engine, const Options &options) {
  std::ostringstream detail;
  for (const auto &mode : engine.ReaderModes()) {
    const auto r = RunLoad(engine, {mode, /*reader_pause_us=*/3000}, options);
    detail << mode.label << ": " << r.Summary() << "; ";
    if (!r.Exact()) {
      return Fail(mode.label + ": " + (r.first_failure.empty() ? "reader errors" : r.first_failure));
    }
    if (r.reads == 0 || r.commits == 0) {
      return Fail(mode.label + ": no work done: " + r.Summary());
    }
  }
  return {Outcome::Pass, detail.str()};
}

Report HitsUnderWrites(Engine &engine, const Options &options) {
  std::ostringstream detail;
  for (const auto &mode : engine.ReaderModes()) {
    const auto r = RunLoad(engine, {mode, /*reader_pause_us=*/0, /*writer_pause_us=*/5000}, options);
    detail << mode.label << ": " << r.Summary() << "; ";
    if (!r.Exact()) {
      return Fail(mode.label + ": " + (r.first_failure.empty() ? "reader errors" : r.first_failure));
    }
    if (r.reads == 0 || r.commits == 0) {
      return Fail(mode.label + ": no work done: " + r.Summary());
    }
    if (r.HitRate() < options.min_hit_rate) {
      return Fail(mode.label + ": hit rate " + std::to_string(r.HitRate()) + " under continuous commits, need " +
                  std::to_string(options.min_hit_rate) + ": " + r.Summary());
    }
  }
  return {Outcome::Pass, detail.str()};
}

Report ReadAfterCommit(Engine &engine, const Options &options) {
  const auto cases = engine.ReadCases();
  auto writer = engine.Connect();
  auto reader = engine.Connect();
  auto writes = engine.NewWriteSource(options.seed);
  std::atomic<bool> stop{false};
  std::atomic<long> background_errors{0};
  std::vector<std::thread> background; // other readers keep the accelerators busy
  for (int t = 0; t < std::max(1, options.readers / 2); ++t) {
    background.emplace_back([&, t, session = engine.Connect()] {
      try {
        for (std::size_t i = static_cast<std::size_t>(t); !stop; ++i) {
          session->Query(cases[i % cases.size()].accel_sql);
        }
      } catch (const std::exception &) {
        ++background_errors;
      }
    });
  }
  long commits = 0;
  std::string failure;
  const auto end = Clock::now() + std::chrono::seconds(options.seconds);
  try {
    while (Clock::now() < end && failure.empty()) {
      RunWriteTxn(*writer, writes->Next());
      ++commits;
      // Another connection, right after the commit returned.
      failure = CheckAll(*reader, cases, "read after commit " + std::to_string(commits));
    }
  } catch (const std::exception &e) {
    failure = e.what();
  }
  stop = true;
  for (auto &thread : background) {
    thread.join();
  }
  if (!failure.empty()) {
    return Fail(failure);
  }
  if (background_errors > 0 || commits == 0) {
    return Fail("no work done or background readers failed");
  }
  return {Outcome::Pass, std::to_string(commits) + " commits, each read from another connection right after"};
}

Report TxnEdges(Engine &engine, const Options &options) {
  const auto cases = engine.ReadCases();
  auto session = engine.Connect();
  auto writes = engine.NewWriteSource(options.seed);
  const auto builds_before = engine.Stats().builds;
  auto check = [&](const std::string &when) { return CheckAll(*session, cases, when); };
  try {
    if (auto e = check("baseline"); !e.empty()) {
      return Fail(e);
    }
    session->Exec("BEGIN");
    for (const auto &sql : writes->Next()) {
      session->Exec(sql);
    }
    session->Exec("ROLLBACK");
    if (auto e = check("after rollback"); !e.empty()) {
      return Fail(e);
    }
    std::string savepoints = "savepoints not supported by the engine";
    if (engine.SupportsSavepoints()) {
      session->Exec("BEGIN");
      session->Exec("SAVEPOINT s");
      for (const auto &sql : writes->Next()) {
        session->Exec(sql);
      }
      session->Exec("ROLLBACK TO SAVEPOINT s");
      session->Exec("SAVEPOINT t");
      for (const auto &sql : writes->Next()) {
        session->Exec(sql);
      }
      session->Exec("RELEASE SAVEPOINT t");
      session->Exec("COMMIT");
      if (auto e = check("after savepoints"); !e.empty()) {
        return Fail(e);
      }
      savepoints = "savepoints rolled back and released";
    }
    const auto builds_after_aborts = engine.Stats().builds;
    if (builds_after_aborts != builds_before) {
      return Fail("an abort or a savepoint rollback cost a rebuild (" + std::to_string(builds_before) + " -> " +
                  std::to_string(builds_after_aborts) + " builds)");
    }
    RunWriteTxn(*session, engine.TruncateAndRestore());
    for (int i = 0; i < 3; ++i) {
      if (auto e = check("after TRUNCATE"); !e.empty()) {
        return Fail(e);
      }
    }
    if (!WaitForHit(engine, 60)) {
      return Fail("the accelerator did not serve again within 60 s of TRUNCATE");
    }
    return {Outcome::Pass, "rollback: no change, no rebuild; " + savepoints + "; TRUNCATE: exact and served again"};
  } catch (const std::exception &e) {
    return Fail(e.what());
  }
}

Report Restart(Engine &engine, const Options &options) {
  std::atomic<bool> restarted{false};
  Load load{engine.ReaderModes().front()};
  load.reader_pause_us = 1000;
  load.disturb = [&] { restarted = engine.RestartAccelerators(); };
  const auto r = RunLoad(engine, load, options);
  if (!restarted) {
    return {Outcome::NotApplicable, "the engine has no separate accelerator worker or process"};
  }
  if (!r.Exact()) {
    return Fail(r.first_failure.empty() ? "reader errors across the restart: " + r.Summary() : r.first_failure);
  }
  if (!WaitForHit(engine, 120)) {
    return Fail("no hits after the restart: " + r.Summary());
  }
  return {Outcome::Pass, "restart mid-load: exact answers, hits resumed; " + r.Summary()};
}

Report OwnWrites(Engine &engine, const Options &options) {
  const auto cases = engine.ReadCases();
  auto writer = engine.Connect();
  auto other = engine.Connect();
  auto writes = engine.NewWriteSource(options.seed);
  const auto builds_before = engine.Stats().builds;
  constexpr int kRoundsPerMode = 6;
  long rounds = 0, refused = 0;
  try {
    for (const auto &mode : engine.ReaderModes()) {
      for (int round = 0; round < kRoundsPerMode; ++round) {
        const bool commit = round % 2 == 1;
        const std::string when = mode.label + " round " + std::to_string(round);
        writer->Exec(mode.begin_sql);
        // Planned (and run once) before the transaction's write.
        std::vector<std::function<Rows()>> planned;
        for (const auto &c : cases) {
          planned.push_back(writer->Prepare(c.accel_sql));
          writer->Query(c.accel_sql);
        }
        for (const auto &sql : writes->Next()) {
          writer->Exec(sql);
        }
        std::vector<Rows> refs;
        for (std::size_t i = 0; i < cases.size(); ++i) {
          refs.push_back(writer->Query(cases[i].ref_sql));
          if (const auto accel = writer->Query(cases[i].accel_sql); accel != refs[i]) {
            return Fail(when + " [" + cases[i].label + "] read after its own write: " + Diff(accel, refs[i]));
          }
        }
        // Uncommitted rows are invisible to everyone else.
        if (auto e = CheckAll(*other, cases, when + ", other connection during the transaction"); !e.empty()) {
          return Fail(e);
        }
        // A statement planned before the write: exact, or refused loudly (which aborts the
        // transaction, so only in the rounds that roll back); a stale answer is the failure.
        bool refused_here = false;
        for (std::size_t i = 0; i < cases.size() && !refused_here; ++i) {
          if (!planned[i] || commit) {
            continue;
          }
          try {
            if (const auto accel = planned[i](); accel != refs[i]) {
              return Fail(when + " [" + cases[i].label + "] statement planned before the write: " + Diff(accel, refs[i]));
            }
          } catch (const std::exception &) {
            refused_here = true;
            ++refused;
          }
        }
        writer->Exec(commit && !refused_here ? "COMMIT" : "ROLLBACK");
        if (auto e = CheckAll(*other, cases, when + (commit ? " after commit" : " after rollback")); !e.empty()) {
          return Fail(e);
        }
        if (auto e = CheckAll(*writer, cases, when + ", writer after the transaction"); !e.empty()) {
          return Fail(e);
        }
        ++rounds;
      }
    }
  } catch (const std::exception &e) {
    return Fail(e.what());
  }
  if (engine.Stats().builds != builds_before) {
    return Fail("a transaction's own reads or its rollback cost a rebuild");
  }
  return {Outcome::Pass, std::to_string(rounds) + " write-then-read transactions (rolled back and committed) exact; " +
                             std::to_string(refused) + " statements planned before a write refused loudly"};
}

Report DdlEvents(Engine &engine, const Options &options) {
  const auto ddl_cases = engine.DdlCases();
  if (ddl_cases.empty()) {
    return {Outcome::NotApplicable, "the engine driver expresses no DDL on the tracked tables"};
  }
  const auto cases = engine.ReadCases();
  auto session = engine.Connect();
  auto reader = engine.Connect();
  auto writes = engine.NewWriteSource(options.seed);
  try {
    if (auto e = CheckAll(*session, cases, "baseline"); !e.empty()) {
      return Fail(e);
    }
    for (const auto &ddl : ddl_cases) {
      for (const auto &sql : ddl.statements) {
        session->Exec(sql);
      }
      for (int i = 0; i < 3; ++i) {
        for (auto *who : {session.get(), reader.get()}) {
          if (auto e = CheckAll(*who, cases, "after " + ddl.label); !e.empty()) {
            return Fail(e);
          }
        }
      }
      RunWriteTxn(*session, writes->Next());
      if (auto e = CheckAll(*reader, cases, "a write after " + ddl.label); !e.empty()) {
        return Fail(e);
      }
      for (const auto &sql : ddl.restore) {
        session->Exec(sql);
      }
      for (auto *who : {session.get(), reader.get()}) {
        if (auto e = CheckAll(*who, cases, "after restoring from " + ddl.label); !e.empty()) {
          return Fail(e);
        }
      }
    }
  } catch (const std::exception &e) {
    return Fail(e.what());
  }
  std::string names;
  for (const auto &ddl : ddl_cases) {
    names += (names.empty() ? "" : ", ") + ddl.label;
  }
  return {Outcome::Pass, "exact after, a write after, and restoring from: " + names};
}

int Run(Engine &engine, const std::string &scenario, const Options &options) {
  const std::vector<std::pair<std::string, Report (*)(Engine &, const Options &)>> scenarios = {
      {"epoch_stress", EpochStress},         {"hits_under_writes", HitsUnderWrites},
      {"read_after_commit", ReadAfterCommit}, {"txn_edges", TxnEdges},
      {"restart", Restart},                  {"own_writes", OwnWrites},
      {"ddl_events", DdlEvents}};
  bool any = false, failed = false, applicable = false;
  for (const auto &[name, run] : scenarios) {
    if (scenario != "all" && scenario != name) {
      continue;
    }
    any = true;
    Report report;
    try {
      report = run(engine, options);
    } catch (const std::exception &e) {
      report = Fail(e.what());
    }
    const char *label = report.outcome == Outcome::Pass ? "PASS" : report.outcome == Outcome::Fail ? "FAIL" : "N/A ";
    std::cout << label << " " << engine.Name() << " " << name << ": " << report.detail << std::endl;
    failed |= report.outcome == Outcome::Fail;
    applicable |= report.outcome != Outcome::NotApplicable;
  }
  if (!any) {
    std::cerr << "unknown scenario: " << scenario << std::endl;
    return 2;
  }
  return failed ? 1 : applicable ? 0 : 77;
}

} // namespace accelerator_codegen::slot_gates
