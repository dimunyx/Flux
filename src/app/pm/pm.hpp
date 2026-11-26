#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pm {

enum class Manager { none, pacman, dnf };

struct Package {
  std::string name;
  std::string version;
  std::string description;
  bool installed = false;
};

Manager getPM(std::string_view distro, std::string_view idLike = {});
std::vector<Package> searchPackages(
    Manager manager, std::string_view query,
    const std::function<void(float)>& reportProgress = {});
void installPackage(Manager manager, std::string_view packageName);
void removePackage(Manager manager, std::string_view packageName);
void clearPackageCache(Manager manager);

}  // namespace pm
