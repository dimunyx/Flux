#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "pm/dnf.hpp"
#include "pm/pacman.hpp"
#include "pm.hpp"

namespace pm {

Manager getPM(std::string_view distro, std::string_view idLike) {
  if (distro == "arch") {
    return Manager::pacman;
  }
  std::unordered_set<std::string> family;
  std::istringstream ids{std::string(idLike)};
  std::string id;
  while (ids >> id) {
    family.insert(id);
  }
  constexpr std::string_view dnfDistros[] = {
      "fedora", "rhel", "centos", "rocky", "almalinux", "ol", "amzn",
      "nobara", "bazzite"};
  for (const std::string_view supported : dnfDistros) {
    if (distro == supported || family.contains(std::string(supported))) {
      return Manager::dnf;
    }
  }
  return Manager::none;
}

std::vector<Package> searchPackages(
    Manager manager, std::string_view query,
    const std::function<void(float)>& reportProgress) {
  switch (manager) {
    case Manager::pacman:
      return searchPacman(query, reportProgress);
    case Manager::dnf:
      return searchDnf(query, reportProgress);
    case Manager::none:
      break;
  }
  throw std::invalid_argument("No supported package manager is selected");
}

void installPackage(Manager manager, std::string_view packageName) {
  switch (manager) {
    case Manager::pacman:
      installPacman(packageName);
      return;
    case Manager::dnf:
      installDnf(packageName);
      return;
    case Manager::none:
      break;
  }
  throw std::invalid_argument("No supported package manager is selected");
}

void removePackage(Manager manager, std::string_view packageName) {
  switch (manager) {
    case Manager::pacman:
      removePacman(packageName);
      return;
    case Manager::dnf:
      removeDnf(packageName);
      return;
    case Manager::none:
      break;
  }
  throw std::invalid_argument("No supported package manager is selected");
}

void clearPackageCache(Manager manager) {
  switch (manager) {
    case Manager::pacman:
      clearPacmanCache();
      return;
    case Manager::dnf:
      clearDnfCache();
      return;
    case Manager::none:
      break;
  }
  throw std::invalid_argument("No supported package manager is selected");
}

}  // namespace pm
