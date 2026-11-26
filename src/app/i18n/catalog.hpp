#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace i18n {

class Catalog {
 public:
  static Catalog fromSystemLocale();
  static Catalog fromLanguage(std::string_view language);

  std::string text(std::string_view key) const;
  std::string format(
      std::string_view key,
      std::initializer_list<std::pair<std::string_view, std::string>> values) const;
  std::string_view language() const;

 private:
  std::string language_ = "en";
  std::unordered_map<std::string, std::string> messages_;
};

}  // namespace i18n
