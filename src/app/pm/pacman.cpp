#include "pm/pacman.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pm {

namespace {

struct CommandResult {
  std::string output;
  int exitCode = 0;
};

CommandResult runCommand(const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    throw std::invalid_argument("Cannot run an empty command");
  }
  if (access(arguments.front().c_str(), X_OK) == -1) {
    throw std::runtime_error("Required command is unavailable: " + arguments.front());
  }

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
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
    static_cast<void>(write(STDERR_FILENO, errorMessage, sizeof(errorMessage) - 1));
    _exit(127);
  }

  close(outputPipe[1]);
  CommandResult result;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = read(outputPipe[0], buffer.data(), buffer.size());
    if (count > 0) {
      result.output.append(buffer.data(), static_cast<size_t>(count));
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
      throw std::runtime_error("Failed to wait for the package manager process");
    }
  }

  if (WIFEXITED(status)) {
    result.exitCode = WEXITSTATUS(status);
  } else {
    result.exitCode = 128;
  }
  return result;
}

CommandResult runSuccessfulCommand(const std::vector<std::string>& arguments) {
  CommandResult result = runCommand(arguments);
  if (result.exitCode != 0) {
    std::string details = result.output;
    while (!details.empty() && std::isspace(static_cast<unsigned char>(details.back()))) {
      details.pop_back();
    }
    if (details.empty()) {
      details = "process exited with status " + std::to_string(result.exitCode);
    }
    throw std::runtime_error(details);
  }
  return result;
}

bool validPackageName(std::string_view name) {
  if (name.empty() || name.front() == '-') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '@' ||
           character == '.' || character == '_' || character == '+' || character == '-';
  });
}

std::unordered_set<std::string> installedPackages() {
  const CommandResult result = runSuccessfulCommand({"/usr/bin/pacman", "-Q"});
  std::istringstream lines(result.output);
  std::unordered_set<std::string> names;
  std::string name;
  std::string version;
  while (lines >> name >> version) {
    names.insert(name);
  }
  return names;
}

}  // namespace

std::vector<Package> searchPacman(
    std::string_view query,
    const std::function<void(float)>& reportProgress) {
  if (query.empty()) {
    return {};
  }

  const CommandResult result =
      runSuccessfulCommand({"/usr/bin/pacman", "-Ss", "--", std::string(query)});
  if (reportProgress) {
    reportProgress(0.55F);
  }
  const std::unordered_set<std::string> installed = installedPackages();
  if (reportProgress) {
    reportProgress(0.9F);
  }

  std::vector<Package> packages;
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) {
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(line.front()))) {
      std::istringstream packageLine(line);
      std::string repositoryAndName;
      Package package;
      if (!(packageLine >> repositoryAndName >> package.version)) {
        continue;
      }
      const size_t separator = repositoryAndName.find('/');
      if (separator == std::string::npos || separator + 1 >= repositoryAndName.size()) {
        continue;
      }
      package.name = repositoryAndName.substr(separator + 1);
      package.installed = installed.contains(package.name);
      packages.push_back(std::move(package));
    } else if (!packages.empty()) {
      const size_t first = line.find_first_not_of(" \t");
      if (first != std::string::npos) {
        packages.back().description = line.substr(first);
      }
    }
  }
  return packages;
}

std::vector<Package> listPacmanUpdates(
    const std::function<void(float)>& reportProgress) {
  const CommandResult result = runSuccessfulCommand({"/usr/bin/pacman", "-Qu"});
  std::vector<Package> packages;
  std::istringstream lines(result.output);
  std::string name;
  std::string installedVersion;
  std::string arrow;
  std::string availableVersion;
  while (lines >> name >> installedVersion >> arrow >> availableVersion) {
    if (arrow != "->") continue;
    Package package;
    package.name = std::move(name);
    package.version = installedVersion + " → " + availableVersion;
    package.installed = true;
    packages.push_back(std::move(package));
  }
  if (reportProgress) reportProgress(1.0F);
  return packages;
}

std::vector<Package> listPacmanInstalled(
    const std::function<void(float)>& reportProgress) {
  const CommandResult result = runSuccessfulCommand({"/usr/bin/pacman", "-Q"});
  if (reportProgress) reportProgress(0.6F);
  std::vector<Package> packages;
  std::istringstream lines(result.output);
  std::string name;
  std::string version;
  while (lines >> name >> version) {
    Package package;
    package.name = std::move(name);
    package.version = std::move(version);
    package.installed = true;
    packages.push_back(std::move(package));
  }
  std::sort(packages.begin(), packages.end(),
            [](const Package& left, const Package& right) {
              return left.name < right.name;
            });
  if (reportProgress) reportProgress(1.0F);
  return packages;
}

void installPacman(std::string_view packageName) {
  if (!validPackageName(packageName)) {
    throw std::invalid_argument("Invalid package name");
  }
  static_cast<void>(runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/pacman", "--sync", "--noconfirm", "--needed", "--",
       std::string(packageName)}));
}

void removePacman(std::string_view packageName) {
  if (!validPackageName(packageName)) {
    throw std::invalid_argument("Invalid package name");
  }
  static_cast<void>(runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/pacman", "--remove", "--noconfirm", "--",
       std::string(packageName)}));
}

void clearPacmanCache() {
  static_cast<void>(runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/find", "/var/cache/pacman/pkg", "-mindepth", "1",
       "-maxdepth", "1", "-exec", "/usr/bin/rm", "-rf", "--", "{}", "+"}));
}

void updatePacmanPackages() {
  static_cast<void>(runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/pacman", "--sync", "--refresh", "--sysupgrade",
       "--noconfirm"}));
}

void updatePacmanPackage(std::string_view packageName) {
  if (!validPackageName(packageName)) {
    throw std::invalid_argument("Invalid package name");
  }
  static_cast<void>(runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/pacman", "--sync", "--noconfirm",
       "--needed", "--", std::string(packageName)}));
}

}  // namespace pm
