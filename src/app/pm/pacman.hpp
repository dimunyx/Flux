#pragma once

#include "pm/pm.hpp"

namespace pm {

std::vector<Package> searchPacman(
    std::string_view query,
    const std::function<void(float)>& reportProgress = {});
void installPacman(std::string_view packageName);
void removePacman(std::string_view packageName);
void clearPacmanCache();

}  // namespace pm
