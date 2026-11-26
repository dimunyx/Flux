#pragma once

#include "pm/pm.hpp"

namespace pm {

std::vector<Package>
searchDnf(std::string_view query,
          const std::function<void(float)> &reportProgress = {});
std::vector<Package> listDnfUpdates(
    const std::function<void(float)> &reportProgress = {});
std::vector<Package> listDnfInstalled(
    const std::function<void(float)> &reportProgress = {});
void installDnf(std::string_view packageName);
void removeDnf(std::string_view packageName);
void clearDnfCache();
void updateDnfPackages();
void updateDnfPackage(std::string_view packageName);

} // namespace pm
