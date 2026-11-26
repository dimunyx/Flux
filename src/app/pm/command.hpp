#pragma once

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pm::detail {

struct CommandResult {
  std::string output;
  int exitCode = 0;
};

inline CommandResult
runSuccessfulCommand(const std::vector<std::string> &arguments) {
  if (arguments.empty()) {
    throw std::invalid_argument("Cannot run an empty command");
  }
  if (access(arguments.front().c_str(), X_OK) == -1) {
    throw std::runtime_error("Required command is unavailable: " +
                             arguments.front());
  }
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto &argument : arguments) {
    argv.push_back(const_cast<char *>(argument.c_str()));
  }
  argv.push_back(nullptr);
  int outputPipe[2];
  if (pipe(outputPipe) == -1) {
    throw std::runtime_error("Failed to create a pipe for the package manager");
  }
  const pid_t child = fork();
  if (child == -1) {
    close(outputPipe[0]);
    close(outputPipe[1]);
    throw std::runtime_error("Failed to start the package manager process");
  }
  if (child == 0) {
    close(outputPipe[0]);
    if (dup2(outputPipe[1], STDOUT_FILENO) == -1 ||
        dup2(outputPipe[1], STDERR_FILENO) == -1) {
      _exit(126);
    }
    close(outputPipe[1]);
    const int nullInput = open("/dev/null", O_RDONLY);
    if (nullInput == -1 || dup2(nullInput, STDIN_FILENO) == -1) {
      _exit(126);
    }
    close(nullInput);
    execv(argv[0], argv.data());
    constexpr char errorMessage[] = "Failed to execute the requested command\n";
    static_cast<void>(
        write(STDERR_FILENO, errorMessage, sizeof(errorMessage) - 1));
    _exit(127);
  }
  close(outputPipe[1]);
  CommandResult result;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = read(outputPipe[0], buffer.data(), buffer.size());
    if (count > 0) {
      result.output.append(buffer.data(), static_cast<std::size_t>(count));
    } else if (count == 0) {
      break;
    } else if (errno != EINTR) {
      close(outputPipe[0]);
      int ignoredStatus = 0;
      static_cast<void>(waitpid(child, &ignoredStatus, 0));
      throw std::runtime_error("Failed to read package manager output");
    }
  }
  close(outputPipe[0]);
  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno != EINTR) {
      throw std::runtime_error(
          "Failed to wait for the package manager process");
    }
  }
  result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  if (result.exitCode != 0) {
    while (!result.output.empty() &&
           std::isspace(static_cast<unsigned char>(result.output.back()))) {
      result.output.pop_back();
    }
    if (result.output.empty()) {
      result.output =
          "process exited with status " + std::to_string(result.exitCode);
    }
    throw std::runtime_error(result.output);
  }
  return result;
}

inline bool validPackageName(std::string_view name) {
  if (name.empty() || name.front() == '-') {
    return false;
  }
  for (unsigned char character : name) {
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '@' ||
          character == '.' || character == '_' || character == '+' ||
          character == '-')) {
      return false;
    }
  }
  return true;
}

} // namespace pm::detail
