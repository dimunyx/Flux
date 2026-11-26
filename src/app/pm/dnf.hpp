#pragma once

#include "pm/pm.hpp"

namespace pm {

std::vector<Package>
searchDnf(std::string_view query,
          const std::function<void(float)> &reportProgress = {});
void installDnf(std::string_view packageName);
void removeDnf(std::string_view packageName);
void clearDnfCache();

} // namespace pm
