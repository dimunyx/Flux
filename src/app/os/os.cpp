#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include "os.hpp"

int getOS(OSInfo& os) {
  std::ifstream file("/etc/os-release");
  if (!file.is_open()) {
    std::cerr << "Failed to open /etc/os-release\n";
    return 1;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("ID=", 0) == 0 || line.rfind("ID_LIKE=", 0) == 0) {
      const bool isId = line.rfind("ID=", 0) == 0;
      std::string value = line.substr(isId ? 3 : 8);
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
      }
      (isId ? os.id : os.idLike) = std::move(value);
    }
  }

  if (os.id.empty()) {
    std::cerr << "ID not found in /etc/os-release\n";
    return 1;
  }
  return 0;
}
