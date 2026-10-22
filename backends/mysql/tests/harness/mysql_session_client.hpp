#pragma once

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "backends/mysql/tests/harness/mysql_tpch_harness.hpp"

namespace accel::mysql::tpch_harness {

// One persistent client connection. Commands are written to the client's stdin;
// Exec() returns its output once the marker statement sent after it is echoed.
class Conn {
public:
  explicit Conn(const Tools &tools) {
    char path[] = "/tmp/accel_sess_XXXXXX";
    const int fd = mkstemp(path);
    Require(fd >= 0, "mkstemp failed");
    out_path_ = path;
    int pipefd[2];
    Require(pipe2(pipefd, O_CLOEXEC) == 0, "pipe failed"); // CLOEXEC: sibling clients must not inherit the write end
    pid_ = fork();
    Require(pid_ >= 0, "fork failed");
    if (pid_ == 0) {
      dup2(pipefd[0], 0);
      dup2(fd, 1);
      dup2(fd, 2);
      close(pipefd[0]);
      close(pipefd[1]);
      close(fd);
      const std::string cmd =
          "exec env " + MysqlPrefix(tools) + " --force --unbuffered 2>&1";
      execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
      _exit(127);
    }
    close(pipefd[0]);
    close(fd);
    in_ = pipefd[1];
  }
  ~Conn() {
    Close();
    ::unlink(out_path_.c_str());
  }

  // Runs `sql` (one or more statements); returns the output it produced.
  std::string Exec(const std::string &sql) {
    const std::string marker = "MARK" + std::to_string(++seq_) + "_" + std::to_string(pid_);
    write_all(sql + ";\nSELECT '" + marker + "';\n");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
      std::string all = ReadAll();
      const auto pos = all.find(marker);
      if (pos != std::string::npos) {
        std::string mine = all.substr(consumed_, pos - consumed_);
        consumed_ = all.find('\n', pos) == std::string::npos ? all.size() : all.find('\n', pos) + 1;
        return mine;
      }
      Require(std::chrono::steady_clock::now() < deadline, "timeout waiting for: " + sql);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  // Exec() that must not report a SQL error.
  void Ok(const std::string &sql) {
    const std::string out = Exec(sql);
    Require(out.find("ERROR") == std::string::npos, "SQL error for [" + sql + "]: " + out);
  }
  // Drops the connection without COMMIT/ROLLBACK (the server rolls back).
  void Kill() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
    if (in_ >= 0) {
      close(in_);
      in_ = -1;
    }
  }
  void Close() {
    if (in_ >= 0) {
      close(in_);
      in_ = -1;
    }
    if (pid_ > 0) {
      waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
  }

private:
  void write_all(const std::string &text) {
    std::size_t off = 0;
    while (off < text.size()) {
      const ssize_t n = ::write(in_, text.data() + off, text.size() - off);
      Require(n > 0, "write to client failed");
      off += static_cast<std::size_t>(n);
    }
  }
  std::string ReadAll() {
    std::ifstream in(out_path_);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
  pid_t pid_ = -1;
  int in_ = -1;
  std::string out_path_;
  int seq_ = 0;
  std::size_t consumed_ = 0;
};

} // namespace accel::mysql::tpch_harness
