#include "ui/package_view.hpp"

#include "core/window.hpp"
#include "i18n/catalog.hpp"
#include "pm/pm.hpp"
#include "render/renderer.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ui {

namespace {

constexpr int kMinimumFontSize = 14;
constexpr int kMaximumFontSize = 30;
constexpr int kDefaultFontSize = 18;
constexpr std::size_t kMaximumQueryBytes = 256;
enum class ThemeMode { system, light, dark };
enum class SettingsCategory { appearance, behavior, themes, system };

struct UserSettings {
  int fontSize = kDefaultFontSize;
  std::string fontPath;
  std::string language = "system";
  ThemeMode themeMode = ThemeMode::system;
  bool compactList = false;
  bool animations = true;
  bool smoothScroll = true;
  bool clickSparks = true;
  bool showPackageDescriptions = true;
  bool restoreLastSearch = false;
  std::string lastSearch;
  float cornerRadius = 16.0F;
  float borderSize = 1.5F;
  float iconScale = 1.0F;
  std::string customTheme;
};

struct Bounds {
  float x;
  float y;
  float width;
  float height;
};

struct Palette {
  std::array<float, 3> background;
  std::array<float, 3> header;
  std::array<float, 3> surface;
  std::array<float, 3> elevated;
  std::array<float, 3> outline;
  std::array<float, 3> primary;
  std::array<float, 3> primaryContainer;
  std::array<float, 3> text;
  std::array<float, 3> secondaryText;
  std::array<float, 3> selection;
  std::array<float, 3> onPrimary;
};

constexpr std::array<float, 3> rgb(unsigned int value) {
  return {static_cast<float>((value >> 16U) & 0xFFU) / 255.0F,
          static_cast<float>((value >> 8U) & 0xFFU) / 255.0F,
          static_cast<float>(value & 0xFFU) / 255.0F};
}

constexpr Palette kDarkPalette{
    rgb(0x111318), rgb(0x191C22), rgb(0x1D2027), rgb(0x272B33),
    rgb(0x8D9199), rgb(0xB3C5FF), rgb(0x334A75), rgb(0xE2E2E9),
    rgb(0xC2C6D3), rgb(0x434D63), rgb(0x1C2F5A)};
constexpr Palette kLightPalette{
    rgb(0xF7F8FC), rgb(0xEFF1F8), rgb(0xF0F2F8), rgb(0xE8EAF1),
    rgb(0x747984), rgb(0x465E91), rgb(0xD9E2FF), rgb(0x191B20),
    rgb(0x44474F), rgb(0xD9E2FF), rgb(0xFFFFFF)};

struct CustomTheme {
  std::string path;
  std::string name;
  Palette palette;
  bool light = false;
};

std::string jsonStringValue(std::string_view document, std::string_view key,
                            std::string fallback);
bool jsonBoolValue(std::string_view document, std::string_view key, bool fallback);
std::filesystem::path configDirectory();

std::array<float, 3> jsonColorValue(std::string_view document,
                                   std::string_view key,
                                   std::array<float, 3> fallback) {
  const std::string value = jsonStringValue(document, key, {});
  if (value.size() != 7 || value.front() != '#') return fallback;
  try {
    return {static_cast<float>(std::stoul(value.substr(1, 2), nullptr, 16)) / 255.0F,
            static_cast<float>(std::stoul(value.substr(3, 2), nullptr, 16)) / 255.0F,
            static_cast<float>(std::stoul(value.substr(5, 2), nullptr, 16)) / 255.0F};
  } catch (...) {
    return fallback;
  }
}

std::vector<CustomTheme> loadCustomThemes() {
  std::vector<CustomTheme> themes;
  const std::filesystem::path baseDirectory = configDirectory();
  if (baseDirectory.empty()) return themes;
  const std::filesystem::path directory = baseDirectory / "themes";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (!error) {
    const std::filesystem::path defaultTheme = directory / "default.json";
    if (!std::filesystem::exists(defaultTheme, error) && !error) {
      const std::array<std::filesystem::path, 2> templates{
          std::filesystem::path(FLUX_ASSET_DIR) / "themes" / "default.json",
          std::filesystem::path(FLUX_INSTALL_ASSET_DIR) / "themes" / "default.json"};
      for (const auto& source : templates) {
        if (!std::filesystem::is_regular_file(source, error)) {
          error.clear();
          continue;
        }
        std::filesystem::copy_file(source, defaultTheme,
                                   std::filesystem::copy_options::skip_existing,
                                   error);
        break;
      }
    }
  }
  error.clear();
  if (!std::filesystem::is_directory(directory, error)) return themes;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || iterator->path().extension() != ".json") {
      continue;
    }
    std::ifstream input(iterator->path(), std::ios::binary);
    if (!input) continue;
    const std::string document((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    Palette palette = kDarkPalette;
    palette.background = jsonColorValue(document, "background", palette.background);
    palette.header = jsonColorValue(document, "header", palette.header);
    palette.surface = jsonColorValue(document, "surface", palette.surface);
    palette.elevated = jsonColorValue(document, "elevated", palette.elevated);
    palette.outline = jsonColorValue(document, "outline", palette.outline);
    palette.primary = jsonColorValue(document, "primary", palette.primary);
    palette.primaryContainer = jsonColorValue(document, "primary_container", palette.primaryContainer);
    palette.text = jsonColorValue(document, "text", palette.text);
    palette.secondaryText = jsonColorValue(document, "secondary_text", palette.secondaryText);
    palette.selection = jsonColorValue(document, "selection", palette.selection);
    palette.onPrimary = jsonColorValue(document, "on_primary", palette.onPrimary);
    const std::string name = jsonStringValue(document, "name", iterator->path().stem().string());
    themes.push_back({iterator->path().string(), name, palette,
                      jsonBoolValue(document, "light", false)});
  }
  std::sort(themes.begin(), themes.end(), [](const CustomTheme& left,
                                             const CustomTheme& right) {
    return left.name < right.name;
  });
  return themes;
}

struct DecodedCodepoint {
  char32_t value;
  std::size_t length;
};

DecodedCodepoint decodeCodepoint(std::string_view text, std::size_t offset) {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80) {
    return {first, 1};
  }
  std::size_t length = 0;
  char32_t value = 0;
  if ((first & 0xE0) == 0xC0) {
    length = 2;
    value = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    value = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    value = first & 0x07;
  } else {
    return {U'\uFFFD', 1};
  }
  if (offset + length > text.size()) {
    return {U'\uFFFD', 1};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0) != 0x80) {
      return {U'\uFFFD', 1};
    }
    value = (value << 6) | (continuation & 0x3F);
  }
  if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
      (length == 4 && value < 0x10000) || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return {U'\uFFFD', 1};
  }
  return {value, length};
}

std::string encodeCodepoint(char32_t codepoint) {
  std::string encoded;
  if (codepoint <= 0x7F) {
    encoded.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    encoded.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    encoded.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0x10FFFF) {
    encoded.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  return encoded;
}

std::size_t previousCodepointPosition(std::string_view text, std::size_t position) {
  if (position == 0) {
    return 0;
  }
  --position;
  while (position > 0 &&
         (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80) {
    --position;
  }
  return position;
}

std::size_t nextCodepointPosition(std::string_view text, std::size_t position) {
  if (position >= text.size()) {
    return text.size();
  }
  return std::min(text.size(), position + decodeCodepoint(text, position).length);
}

struct Layout {
  Bounds packagesButton;
  Bounds installedButton;
  Bounds updatesButton;
  Bounds settingsButton;
  Bounds searchCard;
  Bounds searchField;
  Bounds clearQueryButton;
  Bounds searchButton;
  Bounds progressTrack;
  Bounds resultsPanel;
  Bounds packageScrollbarTrack;
  Bounds settingsPanel;
  Bounds appearanceTab;
  Bounds behaviorTab;
  Bounds themesTab;
  Bounds systemTab;
  Bounds decreaseFontButton;
  Bounds increaseFontButton;
  Bounds themeButton;
  Bounds languageButton;
  Bounds iconSizeDecreaseButton;
  Bounds iconSizeIncreaseButton;
  Bounds cornerRadiusDecreaseButton;
  Bounds cornerRadiusIncreaseButton;
  Bounds borderDecreaseButton;
  Bounds borderIncreaseButton;
  Bounds densityButton;
  Bounds descriptionsButton;
  Bounds restoreSearchButton;
  Bounds animationButton;
  Bounds smoothScrollButton;
  Bounds sparkButton;
  Bounds fontFamilyButton;
  Bounds fontStyleButton;
  Bounds clearCacheButton;
  Bounds resetSettingsButton;
  Bounds closeSettingsButton;
  Bounds settingsHeaderCloseButton;
  Bounds cacheConfirmPanel;
  Bounds cancelCacheButton;
  Bounds confirmCacheButton;
  Bounds updateDialogPanel;
  Bounds cancelUpdateButton;
  Bounds confirmUpdateButton;
  Bounds infoDialogPanel;
  Bounds infoDialogCloseButton;
  Bounds infoDialogContent;
  float headerHeight;
  float horizontalMargin;
  float queryTextX;
  float queryTextBaseline;
  float queryTextWidth;
  float statusBaseline;
  float rowContentX;
  float helpButtonX;
  float helpButtonWidth;
  float actionButtonX;
  float actionButtonWidth;
  float actionButtonHeight;
  float fontSizeValueCenterX;
  float firstRowY;
  float rowHeight;
  float settingsScale;
  float settingsContentOffset;
  float confirmScale;
  float confirmButtonWidth;
  float confirmButtonHeight;
  int visibleRows;
  int drawableRows;
  bool showClearQueryButton;
};

enum class DropdownKind { none, fontFamily, fontStyle, theme, language };

struct DropdownGeometry {
  Bounds panel{};
  float rowHeight = 0.0F;
  int visibleRows = 0;
};

std::vector<std::string> splitCodepoints(std::string_view text) {
  std::vector<std::string> result;
  for (std::size_t offset = 0; offset < text.size();) {
    const std::size_t next = nextCodepointPosition(text, offset);
    result.emplace_back(text.substr(offset, next - offset));
    offset = next;
  }
  return result;
}

std::filesystem::path configDirectory() {
  if (const char* configHome = std::getenv("XDG_CONFIG_HOME");
      configHome != nullptr && configHome[0] != '\0') {
    return std::filesystem::path(configHome) / "flux";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".config" / "flux";
  }
  return {};
}

std::filesystem::path settingsPath() {
  const std::filesystem::path directory = configDirectory();
  return directory.empty() ? std::filesystem::path{} : directory / "config.json";
}

std::filesystem::path legacySettingsPath() {
  const std::filesystem::path directory = configDirectory();
  return directory.empty() ? std::filesystem::path{} : directory / "settings";
}

std::string jsonStringValue(std::string_view document, std::string_view key,
                            std::string fallback = {}) {
  const std::string token = "\"" + std::string(key) + "\"";
  std::size_t position = document.find(token);
  if (position == std::string_view::npos) return fallback;
  position = document.find(':', position + token.size());
  if (position == std::string_view::npos) return fallback;
  ++position;
  while (position < document.size() &&
         std::isspace(static_cast<unsigned char>(document[position]))) ++position;
  if (position >= document.size() || document[position] != '"') return fallback;
  ++position;
  std::string value;
  bool escaped = false;
  for (; position < document.size(); ++position) {
    const char character = document[position];
    if (escaped) {
      switch (character) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(character); break;
      }
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '"') {
      return value;
    } else {
      value.push_back(character);
    }
  }
  return fallback;
}

double jsonNumberValue(std::string_view document, std::string_view key,
                       double fallback) {
  const std::string token = "\"" + std::string(key) + "\"";
  std::size_t position = document.find(token);
  if (position == std::string_view::npos) return fallback;
  position = document.find(':', position + token.size());
  if (position == std::string_view::npos) return fallback;
  ++position;
  while (position < document.size() &&
         std::isspace(static_cast<unsigned char>(document[position]))) ++position;
  double value = fallback;
  const char* begin = document.data() + position;
  const char* end = document.data() + document.size();
  const auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} ? value : fallback;
}

bool jsonBoolValue(std::string_view document, std::string_view key, bool fallback) {
  const std::string token = "\"" + std::string(key) + "\"";
  std::size_t position = document.find(token);
  if (position == std::string_view::npos) return fallback;
  position = document.find(':', position + token.size());
  if (position == std::string_view::npos) return fallback;
  const std::string_view value = document.substr(position + 1);
  const std::size_t truth = value.find("true");
  const std::size_t falsity = value.find("false");
  if (truth == std::string_view::npos && falsity == std::string_view::npos) return fallback;
  return truth != std::string_view::npos &&
         (falsity == std::string_view::npos || truth < falsity);
}

std::string jsonEscape(std::string_view value) {
  std::string escaped;
  for (const char character : value) {
    if (character == '"' || character == '\\') escaped.push_back('\\');
    if (character == '\n') { escaped += "\\n"; continue; }
    if (character == '\r') { escaped += "\\r"; continue; }
    if (character == '\t') { escaped += "\\t"; continue; }
    escaped.push_back(character);
  }
  return escaped;
}

std::optional<bool> systemPreferredLightTheme() {
  if (const char* gtkTheme = std::getenv("GTK_THEME");
      gtkTheme != nullptr && gtkTheme[0] != '\0') {
    std::string theme(gtkTheme);
    std::transform(theme.begin(), theme.end(), theme.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    if (theme.find("dark") != std::string::npos) {
      return false;
    }
    if (theme.find("light") != std::string::npos) {
      return true;
    }
  }

  if (FILE* command =
          popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null",
                "r");
      command != nullptr) {
    std::array<char, 128> buffer{};
    std::string preference;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), command) !=
           nullptr) {
      preference += buffer.data();
    }
    static_cast<void>(pclose(command));
    if (preference.find("prefer-light") != std::string::npos) {
      return true;
    }
    if (preference.find("prefer-dark") != std::string::npos) {
      return false;
    }
  }

  const std::filesystem::path userSettings = settingsPath();
  if (!userSettings.empty()) {
    const std::filesystem::path configFile =
        userSettings.parent_path().parent_path() / "kdeglobals";
    std::ifstream config(configFile);
    std::string line;
    bool inGeneralSection = false;
    while (std::getline(config, line)) {
      if (!line.empty() && line.front() == '[') {
        inGeneralSection = line == "[General]";
      } else if (inGeneralSection && line.starts_with("ColorScheme=")) {
        std::string scheme = line.substr(std::string("ColorScheme=").size());
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char character) {
                         return static_cast<char>(std::tolower(character));
                       });
        if (scheme.find("dark") != std::string::npos) {
          return false;
        }
        if (scheme.find("light") != std::string::npos ||
            scheme.find("breeze") != std::string::npos) {
          return true;
        }
      }
    }
  }

  if (const char* colorForegroundBackground = std::getenv("COLORFGBG");
      colorForegroundBackground != nullptr) {
    const std::string value(colorForegroundBackground);
    const std::size_t separator = value.rfind(';');
    if (separator != std::string::npos) {
      std::istringstream background(value.substr(separator + 1));
      int color = 0;
      if (background >> color) {
        return color >= 7;
      }
    }
  }
  return std::nullopt;
}

std::string_view themeModeName(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::light:
      return "light";
    case ThemeMode::dark:
      return "dark";
    case ThemeMode::system:
      return "system";
  }
  return "system";
}

std::string themeModeLabel(ThemeMode mode, const i18n::Catalog& catalog) {
  switch (mode) {
    case ThemeMode::light:
      return catalog.text("theme.light");
    case ThemeMode::dark:
      return catalog.text("theme.dark");
    case ThemeMode::system:
      return catalog.text("theme.system");
  }
  return catalog.text("theme.system");
}

UserSettings loadSettings() {
  UserSettings settings;
  const std::filesystem::path path = settingsPath();
  std::ifstream jsonInput(path, std::ios::binary);
  if (jsonInput) {
    const std::string document((std::istreambuf_iterator<char>(jsonInput)),
                               std::istreambuf_iterator<char>());
    settings.fontSize = std::clamp(
        static_cast<int>(jsonNumberValue(document, "font_size", settings.fontSize)),
        kMinimumFontSize, kMaximumFontSize);
    settings.fontPath = jsonStringValue(document, "font_path");
    settings.language = jsonStringValue(document, "language", "system");
    if (settings.language != "system" && settings.language != "en" &&
        settings.language != "ru" && settings.language != "ro") {
      settings.language = "system";
    }
    settings.restoreLastSearch = jsonBoolValue(document, "restore_last_search", false);
    settings.lastSearch = jsonStringValue(document, "last_search");
    settings.cornerRadius = static_cast<float>(std::clamp(
        jsonNumberValue(document, "corner_radius",
                        static_cast<double>(settings.cornerRadius)), 0.0, 32.0));
    settings.borderSize = static_cast<float>(std::clamp(
        jsonNumberValue(document, "border_size",
                        static_cast<double>(settings.borderSize)), 0.0, 6.0));
    settings.iconScale = static_cast<float>(std::clamp(
        jsonNumberValue(document, "icon_scale",
                        static_cast<double>(settings.iconScale)), 0.6, 1.8));
    settings.customTheme = jsonStringValue(document, "custom_theme", "");
    const std::string theme = jsonStringValue(document, "theme", "system");
    if (theme == "light") settings.themeMode = ThemeMode::light;
    else if (theme == "dark") settings.themeMode = ThemeMode::dark;
    settings.compactList = jsonBoolValue(document, "compact_list", settings.compactList);
    settings.animations = jsonBoolValue(document, "animations", settings.animations);
    settings.smoothScroll = jsonBoolValue(document, "smooth_scroll", settings.smoothScroll);
    settings.clickSparks = jsonBoolValue(document, "click_sparks", settings.clickSparks);
    settings.showPackageDescriptions = jsonBoolValue(
        document, "show_package_descriptions", settings.showPackageDescriptions);
    return settings;
  }

  // Read the previous line-based config so upgrades preserve existing settings.
  std::ifstream input(legacySettingsPath());
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos) {
      std::istringstream legacyValue(line);
      int size = 0;
      if (legacyValue >> size && size >= kMinimumFontSize &&
          size <= kMaximumFontSize) {
        settings.fontSize = size;
      }
      continue;
    }

    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (key == "font_size") {
      std::istringstream parsed(value);
      int size = 0;
      if (parsed >> size && size >= kMinimumFontSize &&
          size <= kMaximumFontSize) {
        settings.fontSize = size;
      }
    } else if (key == "font_path") {
      settings.fontPath = value;
    } else if (key == "language" &&
               (value == "system" || value == "en" || value == "ru" ||
                value == "ro")) {
      settings.language = value;
    } else if (key == "theme") {
      if (value == "light") {
        settings.themeMode = ThemeMode::light;
      } else if (value == "dark") {
        settings.themeMode = ThemeMode::dark;
      } else {
        settings.themeMode = ThemeMode::system;
      }
    } else if (key == "compact_list") {
      settings.compactList = value == "true";
    } else if (key == "animations") {
      settings.animations = value != "false";
    } else if (key == "smooth_scroll") {
      settings.smoothScroll = value != "false";
    } else if (key == "click_sparks") {
      settings.clickSparks = value != "false";
    } else if (key == "show_package_descriptions") {
      settings.showPackageDescriptions = value != "false";
    } else if (key == "restore_last_search") {
      settings.restoreLastSearch = value == "true";
    } else if (key == "last_search") {
      settings.lastSearch = value;
    } else if (key == "corner_radius") {
      std::istringstream parsed(value);
      parsed >> settings.cornerRadius;
      settings.cornerRadius = std::clamp(settings.cornerRadius, 0.0F, 32.0F);
    } else if (key == "border_size") {
      std::istringstream parsed(value);
      parsed >> settings.borderSize;
      settings.borderSize = std::clamp(settings.borderSize, 0.0F, 6.0F);
    } else if (key == "icon_scale") {
      std::istringstream parsed(value);
      parsed >> settings.iconScale;
      settings.iconScale = std::clamp(settings.iconScale, 0.6F, 1.8F);
    }
  }
  return settings;
}

void saveSettings(const UserSettings& settings) {
  const std::filesystem::path path = settingsPath();
  if (path.empty()) {
    throw std::runtime_error("No configuration directory is available");
  }

  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    throw std::system_error(error, "Could not create the Flux settings directory");
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Could not write the Flux settings file");
  }
  output << "{\n"
         << "  \"font_size\": " << settings.fontSize << ",\n"
         << "  \"font_path\": \"" << jsonEscape(settings.fontPath) << "\",\n"
         << "  \"language\": \"" << jsonEscape(settings.language) << "\",\n"
         << "  \"theme\": \"" << themeModeName(settings.themeMode) << "\",\n"
         << "  \"compact_list\": " << (settings.compactList ? "true" : "false") << ",\n"
         << "  \"animations\": " << (settings.animations ? "true" : "false") << ",\n"
         << "  \"smooth_scroll\": " << (settings.smoothScroll ? "true" : "false") << ",\n"
         << "  \"click_sparks\": " << (settings.clickSparks ? "true" : "false") << ",\n"
         << "  \"show_package_descriptions\": "
         << (settings.showPackageDescriptions ? "true" : "false") << ",\n"
         << "  \"restore_last_search\": "
         << (settings.restoreLastSearch ? "true" : "false") << ",\n"
         << "  \"last_search\": \""
         << jsonEscape(settings.restoreLastSearch ? settings.lastSearch : "") << "\",\n"
         << "  \"corner_radius\": " << settings.cornerRadius << ",\n"
         << "  \"border_size\": " << settings.borderSize << ",\n"
         << "  \"icon_scale\": " << settings.iconScale << ",\n"
         << "  \"custom_theme\": \"" << jsonEscape(settings.customTheme) << "\"\n"
         << "}\n";
  if (!output) {
    throw std::runtime_error("Could not save the settings");
  }
}

Layout makeLayout(int width, int height, const render::Renderer& renderer,
                  bool compactList) {
  const float windowWidth = std::max(0.0F, static_cast<float>(width));
  const float windowHeight = std::max(0.0F, static_cast<float>(height));
  const float textSize = static_cast<float>(renderer.fontSize());
  const float margin = std::clamp(windowWidth * 0.025F, 12.0F, 32.0F);
  const float headerHeight = std::clamp(windowHeight * 0.09F, 48.0F, 64.0F);
  const float packageNavWidth = 42.0F;
  const float installedNavWidth = 42.0F;
  const float updateNavWidth = 42.0F;
  const float settingsNavWidth = 42.0F;
  const float searchCardHeight = std::clamp(windowHeight * 0.11F, 68.0F, 82.0F);
  const float searchCardY = headerHeight + 12.0F;
  const Bounds searchCard{margin, searchCardY,
                          std::max(0.0F, windowWidth - margin * 2.0F),
                          searchCardHeight};

  const float settingsWidth =
      std::min(640.0F, std::max(200.0F, windowWidth - 24.0F));
  const float settingsHeight =
      std::min(560.0F, std::max(180.0F, windowHeight - 24.0F));
  const float settingsScale = settingsHeight / 560.0F;
  const float confirmWidth =
      std::min(460.0F, std::max(220.0F, windowWidth - 24.0F));
  const float confirmHeight =
      std::min(240.0F, std::max(180.0F, windowHeight - 24.0F));
  const float confirmScale = confirmHeight / 240.0F;
  const float confirmButtonWidth = std::min(
      std::max(96.0F, textSize * 6.0F), (confirmWidth - 40.0F) / 2.0F - 6.0F);
  const float confirmButtonHeight = std::min(38.0F, confirmScale * 38.0F);

  const float searchButtonWidth = std::min(
      std::max(92.0F, textSize * 6.4F),
      std::max(92.0F, (searchCard.width - 32.0F) * 0.42F));
  const float searchButtonHeight =
      std::min(48.0F, std::max(36.0F, searchCardHeight - 20.0F));
  const float searchButtonX =
      searchCard.x + searchCard.width - 16.0F - searchButtonWidth;
  const float searchFieldX = searchCard.x + 16.0F;
  const float searchFieldWidth = std::max(0.0F, searchButtonX - 12.0F - searchFieldX);
  const float searchFieldHeight = std::min(48.0F, searchCardHeight - 16.0F);
  const float searchFieldY = searchCard.y + (searchCardHeight - searchFieldHeight) / 2.0F;
  const bool showClearQueryButton = searchFieldWidth >= 96.0F;
  const float queryTextWidth =
      std::max(0.0F, searchFieldWidth - (showClearQueryButton ? 64.0F : 32.0F));

  const float resultsY = searchCard.y + searchCard.height + 22.0F;
  const float statusBaseline = searchCard.y + searchCard.height + 36.0F;
  const float firstRowY = std::max(
      resultsY + 34.0F, statusBaseline + textSize + 10.0F);
  const float bottomMargin = std::max(12.0F, margin);
  const float remainingRowHeight =
      std::max(48.0F, (windowHeight - firstRowY - bottomMargin) / 2.5F);
  const float requestedRowHeight =
      compactList ? std::max(54.0F, textSize * 2.2F)
                  : std::max(72.0F, textSize * 2.8F);
  const float rowHeight = std::min(requestedRowHeight, remainingRowHeight);
  const float actionButtonWidth = std::min(
      std::max(96.0F, textSize * 6.0F), windowWidth * 0.30F);
  const float actionButtonHeight = std::min(40.0F, rowHeight - 8.0F);
  const float actionButtonX = windowWidth - margin - 20.0F - actionButtonWidth;
  const float helpButtonWidth = std::min(36.0F, actionButtonHeight);
  const float helpButtonX = actionButtonX - helpButtonWidth - 8.0F;

  const float panelX = (windowWidth - settingsWidth) / 2.0F;
  const float panelY = (windowHeight - settingsHeight) / 2.0F;
  const float confirmX = (windowWidth - confirmWidth) / 2.0F;
  const float confirmY = (windowHeight - confirmHeight) / 2.0F;
  const float infoWidth = std::min(720.0F, std::max(280.0F, windowWidth - 24.0F));
  const float infoHeight = std::min(520.0F, std::max(260.0F, windowHeight - 24.0F));
  const float infoX = (windowWidth - infoWidth) / 2.0F;
  const float infoY = (windowHeight - infoHeight) / 2.0F;
  const float infoCloseSize = 32.0F;
  const float decrementSize = std::min(36.0F, std::max(24.0F, 44.0F * settingsScale));
  const float appearanceStepSize =
      std::min(36.0F, std::max(24.0F, 36.0F * settingsScale));
  float appearanceRow1Y = panelY + 100.0F * settingsScale;
  float appearanceRow2Y = panelY + 148.0F * settingsScale;
  float appearanceRow3Y = panelY + 196.0F * settingsScale;
  float appearanceRow4Y = panelY + 244.0F * settingsScale;
  float appearanceRow5Y = panelY + 292.0F * settingsScale;
  float appearanceRow6Y = panelY + 340.0F * settingsScale;
  float appearanceRow7Y = panelY + 388.0F * settingsScale;
  const float closeWidth =
      std::min(std::max(100.0F, textSize * 5.4F), settingsWidth - 40.0F);
  const float closeHeight =
      std::min(std::max(24.0F, 36.0F * settingsScale), 52.0F * settingsScale);
  const float headerCloseSize = 32.0F;
  const float settingsControlWidth =
      std::min(260.0F, std::max(92.0F, settingsWidth * 0.46F));
  const float settingsTabGap = 6.0F * settingsScale;
  const bool compactSettingsTabs = settingsWidth < 520.0F || textSize > 18.0F;
  const float settingsTabWidth = compactSettingsTabs
      ? (settingsWidth - 40.0F * settingsScale - settingsTabGap) * 0.5F
      : (settingsWidth - 40.0F * settingsScale - settingsTabGap * 3.0F) * 0.25F;
  const float settingsTabY = panelY + 52.0F * settingsScale;
  const float settingsTabHeight =
      std::max(32.0F * settingsScale, textSize + 10.0F);
  const float secondSettingsTabY = settingsTabY + settingsTabHeight + settingsTabGap;
  const float settingsTabBottom = compactSettingsTabs
      ? secondSettingsTabY + settingsTabHeight
      : settingsTabY + settingsTabHeight;
  const float settingsContentOffset = std::max(
      0.0F, settingsTabBottom + 14.0F - appearanceRow1Y);
  const float settingsControlHeight =
      std::max(38.0F * settingsScale, textSize + 4.0F);
  appearanceRow1Y += settingsContentOffset;
  appearanceRow2Y += settingsContentOffset;
  appearanceRow3Y += settingsContentOffset;
  appearanceRow4Y += settingsContentOffset;
  appearanceRow5Y += settingsContentOffset;
  appearanceRow6Y += settingsContentOffset;
  appearanceRow7Y += settingsContentOffset;
  const float firstSettingsTabX = panelX + 20.0F * settingsScale;
  const float scrollbarHeight =
      std::max(0.0F, windowHeight - firstRowY - bottomMargin);
  const float decreaseX =
      settingsWidth < 400.0F ? settingsWidth - decrementSize * 2.0F - 28.0F
                             : settingsWidth - 228.0F;
  const float increaseX =
      settingsWidth < 400.0F ? settingsWidth - decrementSize - 20.0F
                             : settingsWidth - 72.0F;
  const float appearanceIncreaseX = settingsWidth - 56.0F;
  const float appearanceValueWidth = std::max(48.0F, textSize * 3.6F);
  const float appearanceDecreaseX = std::max(
      20.0F, appearanceIncreaseX - appearanceStepSize - appearanceValueWidth - 16.0F);
  const float valueGapStart = decreaseX + decrementSize + 8.0F;
  const float valueGapEnd = increaseX - 8.0F;
  const float fontSizeValueCenterX =
      panelX + (valueGapStart + valueGapEnd) * 0.5F;
  const float confirmButtonsY =
      confirmY + confirmHeight - 18.0F - confirmButtonHeight;
  const float queryTextX = searchFieldX + 16.0F;
  const float queryBaseline =
      renderer.baselineForCenter(searchFieldY + searchFieldHeight / 2.0F);
  return {
      .packagesButton = {windowWidth - margin - packageNavWidth - installedNavWidth -
                             updateNavWidth - settingsNavWidth - 18.0F,
                         (headerHeight - 36.0F) / 2.0F,
                         packageNavWidth, 36.0F},
      .installedButton = {windowWidth - margin - installedNavWidth - updateNavWidth -
                              settingsNavWidth - 12.0F,
                          (headerHeight - 36.0F) / 2.0F,
                          installedNavWidth, 36.0F},
      .updatesButton = {windowWidth - margin - updateNavWidth - settingsNavWidth - 6.0F,
                        (headerHeight - 36.0F) / 2.0F,
                        updateNavWidth, 36.0F},
      .settingsButton = {windowWidth - margin - settingsNavWidth,
                         (headerHeight - 36.0F) / 2.0F,
                         settingsNavWidth, 36.0F},
      .searchCard = searchCard,
      .searchField = {searchFieldX, searchFieldY, searchFieldWidth,
                      searchFieldHeight},
      .clearQueryButton = {searchFieldX + searchFieldWidth - 38.0F,
                           searchFieldY + (searchFieldHeight - 32.0F) / 2.0F,
                           32.0F, 32.0F},
      .searchButton = {searchButtonX,
                       searchCard.y + (searchCardHeight - searchButtonHeight) / 2.0F,
                       searchButtonWidth, searchButtonHeight},
      .progressTrack = {searchCard.x + 16.0F, searchCard.y + searchCard.height + 4.0F,
                        std::max(0.0F, searchCard.width - 32.0F), 6.0F},
      .resultsPanel = {margin, resultsY, std::max(0.0F, windowWidth - margin * 2.0F),
                       std::max(0.0F, windowHeight - resultsY - bottomMargin)},
      .packageScrollbarTrack = {windowWidth - margin - 7.0F, firstRowY, 5.0F,
                                scrollbarHeight},
      .settingsPanel = {panelX, panelY, settingsWidth, settingsHeight},
      .appearanceTab = {firstSettingsTabX, settingsTabY,
                        settingsTabWidth, settingsTabHeight},
      .behaviorTab = {firstSettingsTabX + settingsTabWidth + settingsTabGap,
                      settingsTabY, settingsTabWidth, settingsTabHeight},
      .themesTab = {
          firstSettingsTabX + (compactSettingsTabs ? 0.0F
                                                   : (settingsTabWidth + settingsTabGap) * 2.0F),
          compactSettingsTabs ? secondSettingsTabY : settingsTabY,
          settingsTabWidth, settingsTabHeight},
      .systemTab = {
          firstSettingsTabX + (compactSettingsTabs
                                   ? settingsTabWidth + settingsTabGap
                                   : (settingsTabWidth + settingsTabGap) * 3.0F),
          compactSettingsTabs ? secondSettingsTabY : settingsTabY,
          settingsTabWidth, settingsTabHeight},
      .decreaseFontButton = {panelX + std::max(20.0F, decreaseX),
                             appearanceRow1Y,
                             decrementSize, decrementSize},
      .increaseFontButton = {panelX + increaseX,
                             appearanceRow1Y,
                             decrementSize, decrementSize},
      .themeButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                      panelY + 128.0F * settingsScale + settingsContentOffset,
                      settingsControlWidth, settingsControlHeight},
      .languageButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                         appearanceRow4Y,
                         settingsControlWidth, settingsControlHeight},
      .iconSizeDecreaseButton = {panelX + appearanceDecreaseX,
                                  appearanceRow5Y, appearanceStepSize,
                                  appearanceStepSize},
      .iconSizeIncreaseButton = {panelX + settingsWidth - 56.0F,
                                  appearanceRow5Y, appearanceStepSize,
                                  appearanceStepSize},
      .cornerRadiusDecreaseButton = {panelX + appearanceDecreaseX,
                                     appearanceRow6Y, appearanceStepSize,
                                     appearanceStepSize},
      .cornerRadiusIncreaseButton = {panelX + settingsWidth - 56.0F,
                                     appearanceRow6Y, appearanceStepSize,
                                     appearanceStepSize},
      .borderDecreaseButton = {panelX + appearanceDecreaseX,
                               appearanceRow7Y, appearanceStepSize,
                               appearanceStepSize},
      .borderIncreaseButton = {panelX + settingsWidth - 56.0F,
                               appearanceRow7Y, appearanceStepSize,
                               appearanceStepSize},
      .densityButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                        panelY + 110.0F * settingsScale + settingsContentOffset,
                        settingsControlWidth, settingsControlHeight},
      .descriptionsButton = {
          panelX + settingsWidth - settingsControlWidth - 20.0F,
          panelY + 334.0F * settingsScale + settingsContentOffset, settingsControlWidth,
          settingsControlHeight},
      .restoreSearchButton = {
          panelX + settingsWidth - settingsControlWidth - 20.0F,
          panelY + 388.0F * settingsScale + settingsContentOffset, settingsControlWidth,
          settingsControlHeight},
      .animationButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                          panelY + 278.0F * settingsScale + settingsContentOffset,
                          settingsControlWidth, settingsControlHeight},
      .smoothScrollButton = {
          panelX + settingsWidth - settingsControlWidth - 20.0F,
          panelY + 166.0F * settingsScale + settingsContentOffset, settingsControlWidth,
          settingsControlHeight},
      .sparkButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                      panelY + 222.0F * settingsScale + settingsContentOffset, settingsControlWidth,
                      settingsControlHeight},
      .fontFamilyButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                           appearanceRow2Y,
                           settingsControlWidth, settingsControlHeight},
      .fontStyleButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                          appearanceRow3Y,
                          settingsControlWidth, settingsControlHeight},
      .clearCacheButton = {panelX + 20.0F,
                           panelY + 156.0F * settingsScale + settingsContentOffset,
                           settingsWidth - 40.0F,
                           settingsControlHeight},
      .resetSettingsButton = {panelX + 20.0F,
                              panelY + 232.0F * settingsScale + settingsContentOffset,
                              settingsWidth - 40.0F,
                              settingsControlHeight},
      .closeSettingsButton = {panelX + settingsWidth - closeWidth - 20.0F,
                              panelY + settingsHeight - 52.0F * settingsScale,
                              closeWidth, closeHeight},
      .settingsHeaderCloseButton = {
          panelX + settingsWidth - headerCloseSize - 20.0F,
          panelY + 12.0F * settingsScale, headerCloseSize, headerCloseSize},
      .cacheConfirmPanel = {confirmX, confirmY, confirmWidth, confirmHeight},
      .cancelCacheButton = {
          confirmX + confirmWidth - confirmButtonWidth * 2.0F - 28.0F,
          confirmButtonsY, confirmButtonWidth, confirmButtonHeight},
      .confirmCacheButton = {
          confirmX + confirmWidth - confirmButtonWidth - 16.0F,
          confirmButtonsY, confirmButtonWidth, confirmButtonHeight},
      .updateDialogPanel = {confirmX, confirmY, confirmWidth, confirmHeight},
      .cancelUpdateButton = {
          confirmX + confirmWidth - confirmButtonWidth * 2.0F - 28.0F,
          confirmButtonsY, confirmButtonWidth, confirmButtonHeight},
      .confirmUpdateButton = {
          confirmX + confirmWidth - confirmButtonWidth - 16.0F,
          confirmButtonsY, confirmButtonWidth, confirmButtonHeight},
      .infoDialogPanel = {infoX, infoY, infoWidth, infoHeight},
      .infoDialogCloseButton = {
          infoX + infoWidth - infoCloseSize - 16.0F, infoY + 12.0F,
          infoCloseSize, infoCloseSize},
      .infoDialogContent = {infoX + 24.0F, infoY + 222.0F,
                            std::max(0.0F, infoWidth - 48.0F),
                            std::max(0.0F, infoHeight - 242.0F)},
      .headerHeight = headerHeight,
      .horizontalMargin = margin,
      .queryTextX = queryTextX,
      .queryTextBaseline = queryBaseline,
      .queryTextWidth = queryTextWidth,
      .statusBaseline = statusBaseline,
      .rowContentX = margin + 20.0F,
      .helpButtonX = helpButtonX,
      .helpButtonWidth = helpButtonWidth,
      .actionButtonX = actionButtonX,
      .actionButtonWidth = actionButtonWidth,
      .actionButtonHeight = actionButtonHeight,
      .fontSizeValueCenterX = fontSizeValueCenterX,
      .firstRowY = firstRowY,
      .rowHeight = rowHeight,
      .settingsScale = settingsScale,
      .settingsContentOffset = settingsContentOffset,
      .confirmScale = confirmScale,
      .confirmButtonWidth = confirmButtonWidth,
      .confirmButtonHeight = confirmButtonHeight,
      .visibleRows = std::max(
          0, static_cast<int>((windowHeight - firstRowY - bottomMargin) / rowHeight)),
      .drawableRows = std::max(
          0, static_cast<int>(std::ceil(
                 (windowHeight - firstRowY - bottomMargin) / rowHeight))),
      .showClearQueryButton = showClearQueryButton};
}

bool contains(const Bounds& bounds, double x, double y) {
  const double left = static_cast<double>(bounds.x);
  const double top = static_cast<double>(bounds.y);
  const double right = left + static_cast<double>(bounds.width);
  const double bottom = top + static_cast<double>(bounds.height);
  return x >= left && x <= right && y >= top && y <= bottom;
}

std::string iconPath(std::string_view filename) {
  const std::string relative = "icons/" + std::string(filename);
  const std::string sourcePath = std::string(FLUX_ASSET_DIR) + "/" + relative;
  if (std::filesystem::exists(sourcePath)) {
    return sourcePath;
  }
  return std::string(FLUX_INSTALL_ASSET_DIR) + "/" + relative;
}

void addRoundedRect(std::vector<render::RectItem>& rectangles, const Bounds& bounds,
                    std::array<float, 3> color, float radius,
                    float opacity = 1.0F) {
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }
  radius = std::min({radius, bounds.width / 2.0F, bounds.height / 2.0F});
  rectangles.push_back(
      {bounds.x, bounds.y, bounds.width, bounds.height, color, radius, opacity});
}

void addButtonSurface(std::vector<render::RectItem>& rectangles,
                      const Bounds& bounds, std::array<float, 3> fill,
                      std::array<float, 3> border, float hoverAmount,
                      float pressAmount) {
  addRoundedRect(rectangles, bounds, border, 12.0F);
  const float inset = 1.0F;
  const std::array<float, 3> hoverFill{
      std::min(1.0F, fill[0] + 0.08F),
      std::min(1.0F, fill[1] + 0.08F),
      std::min(1.0F, fill[2] + 0.08F)};
  const std::array<float, 3> animatedFill{
      std::clamp(fill[0] + (hoverFill[0] - fill[0]) * hoverAmount +
                     (border[0] - fill[0]) * pressAmount * 0.18F,
                 0.0F, 1.0F),
      std::clamp(fill[1] + (hoverFill[1] - fill[1]) * hoverAmount +
                     (border[1] - fill[1]) * pressAmount * 0.18F,
                 0.0F, 1.0F),
      std::clamp(fill[2] + (hoverFill[2] - fill[2]) * hoverAmount +
                     (border[2] - fill[2]) * pressAmount * 0.18F,
                 0.0F, 1.0F)};
  addRoundedRect(rectangles,
                 {bounds.x + inset, bounds.y + inset,
                  bounds.width - inset * 2.0F, bounds.height - inset * 2.0F},
                 animatedFill, 10.0F);
}

}  // namespace

struct PackageView::Impl {
  struct AnimatedGlyph {
    std::string text;
    std::chrono::steady_clock::time_point started;
    bool entering = false;
  };

  struct DisappearingGlyph {
    std::string text;
    float xOffset = 0.0F;
    std::chrono::steady_clock::time_point started;
    float initialOpacity = 1.0F;
  };

  struct ButtonPress {
    double x = 0.0;
    double y = 0.0;
    std::chrono::steady_clock::time_point started;
  };

  struct SparkParticle {
    float x;
    float y;
    float velocityX;
    float velocityY;
    float age;
    float lifetime;
    float size;
    std::array<float, 3> color;
  };

  struct PackageAppearance {
    std::chrono::steady_clock::time_point started;
    float duration = 0.1F;
  };

  enum class ViewMode { packages, installed, updates };

  GLFWwindow* window;
  render::Renderer& renderer;
  pm::Manager packageManager;
  i18n::Catalog catalog;
  bool searchFocused = true;
  bool settingsOpen = false;
  ViewMode view = ViewMode::packages;
  bool cacheConfirmOpen = false;
  bool updatesOpen = false;
  bool packageInfoOpen = false;
  bool cacheClearTask = false;
  bool updateTask = false;
  ViewMode pendingView = ViewMode::packages;
  bool mouseWasDown = false;
  float infoTransition = 0.0F;
  float categoryTransition = 1.0F;
  float infoScrollOffset = 0.0F;
  bool draggingSearchSelection = false;
  bool draggingPackageScrollbar = false;
  bool dropdownOpen = false;
  DropdownKind dropdownKind = DropdownKind::none;
  SettingsCategory settingsCategory = SettingsCategory::appearance;
  bool systemLightTheme = false;
  GLFWcursor* pointerCursor = nullptr;
  GLFWcursor* textCursor = nullptr;
  double cursorX = 0.0;
  double cursorY = 0.0;
  float scrollOffset = 0.0F;
  float scrollPosition = 0.0F;
  mutable float packageScrollbarHover = 0.0F;
  bool packageListExiting = false;
  float packageListTransition = 1.0F;
  float settingsTransition = 0.0F;
  float dropdownTransition = 0.0F;
  float clearQueryButtonTransition = 0.0F;
  float fontSizeTransition = 1.0F;
  double dropdownScrollRemainder = 0.0;
  mutable float lastQueryViewStartWidth = 0.0F;
  int dropdownScroll = 0;
  int dropdownHighlightedIndex = 0;
  int previousFontSize = kDefaultFontSize;
  std::size_t queryCursor = 0;
  std::size_t selectionAnchor = std::string::npos;
  std::string query;
  std::string status;
  std::string settingsStatus;
  pm::Package infoPackage;
  UserSettings settings;
  std::chrono::steady_clock::time_point animationEpoch =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point lastCaretActivity =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point lastUpdateTime =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point nextSystemThemeCheck =
      std::chrono::steady_clock::now();
  float frameDeltaSeconds = 0.0F;
  mutable float cornerScale = 1.0F;
  mutable std::unordered_map<std::string, float> hoverAmounts;
  mutable std::unordered_map<std::string, ButtonPress> buttonPressTimes;
  mutable std::unordered_map<std::string, float> switchPositions;
  mutable std::unordered_map<std::string, PackageAppearance>
      packageAppearanceStarts;
  std::vector<pm::Package> packages;
  std::vector<pm::Package> savedPackages;
  std::vector<render::FontFace> fontFaces;
  std::vector<std::string> fontFamilies;
  std::vector<render::FontFace> dropdownChoices;
  std::vector<std::string> dropdownThemeChoices;
  std::vector<CustomTheme> customThemes;
  std::vector<std::string> dropdownLanguageChoices;
  std::vector<SparkParticle> sparkParticles;
  std::vector<AnimatedGlyph> queryGlyphs;
  std::vector<DisappearingGlyph> disappearingGlyphs;
  std::string activeActionPackage;
  std::atomic<float> searchProgress{0.0F};
  std::future<std::vector<pm::Package>> searchTask;
  std::future<void> actionTask;

  Impl(core::Window& owner, render::Renderer& rendererRef,
       pm::Manager selectedManager)
      : window(owner.nativeHandle()),
        renderer(rendererRef),
        packageManager(selectedManager),
        catalog(i18n::Catalog::fromSystemLocale()) {
    settings = loadSettings();
    customThemes = loadCustomThemes();
    if (!settings.customTheme.empty() &&
        std::none_of(customThemes.begin(), customThemes.end(), [this](const CustomTheme& theme) {
          return theme.path == settings.customTheme;
        })) {
      settings.customTheme.clear();
    }
    catalog = i18n::Catalog::fromLanguage(
        settings.language == "system" ? std::string_view{} : settings.language);
    status = catalog.text("status.initial");
    if (!settingsPath().empty() && !std::filesystem::exists(settingsPath()) &&
        std::filesystem::exists(legacySettingsPath())) {
      try { saveSettings(settings); } catch (...) { }
    }
    if (settings.restoreLastSearch) {
      query = settings.lastSearch;
      queryCursor = query.size();
    }
    renderer.setFontSize(settings.fontSize);
    previousFontSize = settings.fontSize;
    systemLightTheme = systemPreferredLightTheme().value_or(false);
    nextSystemThemeCheck =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    pointerCursor = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    textCursor = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    fontFaces = renderer.fontFaces();
    if (!settings.fontPath.empty()) {
      try {
        renderer.setFontFace(settings.fontPath);
      } catch (const std::exception& error) {
        settingsStatus = catalog.format(
            "settings.error.loadFont", {{"error", error.what()}});
        // Fall back to the system font instead of pinning a replacement path.
        settings.fontPath.clear();
      }
    }
    if (std::none_of(fontFaces.begin(), fontFaces.end(),
                     [this](const render::FontFace& font) {
                       return font.path == renderer.fontFacePath();
                     })) {
      fontFaces.push_back({renderer.fontFacePath(), renderer.fontFamily(),
                           renderer.fontStyle()});
      std::sort(fontFaces.begin(), fontFaces.end(),
                [](const render::FontFace& left, const render::FontFace& right) {
                  if (left.family != right.family) {
                    return left.family < right.family;
                  }
                  if (left.style != right.style) {
                    return left.style < right.style;
                  }
                  return left.path < right.path;
                });
    }
    for (const render::FontFace& font : fontFaces) {
      if (fontFamilies.empty() || fontFamilies.back() != font.family) {
        fontFamilies.push_back(font.family);
      }
    }
    if (packageManager == pm::Manager::none) {
      status = catalog.text("status.unsupported");
    }
    syncQueryGlyphs();
    glfwSetWindowUserPointer(window, this);
    glfwSetCharCallback(window, onCharacter);
    glfwSetKeyCallback(window, onKey);
    glfwSetScrollCallback(window, onScroll);
  }

  ~Impl() {
    glfwSetCharCallback(window, nullptr);
    glfwSetKeyCallback(window, nullptr);
    glfwSetScrollCallback(window, nullptr);
    glfwSetWindowUserPointer(window, nullptr);
    glfwSetCursor(window, nullptr);
    if (pointerCursor != nullptr) {
      glfwDestroyCursor(pointerCursor);
    }
    if (textCursor != nullptr) {
      glfwDestroyCursor(textCursor);
    }
  }

  bool themeIsLight() const {
    if (!settings.customTheme.empty()) {
      const auto selected = std::find_if(
          customThemes.begin(), customThemes.end(), [this](const CustomTheme& theme) {
            return theme.path == settings.customTheme;
          });
      if (selected != customThemes.end()) return selected->light;
    }
    return settings.themeMode == ThemeMode::light ||
           (settings.themeMode == ThemeMode::system && systemLightTheme);
  }

  const Palette& currentPalette() const {
    if (!settings.customTheme.empty()) {
      const auto selected = std::find_if(
          customThemes.begin(), customThemes.end(), [this](const CustomTheme& theme) {
            return theme.path == settings.customTheme;
          });
      if (selected != customThemes.end()) return selected->palette;
    }
    return themeIsLight() ? kLightPalette : kDarkPalette;
  }

  std::string currentThemeName() const {
    if (!settings.customTheme.empty()) {
      const auto selected = std::find_if(
          customThemes.begin(), customThemes.end(), [this](const CustomTheme& theme) {
            return theme.path == settings.customTheme;
          });
      if (selected != customThemes.end()) return selected->name;
    }
    return themeModeLabel(settings.themeMode, catalog);
  }

  void updateCursor(const Layout& layout) const {
    bool pointer = false;
    bool text = false;
    if (packageInfoOpen || infoTransition > 0.005F) {
      pointer = contains(layout.infoDialogCloseButton, cursorX, cursorY);
    } else if (updatesOpen) {
      pointer = contains(layout.cancelUpdateButton, cursorX, cursorY) ||
                contains(layout.confirmUpdateButton, cursorX, cursorY);
    } else if (cacheConfirmOpen) {
      pointer = contains(layout.cancelCacheButton, cursorX, cursorY) ||
                contains(layout.confirmCacheButton, cursorX, cursorY);
    } else if (settingsOpen) {
      pointer =
          contains(layout.settingsHeaderCloseButton, cursorX, cursorY) ||
          contains(layout.closeSettingsButton, cursorX, cursorY) ||
          contains(layout.appearanceTab, cursorX, cursorY) ||
          contains(layout.behaviorTab, cursorX, cursorY) ||
          contains(layout.themesTab, cursorX, cursorY) ||
          contains(layout.systemTab, cursorX, cursorY);
      if (settingsCategory == SettingsCategory::appearance) {
        pointer = pointer || contains(layout.decreaseFontButton, cursorX, cursorY) ||
                  contains(layout.increaseFontButton, cursorX, cursorY) ||
                  contains(layout.fontFamilyButton, cursorX, cursorY) ||
                  contains(layout.fontStyleButton, cursorX, cursorY) ||
                  contains(layout.languageButton, cursorX, cursorY) ||
                  contains(layout.iconSizeDecreaseButton, cursorX, cursorY) ||
                  contains(layout.iconSizeIncreaseButton, cursorX, cursorY) ||
                  contains(layout.cornerRadiusDecreaseButton, cursorX, cursorY) ||
                  contains(layout.cornerRadiusIncreaseButton, cursorX, cursorY) ||
                  contains(layout.borderDecreaseButton, cursorX, cursorY) ||
                  contains(layout.borderIncreaseButton, cursorX, cursorY);
      } else if (settingsCategory == SettingsCategory::themes) {
        pointer = pointer || contains(layout.themeButton, cursorX, cursorY);
      } else if (settingsCategory == SettingsCategory::behavior) {
        pointer = pointer || contains(layout.densityButton, cursorX, cursorY) ||
                  contains(layout.descriptionsButton, cursorX, cursorY) ||
                  contains(layout.smoothScrollButton, cursorX, cursorY) ||
                  contains(layout.sparkButton, cursorX, cursorY) ||
                  contains(layout.animationButton, cursorX, cursorY) ||
                  contains(layout.restoreSearchButton, cursorX, cursorY);
      } else {
        pointer = pointer || contains(layout.clearCacheButton, cursorX, cursorY) ||
                  contains(layout.resetSettingsButton, cursorX, cursorY);
      }
      if (dropdownOpen) {
        const DropdownGeometry geometry = dropdownGeometry(layout);
        pointer = pointer || contains(geometry.panel, cursorX, cursorY);
      }
    } else if (!packageInfoOpen) {
      text = view == ViewMode::packages &&
             contains(layout.searchField, cursorX, cursorY);
      pointer = contains(layout.packagesButton, cursorX, cursorY) ||
                contains(layout.settingsButton, cursorX, cursorY) ||
                (packageManager != pm::Manager::none &&
                 (contains(layout.installedButton, cursorX, cursorY) ||
                  contains(layout.updatesButton, cursorX, cursorY))) ||
                (view != ViewMode::installed &&
                 contains(layout.searchButton, cursorX, cursorY)) ||
                (view == ViewMode::packages && layout.showClearQueryButton &&
                 clearQueryButtonTransition > 0.005F &&
                 contains(layout.clearQueryButton, cursorX, cursorY));
      pointer = pointer ||
                (layout.visibleRows > 0 &&
                 packages.size() > static_cast<std::size_t>(layout.visibleRows) &&
                 contains(scrollbarHoverBounds(layout), cursorX, cursorY));
      if (!busy()) {
        const std::size_t first = std::min(
            static_cast<std::size_t>(std::floor(scrollPosition)), packages.size());
        const float fraction = scrollPosition - std::floor(scrollPosition);
        const int rows = layout.drawableRows + (fraction > 0.0F ? 1 : 0);
        const float listBottom =
            layout.resultsPanel.y + layout.resultsPanel.height;
        const float cursorYFloat = static_cast<float>(cursorY);
        if (cursorYFloat >= layout.firstRowY && cursorYFloat < listBottom) {
          for (int row = 0; row < rows; ++row) {
            const std::size_t index = first + static_cast<std::size_t>(row);
            if (index >= packages.size()) {
              break;
            }
            const float rowY =
                layout.firstRowY +
                (static_cast<float>(row) - fraction) * layout.rowHeight;
            const Bounds help{
                layout.helpButtonX,
                rowY + (layout.rowHeight - layout.helpButtonWidth) / 2.0F,
                layout.helpButtonWidth, layout.helpButtonWidth};
            const Bounds action{
                layout.actionButtonX,
                rowY + (layout.rowHeight - layout.actionButtonHeight) / 2.0F,
                layout.actionButtonWidth, layout.actionButtonHeight};
            pointer = pointer || contains(help, cursorX, cursorY) ||
                      contains(action, cursorX, cursorY);
          }
        }
      }
    }

    text = text && !pointer;
    glfwSetCursor(window, text && textCursor != nullptr
                              ? textCursor
                              : pointer && pointerCursor != nullptr ? pointerCursor
                                                                    : nullptr);
  }

  bool busy() const {
    return searchTask.valid() || actionTask.valid();
  }

  Bounds scrollbarThumb(const Layout& layout) const {
    const Bounds& track = layout.packageScrollbarTrack;
    if (layout.visibleRows <= 0 ||
        packages.size() <= static_cast<std::size_t>(layout.visibleRows) ||
        track.height <= 0.0F) {
      return {};
    }
    const float thumbHeight =
        std::min(track.height,
                 std::max(28.0F, track.height *
                                     static_cast<float>(layout.visibleRows) /
                                     static_cast<float>(packages.size())));
    const int maxOffset =
        std::max(0, static_cast<int>(packages.size()) - layout.visibleRows);
    const float scrollFraction =
        maxOffset > 0
            ? std::clamp(scrollPosition / static_cast<float>(maxOffset), 0.0F, 1.0F)
            : 0.0F;
    return {track.x, track.y + (track.height - thumbHeight) * scrollFraction,
            track.width, thumbHeight};
  }

  Bounds scrollbarHoverBounds(const Layout& layout) const {
    Bounds bounds = layout.packageScrollbarTrack;
    bounds.x -= 5.0F;
    bounds.width += 5.0F;
    return bounds;
  }

  void setScrollFromCursor(const Layout& layout) {
    const Bounds& track = layout.packageScrollbarTrack;
    const Bounds thumb = scrollbarThumb(layout);
    const int maxOffset =
        std::max(0, static_cast<int>(packages.size()) - layout.visibleRows);
    const float movableTrack = track.height - thumb.height;
    if (maxOffset == 0 || movableTrack <= 0.0F) {
      return;
    }
    const float fraction = std::clamp(
        (static_cast<float>(cursorY) - track.y - thumb.height * 0.5F) /
            movableTrack,
        0.0F, 1.0F);
    scrollOffset = fraction * static_cast<float>(maxOffset);
    scrollPosition = scrollOffset;
  }

  static Impl* fromWindow(GLFWwindow* window) {
    return static_cast<Impl*>(glfwGetWindowUserPointer(window));
  }

  bool hasSelection() const {
    return selectionAnchor != std::string::npos && selectionAnchor != queryCursor;
  }

  void eraseSelection() {
    if (!hasSelection()) {
      selectionAnchor = std::string::npos;
      return;
    }
    const std::size_t first = std::min(selectionAnchor, queryCursor);
    const std::size_t last = std::max(selectionAnchor, queryCursor);
    query.erase(first, last - first);
    queryCursor = first;
    selectionAnchor = std::string::npos;
  }

  void moveCursor(std::size_t position, bool extendSelection) {
    if (extendSelection) {
      if (selectionAnchor == std::string::npos) {
        selectionAnchor = queryCursor;
      }
    } else {
      selectionAnchor = std::string::npos;
    }
    queryCursor = std::min(position, query.size());
    lastCaretActivity = std::chrono::steady_clock::now();
  }

  float glyphTransition(std::chrono::steady_clock::time_point started) const {
    const float elapsed =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - started)
            .count();
    const float progress = std::clamp(elapsed / 0.16F, 0.0F, 1.0F);
    return progress * progress * (3.0F - 2.0F * progress);
  }

  void syncQueryGlyphs() {
    const std::string placeholder = catalog.text("search.placeholder");
    const std::vector<std::string> target = splitCodepoints(
        query.empty() ? std::string_view(placeholder) : std::string_view(query));
    if (!settings.animations) {
      queryGlyphs.clear();
      queryGlyphs.reserve(target.size());
      const auto now = std::chrono::steady_clock::now();
      for (const std::string& glyph : target) {
        queryGlyphs.push_back({glyph, now, false});
      }
      disappearingGlyphs.clear();
      return;
    }

    const std::size_t oldCount = queryGlyphs.size();
    const std::size_t newCount = target.size();
    std::vector<std::vector<std::size_t>> common(
        oldCount + 1, std::vector<std::size_t>(newCount + 1));
    for (std::size_t oldIndex = oldCount; oldIndex > 0; --oldIndex) {
      for (std::size_t newIndex = newCount; newIndex > 0; --newIndex) {
        const std::size_t oldPosition = oldIndex - 1;
        const std::size_t newPosition = newIndex - 1;
        common[oldPosition][newPosition] =
            queryGlyphs[oldPosition].text == target[newPosition]
                ? 1 + common[oldPosition + 1][newPosition + 1]
                : std::max(common[oldPosition + 1][newPosition],
                           common[oldPosition][newPosition + 1]);
      }
    }

    const std::size_t missing = std::string::npos;
    std::vector<std::size_t> oldToNew(oldCount, missing);
    std::vector<std::size_t> newToOld(newCount, missing);
    std::size_t oldIndex = 0;
    std::size_t newIndex = 0;
    while (oldIndex < oldCount && newIndex < newCount) {
      if (queryGlyphs[oldIndex].text == target[newIndex]) {
        oldToNew[oldIndex] = newIndex;
        newToOld[newIndex] = oldIndex;
        ++oldIndex;
        ++newIndex;
      } else if (common[oldIndex + 1][newIndex] >=
                 common[oldIndex][newIndex + 1]) {
        ++oldIndex;
      } else {
        ++newIndex;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    float oldPrefixWidth = 0.0F;
    for (std::size_t index = 0; index < oldCount; ++index) {
      if (oldToNew[index] == missing) {
        const AnimatedGlyph& removed = queryGlyphs[index];
        const float opacity =
            removed.entering ? glyphTransition(removed.started) : 1.0F;
        if (opacity > 0.02F) {
          const float delay =
              (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) *
              0.18F;
          disappearingGlyphs.push_back(
              {removed.text, oldPrefixWidth - lastQueryViewStartWidth,
               now + std::chrono::duration_cast<
                         std::chrono::steady_clock::duration>(
                         std::chrono::duration<float>(delay)),
               opacity});
        }
      }
      oldPrefixWidth += renderer.measureText(queryGlyphs[index].text);
    }

    std::vector<AnimatedGlyph> updated;
    updated.reserve(newCount);
    for (std::size_t index = 0; index < newCount; ++index) {
      if (newToOld[index] != missing) {
        updated.push_back(queryGlyphs[newToOld[index]]);
      } else {
        updated.push_back({target[index], now, true});
      }
    }
    queryGlyphs = std::move(updated);
    std::erase_if(disappearingGlyphs, [now](const DisappearingGlyph& glyph) {
      return std::chrono::duration<float>(now - glyph.started).count() >= 0.16F;
    });
    if (disappearingGlyphs.size() > 512) {
      disappearingGlyphs.erase(disappearingGlyphs.begin(),
                               disappearingGlyphs.end() - 512);
    }
  }

  void queryChanged() {
    syncQueryGlyphs();
    lastCaretActivity = std::chrono::steady_clock::now();
    packages.clear();
    scrollOffset = 0;
    scrollPosition = 0.0F;
    status = catalog.text("status.pressSearch");
  }

  void clearQuery() {
    query.clear();
    queryCursor = 0;
    selectionAnchor = std::string::npos;
    queryChanged();
  }

  std::size_t previousWordPosition() const {
    std::size_t position = queryCursor;
    while (position > 0) {
      const std::size_t previous = previousCodepointPosition(query, position);
      const char32_t codepoint = decodeCodepoint(query, previous).value;
      if (codepoint >= 128 ||
          !std::isspace(static_cast<unsigned char>(codepoint))) {
        break;
      }
      position = previous;
    }
    while (position > 0) {
      const std::size_t previous = previousCodepointPosition(query, position);
      const char32_t codepoint = decodeCodepoint(query, previous).value;
      if (codepoint < 128 &&
          std::isspace(static_cast<unsigned char>(codepoint))) {
        break;
      }
      position = previous;
    }
    return position;
  }

  std::pair<std::size_t, std::size_t> queryViewRange(float availableWidth) const {
    const float width = std::max(0.0F, availableWidth);
    std::size_t start = queryCursor;
    std::size_t end = queryCursor;
    float usedWidth = 0.0F;
    float rightWidth = 0.0F;
    const float rightBudget = width * 0.55F;

    while (end < query.size()) {
      const std::size_t next = nextCodepointPosition(query, end);
      const float glyphWidth =
          renderer.measureText(std::string_view(query).substr(end, next - end));
      if ((usedWidth + glyphWidth > width && end != queryCursor) ||
          (rightWidth + glyphWidth > rightBudget && end != queryCursor)) {
        break;
      }
      end = next;
      usedWidth += glyphWidth;
      rightWidth += glyphWidth;
    }
    while (start > 0) {
      const std::size_t previous = previousCodepointPosition(query, start);
      const float glyphWidth =
          renderer.measureText(std::string_view(query).substr(previous, start - previous));
      if (usedWidth + glyphWidth > width) {
        break;
      }
      start = previous;
      usedWidth += glyphWidth;
    }
    while (end < query.size()) {
      const std::size_t next = nextCodepointPosition(query, end);
      const float glyphWidth =
          renderer.measureText(std::string_view(query).substr(end, next - end));
      if (usedWidth + glyphWidth > width) {
        break;
      }
      end = next;
      usedWidth += glyphWidth;
    }
    return {start, end};
  }

  std::size_t queryPositionAtCursor(float availableWidth, float textLeft) const {
    const auto [start, end] = queryViewRange(availableWidth);
    const float targetX =
        static_cast<float>(std::max(0.0, cursorX - static_cast<double>(textLeft)));
    float textWidth = 0.0F;
    for (std::size_t position = start; position < end;) {
      const std::size_t next = nextCodepointPosition(query, position);
      const float glyphWidth =
          renderer.measureText(std::string_view(query).substr(position, next - position));
      if (targetX < textWidth + glyphWidth * 0.5F) {
        return position;
      }
      textWidth += glyphWidth;
      position = next;
    }
    return end;
  }

  std::size_t nextWordPosition() const {
    std::size_t position = queryCursor;
    while (position < query.size()) {
      const char32_t codepoint = decodeCodepoint(query, position).value;
      if (codepoint < 128 &&
          std::isspace(static_cast<unsigned char>(codepoint))) {
        break;
      }
      position = nextCodepointPosition(query, position);
    }
    while (position < query.size()) {
      const char32_t codepoint = decodeCodepoint(query, position).value;
      if (codepoint >= 128 ||
          !std::isspace(static_cast<unsigned char>(codepoint))) {
        break;
      }
      position = nextCodepointPosition(query, position);
    }
    return position;
  }

  static void onCharacter(GLFWwindow* window, unsigned int codepoint) {
    Impl* self = fromWindow(window);
    if (self == nullptr || self->busy() || self->settingsOpen || self->view != ViewMode::packages || !self->searchFocused ||
        self->cacheConfirmOpen || self->packageInfoOpen ||
        self->infoTransition > 0.005F || codepoint < 32 ||
        (codepoint >= 0x7F && codepoint <= 0x9F) || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return;
    }
    const std::size_t selectedBytes =
        self->hasSelection()
            ? std::max(self->selectionAnchor, self->queryCursor) -
                  std::min(self->selectionAnchor, self->queryCursor)
            : 0;
    const std::string inserted = encodeCodepoint(static_cast<char32_t>(codepoint));
    if (self->query.size() - selectedBytes + inserted.size() >
        kMaximumQueryBytes) {
      return;
    }
    self->eraseSelection();
    self->query.insert(self->queryCursor, inserted);
    self->queryCursor += inserted.size();
    self->queryChanged();
  }

  static void onKey(GLFWwindow* window, int key, int, int action, int modifiers) {
    Impl* self = fromWindow(window);
    if (self == nullptr || (action != GLFW_PRESS && action != GLFW_REPEAT)) {
      return;
    }
    if (key == GLFW_KEY_ESCAPE) {
      if (self->packageInfoOpen) {
        self->packageInfoOpen = false;
      } else if (self->dropdownOpen) {
        self->dropdownOpen = false;
      } else if (self->cacheConfirmOpen) {
        self->cacheConfirmOpen = false;
      } else if (self->updatesOpen) {
        self->updatesOpen = false;
      } else if (self->settingsOpen) {
        self->settingsOpen = false;
      } else if (self->view != ViewMode::packages) {
        self->showPackages();
      } else if (self->searchFocused && !self->query.empty()) {
        self->clearQuery();
      }
      return;
    }
    if (self->settingsOpen && self->dropdownOpen) {
      if (key == GLFW_KEY_UP) {
        self->moveDropdownHighlight(-1);
        return;
      }
      if (key == GLFW_KEY_DOWN) {
        self->moveDropdownHighlight(1);
        return;
      }
      if (key == GLFW_KEY_HOME) {
        self->dropdownHighlightedIndex = 0;
        self->dropdownScroll = 0;
        return;
      }
      if (key == GLFW_KEY_END) {
        self->dropdownHighlightedIndex =
            static_cast<int>(self->dropdownChoiceCount()) - 1;
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        const Layout layout = makeLayout(width, height, self->renderer,
                                         self->settings.compactList);
        const int visibleRows = self->dropdownGeometry(layout).visibleRows;
        self->dropdownScroll = std::max(
            0, self->dropdownHighlightedIndex - visibleRows + 1);
        return;
      }
      if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        self->selectDropdownChoice(self->dropdownHighlightedIndex);
        return;
      }
    }
    if (self->busy() || self->settingsOpen || !self->searchFocused) {
      return;
    }

    const bool control = (modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0;
    const bool shift = (modifiers & GLFW_MOD_SHIFT) != 0;
    if (control && key == GLFW_KEY_A) {
      self->selectionAnchor = 0;
      self->queryCursor = self->query.size();
      self->lastCaretActivity = std::chrono::steady_clock::now();
    } else if (control && (key == GLFW_KEY_C || key == GLFW_KEY_X)) {
      if (self->hasSelection()) {
        const std::size_t first =
            std::min(self->selectionAnchor, self->queryCursor);
        const std::size_t last =
            std::max(self->selectionAnchor, self->queryCursor);
        const std::string selected = self->query.substr(first, last - first);
        glfwSetClipboardString(window, selected.c_str());
        if (key == GLFW_KEY_X) {
          self->eraseSelection();
          self->queryChanged();
        }
      }
    } else if (control && key == GLFW_KEY_V) {
      const char* clipboard = glfwGetClipboardString(window);
      if (clipboard != nullptr) {
        std::string pasted;
        const std::string_view clipboardText(clipboard);
        const std::size_t selectedBytes =
            self->hasSelection()
                ? std::max(self->selectionAnchor, self->queryCursor) -
                      std::min(self->selectionAnchor, self->queryCursor)
                : 0;
        const std::size_t availableBytes =
            kMaximumQueryBytes - (self->query.size() - selectedBytes);
        for (std::size_t offset = 0; offset < clipboardText.size();) {
          const DecodedCodepoint decoded = decodeCodepoint(clipboardText, offset);
          offset += decoded.length;
          if (decoded.value < 32 ||
              (decoded.value >= 0x7F && decoded.value <= 0x9F)) {
            continue;
          }
          const std::string encoded = encodeCodepoint(decoded.value);
          if (pasted.size() + encoded.size() > availableBytes) {
            break;
          }
          pasted += encoded;
        }
        if (!pasted.empty()) {
          self->eraseSelection();
          self->query.insert(self->queryCursor, pasted);
          self->queryCursor += pasted.size();
          self->queryChanged();
        }
      }
    } else if (key == GLFW_KEY_BACKSPACE) {
      if (self->hasSelection()) {
        self->eraseSelection();
        self->queryChanged();
      } else if (self->queryCursor > 0) {
        const std::size_t first = control ? self->previousWordPosition()
                                          : previousCodepointPosition(
                                                self->query, self->queryCursor);
        self->query.erase(first, self->queryCursor - first);
        self->queryCursor = first;
        self->selectionAnchor = std::string::npos;
        self->queryChanged();
      }
    } else if (key == GLFW_KEY_DELETE) {
      if (self->hasSelection()) {
        self->eraseSelection();
        self->queryChanged();
      } else if (self->queryCursor < self->query.size()) {
        const std::size_t last = control ? self->nextWordPosition()
                                         : nextCodepointPosition(
                                               self->query, self->queryCursor);
        self->query.erase(self->queryCursor, last - self->queryCursor);
        self->queryChanged();
      }
    } else if (key == GLFW_KEY_LEFT) {
      const std::size_t position =
          control ? self->previousWordPosition()
                  : (self->hasSelection() && !shift
                         ? std::min(self->selectionAnchor, self->queryCursor)
                         : previousCodepointPosition(self->query,
                                                     self->queryCursor));
      self->moveCursor(position, shift);
    } else if (key == GLFW_KEY_RIGHT) {
      const std::size_t position =
          control ? self->nextWordPosition()
                  : (self->hasSelection() && !shift
                         ? std::max(self->selectionAnchor, self->queryCursor)
                         : nextCodepointPosition(self->query, self->queryCursor));
      self->moveCursor(position, shift);
    } else if (key == GLFW_KEY_HOME) {
      self->moveCursor(0, shift);
    } else if (key == GLFW_KEY_END) {
      self->moveCursor(self->query.size(), shift);
    } else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
      self->selectionAnchor = std::string::npos;
      self->startSearch();
    }
  }

  static void onScroll(GLFWwindow* window, double, double yOffset) {
    Impl* self = fromWindow(window);
    if (self == nullptr || self->busy()) {
      return;
    }
    if (self->packageInfoOpen || self->infoTransition > 0.005F) {
      self->infoScrollOffset = std::max(
          0.0F, self->infoScrollOffset - static_cast<float>(yOffset) * 3.0F);
      return;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    const Layout layout = makeLayout(width, height, self->renderer,
                                     self->settings.compactList);
    if (self->settingsOpen && self->dropdownOpen) {
      const DropdownGeometry geometry = self->dropdownGeometry(layout);
      const int maxScroll = std::max(
          0, static_cast<int>(self->dropdownChoiceCount()) -
                 geometry.visibleRows);
      self->dropdownScrollRemainder -= yOffset;
      const int delta = static_cast<int>(self->dropdownScrollRemainder);
      self->dropdownScrollRemainder -= static_cast<double>(delta);
      self->dropdownScroll =
          std::clamp(self->dropdownScroll + delta, 0, maxScroll);
      if (self->dropdownScroll == 0 || self->dropdownScroll == maxScroll) {
        self->dropdownScrollRemainder = 0.0;
      }
      if (self->dropdownHighlightedIndex < self->dropdownScroll) {
        self->dropdownHighlightedIndex = self->dropdownScroll;
      } else if (self->dropdownHighlightedIndex >=
                 self->dropdownScroll + geometry.visibleRows) {
        self->dropdownHighlightedIndex =
            self->dropdownScroll + geometry.visibleRows - 1;
      }
      return;
    }
    if (self->settingsOpen || self->packages.empty()) {
      return;
    }
    const float maxOffset = static_cast<float>(std::max(
        0, static_cast<int>(self->packages.size()) - layout.visibleRows));
    self->scrollOffset = std::clamp(
        self->scrollOffset - static_cast<float>(yOffset), 0.0F, maxOffset);
    if (!self->settings.smoothScroll) {
      self->scrollPosition = self->scrollOffset;
    }
  }

  void startSearch() {
    if (busy()) {
      return;
    }
    if (packageManager == pm::Manager::none) {
      status = catalog.text("status.unsupported");
      return;
    }
    if (query.empty()) {
      beginPackageListExit();
      status = catalog.text("status.enterQuery");
      return;
    }

    const std::string searchTerm = query;
    if (settings.restoreLastSearch) {
      settings.lastSearch = query;
      persistSettings();
    }
    beginPackageListExit();
    pendingView = ViewMode::packages;
    const std::string managerName =
        packageManager == pm::Manager::dnf ? "DNF" : "pacman";
    status = catalog.format("status.searching", {{"manager", managerName}});
    searchProgress.store(0.0F, std::memory_order_relaxed);
    searchTask = std::async(std::launch::async, [searchTerm, this] {
      return pm::searchPackages(packageManager, searchTerm, [this](float progress) {
        searchProgress.store(progress, std::memory_order_relaxed);
      });
    });
  }

  void startUpdateSearch() {
    if (busy() || packageManager == pm::Manager::none) return;
    beginPackageListExit();
    pendingView = ViewMode::updates;
    const pm::Manager manager = packageManager;
    status = catalog.text("updates.searching");
    searchProgress.store(0.0F, std::memory_order_relaxed);
    searchTask = std::async(std::launch::async, [manager, this] {
      return pm::listUpdates(manager, [this](float progress) {
        searchProgress.store(progress, std::memory_order_relaxed);
      });
    });
  }

  void startInstalledSearch() {
    if (busy() || packageManager == pm::Manager::none) return;
    beginPackageListExit();
    pendingView = ViewMode::installed;
    const pm::Manager manager = packageManager;
    status = catalog.text("installed.searching");
    searchProgress.store(0.0F, std::memory_order_relaxed);
    searchTask = std::async(std::launch::async, [manager, this] {
      return pm::listInstalled(manager, [this](float progress) {
        searchProgress.store(progress, std::memory_order_relaxed);
      });
    });
  }

  void showUpdates() {
    if (busy() || packageManager == pm::Manager::none) return;
    if (view == ViewMode::updates) {
      startUpdateSearch();
      return;
    }
    if (view == ViewMode::packages) {
      savedPackages = std::move(packages);
    }
    packageAppearanceStarts.clear();
    view = ViewMode::updates;
    scrollOffset = scrollPosition = 0.0F;
    searchFocused = false;
    startUpdateSearch();
  }

  void showInstalled() {
    if (busy() || packageManager == pm::Manager::none) return;
    if (view == ViewMode::installed) {
      startInstalledSearch();
      return;
    }
    if (view == ViewMode::packages) {
      savedPackages = std::move(packages);
    }
    packageAppearanceStarts.clear();
    view = ViewMode::installed;
    scrollOffset = scrollPosition = 0.0F;
    searchFocused = false;
    startInstalledSearch();
  }

  void showPackages() {
    if (view == ViewMode::packages) return;
    packages = std::move(savedPackages);
    packageAppearanceStarts.clear();
    view = ViewMode::packages;
    packageListExiting = false;
    scrollOffset = scrollPosition = 0.0F;
    searchFocused = true;
    status = packages.empty() ? catalog.text("status.enterQuery")
                              : catalog.format("status.foundResults",
                                  {{"count", std::to_string(packages.size())}});
  }

  void beginPackageListExit() {
    if (settings.animations && !packages.empty()) {
      packageListExiting = true;
      packageListTransition = 1.0F;
      return;
    }
    packages.clear();
    packageAppearanceStarts.clear();
    packageListExiting = false;
    packageListTransition = 1.0F;
    scrollOffset = 0.0F;
    scrollPosition = 0.0F;
  }

  void startAction(const pm::Package& package) {
    if (busy() || packageManager == pm::Manager::none) {
      return;
    }

    const std::string packageName = package.name;
    if (view == ViewMode::updates) {
      updateTask = true;
      cacheClearTask = false;
      activeActionPackage = packageName;
      status = catalog.format("status.updatingPackage", {{"package", packageName}});
      const pm::Manager manager = packageManager;
      actionTask = std::async(std::launch::async, [manager, packageName] {
        pm::updatePackage(manager, packageName);
      });
      return;
    }
    const bool installed = package.installed;
    cacheClearTask = false;
    activeActionPackage = packageName;
    status = catalog.format(installed ? "status.removing" : "status.installing",
                            {{"package", packageName}});
    const pm::Manager manager = packageManager;
    actionTask = std::async(std::launch::async, [packageName, installed, manager] {
      if (installed) {
        pm::removePackage(manager, packageName);
      } else {
        pm::installPackage(manager, packageName);
      }
    });
  }

  void startCacheClear() {
    if (busy() || packageManager == pm::Manager::none) {
      return;
    }
    cacheClearTask = true;
    activeActionPackage.clear();
    const pm::Manager manager = packageManager;
    const std::string managerName =
        manager == pm::Manager::dnf ? "DNF" : "pacman";
    status = catalog.format("status.clearingCache", {{"manager", managerName}});
    actionTask = std::async(std::launch::async,
                            [manager] { pm::clearPackageCache(manager); });
  }

  void startSystemUpdate() {
    if (busy() || packageManager == pm::Manager::none) {
      return;
    }
    updateTask = true;
    cacheClearTask = false;
    activeActionPackage.clear();
    const std::string managerName =
        packageManager == pm::Manager::dnf ? "DNF" : "pacman";
    status = catalog.format("status.updating", {{"manager", managerName}});
    const pm::Manager manager = packageManager;
    actionTask = std::async(std::launch::async,
                            [manager] { pm::updateAllPackages(manager); });
  }

  void updateSearch() {
    if (!searchTask.valid() ||
        searchTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return;
    }
    try {
      std::vector<pm::Package> results = searchTask.get();
      const ViewMode completedView = pendingView;
      pendingView = ViewMode::packages;
      if (completedView != view) return;
      packages = std::move(results);
      packageAppearanceStarts.clear();
      packageListExiting = false;
      packageListTransition = 1.0F;
      searchProgress.store(1.0F, std::memory_order_relaxed);
      scrollOffset = 0;
      scrollPosition = 0.0F;
      switch (view) {
        case ViewMode::updates:
          status = packages.empty()
                       ? catalog.text("updates.none")
                       : catalog.format("updates.found",
                            {{"count", std::to_string(packages.size())}});
          break;
        case ViewMode::installed:
          status = packages.empty()
                       ? catalog.text("installed.none")
                       : catalog.format("installed.found",
                            {{"count", std::to_string(packages.size())}});
          break;
        case ViewMode::packages:
          status = packages.empty()
                       ? catalog.text("status.noResults")
                       : catalog.format("status.foundResults",
                            {{"count", std::to_string(packages.size())}});
          break;
      }
    } catch (const std::exception& error) {
      const ViewMode completedView = pendingView;
      pendingView = ViewMode::packages;
      if (completedView == view) {
        status = catalog.format("status.searchFailed", {{"error", error.what()}});
      }
    }
  }

  void updateAction() {
    if (!actionTask.valid() ||
        actionTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return;
    }
    const bool wasCacheClear = cacheClearTask;
    const bool wasSystemUpdate = updateTask;
    cacheClearTask = false;
    updateTask = false;
    activeActionPackage.clear();
    try {
      actionTask.get();
      if (wasCacheClear) {
        const std::string managerName =
            packageManager == pm::Manager::dnf ? "DNF" : "Pacman";
        status = catalog.format("status.cacheCleared", {{"manager", managerName}});
        return;
      }
      if (wasSystemUpdate) {
        status = catalog.text("status.updated");
        if (view == ViewMode::updates) {
          startUpdateSearch();
        } else if (view == ViewMode::installed) {
          startInstalledSearch();
        }
        return;
      }
      status = catalog.text("status.transactionFinished");
      if (view == ViewMode::updates) {
        startUpdateSearch();
      } else if (view == ViewMode::installed) {
        startInstalledSearch();
      } else {
        startSearch();
      }
    } catch (const std::exception& error) {
      status = catalog.format("status.transactionFailed", {{"error", error.what()}});
    }
  }

  void changeFontSize(int delta) {
    const int size = std::clamp(renderer.fontSize() + delta, kMinimumFontSize,
                                kMaximumFontSize);
    if (size == renderer.fontSize()) {
      return;
    }

    try {
      previousFontSize = renderer.fontSize();
      renderer.setFontSize(size);
      settings.fontSize = size;
      fontSizeTransition = settings.animations ? 0.0F : 1.0F;
      persistSettings();
    } catch (const std::exception& error) {
      settingsStatus = catalog.format("settings.error.fontSize",
                                      {{"error", error.what()}});
    }
  }

  void selectFontFace(const render::FontFace& font) {
    if (font.path.empty()) {
      try {
        renderer.useSystemFont();
        settings.fontPath.clear();
        persistSettings();
      } catch (const std::exception& error) {
        settingsStatus = catalog.format("settings.error.font",
                                        {{"error", error.what()}});
      }
      return;
    }
    if (font.path == renderer.fontFacePath()) {
      return;
    }
    try {
      renderer.setFontFace(font.path);
      settings.fontPath = font.path;
      persistSettings();
    } catch (const std::exception& error) {
      settingsStatus = catalog.format("settings.error.font",
                                      {{"error", error.what()}});
    }
  }

  std::size_t currentDropdownIndex() const {
    if (dropdownKind == DropdownKind::language) {
      const auto current = std::find(dropdownLanguageChoices.begin(),
                                     dropdownLanguageChoices.end(),
                                     settings.language);
      return current == dropdownLanguageChoices.end()
                 ? 0
                 : static_cast<std::size_t>(
                       std::distance(dropdownLanguageChoices.begin(), current));
    }
    if (dropdownKind == DropdownKind::theme) {
      const std::string currentTheme = settings.customTheme.empty()
                                           ? std::string(themeModeName(settings.themeMode))
                                           : settings.customTheme;
      const auto current = std::find(dropdownThemeChoices.begin(),
                                     dropdownThemeChoices.end(), currentTheme);
      return current == dropdownThemeChoices.end()
                 ? 0
                 : static_cast<std::size_t>(
                       std::distance(dropdownThemeChoices.begin(), current));
    }
    const auto current = std::find_if(
        dropdownChoices.begin(), dropdownChoices.end(),
        [this](const render::FontFace& font) {
          if (dropdownKind == DropdownKind::fontFamily) {
            if (settings.fontPath.empty()) {
              return font.path.empty();
            }
            return !font.path.empty() && font.family == renderer.fontFamily();
          }
          return font.path == renderer.fontFacePath();
        });
    return current == dropdownChoices.end()
               ? 0
               : static_cast<std::size_t>(
                     std::distance(dropdownChoices.begin(), current));
  }

  std::size_t dropdownChoiceCount() const {
    if (dropdownKind == DropdownKind::theme) return dropdownThemeChoices.size();
    if (dropdownKind == DropdownKind::language) return dropdownLanguageChoices.size();
    return dropdownChoices.size();
  }

  bool dropdownChoiceIsCurrent(std::size_t index) const {
    if (dropdownKind == DropdownKind::theme) {
      return dropdownThemeChoices[index] ==
             (settings.customTheme.empty()
                  ? std::string(themeModeName(settings.themeMode))
                  : settings.customTheme);
    }
    if (dropdownKind == DropdownKind::language) {
      return dropdownLanguageChoices[index] == settings.language;
    }
    if (dropdownKind == DropdownKind::fontFamily) {
      if (settings.fontPath.empty()) {
        return dropdownChoices[index].path.empty();
      }
      return !dropdownChoices[index].path.empty() &&
             dropdownChoices[index].family == renderer.fontFamily();
    }
    return dropdownChoices[index].path == renderer.fontFacePath();
  }

  std::string dropdownChoiceLabel(std::size_t index) const {
    if (dropdownKind == DropdownKind::theme) {
      const std::string& theme = dropdownThemeChoices[index];
      if (theme == "system") return catalog.text("theme.system");
      if (theme == "light") return catalog.text("theme.light");
      if (theme == "dark") return catalog.text("theme.dark");
      const auto custom = std::find_if(customThemes.begin(), customThemes.end(),
          [&theme](const CustomTheme& candidate) { return candidate.path == theme; });
      if (custom != customThemes.end()) return custom->name;
      return theme;
    }
    if (dropdownKind == DropdownKind::language) {
      const std::string& language = dropdownLanguageChoices[index];
      return catalog.text("language." + language);
    }
    return dropdownKind == DropdownKind::fontFamily
               ? dropdownChoices[index].family
               : dropdownChoices[index].style;
  }

  void openDropdown(DropdownKind kind) {
    if (dropdownOpen && dropdownKind == kind) {
      dropdownOpen = false;
      return;
    }

    dropdownChoices.clear();
    dropdownThemeChoices.clear();
    dropdownLanguageChoices.clear();
    if (kind == DropdownKind::fontFamily) {
      dropdownChoices.push_back({"", catalog.text("font.systemDefault"), ""});
      for (const std::string& family : fontFamilies) {
        const auto regular = std::find_if(
            fontFaces.begin(), fontFaces.end(),
            [&family](const render::FontFace& font) {
              return font.family == family &&
                     (font.style == "Regular" || font.style == "Book" ||
                      font.style == "Normal");
            });
        const auto first = std::find_if(
            fontFaces.begin(), fontFaces.end(),
            [&family](const render::FontFace& font) {
              return font.family == family;
            });
        if (regular != fontFaces.end()) {
          dropdownChoices.push_back(*regular);
        } else if (first != fontFaces.end()) {
          dropdownChoices.push_back(*first);
        }
      }
    } else if (kind == DropdownKind::fontStyle) {
      for (const render::FontFace& font : fontFaces) {
        const bool duplicateStyle =
            std::any_of(dropdownChoices.begin(), dropdownChoices.end(),
                        [&font](const render::FontFace& choice) {
                          return choice.style == font.style;
                        });
        if (font.family == renderer.fontFamily() && !duplicateStyle) {
          dropdownChoices.push_back(font);
        }
      }
    } else if (kind == DropdownKind::theme) {
      dropdownThemeChoices = {"system", "light", "dark"};
      for (const CustomTheme& theme : customThemes) {
        dropdownThemeChoices.push_back(theme.path);
      }
    } else if (kind == DropdownKind::language) {
      dropdownLanguageChoices = {"system", "en", "ru", "ro"};
    }
    dropdownKind = kind;
    if (dropdownChoiceCount() == 0) {
      dropdownKind = DropdownKind::none;
      dropdownOpen = false;
      settingsStatus = catalog.text("settings.noFonts");
      return;
    }

    dropdownOpen = true;
    dropdownScroll = 0;
    dropdownScrollRemainder = 0.0;
    dropdownHighlightedIndex = static_cast<int>(currentDropdownIndex());
  }

  DropdownGeometry dropdownGeometry(const Layout& layout) const {
    const std::size_t choiceCount = dropdownChoiceCount();
    if (dropdownKind == DropdownKind::none || choiceCount == 0) {
      return {};
    }
    const Bounds& trigger =
        dropdownKind == DropdownKind::fontFamily
            ? layout.fontFamilyButton
            : dropdownKind == DropdownKind::fontStyle ? layout.fontStyleButton
            : dropdownKind == DropdownKind::language ? layout.languageButton
                                                     : layout.themeButton;
    const float rowHeight = std::clamp(
        static_cast<float>(renderer.fontSize()) * 1.65F * layout.settingsScale,
        24.0F, 38.0F);
    const float panelY =
        trigger.y + trigger.height + 6.0F * layout.settingsScale;
    const float panelBottom =
        layout.settingsPanel.y + layout.settingsPanel.height -
        8.0F * layout.settingsScale;
    const float availableHeight = std::max(0.0F, panelBottom - panelY - 8.0F);
    const int visibleRows = std::clamp(
        static_cast<int>(availableHeight / rowHeight), 1,
        std::min(6, static_cast<int>(choiceCount)));
    const float panelHeight = static_cast<float>(visibleRows) * rowHeight + 8.0F;
    const float animationOffset = (dropdownTransition - 1.0F) * 7.0F;
    return {{trigger.x, panelY + animationOffset, trigger.width, panelHeight},
            rowHeight, visibleRows};
  }

  void selectDropdownChoice(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < dropdownChoiceCount()) {
      const std::size_t choiceIndex = static_cast<std::size_t>(index);
      if (dropdownKind == DropdownKind::theme) {
        const std::string choice = dropdownThemeChoices[choiceIndex];
        if (choice == "system" || choice == "light" || choice == "dark") {
          settings.customTheme.clear();
          settings.themeMode = choice == "light" ? ThemeMode::light
                               : choice == "dark" ? ThemeMode::dark
                                                   : ThemeMode::system;
        } else {
          settings.customTheme = choice;
        }
        if (settings.customTheme.empty() && settings.themeMode == ThemeMode::system) {
          systemLightTheme = systemPreferredLightTheme().value_or(false);
          nextSystemThemeCheck =
              std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
        persistSettings();
      } else if (dropdownKind == DropdownKind::language) {
        settings.language = dropdownLanguageChoices[choiceIndex];
        catalog = i18n::Catalog::fromLanguage(
            settings.language == "system" ? std::string_view{} : settings.language);
        status = catalog.text("status.initial");
        settingsStatus.clear();
        persistSettings();
      } else {
        selectFontFace(dropdownChoices[choiceIndex]);
      }
    }
    dropdownOpen = false;
  }

  void moveDropdownHighlight(int delta) {
    const std::size_t choiceCount = dropdownChoiceCount();
    if (!dropdownOpen || choiceCount == 0) {
      return;
    }
    const int last = static_cast<int>(choiceCount) - 1;
    dropdownHighlightedIndex =
        std::clamp(dropdownHighlightedIndex + delta, 0, last);
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    const Layout layout =
        makeLayout(width, height, renderer, settings.compactList);
    const int visibleRows = dropdownGeometry(layout).visibleRows;
    if (dropdownHighlightedIndex < dropdownScroll) {
      dropdownScroll = dropdownHighlightedIndex;
    } else if (dropdownHighlightedIndex >= dropdownScroll + visibleRows) {
      dropdownScroll = dropdownHighlightedIndex - visibleRows + 1;
    }
  }

  void resetSettings() {
    const int oldFontSize = renderer.fontSize();
    try {
      renderer.useSystemFont();
      renderer.setFontSize(kDefaultFontSize);
      settings = UserSettings{};
      catalog = i18n::Catalog::fromSystemLocale();
      status = catalog.text("status.initial");
      previousFontSize = oldFontSize;
      fontSizeTransition = settings.animations ? 0.0F : 1.0F;
      systemLightTheme = systemPreferredLightTheme().value_or(false);
      nextSystemThemeCheck =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      persistSettings();
    } catch (const std::exception& error) {
      settingsStatus = catalog.format("settings.error.reset",
                                      {{"error", error.what()}});
    }
  }

  void persistSettings() {
    try {
      saveSettings(settings);
      settingsStatus = catalog.text("settings.saved");
    } catch (const std::exception& error) {
      settingsStatus = catalog.format("settings.error.save",
                                      {{"error", error.what()}});
    }
  }

  void handleClick(int framebufferWidth, int framebufferHeight) {
    const bool mouseDown =
        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (!mouseDown) {
      mouseWasDown = false;
      return;
    }
    if (mouseWasDown) {
      return;
    }
    mouseWasDown = true;

    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
      return;
    }

    double mouseX = 0.0;
    double mouseY = 0.0;
    glfwGetCursorPos(window, &mouseX, &mouseY);
    cursorX = mouseX * static_cast<double>(framebufferWidth) / windowWidth;
    cursorY = mouseY * static_cast<double>(framebufferHeight) / windowHeight;
    spawnClickSparks();
    const Layout layout = makeLayout(framebufferWidth, framebufferHeight,
                                     renderer, settings.compactList);

    if (updatesOpen) {
      if (contains(layout.cancelUpdateButton, cursorX, cursorY)) {
        triggerButtonPress(layout.cancelUpdateButton);
        updatesOpen = false;
      } else if (contains(layout.confirmUpdateButton, cursorX, cursorY)) {
        triggerButtonPress(layout.confirmUpdateButton);
        updatesOpen = false;
        searchFocused = false;
        startSystemUpdate();
      } else if (!contains(layout.updateDialogPanel, cursorX, cursorY)) {
        updatesOpen = false;
      }
      return;
    }

    if (cacheConfirmOpen) {
      if (contains(layout.cancelCacheButton, cursorX, cursorY)) {
        triggerButtonPress(layout.cancelCacheButton);
        cacheConfirmOpen = false;
      } else if (!contains(layout.cacheConfirmPanel, cursorX, cursorY)) {
        cacheConfirmOpen = false;
      } else if (contains(layout.confirmCacheButton, cursorX, cursorY)) {
        triggerButtonPress(layout.confirmCacheButton);
        cacheConfirmOpen = false;
        dropdownOpen = false;
        searchFocused = false;
        startCacheClear();
      }
      return;
    }

    if (packageInfoOpen || infoTransition > 0.005F) {
      if (contains(layout.infoDialogCloseButton, cursorX, cursorY)) {
        triggerButtonPress(layout.infoDialogCloseButton);
        packageInfoOpen = false;
      } else if (!contains(layout.infoDialogPanel, cursorX, cursorY)) {
        packageInfoOpen = false;
      }
      return;
    }

    if (contains(layout.packagesButton, cursorX, cursorY)) {
      triggerButtonPress(layout.packagesButton);
      showPackages();
      return;
    }

    if (packageManager != pm::Manager::none &&
        contains(layout.installedButton, cursorX, cursorY)) {
      triggerButtonPress(layout.installedButton);
      showInstalled();
      return;
    }

    if (packageManager != pm::Manager::none &&
        contains(layout.updatesButton, cursorX, cursorY)) {
      triggerButtonPress(layout.updatesButton);
      showUpdates();
      return;
    }

    if (contains(layout.settingsButton, cursorX, cursorY)) {
      triggerButtonPress(layout.settingsButton);
      settingsOpen = !settingsOpen;
      dropdownOpen = false;
      searchFocused = false;
      settingsStatus.clear();
      return;
    }

    if (settingsOpen) {
      if (contains(layout.appearanceTab, cursorX, cursorY)) {
        if (settingsCategory != SettingsCategory::appearance) categoryTransition = 0.0F;
        settingsCategory = SettingsCategory::appearance;
        dropdownOpen = false;
        settingsStatus.clear();
        return;
      }
      if (contains(layout.behaviorTab, cursorX, cursorY)) {
        if (settingsCategory != SettingsCategory::behavior) categoryTransition = 0.0F;
        settingsCategory = SettingsCategory::behavior;
        dropdownOpen = false;
        settingsStatus.clear();
        return;
      }
      if (contains(layout.themesTab, cursorX, cursorY)) {
        if (settingsCategory != SettingsCategory::themes) categoryTransition = 0.0F;
        settingsCategory = SettingsCategory::themes;
        dropdownOpen = false;
        settingsStatus.clear();
        return;
      }
      if (contains(layout.systemTab, cursorX, cursorY)) {
        if (settingsCategory != SettingsCategory::system) categoryTransition = 0.0F;
        settingsCategory = SettingsCategory::system;
        dropdownOpen = false;
        settingsStatus.clear();
        return;
      }
      if (dropdownOpen) {
        const DropdownGeometry geometry = dropdownGeometry(layout);
        if (contains(geometry.panel, cursorX, cursorY)) {
          const float rowPosition =
              (static_cast<float>(cursorY) - geometry.panel.y - 4.0F) /
              geometry.rowHeight;
          if (rowPosition >= 0.0F) {
            const int row = static_cast<int>(rowPosition);
            if (row < geometry.visibleRows) {
              selectDropdownChoice(dropdownScroll + row);
            }
          }
          return;
        }
        const Bounds& trigger =
            dropdownKind == DropdownKind::fontFamily
                ? layout.fontFamilyButton
                : dropdownKind == DropdownKind::fontStyle
                      ? layout.fontStyleButton
                : dropdownKind == DropdownKind::language ? layout.languageButton
                                                         : layout.themeButton;
        if (contains(trigger, cursorX, cursorY)) {
          dropdownOpen = false;
          return;
        }
        dropdownOpen = false;
      }
      if (settingsCategory == SettingsCategory::appearance) {
        if (contains(layout.decreaseFontButton, cursorX, cursorY)) {
          triggerButtonPress(layout.decreaseFontButton);
          changeFontSize(-2);
        } else if (contains(layout.increaseFontButton, cursorX, cursorY)) {
          triggerButtonPress(layout.increaseFontButton);
          changeFontSize(2);
        } else if (contains(layout.fontFamilyButton, cursorX, cursorY)) {
          triggerButtonPress(layout.fontFamilyButton);
          openDropdown(DropdownKind::fontFamily);
        } else if (contains(layout.fontStyleButton, cursorX, cursorY)) {
          triggerButtonPress(layout.fontStyleButton);
          openDropdown(DropdownKind::fontStyle);
        } else if (contains(layout.languageButton, cursorX, cursorY)) {
          triggerButtonPress(layout.languageButton);
          openDropdown(DropdownKind::language);
        } else if (contains(layout.iconSizeDecreaseButton, cursorX, cursorY) ||
                   contains(layout.iconSizeIncreaseButton, cursorX, cursorY)) {
          const bool increase = contains(layout.iconSizeIncreaseButton,
                                         cursorX, cursorY);
          triggerButtonPress(increase ? layout.iconSizeIncreaseButton
                                      : layout.iconSizeDecreaseButton);
          settings.iconScale = std::clamp(
              std::round((settings.iconScale + (increase ? 0.1F : -0.1F)) * 10.0F) /
                  10.0F,
              0.6F, 1.8F);
          persistSettings();
        } else if (contains(layout.cornerRadiusDecreaseButton, cursorX, cursorY) ||
                   contains(layout.cornerRadiusIncreaseButton, cursorX, cursorY)) {
          const bool increase = contains(layout.cornerRadiusIncreaseButton,
                                         cursorX, cursorY);
          triggerButtonPress(increase ? layout.cornerRadiusIncreaseButton
                                      : layout.cornerRadiusDecreaseButton);
          settings.cornerRadius = std::clamp(
              settings.cornerRadius + (increase ? 2.0F : -2.0F), 0.0F, 32.0F);
          persistSettings();
        } else if (contains(layout.borderDecreaseButton, cursorX, cursorY) ||
                   contains(layout.borderIncreaseButton, cursorX, cursorY)) {
          const bool increase = contains(layout.borderIncreaseButton,
                                         cursorX, cursorY);
          triggerButtonPress(increase ? layout.borderIncreaseButton
                                      : layout.borderDecreaseButton);
          settings.borderSize = std::clamp(
              settings.borderSize + (increase ? 0.5F : -0.5F), 0.0F, 6.0F);
          persistSettings();
        }
      } else if (settingsCategory == SettingsCategory::themes) {
        if (contains(layout.themeButton, cursorX, cursorY)) {
          triggerButtonPress(layout.themeButton);
          openDropdown(DropdownKind::theme);
        }
      } else if (settingsCategory == SettingsCategory::behavior) {
        if (contains(layout.densityButton, cursorX, cursorY)) {
          triggerButtonPress(layout.densityButton);
          settings.compactList = !settings.compactList;
          persistSettings();
        } else if (contains(layout.smoothScrollButton, cursorX, cursorY)) {
          triggerButtonPress(layout.smoothScrollButton);
          settings.smoothScroll = !settings.smoothScroll;
          if (!settings.smoothScroll) {
            scrollPosition = scrollOffset;
          }
          persistSettings();
        } else if (contains(layout.sparkButton, cursorX, cursorY)) {
          triggerButtonPress(layout.sparkButton);
          settings.clickSparks = !settings.clickSparks;
          if (!settings.clickSparks) {
            sparkParticles.clear();
          }
          persistSettings();
        } else if (contains(layout.animationButton, cursorX, cursorY)) {
          triggerButtonPress(layout.animationButton);
          settings.animations = !settings.animations;
          persistSettings();
        } else if (contains(layout.descriptionsButton, cursorX, cursorY)) {
          triggerButtonPress(layout.descriptionsButton);
          settings.showPackageDescriptions = !settings.showPackageDescriptions;
          persistSettings();
        } else if (contains(layout.restoreSearchButton, cursorX, cursorY)) {
          triggerButtonPress(layout.restoreSearchButton);
          settings.restoreLastSearch = !settings.restoreLastSearch;
          if (!settings.restoreLastSearch) {
            settings.lastSearch.clear();
          } else {
            settings.lastSearch = query;
          }
          persistSettings();
        }
      } else {
        if (contains(layout.clearCacheButton, cursorX, cursorY)) {
          triggerButtonPress(layout.clearCacheButton);
          dropdownOpen = false;
          cacheConfirmOpen = true;
        } else if (contains(layout.resetSettingsButton, cursorX, cursorY)) {
          triggerButtonPress(layout.resetSettingsButton);
          resetSettings();
        }
      }
      if (contains(layout.closeSettingsButton, cursorX, cursorY)) {
        triggerButtonPress(layout.closeSettingsButton);
        persistSettings();
      } else if (contains(layout.settingsHeaderCloseButton, cursorX, cursorY)) {
        triggerButtonPress(layout.settingsHeaderCloseButton);
        settingsOpen = false;
        dropdownOpen = false;
      }
      return;
    }

    if (view == ViewMode::packages && layout.showClearQueryButton &&
        clearQueryButtonTransition > 0.5F &&
        contains(layout.clearQueryButton, cursorX, cursorY)) {
      triggerButtonPress(layout.clearQueryButton);
      searchFocused = true;
      clearQuery();
      return;
    }
    if (view == ViewMode::packages &&
        contains(layout.searchField, cursorX, cursorY)) {
      searchFocused = true;
      queryCursor = queryPositionAtCursor(layout.queryTextWidth, layout.queryTextX);
      selectionAnchor = queryCursor;
      draggingSearchSelection = true;
      lastCaretActivity = std::chrono::steady_clock::now();
      return;
    }
    if (view != ViewMode::installed &&
        contains(layout.searchButton, cursorX, cursorY)) {
      triggerButtonPress(layout.searchButton);
      searchFocused = false;
      if (view == ViewMode::updates) updatesOpen = true;
      else startSearch();
      return;
    }
    if (!busy() && layout.visibleRows > 0 &&
        packages.size() > static_cast<std::size_t>(layout.visibleRows) &&
        contains(scrollbarHoverBounds(layout), cursorX, cursorY)) {
      searchFocused = false;
      draggingPackageScrollbar = true;
      setScrollFromCursor(layout);
      return;
    }

    searchFocused = false;
    if (busy()) {
      return;
    }
    const float listBottom =
        layout.resultsPanel.y + layout.resultsPanel.height;
    const float cursorYFloat = static_cast<float>(cursorY);
    if (cursorYFloat < layout.firstRowY || cursorYFloat >= listBottom) {
      return;
    }
    const std::size_t first =
        std::min(static_cast<std::size_t>(std::floor(scrollPosition)), packages.size());
    const float fractionalScroll =
        scrollPosition - std::floor(scrollPosition);
    for (int row = 0; row < layout.drawableRows + (fractionalScroll > 0.0F ? 1 : 0);
         ++row) {
      const std::size_t packageIndex =
          first + static_cast<std::size_t>(row);
      if (packageIndex >= packages.size()) {
        break;
      }
      const Bounds helpButton{
          layout.helpButtonX,
          layout.firstRowY +
              (static_cast<float>(row) - fractionalScroll) * layout.rowHeight +
              (layout.rowHeight - layout.helpButtonWidth) / 2.0F,
          layout.helpButtonWidth, layout.helpButtonWidth};
      const Bounds actionButton{
          layout.actionButtonX,
          layout.firstRowY +
              (static_cast<float>(row) - fractionalScroll) * layout.rowHeight +
              (layout.rowHeight - layout.actionButtonHeight) / 2.0F,
          layout.actionButtonWidth, layout.actionButtonHeight};
      if (contains(helpButton, cursorX, cursorY)) {
        triggerButtonPress(helpButton,
                           "package-help:" + packages[packageIndex].name);
        infoPackage = packages[packageIndex];
        infoScrollOffset = 0.0F;
        packageInfoOpen = true;
        searchFocused = false;
        return;
      }
      if (contains(actionButton, cursorX, cursorY)) {
        triggerButtonPress(actionButton,
                           "package-action:" + packages[packageIndex].name);
        startAction(packages[packageIndex]);
        return;
      }
    }
  }

  void spawnClickSparks() {
    if (!settings.clickSparks) {
      return;
    }
    constexpr int particleCount = 12;
    constexpr float fullTurn = 6.28318530718F;
    for (int index = 0; index < particleCount; ++index) {
      const float phase =
          fullTurn * static_cast<float>(index) / particleCount;
      const float speed = 80.0F + static_cast<float>((index * 37) % 70);
      sparkParticles.push_back(
          {static_cast<float>(cursorX), static_cast<float>(cursorY),
           std::cos(phase) * speed, std::sin(phase) * speed - 20.0F, 0.0F,
           0.42F, 2.0F + static_cast<float>(index % 3),
           (index % 3 == 0) ? kLightPalette.primary : kDarkPalette.primary});
    }
    constexpr std::size_t maximumParticles = 180;
    if (sparkParticles.size() > maximumParticles) {
      sparkParticles.erase(
          sparkParticles.begin(),
          sparkParticles.begin() +
              static_cast<std::ptrdiff_t>(sparkParticles.size() -
                                          maximumParticles));
    }
  }

  void update(int framebufferWidth, int framebufferHeight) {
    updateSearch();
    updateAction();
    if (framebufferWidth > 0 && framebufferHeight > 0 && !packages.empty()) {
      const Layout layout = makeLayout(framebufferWidth, framebufferHeight,
                                       renderer, settings.compactList);
      const int maxOffset = std::max(
          0, static_cast<int>(packages.size()) - layout.visibleRows);
      scrollOffset =
          std::clamp(scrollOffset, 0.0F, static_cast<float>(maxOffset));
      scrollPosition = std::min(scrollPosition, static_cast<float>(maxOffset));
    }
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    if (windowWidth > 0 && windowHeight > 0) {
      double mouseX = 0.0;
      double mouseY = 0.0;
      glfwGetCursorPos(window, &mouseX, &mouseY);
      cursorX = mouseX * static_cast<double>(framebufferWidth) / windowWidth;
      cursorY = mouseY * static_cast<double>(framebufferHeight) / windowHeight;
    }
    if (draggingSearchSelection) {
      if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        const Layout layout = makeLayout(framebufferWidth, framebufferHeight,
                                         renderer, settings.compactList);
        queryCursor = queryPositionAtCursor(
            layout.queryTextWidth,
            layout.queryTextX);
      } else {
        draggingSearchSelection = false;
      }
    }
    if (draggingPackageScrollbar) {
      if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS &&
          !settingsOpen && !busy()) {
        const Layout layout = makeLayout(framebufferWidth, framebufferHeight,
                                         renderer, settings.compactList);
        setScrollFromCursor(layout);
      } else {
        draggingPackageScrollbar = false;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(buttonPressTimes, [now](const auto& entry) {
      return std::chrono::duration<float>(now - entry.second.started).count() >
             0.4F;
    });
    frameDeltaSeconds = std::clamp(
        std::chrono::duration<float>(now - lastUpdateTime).count(), 0.0F, 0.05F);
    lastUpdateTime = now;
    if (packageListExiting) {
      const float exitDuration = std::clamp(
          frameDeltaSeconds * 6.0F, 0.07F, 0.13F);
      packageListTransition = std::max(
          0.0F, packageListTransition - frameDeltaSeconds / exitDuration);
      if (packageListTransition <= 0.0F) {
        packages.clear();
        packageAppearanceStarts.clear();
        packageListExiting = false;
        packageListTransition = 1.0F;
        scrollOffset = 0.0F;
        scrollPosition = 0.0F;
      }
    }
    if (settings.themeMode == ThemeMode::system && now >= nextSystemThemeCheck) {
      if (const std::optional<bool> preference = systemPreferredLightTheme()) {
        systemLightTheme = *preference;
      }
      nextSystemThemeCheck = now + std::chrono::seconds(10);
    }
    if (settings.clickSparks) {
      for (SparkParticle& particle : sparkParticles) {
        particle.age += frameDeltaSeconds;
        particle.x += particle.velocityX * frameDeltaSeconds;
        particle.y += particle.velocityY * frameDeltaSeconds;
        particle.velocityY += 180.0F * frameDeltaSeconds;
      }
      std::erase_if(sparkParticles, [](const SparkParticle& particle) {
        return particle.age >= particle.lifetime;
      });
    } else {
      sparkParticles.clear();
    }
    const float infoBlend = 1.0F - std::exp(-frameDeltaSeconds * 24.0F);
    infoTransition += ((packageInfoOpen ? 1.0F : 0.0F) - infoTransition) *
                      (settings.animations ? infoBlend : 1.0F);
    infoTransition = std::clamp(infoTransition, 0.0F, 1.0F);
    const float clearBlend = 1.0F - std::exp(-frameDeltaSeconds * 22.0F);
    const bool clearButtonVisible =
        view == ViewMode::packages && !query.empty() && framebufferWidth > 0 &&
        framebufferHeight > 0 &&
        makeLayout(framebufferWidth, framebufferHeight, renderer,
                   settings.compactList)
            .showClearQueryButton;
    clearQueryButtonTransition +=
        ((clearButtonVisible ? 1.0F : 0.0F) - clearQueryButtonTransition) *
        (settings.animations ? clearBlend : 1.0F);
    const float fontBlend = 1.0F - std::exp(-frameDeltaSeconds * 18.0F);
    fontSizeTransition +=
        (1.0F - fontSizeTransition) * (settings.animations ? fontBlend : 1.0F);
    fontSizeTransition = std::clamp(fontSizeTransition, 0.0F, 1.0F);
    if (framebufferWidth > 0 && framebufferHeight > 0) {
      updateCursor(makeLayout(framebufferWidth, framebufferHeight, renderer,
                              settings.compactList));
    }
    if (settings.smoothScroll) {
      const float scrollBlend = 1.0F - std::exp(-frameDeltaSeconds * 30.0F);
      scrollPosition += (scrollOffset - scrollPosition) * scrollBlend;
      if (std::abs(scrollPosition - scrollOffset) < 0.005F) {
        scrollPosition = scrollOffset;
      }
    } else {
      scrollPosition = scrollOffset;
    }
    if (settings.animations) {
      const float settingsBlend = 1.0F - std::exp(-frameDeltaSeconds * 32.0F);
      const float dropdownBlend = 1.0F - std::exp(-frameDeltaSeconds * 14.0F);
      settingsTransition +=
          ((settingsOpen ? 1.0F : 0.0F) - settingsTransition) * settingsBlend;
      dropdownTransition +=
          ((dropdownOpen ? 1.0F : 0.0F) - dropdownTransition) * dropdownBlend;
      if (std::abs(settingsTransition - (settingsOpen ? 1.0F : 0.0F)) < 0.005F) {
        settingsTransition = settingsOpen ? 1.0F : 0.0F;
      }
      if (std::abs(dropdownTransition - (dropdownOpen ? 1.0F : 0.0F)) < 0.005F) {
        dropdownTransition = dropdownOpen ? 1.0F : 0.0F;
      }
    } else {
      settingsTransition = settingsOpen ? 1.0F : 0.0F;
      dropdownTransition = dropdownOpen ? 1.0F : 0.0F;
    }
    categoryTransition = std::min(
        1.0F, categoryTransition + frameDeltaSeconds *
                  (settings.animations ? 7.5F : 100.0F));
    if (!dropdownOpen && dropdownTransition < 0.005F) {
      dropdownKind = DropdownKind::none;
      dropdownChoices.clear();
      dropdownThemeChoices.clear();
      dropdownScroll = 0;
    }
    if (settings.animations) {
      for (AnimatedGlyph& glyph : queryGlyphs) {
        if (glyph.entering &&
            std::chrono::duration<float>(now - glyph.started).count() >= 0.16F) {
          glyph.entering = false;
        }
      }
      std::erase_if(disappearingGlyphs, [now](const DisappearingGlyph& glyph) {
        return std::chrono::duration<float>(now - glyph.started).count() >= 0.16F;
      });
    } else {
      for (AnimatedGlyph& glyph : queryGlyphs) {
        glyph.entering = false;
      }
      disappearingGlyphs.clear();
    }
    handleClick(framebufferWidth, framebufferHeight);
  }

  std::string fitText(std::string value, float maxWidth) const {
    if (maxWidth <= 0.0F) return {};
    if (renderer.measureText(value) <= maxWidth) return value;
    constexpr std::string_view ellipsis = "…";
    const float ellipsisWidth = renderer.measureText(ellipsis);
    if (ellipsisWidth > maxWidth) return {};
    std::vector<std::size_t> boundaries{0};
    for (std::size_t offset = 0; offset < value.size();) {
      offset = nextCodepointPosition(value, offset);
      boundaries.push_back(offset);
    }
    std::size_t low = 0;
    std::size_t high = boundaries.size() - 1;
    while (low < high) {
      const std::size_t middle = low + (high - low + 1) / 2;
      const std::string candidate = value.substr(0, boundaries[middle]) +
                                    std::string(ellipsis);
      if (renderer.measureText(candidate) <= maxWidth) low = middle;
      else high = middle - 1;
    }
    return value.substr(0, boundaries[low]) + std::string(ellipsis);
  }

  std::string buttonKey(const Bounds& bounds) const {
    return std::to_string(bounds.x) + ":" + std::to_string(bounds.y) + ":" +
           std::to_string(bounds.width) + ":" + std::to_string(bounds.height);
  }

  void triggerButtonPress(const Bounds& bounds,
                          std::string_view interactionId = {}) const {
    const std::string key =
        interactionId.empty() ? buttonKey(bounds) : std::string(interactionId);
    buttonPressTimes[key] = {cursorX, cursorY,
                             std::chrono::steady_clock::now()};
  }

  void drawButton(std::vector<render::RectItem>& rectangles,
                  std::vector<render::TextItem>& text,
                  std::vector<render::ImageItem>& images, const Bounds& bounds,
                  std::string label, std::array<float, 3> fill,
                  std::array<float, 3> border,
                  std::array<float, 3> textColor,
                  std::string icon = {},
                  std::string interactionId = {},
                  float opacity = 1.0F) const {
    const std::size_t rectangleStart = rectangles.size();
    const std::size_t textStart = text.size();
    const std::size_t imageStart = images.size();
    const std::string hoverKey =
        interactionId.empty() ? buttonKey(bounds) : std::move(interactionId);
    float& hoverAmount = hoverAmounts[hoverKey];
    const float target = contains(bounds, cursorX, cursorY) ? 1.0F : 0.0F;
    const float blend = settings.animations
                            ? 1.0F - std::exp(-frameDeltaSeconds * 18.0F)
                            : 1.0F;
    hoverAmount += (target - hoverAmount) * blend;
    float pressAmount = 0.0F;
    Bounds rippleBounds{};
    float rippleOpacity = 0.0F;
    const auto pressed = buttonPressTimes.find(hoverKey);
    if (pressed != buttonPressTimes.end()) {
      const float elapsed =
          std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                       pressed->second.started)
              .count();
      pressAmount = std::clamp(1.0F - elapsed / 0.32F, 0.0F, 1.0F);
      if (pressAmount <= 0.0F) {
        buttonPressTimes.erase(pressed);
      } else {
        const float progress = std::clamp(elapsed / 0.32F, 0.0F, 1.0F);
        const float diameter =
            std::min(bounds.width, bounds.height) * progress;
        if (diameter > 1.0F) {
          const float rippleX = std::clamp(
              static_cast<float>(pressed->second.x) - diameter * 0.5F,
              bounds.x, bounds.x + bounds.width - diameter);
          const float rippleY = std::clamp(
              static_cast<float>(pressed->second.y) - diameter * 0.5F,
              bounds.y, bounds.y + bounds.height - diameter);
          rippleBounds = {rippleX, rippleY, diameter, diameter};
          rippleOpacity = (1.0F - progress) * 0.18F;
        }
      }
    }
    // Keep hit testing on stable bounds while animating the drawn button.
    const float visualScale = 1.0F + hoverAmount * 0.025F - pressAmount * 0.035F;
    const float centerX = bounds.x + bounds.width * 0.5F;
    const float centerY = bounds.y + bounds.height * 0.5F;
    const Bounds visualBounds{
        centerX - bounds.width * visualScale * 0.5F,
        centerY - bounds.height * visualScale * 0.5F,
        bounds.width * visualScale, bounds.height * visualScale};
    addButtonSurface(rectangles, visualBounds, fill, border, hoverAmount, pressAmount);
    if (pressAmount > 0.0F) {
      addRoundedRect(rectangles, visualBounds, border,
                     std::min(12.0F, visualBounds.height * 0.5F),
                     pressAmount * 0.12F);
    }
    if (rippleOpacity > 0.0F) {
      addRoundedRect(rectangles, rippleBounds, border,
                     rippleBounds.width * 0.5F, rippleOpacity);
    }

    const float iconRatio = (label.empty() ? 0.82F : 0.50F) * settings.iconScale;
    float iconSize =
        icon.empty() ? 0.0F
                     : std::min({32.0F, bounds.height * iconRatio,
                                 bounds.width * iconRatio});
    const float gap = iconSize > 0.0F && !label.empty() ? 7.0F : 0.0F;
    if (!label.empty()) {
      label = fitText(std::move(label),
                      std::max(0.0F, bounds.width - iconSize - gap - 16.0F));
    }
    const float textWidth = label.empty() ? 0.0F : renderer.measureText(label);
    const float contentWidth = iconSize + gap + textWidth;
    float contentX = visualBounds.x +
        std::max(0.0F, (visualBounds.width - contentWidth) * 0.5F);
    if (iconSize > 0.0F) {
      const float iconY = visualBounds.y + (visualBounds.height - iconSize) * 0.5F;
      render::ImageItem image{
          iconPath(icon), contentX, iconY, iconSize, iconSize, 1.0F};
      if (std::filesystem::path(icon).extension() == ".svg") {
        image.tint = textColor;
        image.themed = true;
      }
      images.push_back(std::move(image));
      contentX += iconSize + gap;
    }
    if (!label.empty()) {
      text.push_back({std::move(label), contentX,
                      renderer.baselineForCenter(visualBounds.y + visualBounds.height * 0.5F),
                      textColor});
    }
    opacity = std::clamp(opacity, 0.0F, 1.0F);
    for (std::size_t index = rectangleStart; index < rectangles.size(); ++index) {
      rectangles[index].opacity *= opacity;
    }
    for (std::size_t index = textStart; index < text.size(); ++index) {
      text[index].opacity *= opacity;
    }
    for (std::size_t index = imageStart; index < images.size(); ++index) {
      images[index].opacity *= opacity;
    }
  }

  void drawSwitch(std::vector<render::RectItem>& rectangles, const Bounds& bounds,
                  std::string_view key, bool enabled, float scale,
                  const Palette& palette) const {
    const std::string switchKey(key);
    float& position = switchPositions[switchKey];
    const float target = enabled ? 1.0F : 0.0F;
    const float blend = settings.animations
                            ? 1.0F - std::exp(-frameDeltaSeconds * 20.0F)
                            : 1.0F;
    position += (target - position) * blend;
    if (std::abs(position - target) < 0.005F) {
      position = target;
    }

    const float trackWidth = 42.0F * scale;
    const float trackHeight = 24.0F * scale;
    const float thumbSize = 16.0F * scale;
    const Bounds track{
        bounds.x + bounds.width - trackWidth,
        bounds.y + (bounds.height - trackHeight) * 0.5F,
        trackWidth, trackHeight};
    addRoundedRect(rectangles, track,
                   enabled ? palette.primary : palette.outline,
                   trackHeight * 0.5F);
    addRoundedRect(rectangles,
                   {track.x + 1.0F * scale, track.y + 1.0F * scale,
                    track.width - 2.0F * scale,
                    track.height - 2.0F * scale},
                   enabled ? palette.primary : palette.elevated,
                   trackHeight * 0.5F);
    const auto pressed = buttonPressTimes.find(buttonKey(bounds));
    if (pressed != buttonPressTimes.end()) {
      const float elapsed =
          std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                       pressed->second.started)
              .count();
      const float progress = std::clamp(elapsed / 0.32F, 0.0F, 1.0F);
      if (progress < 1.0F) {
        addRoundedRect(rectangles, track,
                       enabled ? palette.primaryContainer : palette.outline,
                       trackHeight * 0.5F, (1.0F - progress) * 0.12F);
        const float diameter = trackHeight * progress;
        if (diameter > 1.0F) {
          const float rippleX = std::clamp(
              static_cast<float>(pressed->second.x) - diameter * 0.5F,
              track.x, track.x + track.width - diameter);
          const float rippleY = std::clamp(
              static_cast<float>(pressed->second.y) - diameter * 0.5F,
              track.y, track.y + track.height - diameter);
          addRoundedRect(rectangles,
                         {rippleX, rippleY, diameter, diameter},
                         palette.primaryContainer, diameter * 0.5F,
                         (1.0F - progress) * 0.18F);
        }
      }
    }
    const float thumbX =
        track.x + 3.0F * scale +
        position * (trackWidth - thumbSize - 6.0F * scale);
    addRoundedRect(rectangles,
                   {thumbX, track.y + (trackHeight - thumbSize) * 0.5F,
                    thumbSize, thumbSize},
                   enabled ? std::array<float, 3>{1.0F, 1.0F, 1.0F}
                           : palette.secondaryText,
                   thumbSize * 0.5F);
  }

  std::vector<std::string> wrappedText(std::string_view value,
                                       float maxWidth) const {
    std::vector<std::string> lines;
    std::string line;
    for (std::size_t offset = 0; offset < value.size();) {
      const DecodedCodepoint decoded = decodeCodepoint(value, offset);
      const std::string glyph(value.substr(offset, decoded.length));
      offset += decoded.length;
      if (decoded.value == U'\n') {
        if (!line.empty()) {
          lines.push_back(std::move(line));
          line.clear();
        }
        continue;
      }
      if (decoded.value == U' ' && line.empty()) {
        continue;
      }
      const std::string candidate = line + glyph;
      if (!line.empty() && renderer.measureText(candidate) > maxWidth) {
        const std::size_t breakAt = line.find_last_of(' ');
        if (breakAt != std::string::npos) {
          std::string remainder = line.substr(breakAt + 1);
          line.resize(breakAt);
          lines.push_back(std::move(line));
          line = std::move(remainder);
          if (decoded.value != U' ') {
            line += glyph;
          }
        } else {
          lines.push_back(std::move(line));
          line = decoded.value == U' ' ? std::string{} : glyph;
        }
      } else {
        line = candidate;
      }
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
    if (lines.empty()) {
      lines.push_back(catalog.text("info.noDescription"));
    }
    return lines;
  }

  void drawDropdown(const Layout& layout,
                    std::vector<render::RectItem>& rectangles,
                    std::vector<render::TextItem>& text,
                    std::vector<render::ImageItem>& images,
                    const Palette& palette) const {
    const std::size_t choiceCount = dropdownChoiceCount();
    if (dropdownKind == DropdownKind::none || choiceCount == 0 ||
        dropdownTransition <= 0.005F || cacheConfirmOpen) {
      return;
    }

    const DropdownGeometry geometry = dropdownGeometry(layout);
    const Bounds& panel = geometry.panel;
    addRoundedRect(rectangles,
                   {panel.x, panel.y + 4.0F, panel.width, panel.height},
                   {0.025F, 0.025F, 0.04F}, 16.0F, dropdownTransition * 0.45F);
    addRoundedRect(rectangles, panel, palette.outline, 16.0F, dropdownTransition);
    addRoundedRect(rectangles,
                   {panel.x + 1.0F, panel.y + 1.0F, panel.width - 2.0F,
                    panel.height - 2.0F},
                   palette.elevated, 15.0F, dropdownTransition);

    const std::size_t first =
        static_cast<std::size_t>(std::max(0, dropdownScroll));
    for (int row = 0; row < geometry.visibleRows; ++row) {
      const std::size_t index = first + static_cast<std::size_t>(row);
      if (index >= choiceCount) {
        break;
      }
      const Bounds rowBounds{
          panel.x + 4.0F,
          panel.y + 4.0F + static_cast<float>(row) * geometry.rowHeight,
          panel.width - 8.0F, geometry.rowHeight - 2.0F};
      const bool currentChoice = dropdownChoiceIsCurrent(index);
      const bool highlighted =
          static_cast<int>(index) == dropdownHighlightedIndex;
      const bool hovered = contains(rowBounds, cursorX, cursorY);
      std::string hoverKey = "font-dropdown:" + std::to_string(index);
      float& hoverAmount = hoverAmounts[hoverKey];
      const float blend = settings.animations
                              ? 1.0F - std::exp(-frameDeltaSeconds * 18.0F)
                              : 1.0F;
      hoverAmount += ((hovered ? 1.0F : 0.0F) - hoverAmount) * blend;
      if (currentChoice || highlighted || hoverAmount > 0.005F) {
        const std::array<float, 3> color =
            currentChoice || highlighted ? palette.primaryContainer
                                         : palette.surface;
        addRoundedRect(rectangles, rowBounds, color, 10.0F,
                       dropdownTransition *
                           (currentChoice ? 0.9F
                                          : highlighted
                                                ? std::max(0.55F, hoverAmount * 0.8F)
                                                : hoverAmount * 0.8F));
      }

      const std::string label = dropdownChoiceLabel(index);
      const float indicatorSize =
          std::min({32.0F, geometry.rowHeight * 0.80F,
                    18.0F * settings.iconScale});
      const float labelWidth =
          std::max(0.0F, rowBounds.width - indicatorSize - 28.0F);
      text.push_back(
          {fitText(label, labelWidth),
           rowBounds.x + 12.0F,
           renderer.baselineForCenter(rowBounds.y + rowBounds.height * 0.5F),
           palette.text, dropdownTransition});
      if (currentChoice) {
        images.push_back(
            {iconPath("done.svg"),
             rowBounds.x + rowBounds.width - indicatorSize - 10.0F,
             rowBounds.y + (rowBounds.height - indicatorSize) / 2.0F,
             indicatorSize, indicatorSize, dropdownTransition, false,
             palette.primary, true});
      }
    }
    if (choiceCount >
        static_cast<std::size_t>(geometry.visibleRows)) {
      const Bounds track{panel.x + panel.width - 5.0F, panel.y + 6.0F, 2.0F,
                         panel.height - 12.0F};
      const float thumbHeight =
          std::max(16.0F, track.height *
                              static_cast<float>(geometry.visibleRows) /
                              static_cast<float>(choiceCount));
      const int maxScroll =
          static_cast<int>(choiceCount) - geometry.visibleRows;
      const float fraction =
          maxScroll > 0
              ? static_cast<float>(dropdownScroll) / static_cast<float>(maxScroll)
              : 0.0F;
      addRoundedRect(rectangles, track, palette.outline, 1.0F,
                     dropdownTransition * 0.45F);
      addRoundedRect(
          rectangles,
          {track.x, track.y + (track.height - thumbHeight) * fraction,
           track.width, thumbHeight},
          palette.primary, 1.0F, dropdownTransition);
    }
  }

  void drawSettings(const Layout& layout, std::vector<render::RectItem>& rectangles,
                    std::vector<render::TextItem>& text,
                    std::vector<render::ImageItem>& images,
                    const Palette& palette) const {
    const Bounds& panel = layout.settingsPanel;
    const float scale = layout.settingsScale;
    addRoundedRect(rectangles, {panel.x, panel.y + 6.0F, panel.width, panel.height},
                   themeIsLight() ? std::array<float, 3>{0.76F, 0.74F, 0.79F}
                                  : std::array<float, 3>{0.035F, 0.03F, 0.05F},
                   28.0F);
    addRoundedRect(rectangles, panel, palette.outline, 28.0F);
    addRoundedRect(rectangles, {panel.x + 1.5F, panel.y + 1.5F,
                                panel.width - 3.0F, panel.height - 3.0F},
                   palette.surface, 26.0F);

    {
      const std::string settingsTitle = catalog.text("settings.header");
      text.push_back({fitText(settingsTitle,
                              layout.settingsHeaderCloseButton.x - panel.x - 48.0F),
                      panel.x + 30.0F,
                      panel.y + 44.0F * scale,
                      palette.text});
      if (!settingsStatus.empty()) {
        const float titleEnd = panel.x + 30.0F + renderer.measureText(settingsTitle);
        const float statusRight = layout.settingsHeaderCloseButton.x - 8.0F;
        const float statusWidth = std::max(0.0F, statusRight - titleEnd - 16.0F);
        const std::string fittedStatus = fitText(settingsStatus, statusWidth);
        text.push_back({fittedStatus,
                        statusRight - renderer.measureText(fittedStatus),
                        panel.y + 44.0F * scale, palette.secondaryText});
      }
      drawButton(rectangles, text, images, layout.settingsHeaderCloseButton, "",
                 palette.elevated, palette.outline, palette.text, "close.svg");
      const bool narrowPanel = panel.width < 500.0F;
      const std::string appearanceLabel =
          catalog.text(narrowPanel ? "settings.look" : "settings.appearance");
      const std::string behaviorLabel = catalog.text("settings.behavior");
      const auto tabFill = [&palette](bool selected) {
        return selected ? palette.primaryContainer : palette.elevated;
      };
      const auto tabBorder = [&palette](bool selected) {
        return selected ? palette.primary : palette.outline;
      };
      drawButton(rectangles, text, images, layout.appearanceTab,
                 appearanceLabel,
                 tabFill(settingsCategory == SettingsCategory::appearance),
                 tabBorder(settingsCategory == SettingsCategory::appearance),
                 palette.text);
      drawButton(rectangles, text, images, layout.behaviorTab, behaviorLabel,
                 tabFill(settingsCategory == SettingsCategory::behavior),
                 tabBorder(settingsCategory == SettingsCategory::behavior),
                 palette.text);
      drawButton(rectangles, text, images, layout.themesTab,
                 catalog.text("settings.themes"),
                 tabFill(settingsCategory == SettingsCategory::themes),
                 tabBorder(settingsCategory == SettingsCategory::themes),
                 palette.text);
      drawButton(rectangles, text, images, layout.systemTab,
                 catalog.text("settings.system"),
                 tabFill(settingsCategory == SettingsCategory::system),
                 tabBorder(settingsCategory == SettingsCategory::system),
                 palette.text);

      const std::size_t categoryRectangleStart = rectangles.size();
      const std::size_t categoryTextStart = text.size();
      const std::size_t categoryImageStart = images.size();
      const float settingLabelX = panel.x + 30.0F;
      const auto addSettingLabel = [&](std::string label, const Bounds& control) {
        const float available = std::max(0.0F, control.x - settingLabelX - 12.0F);
        text.push_back({fitText(std::move(label), available), settingLabelX,
                        renderer.baselineForCenter(control.y + control.height * 0.5F),
                        palette.secondaryText});
      };

      if (settingsCategory == SettingsCategory::appearance) {
        addSettingLabel(catalog.text(narrowPanel ? "settings.font"
                                                 : "settings.fontSize"),
                        layout.decreaseFontButton);
        const float fontProgress =
            settings.animations ? fontSizeTransition : 1.0F;
        const std::string currentSize =
            std::to_string(renderer.fontSize()) + " px";
        const std::string previousSize =
            std::to_string(previousFontSize) + " px";
        if (fontProgress < 1.0F) {
          text.push_back({previousSize,
                          layout.fontSizeValueCenterX -
                              renderer.measureText(previousSize) * 0.5F,
                          renderer.baselineForCenter(
                              layout.decreaseFontButton.y +
                              layout.decreaseFontButton.height * 0.5F) -
                              6.0F * fontProgress * scale,
                          palette.secondaryText, 1.0F - fontProgress});
        }
        text.push_back({currentSize,
                        layout.fontSizeValueCenterX -
                            renderer.measureText(currentSize) * 0.5F,
                        renderer.baselineForCenter(
                            layout.decreaseFontButton.y +
                            layout.decreaseFontButton.height * 0.5F) +
                            6.0F * (1.0F - fontProgress) * scale,
                        palette.text, fontProgress});
        addSettingLabel(catalog.text(narrowPanel ? "settings.fontFace"
                                                 : "settings.fontFamily"),
                        layout.fontFamilyButton);
        addSettingLabel(catalog.text(narrowPanel ? "settings.style"
                                                 : "settings.fontStyle"),
                        layout.fontStyleButton);
        addSettingLabel(catalog.text("settings.language"), layout.languageButton);
        addSettingLabel(catalog.text("settings.iconSize"),
                        layout.iconSizeDecreaseButton);
        addSettingLabel(catalog.text("settings.cornerRadius"),
                        layout.cornerRadiusDecreaseButton);
        addSettingLabel(catalog.text("settings.borderSize"),
                        layout.borderDecreaseButton);
        const std::string iconSizeValue =
            std::to_string(static_cast<int>(std::round(settings.iconScale * 100.0F))) + "%";
        const std::string radiusValue =
            std::to_string(static_cast<int>(settings.cornerRadius));
        std::ostringstream borderValueStream;
        borderValueStream << std::fixed << std::setprecision(1) << settings.borderSize;
        const std::string borderValue = borderValueStream.str();
        const float styleValueCenter =
            (layout.iconSizeDecreaseButton.x +
             layout.iconSizeDecreaseButton.width +
             layout.iconSizeIncreaseButton.x) * 0.5F;
        text.push_back({iconSizeValue,
                        styleValueCenter - renderer.measureText(iconSizeValue) * 0.5F,
                        renderer.baselineForCenter(
                            layout.iconSizeDecreaseButton.y +
                            layout.iconSizeDecreaseButton.height * 0.5F),
                        palette.text});
        text.push_back({radiusValue,
                        styleValueCenter - renderer.measureText(radiusValue) * 0.5F,
                        renderer.baselineForCenter(
                            layout.cornerRadiusDecreaseButton.y +
                            layout.cornerRadiusDecreaseButton.height * 0.5F),
                        palette.text});
        text.push_back({borderValue,
                        styleValueCenter - renderer.measureText(borderValue) * 0.5F,
                        renderer.baselineForCenter(
                            layout.borderDecreaseButton.y +
                            layout.borderDecreaseButton.height * 0.5F),
                        palette.text});
      } else if (settingsCategory == SettingsCategory::behavior) {
        addSettingLabel(catalog.text("settings.compactList"), layout.densityButton);
        addSettingLabel(catalog.text("settings.smoothScrolling"),
                        layout.smoothScrollButton);
        addSettingLabel(catalog.text("settings.clickSparks"), layout.sparkButton);
        addSettingLabel(catalog.text("settings.animations"), layout.animationButton);
        addSettingLabel(catalog.text(narrowPanel ? "settings.descriptions"
                                                 : "settings.packageDescriptions"),
                        layout.descriptionsButton);
        addSettingLabel(catalog.text("settings.restoreSearch"),
                        layout.restoreSearchButton);
      } else if (settingsCategory == SettingsCategory::themes) {
        text.push_back({fitText(catalog.text("settings.themesDescription"),
                                panel.width - 60.0F),
                        panel.x + 30.0F,
                        panel.y + 104.0F * scale + layout.settingsContentOffset,
                        palette.secondaryText});
        text.push_back({fitText(catalog.text("settings.themeFolder"),
                                panel.width - 60.0F),
                        panel.x + 30.0F,
                        panel.y + 208.0F * scale + layout.settingsContentOffset,
                        palette.secondaryText});
        text.push_back({fitText("~/.config/flux/themes/", panel.width - 60.0F),
                        panel.x + 30.0F,
                        panel.y + 236.0F * scale + layout.settingsContentOffset,
                        palette.text});
      } else {
        text.push_back({fitText(catalog.text("settings.systemThemeDescription"),
                                panel.width - 60.0F),
                        panel.x + 30.0F,
                        panel.y + 100.0F * scale + layout.settingsContentOffset,
                        palette.secondaryText});
        text.push_back({fitText(catalog.text("settings.packageCache"),
                                panel.width - 60.0F), panel.x + 30.0F,
                        panel.y + 142.0F * scale + layout.settingsContentOffset,
                        palette.secondaryText});
        text.push_back({fitText(catalog.text("settings.resetDescription"),
                                panel.width - 60.0F),
                        panel.x + 30.0F,
                        panel.y + 216.0F * scale + layout.settingsContentOffset,
                        palette.secondaryText});
      }

      if (settingsCategory == SettingsCategory::appearance) {
        const std::string dropdownArrow = "\xE2\x96\xBE";
        std::string familyLabel =
            fitText(renderer.fontFamily(), layout.fontFamilyButton.width - 32.0F);
        std::string styleLabel =
            fitText(renderer.fontStyle(), layout.fontStyleButton.width - 32.0F);
        familyLabel += " " + dropdownArrow;
        styleLabel += " " + dropdownArrow;
        const std::string languageLabel =
            catalog.text("language." + settings.language) + " " + dropdownArrow;

        drawButton(rectangles, text, images, layout.decreaseFontButton, "",
                   palette.elevated, palette.outline, palette.text, "remove.svg");
        drawButton(rectangles, text, images, layout.increaseFontButton, "",
                   palette.elevated, palette.outline, palette.text, "add.svg");
        drawButton(rectangles, text, images, layout.fontFamilyButton,
                   std::move(familyLabel), palette.elevated, palette.outline,
                   palette.text);
        drawButton(rectangles, text, images, layout.fontStyleButton,
                   std::move(styleLabel), palette.elevated, palette.outline,
                   palette.text);
        drawButton(rectangles, text, images, layout.languageButton,
                   languageLabel, palette.elevated, palette.outline,
                   palette.text);
        drawButton(rectangles, text, images, layout.iconSizeDecreaseButton,
                   "−", palette.elevated, palette.outline, palette.text);
        drawButton(rectangles, text, images, layout.iconSizeIncreaseButton,
                   "+", palette.elevated, palette.outline, palette.text);
        drawButton(rectangles, text, images, layout.cornerRadiusDecreaseButton,
                   "-", palette.elevated, palette.outline, palette.text);
        drawButton(rectangles, text, images, layout.cornerRadiusIncreaseButton,
                   "+", palette.elevated, palette.outline, palette.text);
        drawButton(rectangles, text, images, layout.borderDecreaseButton,
                   "-", palette.elevated, palette.outline, palette.text);
        drawButton(rectangles, text, images, layout.borderIncreaseButton,
                   "+", palette.elevated, palette.outline, palette.text);
      } else if (settingsCategory == SettingsCategory::themes) {
        std::string themeLabel =
            fitText(currentThemeName(), layout.themeButton.width - 32.0F);
        themeLabel += " " + std::string("\xE2\x96\xBE");
        text.push_back({fitText(catalog.text("settings.theme"),
                                layout.themeButton.x - panel.x - 36.0F),
                        panel.x + 24.0F,
                        renderer.baselineForCenter(
                            layout.themeButton.y - 16.0F * scale),
                        palette.text});
        drawButton(rectangles, text, images, layout.themeButton,
                   std::move(themeLabel), palette.elevated, palette.outline,
                   palette.text);
      } else if (settingsCategory == SettingsCategory::behavior) {
        drawSwitch(rectangles, layout.densityButton, "compact-list",
                   settings.compactList, scale, palette);
        drawSwitch(rectangles, layout.smoothScrollButton, "smooth-scroll",
                   settings.smoothScroll, scale, palette);
        drawSwitch(rectangles, layout.sparkButton, "click-sparks",
                   settings.clickSparks, scale, palette);
        drawSwitch(rectangles, layout.animationButton, "animations",
                   settings.animations, scale, palette);
        drawSwitch(rectangles, layout.descriptionsButton,
                   "package-descriptions", settings.showPackageDescriptions,
                   scale, palette);
        drawSwitch(rectangles, layout.restoreSearchButton, "restore-last-search",
                   settings.restoreLastSearch, scale, palette);
      } else {
        drawButton(rectangles, text, images, layout.clearCacheButton,
                   catalog.text("settings.clear"),
                   {0.38F, 0.20F, 0.23F}, {0.64F, 0.33F, 0.38F},
                   {1.0F, 0.88F, 0.89F}, "delete.svg");
        drawButton(rectangles, text, images, layout.resetSettingsButton,
                   catalog.text("settings.reset"), palette.elevated, palette.outline,
                   palette.text);
      }
      const float categoryOpacity = std::clamp(categoryTransition, 0.0F, 1.0F);
      const float categoryOffset = (1.0F - categoryOpacity) * 10.0F * scale;
      for (std::size_t index = categoryRectangleStart; index < rectangles.size(); ++index) {
        rectangles[index].y += categoryOffset;
        rectangles[index].opacity *= categoryOpacity;
      }
      for (std::size_t index = categoryTextStart; index < text.size(); ++index) {
        text[index].baseline += categoryOffset;
        text[index].opacity *= categoryOpacity;
      }
      for (std::size_t index = categoryImageStart; index < images.size(); ++index) {
        images[index].y += categoryOffset;
        images[index].opacity *= categoryOpacity;
      }
      drawButton(rectangles, text, images, layout.closeSettingsButton,
                 catalog.text("settings.done"),
                 palette.primaryContainer, palette.primary, palette.text,
                 "done.svg");
    }
  }

  void drawCacheConfirmation(int width, int height, const Layout& layout,
                             const Palette& palette) const {
    std::vector<render::RectItem> rectangles;
    std::vector<render::TextItem> text;
    std::vector<render::ImageItem> images;
    const Bounds& panel = layout.cacheConfirmPanel;
    const float scale = layout.confirmScale;
    addRoundedRect(rectangles,
                   {0.0F, 0.0F, static_cast<float>(width),
                    static_cast<float>(height)},
                   {0.01F, 0.015F, 0.025F}, 0.0F, 0.46F);
    addRoundedRect(rectangles,
                   {panel.x, panel.y + 6.0F, panel.width, panel.height},
                   themeIsLight() ? std::array<float, 3>{0.76F, 0.74F, 0.79F}
                                  : std::array<float, 3>{0.035F, 0.03F, 0.05F},
                   24.0F);
    addRoundedRect(rectangles, panel, palette.outline, 24.0F);
    addRoundedRect(rectangles,
                   {panel.x + 1.5F, panel.y + 1.5F,
                    panel.width - 3.0F, panel.height - 3.0F},
                   palette.surface, 22.0F);

    const float inset = std::min(24.0F, panel.width * 0.08F);
    const float lineHeight = std::max(16.0F, renderer.fontSize() * 1.25F);
    const float titleBaseline = panel.y + 46.0F * scale;
    const float bodyStart = panel.y + 82.0F * scale;
    const float bodyEnd = layout.cancelCacheButton.y - 10.0F;
    const int maxBodyLines = std::max(
        1, static_cast<int>((bodyEnd - bodyStart) / lineHeight));
    const float bodyWidth = std::max(1.0F, panel.width - inset * 2.0F);
    const std::string title = fitText(catalog.text("cache.confirmTitle"), bodyWidth);
    text.push_back({title, panel.x + inset, titleBaseline, palette.text});

    const std::string details = catalog.text(
        packageManager == pm::Manager::dnf ? "cache.details.dnf"
                                           : "cache.details.pacman");
    const std::string location = catalog.text(
        packageManager == pm::Manager::dnf ? "cache.location.dnf"
                                           : "cache.location.pacman");
    std::vector<std::string> bodyLines = wrappedText(details, bodyWidth);
    const std::vector<std::string> locationLines = wrappedText(location, bodyWidth);
    bodyLines.insert(bodyLines.end(), locationLines.begin(), locationLines.end());
    if (bodyLines.size() > static_cast<std::size_t>(maxBodyLines)) {
      bodyLines.resize(static_cast<std::size_t>(maxBodyLines));
    }
    for (std::size_t index = 0; index < bodyLines.size(); ++index) {
      text.push_back({std::move(bodyLines[index]), panel.x + inset,
                      bodyStart + static_cast<float>(index) * lineHeight,
                      palette.secondaryText});
    }
    drawButton(rectangles, text, images, layout.cancelCacheButton,
               catalog.text("button.cancel"), palette.elevated,
               palette.outline, palette.text, "close.svg");
    drawButton(rectangles, text, images, layout.confirmCacheButton,
               catalog.text("button.clear"), {0.45F, 0.17F, 0.20F},
               {0.68F, 0.30F, 0.34F}, {1.0F, 0.90F, 0.91F}, "delete.svg");
    applyVisualSettings(rectangles);
    renderer.drawOverlay(width, height, rectangles, text, images);
  }

  void drawUpdateDialog(int width, int height, const Layout& layout,
                        const Palette& palette) const {
    std::vector<render::RectItem> rectangles;
    std::vector<render::TextItem> text;
    std::vector<render::ImageItem> images;
    const Bounds& panel = layout.updateDialogPanel;
    const float scale = layout.confirmScale;
    addRoundedRect(rectangles,
                   {0.0F, 0.0F, static_cast<float>(width),
                    static_cast<float>(height)},
                   {0.01F, 0.015F, 0.025F}, 0.0F, 0.48F);
    addRoundedRect(rectangles,
                   {panel.x, panel.y + 6.0F, panel.width, panel.height},
                   themeIsLight() ? std::array<float, 3>{0.76F, 0.74F, 0.79F}
                                  : std::array<float, 3>{0.035F, 0.03F, 0.05F},
                   24.0F);
    addRoundedRect(rectangles, panel, palette.outline, 24.0F);
    addRoundedRect(rectangles,
                   {panel.x + 1.5F, panel.y + 1.5F,
                    panel.width - 3.0F, panel.height - 3.0F},
                   palette.surface, 22.0F);

    const float inset = std::min(24.0F, panel.width * 0.08F);
    const std::string managerName =
        packageManager == pm::Manager::dnf ? "DNF" : "pacman";
    const std::string details = catalog.format(
        "updates.description", {{"manager", managerName}});
    const auto lines = wrappedText(details, panel.width - inset * 2.0F);
    text.push_back({fitText(catalog.text("updates.title"), panel.width - inset * 2.0F),
                    panel.x + inset,
                    panel.y + 48.0F * scale, palette.text});
    const float lineHeight = std::max(16.0F, renderer.fontSize() * 1.3F);
    const std::size_t maxLines = static_cast<std::size_t>(std::max(
        1.0F, (layout.confirmUpdateButton.y - panel.y - 92.0F * scale) /
                  lineHeight));
    for (std::size_t index = 0; index < std::min(lines.size(), maxLines); ++index) {
      text.push_back({lines[index], panel.x + inset,
                      panel.y + 90.0F * scale +
                          static_cast<float>(index) * lineHeight,
                      palette.secondaryText});
    }
    drawButton(rectangles, text, images, layout.cancelUpdateButton,
               catalog.text("button.cancel"), palette.elevated,
               palette.outline, palette.text, "close.svg");
    drawButton(rectangles, text, images, layout.confirmUpdateButton,
               catalog.text("updates.action"), palette.primary,
               palette.primary, palette.onPrimary);
    applyVisualSettings(rectangles);
    renderer.drawOverlay(width, height, rectangles, text, images);
  }

  void drawPackageInfo(int width, int height, const Layout& layout,
                       const Palette& palette) const {
    std::vector<render::RectItem> rectangles;
    std::vector<render::TextItem> text;
    std::vector<render::ImageItem> images;
    const Bounds& panel = layout.infoDialogPanel;
    addRoundedRect(rectangles,
                   {0.0F, 0.0F, static_cast<float>(width),
                    static_cast<float>(height)},
                   {0.015F, 0.02F, 0.035F}, 0.0F, 0.58F);
    addRoundedRect(rectangles,
                   {panel.x, panel.y + 5.0F, panel.width, panel.height},
                   {0.025F, 0.025F, 0.04F}, 24.0F, 0.45F);
    addRoundedRect(rectangles, panel, palette.outline, 24.0F);
    addRoundedRect(rectangles,
                   {panel.x + 1.5F, panel.y + 1.5F, panel.width - 3.0F,
                    panel.height - 3.0F},
                   palette.surface, 22.0F);

    text.push_back({fitText(catalog.text("info.title"),
                            panel.width - 24.0F - layout.infoDialogCloseButton.width - 32.0F),
                    panel.x + 24.0F,
                    panel.y + 42.0F, palette.secondaryText});
    drawButton(rectangles, text, images, layout.infoDialogCloseButton, "",
               palette.elevated, palette.outline, palette.text, "close.svg");
    const float contentInset = 24.0F;
    const float fontSize = static_cast<float>(renderer.fontSize());
    const float maxVersionWidth =
        std::max(0.0F, panel.width - contentInset * 2.0F);
    const std::string versionText = fitText(infoPackage.version, maxVersionWidth - 24.0F);
    const float versionBadgeHeight = std::max(28.0F, fontSize + 10.0F);
    const float versionBadgeWidth = std::min(
        maxVersionWidth, renderer.measureText(versionText) + 24.0F);
    const Bounds versionBadge{panel.x + contentInset, panel.y + 94.0F,
                              versionBadgeWidth, versionBadgeHeight};
    addRoundedRect(rectangles, versionBadge, palette.elevated, 12.0F);
    const float versionBaseline =
        versionBadge.y + (versionBadge.height - fontSize) * 0.5F + fontSize * 0.8F;
    text.push_back({versionText, versionBadge.x + 12.0F, versionBaseline,
                    palette.secondaryText});
    text.push_back({fitText(infoPackage.name, maxVersionWidth - 12.0F), panel.x + contentInset,
                    panel.y + 80.0F, palette.text});
    const std::string packageState = catalog.text(
        infoPackage.installed ? "info.installed" : "info.available");
    text.push_back({fitText(packageState, 144.0F),
                    panel.x + contentInset, panel.y + 154.0F,
                    infoPackage.installed ? palette.primary : palette.secondaryText});
    const std::string managerName =
        packageManager == pm::Manager::dnf ? "DNF" : "pacman";
    const std::string managerLabel = catalog.format(
        "info.manager", {{"manager", managerName}});
    text.push_back({fitText(managerLabel, panel.width - 208.0F),
                    panel.x + contentInset + 160.0F, panel.y + 154.0F,
                    palette.secondaryText});
    text.push_back({fitText(catalog.text("info.description"), panel.width - 48.0F),
                    panel.x + contentInset,
                    panel.y + 201.0F, palette.primary});
    const float lineHeight = std::max(18.0F, fontSize * 1.45F);
    const float firstDescriptionBaseline =
        layout.infoDialogContent.y + lineHeight;
    const int maxLines = std::max(
        1, static_cast<int>(std::floor(
               layout.infoDialogContent.height / lineHeight)));
    struct MarkdownLine {
      std::string text;
      bool heading;
      bool bullet;
    };
    std::vector<MarkdownLine> lines;
    std::istringstream description(infoPackage.description);
    std::string rawLine;
    const float textWidth = panel.width - 48.0F;
    while (std::getline(description, rawLine)) {
      const std::size_t first = rawLine.find_first_not_of(" \t\r");
      if (first == std::string::npos) {
        lines.push_back({"", false, false});
        continue;
      }
      rawLine.erase(0, first);
      bool heading = false;
      if (rawLine.starts_with("### ")) {
        rawLine.erase(0, 4);
        heading = true;
      } else if (rawLine.starts_with("## ") || rawLine.starts_with("# ")) {
        rawLine.erase(0, rawLine.starts_with("## ") ? 3 : 2);
        heading = true;
      }
      bool bullet = false;
      if (!heading && (rawLine.starts_with("- ") || rawLine.starts_with("* ") ||
                       rawLine.starts_with("+ "))) {
        rawLine.erase(0, 2);
        bullet = true;
      }
      for (std::size_t marker = 0;
           (marker = rawLine.find("**", marker)) != std::string::npos;) {
        rawLine.erase(marker, 2);
      }
      for (std::size_t marker = 0;
           (marker = rawLine.find('`', marker)) != std::string::npos;) {
        rawLine.erase(marker, 1);
      }
      const std::vector<std::string> wrapped = wrappedText(
          rawLine, textWidth - (bullet ? 14.0F : 0.0F));
      for (std::size_t index = 0; index < wrapped.size(); ++index) {
        lines.push_back({(bullet && index == 0 ? "- " : "") + wrapped[index],
                         heading && index == 0, bullet && index == 0});
      }
    }
    if (lines.empty()) {
      lines.push_back({catalog.text("info.noDescription"), false, false});
    }
    const std::size_t maxScroll = lines.size() > static_cast<std::size_t>(maxLines)
        ? lines.size() - static_cast<std::size_t>(maxLines) : 0;
    const float scroll = std::clamp(infoScrollOffset, 0.0F,
                                    static_cast<float>(maxScroll));
    const std::size_t firstLine = static_cast<std::size_t>(scroll);
    const float fractionalLine = scroll - static_cast<float>(firstLine);
    for (std::size_t row = 0; row < static_cast<std::size_t>(maxLines) + 1; ++row) {
      const std::size_t index = firstLine + row;
      if (index >= lines.size()) break;
      const float baseline = firstDescriptionBaseline +
                             (static_cast<float>(row) - fractionalLine) * lineHeight;
      if (baseline < layout.infoDialogContent.y + fontSize * 0.5F ||
          baseline > layout.infoDialogContent.y + layout.infoDialogContent.height) {
        continue;
      }
      if (lines[index].heading) {
        const float headingWidth =
            std::min(textWidth, renderer.measureText(lines[index].text) + 16.0F);
        addRoundedRect(rectangles,
                       {panel.x + contentInset, baseline - lineHeight + 2.0F,
                        headingWidth, lineHeight - 2.0F},
                       palette.primaryContainer, 8.0F, 0.65F);
      }
      text.push_back({std::move(lines[index].text),
                      panel.x + (lines[index].bullet ? 36.0F : 24.0F),
                      baseline,
                      lines[index].heading || lines[index].bullet
                          ? palette.primary
                          : palette.text});
    }
    if (maxScroll > 0) {
      const Bounds track{panel.x + panel.width - 8.0F,
                         layout.infoDialogContent.y, 3.0F,
                         layout.infoDialogContent.height};
      const float thumbHeight = std::max(
          22.0F, track.height * static_cast<float>(maxLines) /
                     static_cast<float>(lines.size()));
      const float fraction = scroll / static_cast<float>(maxScroll);
      addRoundedRect(rectangles, track, palette.outline, 2.0F, 0.45F);
      addRoundedRect(rectangles,
                     {track.x, track.y + (track.height - thumbHeight) * fraction,
                      track.width, thumbHeight},
                     palette.primary, 2.0F);
    }

    for (render::RectItem& item : rectangles) {
      item.opacity *= infoTransition;
    }
    for (render::TextItem& item : text) {
      item.opacity *= infoTransition;
    }
    for (render::ImageItem& item : images) {
      item.opacity *= infoTransition;
    }
    applyVisualSettings(rectangles);
    renderer.drawOverlay(width, height, rectangles, text, images);
  }

  void applyVisualSettings(std::vector<render::RectItem>& rectangles) const {
    const float radiusScale = settings.cornerRadius * cornerScale / 16.0F;
    for (std::size_t index = 0; index < rectangles.size(); ++index) {
      render::RectItem& rectangle = rectangles[index];
      const float rectangleMinDimension =
          std::min(rectangle.width, rectangle.height);
      rectangle.radius = std::clamp(
          rectangle.radius * radiusScale, 0.0F, rectangleMinDimension * 0.5F);
      if (index + 1 >= rectangles.size()) continue;
      render::RectItem& inner = rectangles[index + 1];
      const float insetX = inner.x - rectangle.x;
      const float insetY = inner.y - rectangle.y;
      const float oppositeInsetX =
          rectangle.x + rectangle.width - inner.x - inner.width;
      const float oppositeInsetY =
          rectangle.y + rectangle.height - inner.y - inner.height;
      const bool nestedBorder = insetX > 0.1F && insetY > 0.1F &&
          std::abs(insetX - insetY) < 0.1F &&
          std::abs(insetX - oppositeInsetX) < 0.1F &&
          std::abs(insetY - oppositeInsetY) < 0.1F && insetX <= 4.0F &&
          inner.width > 0.0F && inner.height > 0.0F;
      if (nestedBorder) {
        const float inset = std::min(
            settings.borderSize * cornerScale,
            std::min(rectangle.width, rectangle.height) * 0.25F);
        const float outerRadius = std::clamp(
            rectangle.radius, 0.0F,
            std::min(rectangle.width, rectangle.height) * 0.5F);
        inner.x = rectangle.x + inset;
        inner.y = rectangle.y + inset;
        inner.width = std::max(0.0F, rectangle.width - inset * 2.0F);
        inner.height = std::max(0.0F, rectangle.height - inset * 2.0F);
        const float innerMaxRadius =
            std::min(inner.width, inner.height) * 0.5F;
        rectangle.radius = outerRadius;
        inner.radius = std::clamp(outerRadius - inset, 0.0F, innerMaxRadius);
        // The inner fill already received its final radius above. Skipping it
        // here prevents the settings multiplier from being applied twice.
        ++index;
      }
    }
  }

  void drawSparkParticles(int width, int height) const {
    if (sparkParticles.empty()) {
      return;
    }
    std::vector<render::RectItem> rectangles;
    for (const SparkParticle& particle : sparkParticles) {
      const float progress =
          std::clamp(particle.age / particle.lifetime, 0.0F, 1.0F);
      const float size = particle.size * (1.0F - progress * 0.75F);
      addRoundedRect(rectangles,
                     {particle.x - size * 0.5F, particle.y - size * 0.5F,
                      size, size},
                     particle.color, size * 0.5F, 1.0F - progress);
    }
    applyVisualSettings(rectangles);
    renderer.drawOverlay(width, height, rectangles, {}, {});
  }

  void drawMainScene(int width, int height, const Layout& layout,
                     const Palette& palette) const {
    std::vector<render::RectItem> rectangles;
    std::vector<render::TextItem> text;
    std::vector<render::ImageItem> images;
    std::vector<render::RectItem> packageRectangles;
    std::vector<render::TextItem> packageText;
    std::vector<render::ImageItem> packageImages;

    rectangles.push_back(
        {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height),
         palette.background});
    rectangles.push_back({0.0F, 0.0F, static_cast<float>(width), layout.headerHeight,
                          palette.header});
    addRoundedRect(rectangles,
                   {layout.searchCard.x, layout.searchCard.y + 3.0F,
                    layout.searchCard.width, layout.searchCard.height},
                   {0.0F, 0.0F, 0.0F}, 22.0F,
                   themeIsLight() ? 0.07F : 0.20F);
    addRoundedRect(rectangles, layout.searchCard, palette.surface, 22.0F);
    addRoundedRect(rectangles,
                   {layout.searchField.x - 2.0F, layout.searchField.y - 2.0F,
                    layout.searchField.width + 4.0F, layout.searchField.height + 4.0F},
                   searchFocused ? palette.primary : palette.outline, 18.0F);
    addRoundedRect(rectangles, layout.searchField, palette.elevated, 16.0F);
    addRoundedRect(rectangles, layout.resultsPanel, palette.surface, 24.0F);

    const float logoSize = std::min(40.0F, layout.headerHeight * 0.72F);
    const Bounds logoSurface{layout.horizontalMargin,
                             (layout.headerHeight - logoSize) * 0.5F,
                             logoSize, logoSize};
    addRoundedRect(rectangles, logoSurface, palette.elevated, logoSize * 0.5F);
    const float logoInset = logoSize * 0.08F;
    images.push_back({iconPath("Flux.svg"), logoSurface.x + logoInset,
                      logoSurface.y + logoInset,
                      logoSize - logoInset * 2.0F,
                      logoSize - logoInset * 2.0F, 1.0F});
    text.push_back({"Flux", layout.horizontalMargin + logoSize + 8.0F,
                    renderer.baselineForCenter(layout.headerHeight * 0.5F),
                    palette.text});
    if (status != catalog.text("status.initial") &&
        status != catalog.text("status.enterQuery")) {
      text.push_back({fitText(status, static_cast<float>(width) -
                                         layout.queryTextX -
                                         layout.horizontalMargin),
                      layout.queryTextX - 16.0F,
                      layout.statusBaseline + 16.0F, palette.secondaryText});
    }
    const bool packagesActive = view == ViewMode::packages;
    const bool installedActive = view == ViewMode::installed;
    const bool updatesActive = view == ViewMode::updates;
    drawButton(rectangles, text, images, layout.packagesButton, "",
               packagesActive ? palette.primaryContainer : palette.elevated,
               packagesActive ? palette.primary : palette.outline,
               palette.text, "packages.svg", "navigation:packages");
    if (packageManager != pm::Manager::none) {
      drawButton(rectangles, text, images, layout.installedButton, "",
                 installedActive ? palette.primaryContainer : palette.elevated,
                 installedActive ? palette.primary : palette.outline,
                 palette.text, "installed.svg", "navigation:installed");
      drawButton(rectangles, text, images, layout.updatesButton, "",
                 updatesActive ? palette.primaryContainer : palette.elevated,
                 updatesActive ? palette.primary : palette.outline,
                 palette.text, "update.svg", "navigation:updates");
    }
    drawButton(rectangles, text, images, layout.settingsButton, "",
               palette.elevated, palette.outline, palette.text, "settings.svg",
               "navigation:settings");
    if (view != ViewMode::installed) {
      drawButton(rectangles, text, images, layout.searchButton,
                 view == ViewMode::updates ? catalog.text("updates.action")
                                           : catalog.text("search.button"),
                 palette.primary, palette.primary, palette.onPrimary);
    }
    if (view == ViewMode::packages && layout.showClearQueryButton &&
        clearQueryButtonTransition > 0.005F) {
      const float scale = 0.82F + clearQueryButtonTransition * 0.18F;
      const Bounds animatedClearButton{
          layout.clearQueryButton.x +
              layout.clearQueryButton.width * (1.0F - scale) * 0.5F,
          layout.clearQueryButton.y +
              layout.clearQueryButton.height * (1.0F - scale) * 0.5F,
          layout.clearQueryButton.width * scale,
          layout.clearQueryButton.height * scale};
      drawButton(rectangles, text, images, animatedClearButton, "",
                 palette.elevated, palette.outline, palette.secondaryText,
                 "close.svg", {}, clearQueryButtonTransition);
    }

    const float availableTextWidth = layout.queryTextWidth;
    const auto [viewStart, viewEnd] = queryViewRange(availableTextWidth);
    lastQueryViewStartWidth =
        query.empty()
            ? 0.0F
            : renderer.measureText(std::string_view(query).substr(0, viewStart));
    if (view == ViewMode::packages && hasSelection()) {
      const std::size_t selectedStart = std::max(viewStart, std::min(selectionAnchor, queryCursor));
      const std::size_t selectedEnd = std::min(viewEnd, std::max(selectionAnchor, queryCursor));
      if (selectedEnd > selectedStart) {
        const float selectionX =
            layout.queryTextX +
            renderer.measureText(std::string_view(query).substr(
                viewStart, selectedStart - viewStart));
        const float selectionWidth = renderer.measureText(
            std::string_view(query).substr(selectedStart,
                                           selectedEnd - selectedStart));
        addRoundedRect(
            rectangles,
            {selectionX,
             layout.searchField.y + 8.0F,
             selectionWidth,
             layout.searchField.height - 16.0F},
            palette.selection, 5.0F);
      }
    }
    if (view == ViewMode::packages && settings.animations) {
      for (const DisappearingGlyph& glyph : disappearingGlyphs) {
        const float progress = glyphTransition(glyph.started);
        if (progress >= 1.0F) {
          continue;
        }
        const float x = layout.queryTextX + glyph.xOffset;
        const float glyphWidth = renderer.measureText(glyph.text);
        if (x + glyphWidth < layout.queryTextX ||
            x > layout.queryTextX + availableTextWidth) {
          continue;
        }
        const float drop = 20.0F * progress * progress;
        text.push_back(
            {glyph.text, x, layout.queryTextBaseline + drop,
             palette.text, glyph.initialOpacity * (1.0F - progress)});
      }
    }
    if (view == ViewMode::updates) {
      text.push_back({fitText(catalog.text("updates.listTitle"), availableTextWidth),
                      layout.queryTextX,
                      layout.queryTextBaseline, palette.secondaryText});
    } else if (view == ViewMode::installed) {
      text.push_back({fitText(catalog.text("installed.listTitle"), availableTextWidth),
                      layout.queryTextX,
                      layout.queryTextBaseline, palette.secondaryText});
    } else if (query.empty()) {
      float glyphX = layout.queryTextX;
      for (const AnimatedGlyph& glyph : queryGlyphs) {
        const float glyphWidth = renderer.measureText(glyph.text);
        const float progress =
            settings.animations && glyph.entering
                ? glyphTransition(glyph.started)
                : 1.0F;
        if (glyphX + glyphWidth <= layout.queryTextX + availableTextWidth) {
          text.push_back(
              {glyph.text, glyphX + 8.0F * (1.0F - progress),
               layout.queryTextBaseline + 2.0F * (1.0F - progress),
               palette.secondaryText, progress});
        }
        glyphX += glyphWidth;
      }
    } else {
      std::size_t bytePosition = 0;
      for (const AnimatedGlyph& glyph : queryGlyphs) {
        const std::size_t next = nextCodepointPosition(query, bytePosition);
        if (bytePosition >= viewStart && next <= viewEnd) {
          const float x =
              layout.queryTextX +
              renderer.measureText(std::string_view(query).substr(
                  viewStart, bytePosition - viewStart));
          const float progress =
              settings.animations && glyph.entering
                  ? glyphTransition(glyph.started)
                  : 1.0F;
          text.push_back({glyph.text, x + 8.0F * (1.0F - progress),
                          layout.queryTextBaseline + 2.0F * (1.0F - progress),
                          palette.text, progress});
        }
        bytePosition = next;
      }
    }
    const auto animationElapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - animationEpoch);
    const bool caretRecentlyActive =
        std::chrono::steady_clock::now() - lastCaretActivity <
        std::chrono::milliseconds(650);
    if (searchFocused &&
        (!settings.animations || caretRecentlyActive ||
         std::fmod(animationElapsed.count(), 1.0F) < 0.55F)) {
      const std::size_t cursorInView =
          std::clamp(queryCursor, viewStart, viewEnd) - viewStart;
      const float caretX = layout.queryTextX +
                           renderer.measureText(std::string_view(query).substr(
                               viewStart, cursorInView));
      rectangles.push_back(
          {caretX,
           layout.searchField.y + 10.0F, 2.0F,
           std::max(1.0F, layout.searchField.height - 20.0F),
           palette.primary});
    }

    if (searchTask.valid()) {
      addRoundedRect(rectangles, layout.progressTrack, palette.outline, 3.0F);
      const float progress = std::clamp(
          searchProgress.load(std::memory_order_relaxed), 0.0F, 1.0F);
      addRoundedRect(rectangles,
                     {layout.progressTrack.x, layout.progressTrack.y,
                      layout.progressTrack.width * progress,
                      layout.progressTrack.height},
                     palette.primary, 3.0F);
    }

    const std::size_t first =
        std::min(static_cast<std::size_t>(std::floor(scrollPosition)), packages.size());
    const float fractionalScroll =
        scrollPosition - std::floor(scrollPosition);
    const std::size_t count = std::min(
        packages.size() - first,
        static_cast<std::size_t>(layout.drawableRows + (fractionalScroll > 0.0F ? 1 : 0)));
    for (std::size_t row = 0; row < count; ++row) {
      const pm::Package& package = packages[first + row];
      const std::size_t rectangleStart = packageRectangles.size();
      const std::size_t textStart = packageText.size();
      const std::size_t imageStart = packageImages.size();
      const float rowY = layout.firstRowY +
                         (static_cast<float>(row) - fractionalScroll) * layout.rowHeight;
      const float rowHeight = layout.rowHeight - 5.0F;
      const Bounds rowBounds{
          layout.horizontalMargin + 8.0F, rowY,
          std::max(0.0F, static_cast<float>(width) -
                             layout.horizontalMargin * 2.0F - 16.0F),
          rowHeight};
      addRoundedRect(packageRectangles,
                     {rowBounds.x, rowBounds.y + 2.0F,
                      rowBounds.width, rowBounds.height},
                     {0.0F, 0.0F, 0.0F}, 16.0F,
                     themeIsLight() ? 0.08F : 0.22F);
      addRoundedRect(packageRectangles, rowBounds, palette.elevated, 16.0F);
      const bool actionInProgress =
          actionTask.valid() && !cacheClearTask &&
          package.name == activeActionPackage;
      if (actionInProgress) {
        const Bounds progressTrack{rowBounds.x + 12.0F,
                                   rowBounds.y + rowBounds.height - 7.0F,
                                   std::max(0.0F, rowBounds.width - 24.0F), 3.0F};
        const float segmentWidth =
            std::min(progressTrack.width,
                     std::max(24.0F, progressTrack.width * 0.24F));
        const float phase =
            settings.animations
                ? 0.5F + 0.5F * std::sin(animationElapsed.count() * 2.4F)
                : 0.5F;
        addRoundedRect(packageRectangles, progressTrack, palette.outline, 2.0F);
        addRoundedRect(
            packageRectangles,
            {progressTrack.x + (progressTrack.width - segmentWidth) * phase,
             progressTrack.y, segmentWidth, progressTrack.height},
            palette.primary, 2.0F);
      }

      const Bounds helpButton{
          layout.helpButtonX,
          rowY + (layout.rowHeight - layout.helpButtonWidth) / 2.0F,
          layout.helpButtonWidth, layout.helpButtonWidth};
      const Bounds actionButton{
          layout.actionButtonX,
          rowY + (layout.rowHeight - layout.actionButtonHeight) / 2.0F,
          layout.actionButtonWidth, layout.actionButtonHeight};
      const bool installed = package.installed;
      drawButton(packageRectangles, packageText, packageImages, helpButton, "",
                 palette.elevated, palette.outline, palette.text, "help.svg",
                 "package-help:" + package.name);
      drawButton(packageRectangles, packageText, packageImages, actionButton,
                 view == ViewMode::updates
                     ? catalog.text("updates.packageAction")
                     : catalog.text(installed ? "package.remove" : "package.install"),
                 view == ViewMode::updates
                     ? palette.primaryContainer
                     : installed ? std::array<float, 3>{0.38F, 0.20F, 0.23F}
                                 : palette.primaryContainer,
                 view == ViewMode::updates
                     ? palette.primary
                     : installed ? std::array<float, 3>{0.64F, 0.33F, 0.38F}
                                 : palette.primary,
                 palette.text, {},
                 "package-action:" + package.name);

      std::string packageLabel = package.name + "  " + package.version;
      const float textRight = layout.helpButtonX;
      const float packageTextWidth =
          std::max(0.0F, textRight - layout.rowContentX - 12.0F);
      packageLabel = fitText(std::move(packageLabel), packageTextWidth);
      packageText.push_back({std::move(packageLabel), layout.rowContentX,
                             rowY + layout.rowHeight * 0.40F, palette.text});
      if (settings.showPackageDescriptions && !actionInProgress &&
          !package.description.empty() && width >= 600 &&
          rowHeight >= 54.0F) {
        packageText.push_back(
            {fitText(package.description, packageTextWidth),
             layout.rowContentX, rowY + layout.rowHeight * 0.78F,
             palette.secondaryText});
      }

      if (settings.animations && !packageListExiting) {
        const auto now = std::chrono::steady_clock::now();
        const float appearanceDuration =
            std::clamp(frameDeltaSeconds * 6.0F, 0.075F, 0.13F);
        const float appearanceStagger =
            std::clamp(frameDeltaSeconds * 0.5F, 0.004F, 0.012F);
        const auto [appearance, inserted] = packageAppearanceStarts.try_emplace(
            package.name,
            PackageAppearance{
                now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                          std::chrono::duration<float>(
                              static_cast<float>(row) * appearanceStagger)),
                appearanceDuration});
        static_cast<void>(inserted);
        const float elapsed =
            std::chrono::duration<float>(now - appearance->second.started).count();
        const float progress =
            std::clamp(elapsed / appearance->second.duration, 0.0F, 1.0F);
        const float eased = 1.0F - std::pow(1.0F - progress, 3.0F);
        const float offset = 12.0F * (1.0F - eased);
        for (std::size_t index = rectangleStart;
             index < packageRectangles.size(); ++index) {
          packageRectangles[index].y += offset;
          packageRectangles[index].opacity *= eased;
        }
        for (std::size_t index = textStart; index < packageText.size(); ++index) {
          packageText[index].baseline += offset;
          packageText[index].opacity *= eased;
        }
        for (std::size_t index = imageStart; index < packageImages.size(); ++index) {
          packageImages[index].y += offset;
          packageImages[index].opacity *= eased;
        }
      }
      if (packageListExiting) {
        const float offset = -8.0F * (1.0F - packageListTransition);
        for (std::size_t index = rectangleStart;
             index < packageRectangles.size(); ++index) {
          packageRectangles[index].y += offset;
          packageRectangles[index].opacity *= packageListTransition;
        }
        for (std::size_t index = textStart; index < packageText.size(); ++index) {
          packageText[index].baseline += offset;
          packageText[index].opacity *= packageListTransition;
        }
        for (std::size_t index = imageStart; index < packageImages.size(); ++index) {
          packageImages[index].y += offset;
          packageImages[index].opacity *= packageListTransition;
        }
      }
    }

    if (packages.empty() && !busy() &&
        (status == catalog.text("status.initial") ||
         status == catalog.text("status.noResults"))) {
      const bool noMatches = status == catalog.text("status.noResults");
      const float centerX = static_cast<float>(width) * 0.5F;
      const float centerY =
          layout.firstRowY +
          (layout.resultsPanel.y + layout.resultsPanel.height - layout.firstRowY) *
              0.5F;
      const Bounds iconSurface{centerX - 34.0F, centerY - 82.0F, 68.0F, 68.0F};
      addRoundedRect(packageRectangles, iconSurface, palette.elevated, 34.0F);
      const float emptyIconSize = std::min(54.0F, 32.0F * settings.iconScale);
      packageImages.push_back(
          {iconPath("search.svg"), centerX - emptyIconSize * 0.5F,
           centerY - 82.0F + (68.0F - emptyIconSize) * 0.5F,
           emptyIconSize, emptyIconSize, 0.82F, false, palette.primary, true});
      const std::string heading = fitText(
          catalog.text(noMatches ? "home.noPackages" : "home.emptyTitle"),
          layout.resultsPanel.width - 48.0F);
      const std::string detail = fitText(
          catalog.text(noMatches ? "home.tryAnother" : "home.emptyPrompt"),
          layout.resultsPanel.width - 48.0F);
      const float headingBaseline =
          centerY + static_cast<float>(renderer.fontSize()) * 0.75F;
      const float detailBaseline =
          headingBaseline + static_cast<float>(renderer.fontSize()) * 1.55F;
      packageText.push_back(
          {heading, centerX - renderer.measureText(heading) * 0.5F,
           headingBaseline, palette.text});
      packageText.push_back(
          {detail, centerX - renderer.measureText(detail) * 0.5F,
           detailBaseline, palette.secondaryText});
    }

    Bounds scrollbar = scrollbarThumb(layout);
    if (scrollbar.height > 0.0F) {
      const bool hovered =
          draggingPackageScrollbar ||
          contains(scrollbarHoverBounds(layout), cursorX, cursorY);
      const float target = hovered ? 1.0F : 0.0F;
      const float blend = settings.animations
                              ? 1.0F - std::exp(-frameDeltaSeconds * 22.0F)
                              : 1.0F;
      packageScrollbarHover += (target - packageScrollbarHover) * blend;

      const float extraWidth = 4.0F * packageScrollbarHover;
      Bounds track = layout.packageScrollbarTrack;
      track.x -= extraWidth;
      track.width += extraWidth;
      scrollbar.x -= extraWidth;
      scrollbar.width += extraWidth;
      const float radius = track.width * 0.5F;
      addRoundedRect(packageRectangles, track, palette.outline, radius, 0.30F);
      addRoundedRect(packageRectangles, scrollbar, palette.primary, radius, 0.90F);
    } else {
      packageScrollbarHover = 0.0F;
    }

    if (packages.size() > count) {
      const std::string range = catalog.format(
          "home.range",
          {{"first", std::to_string(first + 1)},
           {"last", std::to_string(first + count)},
           {"total", std::to_string(packages.size())}});
      const float rangeBaseline =
          static_cast<float>(height) - layout.horizontalMargin * 0.5F;
      text.push_back({fitText(range, static_cast<float>(width) -
                                         layout.horizontalMargin * 2.0F - 16.0F),
                      layout.horizontalMargin + 16.0F,
                      rangeBaseline, palette.secondaryText});
    }

    applyVisualSettings(rectangles);
    applyVisualSettings(packageRectangles);
    renderer.draw(width, height, rectangles, text, images);
    renderer.drawClipped(
        width, height,
        {layout.resultsPanel.x, layout.firstRowY, layout.resultsPanel.width,
         std::max(0.0F, layout.resultsPanel.y + layout.resultsPanel.height -
                            layout.firstRowY)},
        packageRectangles, packageText, packageImages);
    if (packageInfoOpen || infoTransition > 0.005F) {
      drawPackageInfo(width, height, layout, palette);
    }
    if (updatesOpen) {
      drawUpdateDialog(width, height, layout, palette);
    }
  }

  void draw(int width, int height) const {
    if (width <= 0 || height <= 0) {
      renderer.draw(width, height, {}, {});
      return;
    }

    const Layout layout = makeLayout(width, height, renderer,
                                     settings.compactList);
    cornerScale = std::clamp(static_cast<float>(height) / 560.0F, 0.35F, 1.0F);
    const Palette& palette = currentPalette();
    if (settingsOpen || settingsTransition > 0.005F) {
      drawMainScene(width, height, layout, palette);
      renderer.blurCurrentFramebuffer(width, height, settingsTransition);

      std::vector<render::RectItem> rectangles;
      std::vector<render::TextItem> text;
      std::vector<render::ImageItem> images;
      std::vector<render::RectItem> dropdownRectangles;
      std::vector<render::TextItem> dropdownText;
      std::vector<render::ImageItem> dropdownImages;
      addRoundedRect(rectangles,
                     {0.0F, 0.0F, static_cast<float>(width),
                      static_cast<float>(height)},
                     {0.035F, 0.035F, 0.055F}, 0.0F,
                     0.52F * settingsTransition);
      drawSettings(layout, rectangles, text, images, palette);
      drawDropdown(layout, dropdownRectangles, dropdownText,
                   dropdownImages, palette);
      for (std::size_t index = 1; index < rectangles.size(); ++index) {
        rectangles[index].opacity *= settingsTransition;
      }
      for (render::TextItem& item : text) {
        item.opacity *= settingsTransition;
      }
      for (render::ImageItem& item : images) {
        item.opacity *= settingsTransition;
      }
      applyVisualSettings(rectangles);
      applyVisualSettings(dropdownRectangles);
      renderer.drawOverlay(width, height, rectangles, text, images);
      for (render::RectItem& item : dropdownRectangles) {
        item.opacity *= settingsTransition;
      }
      for (render::TextItem& item : dropdownText) {
        item.opacity *= settingsTransition;
      }
      for (render::ImageItem& item : dropdownImages) {
        item.opacity *= settingsTransition;
      }
      renderer.drawOverlay(width, height, dropdownRectangles, dropdownText,
                           dropdownImages);
      if (cacheConfirmOpen) {
        drawCacheConfirmation(width, height, layout, palette);
      }
      drawSparkParticles(width, height);
      return;
    }

    drawMainScene(width, height, layout, palette);
    drawSparkParticles(width, height);
  }

};

PackageView::PackageView(core::Window& window, render::Renderer& renderer,
                         pm::Manager packageManager)
    : impl_(std::make_unique<Impl>(window, renderer, packageManager)) {}

PackageView::~PackageView() = default;

void PackageView::update(int framebufferWidth, int framebufferHeight) {
  impl_->update(framebufferWidth, framebufferHeight);
}

void PackageView::draw(int width, int height) const {
  impl_->draw(width, height);
}

}  // namespace ui
