#include <string>

#include "pm.hpp"

int getPM(const std::string& distro) {
  if (distro == "arch" || distro == "gentoo") {
    return 0;
  }
  return 1;
}
