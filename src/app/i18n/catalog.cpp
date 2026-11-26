#include "i18n/catalog.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace i18n {
namespace {

class JsonObjectParser {
 public:
  explicit JsonObjectParser(std::string_view source) : source_(source) {}

  std::unordered_map<std::string, std::string> parse() {
    std::unordered_map<std::string, std::string> result;
    skipWhitespace();
    expect('{');
    skipWhitespace();
    if (consume('}')) {
      return result;
    }

    while (true) {
      skipWhitespace();
      std::string key = parseString();
      skipWhitespace();
      expect(':');
      skipWhitespace();
      std::string value = parseString();
      if (!result.emplace(std::move(key), std::move(value)).second) {
        throw std::runtime_error("Duplicate JSON translation key");
      }
      skipWhitespace();
      if (consume('}')) {
        break;
      }
      expect(',');
    }
    skipWhitespace();
    if (position_ != source_.size()) {
      throw std::runtime_error("Unexpected data after JSON translation object");
    }
    return result;
  }

 private:
  std::string_view source_;
  std::size_t position_ = 0;

  void skipWhitespace() {
    while (position_ < source_.size() &&
           std::isspace(static_cast<unsigned char>(source_[position_])) != 0) {
      ++position_;
    }
  }

  bool consume(char expected) {
    if (position_ < source_.size() && source_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  void expect(char expected) {
    if (!consume(expected)) {
      throw std::runtime_error("Invalid JSON translation file");
    }
  }

  unsigned int parseHexQuad() {
    if (position_ + 4 > source_.size()) {
      throw std::runtime_error("Invalid Unicode escape in translation file");
    }
    unsigned int value = 0;
    for (int digit = 0; digit < 4; ++digit) {
      const char character = source_[position_++];
      value <<= 4U;
      if (character >= '0' && character <= '9') {
        value |= static_cast<unsigned int>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        value |= static_cast<unsigned int>(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        value |= static_cast<unsigned int>(character - 'A' + 10);
      } else {
        throw std::runtime_error("Invalid Unicode escape in translation file");
      }
    }
    return value;
  }

  static void appendCodepoint(std::string& output, unsigned int value) {
    if (value <= 0x7FU) {
      output.push_back(static_cast<char>(value));
    } else if (value <= 0x7FFU) {
      output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
      output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value <= 0xFFFFU) {
      output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value <= 0x10FFFFU) {
      output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
      output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
      throw std::runtime_error("Unicode escape is outside the valid range");
    }
  }

  std::string parseString() {
    expect('"');
    std::string result;
    while (position_ < source_.size()) {
      const char character = source_[position_++];
      if (character == '"') {
        return result;
      }
      if (character != '\\') {
        if (static_cast<unsigned char>(character) < 0x20U) {
          throw std::runtime_error("Control character in JSON translation string");
        }
        result.push_back(character);
        continue;
      }
      if (position_ >= source_.size()) {
        break;
      }
      switch (source_[position_++]) {
        case '"':
          result.push_back('"');
          break;
        case '\\':
          result.push_back('\\');
          break;
        case '/':
          result.push_back('/');
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u': {
          unsigned int codepoint = parseHexQuad();
          if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
            if (position_ + 2 > source_.size() || source_[position_] != '\\' ||
                source_[position_ + 1] != 'u') {
              throw std::runtime_error("Invalid Unicode surrogate pair");
            }
            position_ += 2;
            const unsigned int low = parseHexQuad();
            if (low < 0xDC00U || low > 0xDFFFU) {
              throw std::runtime_error("Invalid Unicode surrogate pair");
            }
            codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                        (low - 0xDC00U);
          } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
            throw std::runtime_error("Invalid Unicode surrogate pair");
          }
          appendCodepoint(result, codepoint);
          break;
        }
        default:
          throw std::runtime_error("Invalid escape in JSON translation string");
      }
    }
    throw std::runtime_error("Unterminated JSON translation string");
  }
};

std::string detectLanguage() {
  if (const char* overrideLanguage = std::getenv("FLUX_LANG");
      overrideLanguage != nullptr && overrideLanguage[0] != '\0') {
    const std::string_view locale(overrideLanguage);
    if (locale.starts_with("ru") || locale.starts_with("RU")) {
      return "ru";
    }
    if (locale.starts_with("ro") || locale.starts_with("RO")) {
      return "ro";
    }
    return "en";
  }
  constexpr const char* localeVariables[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
  for (const char* variable : localeVariables) {
    const char* value = std::getenv(variable);
    if (value == nullptr || value[0] == '\0') {
      continue;
    }
    const std::string_view locale(value);
    if (locale.starts_with("ru") || locale.starts_with("RU")) {
      return "ru";
    }
    if (locale.starts_with("ro") || locale.starts_with("RO")) {
      return "ro";
    }
    return "en";
  }
  return "en";
}

bool loadMessages(std::string_view language,
                  std::unordered_map<std::string, std::string>& messages) {
  const std::filesystem::path sourcePath =
      std::filesystem::path(FLUX_ASSET_DIR) / "locales" /
      (std::string(language) + ".json");
  const std::filesystem::path installPath =
      std::filesystem::path(FLUX_INSTALL_ASSET_DIR) / "locales" /
      (std::string(language) + ".json");
  const std::filesystem::path path =
      std::filesystem::exists(sourcePath) ? sourcePath : installPath;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  messages = JsonObjectParser(contents).parse();
  return !messages.empty();
}

}  // namespace

Catalog Catalog::fromSystemLocale() {
  return fromLanguage(detectLanguage());
}

Catalog Catalog::fromLanguage(std::string_view language) {
  Catalog catalog;
  if (language == "ru" || language == "ro" || language == "en") {
    catalog.language_ = language;
  } else {
    catalog.language_ = detectLanguage();
  }
  try {
    if (loadMessages(catalog.language_, catalog.messages_)) {
      return catalog;
    }
  } catch (...) {
    catalog.messages_.clear();
  }

  catalog.language_ = "en";
  try {
    static_cast<void>(loadMessages(catalog.language_, catalog.messages_));
  } catch (...) {
    catalog.messages_.clear();
  }
  return catalog;
}

std::string Catalog::text(std::string_view key) const {
  const auto found = messages_.find(std::string(key));
  return found == messages_.end() ? std::string(key) : found->second;
}

std::string Catalog::format(
    std::string_view key,
    std::initializer_list<std::pair<std::string_view, std::string>> values) const {
  std::string result = text(key);
  for (const auto& [name, value] : values) {
    const std::string placeholder = "{" + std::string(name) + "}";
    std::size_t position = 0;
    while ((position = result.find(placeholder, position)) != std::string::npos) {
      result.replace(position, placeholder.size(), value);
      position += value.size();
    }
  }
  return result;
}

std::string_view Catalog::language() const {
  return language_;
}

}  // namespace i18n
