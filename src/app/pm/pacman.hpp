#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pm {

struct Package {
  std::string name;
  std::string version;
  std::string description;
  bool installed = false;
};

std::vector<Package> searchPacman(
    std::string_view query,
    const std::function<void(float)>& reportProgress = {});
void installPacman(std::string_view packageName);
void removePacman(std::string_view packageName);
void clearPacmanCache();

}  // namespace pm
