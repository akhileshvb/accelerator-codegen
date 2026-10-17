// A killed test run must not leave its scratch directory behind:
// SIGTERM is cleaned by the dying process, SIGKILL by the next ScratchDir.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "eval/tests/test_support/db_paths.hpp"

namespace fs = std::filesystem;
namespace ts = accelerator_codegen::test_support;

namespace {

int failures = 0;

void Check(bool ok, const char *what) {
  std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
  failures += ok ? 0 : 1;
}

// Forks a child that creates a ScratchDir, reports its path and waits to be
// killed with `signo`. Returns the directory the child had.
std::string KillChildWith(int signo) {
  int fds[2];
  if (::pipe(fds) != 0) std::abort();
  const pid_t child = ::fork();
  if (child == 0) {
    ::close(fds[0]);
    ts::ScratchDir scratch("scratch_test");
    const std::string path = scratch.path().string() + "\n";
    if (::write(fds[1], path.data(), path.size()) < 0) std::_Exit(2);
    ::pause();
    std::_Exit(0);
  }
  ::close(fds[1]);
  char buffer[512] = {};
  const ssize_t got = ::read(fds[0], buffer, sizeof(buffer) - 1);
  ::close(fds[0]);
  std::string path(buffer, got > 0 ? static_cast<std::size_t>(got) : 0);
  while (!path.empty() && path.back() == '\n') path.pop_back();
  ::kill(child, signo);
  ::waitpid(child, nullptr, 0);
  return path;
}

} // namespace

int main() {
  std::string sandbox = (fs::temp_directory_path() / "scratch_dir_test_XXXXXX").string();
  if (::mkdtemp(sandbox.data()) == nullptr) return 2;
  ::setenv("TMPDIR", sandbox.c_str(), 1);

  const std::string terminated = KillChildWith(SIGTERM);
  Check(!terminated.empty() && !fs::exists(terminated), "SIGTERM removes the scratch dir");

  const std::string killed = KillChildWith(SIGKILL);
  Check(!killed.empty() && fs::exists(killed), "SIGKILL leaves the scratch dir");
  {
    ts::ScratchDir next("scratch_test");
    Check(!fs::exists(killed), "the next ScratchDir reaps the dir of a dead pid");
    Check(fs::exists(next.path()), "the new ScratchDir itself is kept");
  }

  fs::remove_all(sandbox);
  return failures == 0 ? 0 : 1;
}
