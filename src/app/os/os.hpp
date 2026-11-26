#pragma once

#include <string>

struct OSInfo {
  std::string id;
  std::string idLike;
};

int getOS(OSInfo& os);
