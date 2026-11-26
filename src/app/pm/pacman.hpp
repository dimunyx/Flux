#pragma once

#include "pm/pm.hpp"

namespace pm {

std::vector<Package> searchPacman(
    std::string_view query,
    const std::function<void(float)>& reportProgress = {});
std::vector<Package> listPacmanUpdates(
    const std::function<void(float)>& reportProgress = {});
std::vector<Package> listPacmanInstalled(
    const std::function<void(float)>& reportProgress = {});
void installPacman(std::string_view packageName);
void removePacman(std::string_view packageName);
void clearPacmanCache();
void updatePacmanPackages();
void updatePacmanPackage(std::string_view packageName);

}  // namespace pm
