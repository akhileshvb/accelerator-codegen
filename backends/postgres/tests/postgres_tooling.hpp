#pragma once

// Resolve PostgreSQL tooling for contract / differential harnesses.
//
// Docker pipeline (ACCEL_PG_DOCKER=1): connect to orchestrated server via
// PGHOST/PGPORT — no host initdb / temp cluster.
// Local/SDK mode: orchestrator-injected PG_CONFIG with complete server SDK.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

namespace accel::postgres {
namespace fs = std::filesystem;

struct Tools {
  bool ok = false;
  bool docker_mode = false;
  std::string error;
  fs::path pg_config;
  fs::path bindir;
  fs::path initdb;
  fs::path postgres;
  fs::path psql;
  fs::path includedir_server;
  std::string version;
  std::string host = "127.0.0.1";
  int port = 5432;
  std::string user = "postgres";
  std::string password = "postgres";
  std::string database = "postgres";
  std::string extension_so = "/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so";
};

namespace detail {

inline bool IsExe(const fs::path &p) {
  return ::access(p.c_str(), X_OK) == 0;
}

inline std::string EnvOr(const char *key, const char *fallback) {
  const char *v = std::getenv(key);
  if (v && *v) {
    return v;
  }
  return fallback;
}

inline bool EnvTruthy(const char *key) {
  const char *v = std::getenv(key);
  if (!v || !*v) {
    return false;
  }
  if (v[0] == '0' && v[1] == '\0') {
    return false;
  }
  if ((v[0] == 'f' || v[0] == 'F') && (v[1] == 'a' || v[1] == 'A')) {
    return false;
  }
  return true;
}

inline std::string RunPgConfig(const fs::path &pg_config, const char *arg) {
  std::string cmd = "'";
  cmd += pg_config.string();
  cmd += "' ";
  cmd += arg;
  cmd += " 2>/dev/null";
  FILE *fp = popen(cmd.c_str(), "r");
  if (!fp) {
    return {};
  }
  char buf[4096];
  std::string out;
  while (fgets(buf, sizeof(buf), fp)) {
    out += buf;
  }
  pclose(fp);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

inline bool EnvPath(const char *key, fs::path *out) {
  const char *v = std::getenv(key);
  if (!v || !*v) {
    return false;
  }
  *out = fs::path(v);
  return true;
}

inline fs::path Which(const char *name) {
  const char *path = std::getenv("PATH");
  if (!path) {
    return {};
  }
  std::string path_copy(path);
  size_t start = 0;
  while (start < path_copy.size()) {
    size_t end = path_copy.find(':', start);
    if (end == std::string::npos) {
      end = path_copy.size();
    }
    fs::path cand = fs::path(path_copy.substr(start, end - start)) / name;
    if (IsExe(cand)) {
      return cand;
    }
    start = end + 1;
  }
  return {};
}

inline Tools TryPgConfig(const fs::path &cfg) {
  Tools t;
  if (!IsExe(cfg)) {
    t.error = "pg_config not executable";
    return t;
  }
  const std::string inc = RunPgConfig(cfg, "--includedir-server");
  const std::string bindir = RunPgConfig(cfg, "--bindir");
  if (inc.empty() || bindir.empty()) {
    t.error = "pg_config missing --includedir-server/--bindir";
    return t;
  }
  const fs::path server_inc(inc);
  const fs::path bin(bindir);
  if (!fs::exists(server_inc / "postgres.h")) {
    t.error = "postgres.h missing under includedir-server (client-only install?)";
    return t;
  }
  const fs::path initdb = bin / "initdb";
  const fs::path postgres = bin / "postgres";
  if (!IsExe(initdb) || !IsExe(postgres)) {
    t.error = "initdb/postgres missing in bindir";
    return t;
  }
  t.ok = true;
  t.docker_mode = false;
  t.pg_config = cfg;
  t.bindir = bin;
  t.initdb = initdb;
  t.postgres = postgres;
  t.psql = bin / "psql";
  if (!IsExe(t.psql)) {
    t.psql = Which("psql");
  }
  t.includedir_server = server_inc;
  t.version = RunPgConfig(cfg, "--version");
  return t;
}

inline bool PsqlOk(const Tools &t) {
  if (!IsExe(t.psql)) {
    return false;
  }
  std::string cmd = "PGPASSWORD='";
  cmd += t.password;
  cmd += "' '";
  cmd += t.psql.string();
  cmd += "' -h '";
  cmd += t.host;
  cmd += "' -p ";
  cmd += std::to_string(t.port);
  cmd += " -U '";
  cmd += t.user;
  cmd += "' -d '";
  cmd += t.database;
  cmd += "' -tAc 'select 1' >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

inline Tools DiscoverDocker() {
  Tools t;
  t.docker_mode = true;
  t.host = EnvOr("PGHOST", EnvOr("PG_HOST", "127.0.0.1").c_str());
  // Inside container the server is on 5432; from host typically 5434.
  t.port = std::atoi(EnvOr("PGPORT", EnvOr("PG_PORT", "5432").c_str()).c_str());
  if (t.port <= 0) {
    t.port = 5432;
  }
  t.user = EnvOr("PGUSER", "postgres");
  t.password = EnvOr("PGPASSWORD", "postgres");
  t.database = EnvOr("PGDATABASE", "postgres");
  t.extension_so =
      EnvOr("ACCEL_POSTGRES_SO", "/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so");
  t.psql = Which("psql");
  if (!IsExe(t.psql)) {
    // Common in-container location.
    if (IsExe("/usr/bin/psql")) {
      t.psql = "/usr/bin/psql";
    }
  }
  if (!IsExe(t.psql)) {
    t.error = "ACCEL_PG_DOCKER=1 but psql not found on PATH";
    return t;
  }
  if (!PsqlOk(t)) {
    t.error =
        "ACCEL_PG_DOCKER=1 but cannot connect to Postgres at " + t.host + ":" +
        std::to_string(t.port) +
        " (run scripts/ensure_accel_pg_test.sh; fail closed — do not SKIP)";
    return t;
  }
  // Prefer in-container complete SDK when present (for optional local initdb).
  fs::path cfg = Which("pg_config");
  if (IsExe(cfg)) {
    Tools local = TryPgConfig(cfg);
    if (local.ok) {
      t.bindir = local.bindir;
      t.initdb = local.initdb;
      t.postgres = local.postgres;
      t.pg_config = local.pg_config;
      t.includedir_server = local.includedir_server;
      t.version = local.version.empty() ? "docker" : local.version;
    } else {
      t.version = "docker";
    }
  } else {
    t.version = "docker";
  }
  t.ok = true;
  return t;
}

} // namespace detail

inline Tools DiscoverTools() {
  using detail::EnvPath;
  using detail::TryPgConfig;

  if (detail::EnvTruthy("ACCEL_PG_DOCKER")) {
    return detail::DiscoverDocker();
  }

  std::vector<fs::path> candidates;
  fs::path tmp;
  if (EnvPath("PG_CONFIG", &tmp)) {
    candidates.push_back(tmp);
  }
  if (EnvPath("ACCEL_PG_CONFIG", &tmp)) {
    candidates.push_back(tmp);
  }
  if (EnvPath("ACCEL_PG_BINDIR", &tmp)) {
    candidates.push_back(tmp / "pg_config");
  }
  if (EnvPath("ACCEL_PG_ROOT", &tmp)) {
    candidates.push_back(tmp / "bin" / "pg_config");
  }

  Tools last_err;
  last_err.error =
      "PostgreSQL not available. For the Docker pipeline set ACCEL_PG_DOCKER=1 "
      "(scripts/ensure_accel_pg_test.sh). Or provide PG_CONFIG with a complete "
      "server SDK (postgres.h + initdb/postgres).";
  for (const auto &cand : candidates) {
    Tools t = TryPgConfig(cand);
    if (t.ok) {
      return t;
    }
    last_err = t;
  }
  return last_err;
}

} // namespace accel::postgres
