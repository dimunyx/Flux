#include <fstream>
#include <iostream>
#include <string>

#include "os.hpp"

int getOS(std::string& distro) {
  std::ifstream file("/etc/os-release");
  if (!file.is_open()) {
    std::cerr << "Failed to open /etc/os-release\n";
    return 1;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("ID=", 0) == 0) {
      std::string id = line.substr(3);
      if (id.size() >= 2 && id.front() == '"' && id.back() == '"') {
        id = id.substr(1, id.size() - 2);
      }
      distro = id;
      return 0;
    }
  }

  std::cerr << "ID not found in /etc/os-release\n";
  return 1;
}