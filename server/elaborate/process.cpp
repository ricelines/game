#include "private.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;
namespace amber::elaborate::process {
Outcome run(const std::filesystem::path &executable, std::span<const std::string> arguments,
            std::chrono::steady_clock::time_point deadline, std::size_t diagnostic_bytes,
            bool own_process_group) {
  Outcome result;
  if (std::chrono::steady_clock::now() >= deadline) {
    result.timed_out = true;
    result.error = "fixture timed out";
    return result;
  }
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK)) {
    result.error = std::strerror(errno);
    return result;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  if (own_process_group) {
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
  }
  std::vector<std::string> words{executable.string()};
  words.insert(words.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(words.size() + 1);
  for (auto &word : words)
    argv.push_back(word.data());
  argv.push_back(nullptr);
  pid_t child = 0;
  const auto spawned =
      posix_spawn(&child, executable.c_str(), &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(pipefd[1]);
  if (spawned) {
    close(pipefd[0]);
    result.unavailable = true;
    result.error = std::string("fixture executable unavailable: ") + std::strerror(spawned);
    return result;
  }
  auto drain = [&] {
    std::array<char, 4096> buffer;
    ssize_t count = 0;
    for (unsigned reads = 0;
         reads < 64 && (count = read(pipefd[0], buffer.data(), buffer.size())) > 0; ++reads)
      if (result.diagnostics.size() < diagnostic_bytes)
        result.diagnostics.append(buffer.data(),
                                  std::min(static_cast<std::size_t>(count),
                                           diagnostic_bytes - result.diagnostics.size()));
  };
  auto terminate = [&] {
    kill(own_process_group ? -child : child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  };
  int status = 0;
  for (;;) {
    drain();
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
      break;
    }
    if (waited < 0 && errno != EINTR) {
      terminate();
      result.error = "could not reap fixture process";
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      terminate();
      result.error = "fixture timed out";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  drain();
  close(pipefd[0]);
  return result;
}
} // namespace amber::elaborate::process
