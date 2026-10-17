#pragma once

// Where tests, benches and harnesses find their DuckDB databases, and private
// scratch space for the files they write.
//
// Every database path resolves here: an explicit env var wins, otherwise the
// file of that name in the data directory (ACCEL_DUCKDB_ROOT, defaulting to the
// sibling DuckDB checkout that also holds the shared databases).
//
//   ACCEL_TPCH_DB            TPC-H SF1 (or whatever scale the caller points at)
//   ACCEL_TPCH_SF10_DB       TPC-H SF10
//   ACCEL_TPCH_TEST_DB       small TPC-H subset (scripts/create_tpch_test_db.py)
//   ACCEL_SEC_EDGAR_DB       SEC-EDGAR benchmark database
//   ACCEL_SEC_EDGAR_TEST_DB  small SEC-EDGAR subset (scripts/create_sec_edgar_test_db.py)
//   ACCEL_DUCKDB_ROOT        data directory for the defaults above
//
// The shared databases are read by many concurrent runs (worktrees, ctest -j,
// differential scripts). A test that writes must work on a PrivateDbCopy; one
// that only reads should open the file read-only. Scratch files go in a
// ScratchDir (unique per process, removed on exit), never a fixed /tmp name.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <signal.h>
#include <unistd.h>

namespace accelerator_codegen::test_support {

inline std::string EnvOr(const char *env_var, const std::string &fallback) {
  const char *value = std::getenv(env_var);
  if (value != nullptr && *value != '\0') {
    return value;
  }
  return fallback;
}

inline int EnvOrInt(const char *env_var, int fallback) {
  const char *value = std::getenv(env_var);
  return value != nullptr && *value != '\0' ? std::stoi(value) : fallback;
}

inline std::string DuckDbDataDir() {
  return EnvOr("ACCEL_DUCKDB_ROOT", "/home/akhilvb/accelerator-codegen-duckdb");
}

// `<data dir>/<file_name>`, e.g. DataDbPath("tpch_sf20.duckdb").
inline std::string DataDbPath(const std::string &file_name) {
  return (std::filesystem::path(DuckDbDataDir()) / file_name).string();
}

inline std::string TpchDbPath() { return EnvOr("ACCEL_TPCH_DB", DataDbPath("tpch_sf1.duckdb")); }
inline std::string TpchSf10DbPath() { return EnvOr("ACCEL_TPCH_SF10_DB", DataDbPath("tpch_sf10.duckdb")); }
inline std::string TpchTestDbPath() { return EnvOr("ACCEL_TPCH_TEST_DB", DataDbPath("tpch_test.duckdb")); }
inline std::string SecEdgarDbPath() { return EnvOr("ACCEL_SEC_EDGAR_DB", DataDbPath("sec_edgar_sf3.duckdb")); }
inline std::string SecEdgarTestDbPath() {
  return EnvOr("ACCEL_SEC_EDGAR_TEST_DB", DataDbPath("sec_edgar_test.duckdb"));
}

// The SEC-EDGAR database the base-query smoke tests check: ACCEL_SEC_EDGAR_DB
// when set (e.g. the pipeline pointing them at the benchmark DB), otherwise the
// small test subset, which has every table and keeps the smoke tests quick.
inline std::string SecEdgarSmokeDbPath() { return EnvOr("ACCEL_SEC_EDGAR_DB", SecEdgarTestDbPath()); }

// A fresh directory under $TMPDIR (default /tmp), unique to this process and
// removed with everything in it (DuckDB .wal files included) on destruction.
// A test killed by SIGTERM/SIGINT/SIGALRM/SIGHUP removes its directories before
// dying; one killed by SIGKILL (a ctest timeout) leaves its directory, which
// the next ScratchDir in any process reaps once the owning pid is gone.
class ScratchDir {
 public:
  explicit ScratchDir(const std::string &prefix = "accel_test") {
    ReapStale();
    const auto base = std::filesystem::temp_directory_path();
    std::string templ = (base / (prefix + "_" + std::to_string(::getpid()) + "_XXXXXX")).string();
    if (::mkdtemp(templ.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed for " + templ);
    }
    path_ = templ;
    std::ofstream(path_ / kMarker).put('\n');
    Live().Add(path_.string());
  }
  ~ScratchDir() {
    Live().Remove(path_.string());
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  ScratchDir(const ScratchDir &) = delete;
  ScratchDir &operator=(const ScratchDir &) = delete;

  const std::filesystem::path &path() const { return path_; }
  std::string File(const std::string &name) const { return (path_ / name).string(); }

  // Removes `<prefix>_<pid>_<suffix>` directories (recognised by their marker
  // file) whose process no longer exists. Runs once per process.
  static void ReapStale() {
    static const bool done = [] {
      namespace fs = std::filesystem;
      std::error_code ec;
      for (fs::directory_iterator it(fs::temp_directory_path(), ec), end; !ec && it != end;
           it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::size_t tail = name.rfind('_');
        if (tail == std::string::npos || tail == 0) continue;
        const std::size_t head = name.rfind('_', tail - 1);
        if (head == std::string::npos) continue;
        const std::string pid_text = name.substr(head + 1, tail - head - 1);
        if (pid_text.empty() || pid_text.find_first_not_of("0123456789") != std::string::npos) continue;
        std::error_code probe;
        if (!fs::exists(it->path() / kMarker, probe)) continue;
        const long pid = std::stol(pid_text);
        if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) continue;
        std::error_code ignored;
        fs::remove_all(it->path(), ignored);
      }
      return true;
    }();
    (void)done;
  }

 private:
  static constexpr const char *kMarker = ".accel_scratch";

  // Directories of this process, for the signal handler.
  class LiveSet {
   public:
    void Add(const std::string &path) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!installed_) {
        for (const int signo : {SIGTERM, SIGINT, SIGALRM, SIGHUP}) std::signal(signo, &LiveSet::OnSignal);
        installed_ = true;
      }
      paths_.push_back(path);
    }
    void Remove(const std::string &path) {
      std::lock_guard<std::mutex> lock(mutex_);
      paths_.erase(std::remove(paths_.begin(), paths_.end(), path), paths_.end());
    }

   private:
    // Not async-signal-safe (no lock, remove_all allocates), but the process is
    // about to die and a leaked tmpfs copy is the worse outcome.
    static void OnSignal(int signo) {
      std::error_code ignored;
      for (const std::string &path : Live().paths_) std::filesystem::remove_all(path, ignored);
      std::signal(signo, SIG_DFL);
      ::raise(signo);
    }
    std::mutex mutex_;
    std::vector<std::string> paths_;
    bool installed_ = false;
  };
  static LiveSet &Live() {
    static LiveSet live;
    return live;
  }

  std::filesystem::path path_;
};

// A private, writable copy of `source` (plus its .wal, if any) inside `dir`;
// calling it again replaces the copy with a fresh one.
inline std::string PrivateDbCopy(const std::string &source, const ScratchDir &dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(source)) {
    throw std::runtime_error("missing source DB: " + source);
  }
  const fs::path dest = dir.path() / fs::path(source).filename();
  fs::copy_file(source, dest, fs::copy_options::overwrite_existing);
  // A WAL left by an earlier use of this copy would replay onto the new one.
  fs::remove(fs::path(dest.string() + ".wal"));
  const fs::path wal = fs::path(source + ".wal");
  if (fs::exists(wal)) {
    fs::copy_file(wal, fs::path(dest.string() + ".wal"), fs::copy_options::overwrite_existing);
  }
  return dest.string();
}

} // namespace accelerator_codegen::test_support
