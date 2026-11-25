#include <iostream>
#include <string>

#include "pm.hpp"

int getPM(const std::string& distro) {
  if (distro == "arch") {
    return 0;
  }
  if (distro == "gentoo") {
    return 0;
  }

  std::cerr << "Unsupported distribution: " << distro << '\n';
  return 1;
}