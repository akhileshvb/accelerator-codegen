// The canary runner. Each check runs in its own child process on a fresh engine, bounded in time:
// a crash, a sanitizer abort or a deadlock fails that check and nothing else. One JSON object per
// check goes to stdout; the exit status is 0 (all pass), 1 (a check failed), 2 (usage).
//
//   test_<engine>_canary [--list] [--check <id>]... [--owner <layer>]...
// Env: ACCEL_CANARY_TIMEOUT_SEC (per check, default 180).
#pragma once

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

#include "canary_checks.hpp"

namespace accelerator_codegen::canary {

namespace detail {

inline std::string Json(const std::string &s) {
  std::string out = "\"";
  for (const unsigned char ch : s) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch < 0x20) {
      out += ' ';
    } else {
      out += static_cast<char>(ch);
    }
  }
  return out + "\"";
}

struct Outcome {
  std::string status; // pass | fail
  std::string detail;
};

inline Outcome RunInChild(const Check &check, const EngineFactory &factory, int timeout_sec) {
  int fds[2];
  if (pipe(fds) != 0) {
    return {"fail", "pipe failed"};
  }
  std::cout.flush();
  const pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int rc = 0;
    std::string message;
    try {
      auto engine = factory();
      check.run(*engine);
    } catch (const std::exception &e) {
      message = e.what();
      rc = 1;
    }
    if (!message.empty() && write(fds[1], message.data(), message.size()) < 0) {
      rc = 1;
    }
    close(fds[1]);
    _exit(rc);
  }
  close(fds[1]);
  std::string message;
  bool timed_out = false;
  const auto deadline = Clock::now() + std::chrono::seconds(timeout_sec);
  int status = 0;
  while (true) {
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) {
      break;
    }
    if (Clock::now() >= deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      timed_out = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  char buffer[4096];
  ssize_t got;
  while ((got = read(fds[0], buffer, sizeof buffer)) > 0) {
    message.append(buffer, static_cast<std::size_t>(got));
  }
  close(fds[0]);
  if (timed_out) {
    return {"fail", "no result within " + std::to_string(timeout_sec) + "s: a deadlock or a wait with no bound"};
  }
  if (WIFSIGNALED(status)) {
    return {"fail", std::string("killed by signal ") + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")"};
  }
  if (WEXITSTATUS(status) != 0) {
    return {"fail", message.empty() ? "exited " + std::to_string(WEXITSTATUS(status)) + " (a sanitizer report or abort; see the log)" : message};
  }
  return {"pass", ""};
}

} // namespace detail

inline int Main(const EngineFactory &factory, int argc, char **argv) {
  std::set<std::string> only, owners;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--list") {
      list = true;
    } else if (arg == "--check" && i + 1 < argc) {
      only.insert(argv[++i]);
    } else if (arg == "--owner" && i + 1 < argc) {
      owners.insert(argv[++i]);
    } else {
      std::cerr << "usage: canary [--list] [--check <id>]... [--owner <layer>]...\n";
      return 2;
    }
  }
  const char *timeout_env = std::getenv("ACCEL_CANARY_TIMEOUT_SEC");
  const int timeout = timeout_env && *timeout_env ? std::atoi(timeout_env) : 180;
  bool failed = false;
  for (const auto &check : AllChecks()) {
    if ((!only.empty() && !only.count(check.id)) || (!owners.empty() && !owners.count(check.owner))) {
      continue;
    }
    if (list) {
      std::cout << check.id << "\n";
      continue;
    }
    const auto started = Clock::now();
    const auto outcome = detail::RunInChild(check, factory, timeout);
    const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
    failed |= outcome.status != "pass";
    std::cout << "{\"check\": " << detail::Json(check.id) << ", \"owner_layer\": " << detail::Json(check.owner)
              << ", \"status\": " << detail::Json(outcome.status) << ", \"seconds\": " << seconds
              << ", \"defect_class\": " << detail::Json(check.defect) << ", \"detail\": " << detail::Json(outcome.detail)
              << "}" << std::endl;
  }
  return failed ? 1 : 0;
}

} // namespace accelerator_codegen::canary
