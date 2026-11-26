#include "pm/dnf.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pm/command.hpp"

namespace pm {

namespace {

std::unordered_set<std::string> installedPackages() {
  const detail::CommandResult result = detail::runSuccessfulCommand(
      {"/usr/bin/rpm", "-qa", "--qf", "%{NAME}\\n"});
  std::istringstream lines(result.output);
  std::unordered_set<std::string> names;
  std::string name;
  while (lines >> name) {
    names.insert(name);
  }
  return names;
}

} // namespace

std::vector<Package>
searchDnf(std::string_view query,
          const std::function<void(float)> &reportProgress) {
  if (query.empty()) {
    return {};
  }
  std::string packageSpec = "*" + std::string(query) + "*";
  const detail::CommandResult result = detail::runSuccessfulCommand(
      {"/usr/bin/dnf", "repoquery", "--available", "--latest-limit=1",
       "--queryformat=%{name}|%{evr}|%{summary}", "--", packageSpec});
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
    const std::size_t first = line.find('|');
    const std::size_t second = first == std::string::npos
                                   ? std::string::npos
                                   : line.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
      continue;
    }
    Package package;
    package.name = line.substr(0, first);
    package.version = line.substr(first + 1, second - first - 1);
    package.description = line.substr(second + 1);
    if (package.name.empty()) {
      continue;
    }
    package.installed = installed.contains(package.name);
    packages.push_back(std::move(package));
  }
  return packages;
}

std::vector<Package> listDnfUpdates(
    const std::function<void(float)> &reportProgress) {
  const detail::CommandResult result = detail::runSuccessfulCommand(
      {"/usr/bin/dnf", "repoquery", "--upgrades", "--latest-limit=1",
       "--queryformat=%{name}|%{evr}|%{summary}"});
  std::vector<Package> packages;
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t first = line.find('|');
    const std::size_t second = first == std::string::npos
                                   ? std::string::npos
                                   : line.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    Package package;
    package.name = line.substr(0, first);
    package.version = line.substr(first + 1, second - first - 1);
    package.description = line.substr(second + 1);
    package.installed = true;
    if (!package.name.empty()) packages.push_back(std::move(package));
  }
  if (reportProgress) reportProgress(1.0F);
  return packages;
}

std::vector<Package> listDnfInstalled(
    const std::function<void(float)> &reportProgress) {
  const detail::CommandResult result = detail::runSuccessfulCommand(
      {"/usr/bin/rpm", "-qa", "--qf", "%{NAME}|%{EVR}|%{SUMMARY}\n"});
  if (reportProgress) reportProgress(0.6F);
  std::vector<Package> packages;
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t first = line.find('|');
    const std::size_t second = first == std::string::npos
                                   ? std::string::npos
                                   : line.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    Package package;
    package.name = line.substr(0, first);
    package.version = line.substr(first + 1, second - first - 1);
    package.description = line.substr(second + 1);
    package.installed = true;
    if (!package.name.empty()) packages.push_back(std::move(package));
  }
  std::sort(packages.begin(), packages.end(),
            [](const Package &left, const Package &right) {
              return left.name < right.name;
            });
  if (reportProgress) reportProgress(1.0F);
  return packages;
}

void installDnf(std::string_view packageName) {
  if (!detail::validPackageName(packageName)) {
    throw std::invalid_argument("Invalid package name");
  }
  static_cast<void>(detail::runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/dnf", "install", "-y", "--",
       std::string(packageName)}));
}

void removeDnf(std::string_view packageName) {
  if (!detail::validPackageName(packageName)) {
    throw std::invalid_argument("Invalid package name");
  }
  static_cast<void>(
      detail::runSuccessfulCommand({"/usr/bin/pkexec", "/usr/bin/dnf", "remove",
                                    "-y", "--", std::string(packageName)}));
}

void clearDnfCache() {
  static_cast<void>(detail::runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/dnf", "clean", "all"}));
}

void updateDnfPackages() {
  static_cast<void>(detail::runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/dnf", "upgrade", "-y"}));
}

void updateDnfPackage(std::string_view packageName) {
  if (!detail::validPackageName(packageName)) {
    throw std::invalid_argument("Invalid package name");
  }
  static_cast<void>(detail::runSuccessfulCommand(
      {"/usr/bin/pkexec", "/usr/bin/dnf", "upgrade", "-y", "--",
       std::string(packageName)}));
}

} // namespace pm
