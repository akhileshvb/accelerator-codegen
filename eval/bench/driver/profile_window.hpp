// SCAFFOLDING: the profiler's sampling window around a bench's measured read loop.
// ACCEL_PROFILE_CONTROL is a comma-separated list of `perf --control` command fifos; the profiler
// (run_profile) creates them and starts its counters and samplers disabled. A bench that does not set the
// variable pays one getenv.
#pragma once

#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace accelerator_codegen::benchmarks::driver {

class ProfileWindow {
public:
  // Opens the window on construction and closes it on destruction.
  ProfileWindow() : fds_(OpenControls()) { Send("enable\n"); }
  ~ProfileWindow() {
    Send("disable\n");
    for (const int fd : fds_) {
      close(fd);
    }
  }
  ProfileWindow(const ProfileWindow &) = delete;
  ProfileWindow &operator=(const ProfileWindow &) = delete;

private:
  static std::vector<int> OpenControls() {
    std::vector<int> fds;
    const char *list = std::getenv("ACCEL_PROFILE_CONTROL");
    if (list == nullptr) {
      return fds;
    }
    std::stringstream stream(list);
    std::string path;
    while (std::getline(stream, path, ',')) {
      const int fd = open(path.c_str(), O_WRONLY | O_NONBLOCK);
      if (fd >= 0) {
        fds.push_back(fd);
      }
    }
    return fds;
  }

  void Send(const char *command) const {
    for (const int fd : fds_) {
      const std::string text(command);
      if (write(fd, text.data(), text.size()) < 0) {
        continue;
      }
    }
  }

  std::vector<int> fds_;
};

} // namespace accelerator_codegen::benchmarks::driver
