#include "ui/package_view.hpp"

#include "core/window.hpp"
#include "pm/pm.hpp"
#include "render/renderer.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
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
constexpr std::string_view kSearchPlaceholder = "Search...";

enum class ThemeMode { system, light, dark };
enum class SettingsCategory { appearance, behavior, system };

struct UserSettings {
  int fontSize = kDefaultFontSize;
  std::string fontPath;
  ThemeMode themeMode = ThemeMode::system;
  bool compactList = false;
  bool animations = true;
  bool smoothScroll = true;
  bool clickSparks = true;
  bool showPackageDescriptions = true;
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
};

constexpr Palette kDarkPalette{
    {0.067F, 0.067F, 0.106F}, {0.118F, 0.118F, 0.18F},
    {0.141F, 0.141F, 0.212F}, {0.192F, 0.196F, 0.267F},
    {0.424F, 0.439F, 0.525F}, {0.537F, 0.706F, 0.980F},
    {0.192F, 0.196F, 0.267F}, {0.804F, 0.839F, 0.957F},
    {0.729F, 0.761F, 0.871F}, {0.271F, 0.278F, 0.353F}};
constexpr Palette kLightPalette{
    {0.937F, 0.945F, 0.961F}, {0.902F, 0.914F, 0.941F},
    {0.902F, 0.914F, 0.941F}, {0.863F, 0.878F, 0.910F},
    {0.612F, 0.627F, 0.690F}, {0.118F, 0.400F, 0.961F},
    {0.800F, 0.816F, 0.855F}, {0.298F, 0.310F, 0.412F},
    {0.361F, 0.373F, 0.486F}, {0.800F, 0.816F, 0.855F}};

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
  Bounds systemTab;
  Bounds decreaseFontButton;
  Bounds increaseFontButton;
  Bounds themeButton;
  Bounds densityButton;
  Bounds descriptionsButton;
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
  Bounds infoDialogPanel;
  Bounds infoDialogCloseButton;
  float headerHeight;
  float horizontalMargin;
  float queryTextX;
  float queryTextBaseline;
  float queryTextWidth;
  float statusBaseline;
  float packagesTitleBaseline;
  float rowContentX;
  float helpButtonX;
  float helpButtonWidth;
  float actionButtonX;
  float actionButtonWidth;
  float actionButtonHeight;
  float fontSizeValueX;
  float firstRowY;
  float rowHeight;
  float settingsScale;
  float confirmScale;
  float confirmButtonWidth;
  float confirmButtonHeight;
  int visibleRows;
  bool showClearQueryButton;
};

enum class DropdownKind { none, fontFamily, fontStyle, theme };

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

std::filesystem::path settingsPath() {
  if (const char* configHome = std::getenv("XDG_CONFIG_HOME");
      configHome != nullptr && configHome[0] != '\0') {
    return std::filesystem::path(configHome) / "flux" / "settings";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".config" / "flux" / "settings";
  }
  return {};
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

std::string_view themeModeLabel(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::light:
      return "Light";
    case ThemeMode::dark:
      return "Dark";
    case ThemeMode::system:
      return "System";
  }
  return "System";
}

UserSettings loadSettings() {
  UserSettings settings;
  const std::filesystem::path path = settingsPath();
  if (path.empty()) {
    return settings;
  }

  std::ifstream input(path);
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

  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Could not write the Flux settings file");
  }
  output << "font_size=" << settings.fontSize << '\n'
         << "font_path=" << settings.fontPath << '\n'
         << "theme=" << themeModeName(settings.themeMode) << '\n'
         << "compact_list=" << (settings.compactList ? "true" : "false") << '\n'
         << "animations=" << (settings.animations ? "true" : "false") << '\n'
         << "smooth_scroll=" << (settings.smoothScroll ? "true" : "false")
         << '\n'
         << "click_sparks=" << (settings.clickSparks ? "true" : "false") << '\n'
         << "show_package_descriptions="
         << (settings.showPackageDescriptions ? "true" : "false") << '\n';
  if (!output) {
    throw std::runtime_error("Could not save the settings");
  }
}

Layout makeLayout(int width, int height, int fontSize, bool compactList) {
  const float windowWidth = std::max(0.0F, static_cast<float>(width));
  const float windowHeight = std::max(0.0F, static_cast<float>(height));
  const float textSize = static_cast<float>(fontSize);
  const float margin = std::clamp(windowWidth * 0.025F, 12.0F, 32.0F);
  const float headerHeight = std::clamp(windowHeight * 0.09F, 48.0F, 64.0F);
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
  const float confirmButtonWidth =
      std::min(96.0F, (confirmWidth - 40.0F) / 2.0F - 6.0F);
  const float confirmButtonHeight = std::min(38.0F, confirmScale * 38.0F);

  const float searchButtonWidth =
      std::clamp((searchCard.width - 32.0F) * 0.30F,
                 std::max(92.0F, textSize * 3.5F), 136.0F);
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

  const float resultsY = searchCard.y + searchCard.height + 48.0F;
  const float firstRowY = resultsY + 42.0F;
  const float bottomMargin = std::max(12.0F, margin);
  const float remainingRowHeight =
      std::max(48.0F, (windowHeight - firstRowY - bottomMargin) / 2.5F);
  const float requestedRowHeight =
      compactList ? std::max(54.0F, textSize * 2.2F)
                  : std::max(72.0F, textSize * 2.8F);
  const float rowHeight = std::min(requestedRowHeight, remainingRowHeight);
  const float actionButtonWidth = std::clamp(
      windowWidth * 0.10F, std::max(96.0F, textSize * 3.75F), 120.0F);
  const float actionButtonHeight = std::min(40.0F, rowHeight - 8.0F);
  const float actionButtonX = windowWidth - margin - 20.0F - actionButtonWidth;
  const float helpButtonWidth = std::min(36.0F, actionButtonHeight);
  const float helpButtonX = actionButtonX - helpButtonWidth - 8.0F;

  const float panelX = (windowWidth - settingsWidth) / 2.0F;
  const float panelY = (windowHeight - settingsHeight) / 2.0F;
  const float confirmX = (windowWidth - confirmWidth) / 2.0F;
  const float confirmY = (windowHeight - confirmHeight) / 2.0F;
  const float infoWidth = std::min(520.0F, std::max(220.0F, windowWidth - 24.0F));
  const float infoHeight = std::min(360.0F, std::max(200.0F, windowHeight - 24.0F));
  const float infoX = (windowWidth - infoWidth) / 2.0F;
  const float infoY = (windowHeight - infoHeight) / 2.0F;
  const float infoCloseSize = 32.0F;
  const float decrementSize = std::min(36.0F, std::max(24.0F, 44.0F * settingsScale));
  const float closeWidth = std::min(100.0F, settingsWidth - 40.0F);
  const float closeHeight =
      std::min(std::max(24.0F, 36.0F * settingsScale), 52.0F * settingsScale);
  const float headerCloseSize = 32.0F;
  const float settingsControlWidth =
      std::min(260.0F, std::max(92.0F, settingsWidth * 0.46F));
  const float settingsTabGap = 6.0F * settingsScale;
  const float settingsTabWidth =
      (settingsWidth - 40.0F * settingsScale - settingsTabGap * 2.0F) / 3.0F;
  const float settingsTabY = panelY + 52.0F * settingsScale;
  const float settingsTabHeight = 32.0F * settingsScale;
  const float scrollbarHeight =
      std::max(0.0F, windowHeight - firstRowY - bottomMargin);
  const float decreaseX =
      settingsWidth < 400.0F ? settingsWidth - decrementSize * 2.0F - 28.0F
                             : settingsWidth - 228.0F;
  const float increaseX =
      settingsWidth < 400.0F ? settingsWidth - decrementSize - 20.0F
                             : settingsWidth - 72.0F;
  const float valueWidth = textSize * 2.8F;
  const float valueGapStart = decreaseX + decrementSize + 8.0F;
  const float valueGapEnd = increaseX - 8.0F;
  const float fontSizeValueX =
      panelX + (valueGapEnd - valueGapStart >= valueWidth
                   ? (valueGapStart + valueGapEnd - valueWidth) / 2.0F
                   : std::max(24.0F, decreaseX - valueWidth - 8.0F));
  const float confirmButtonsY =
      confirmY + confirmHeight - 18.0F - confirmButtonHeight;
  const float queryTextX = searchFieldX + 16.0F;
  const float queryBaseline =
      searchFieldY + searchFieldHeight / 2.0F + textSize * 0.34F;
  const float statusBaseline = searchCard.y + searchCard.height + 36.0F;

  return {
      .settingsButton = {
          windowWidth - margin - std::clamp(windowWidth * 0.10F, 90.0F, 118.0F),
          (headerHeight - 38.0F) / 2.0F,
          std::clamp(windowWidth * 0.10F, 90.0F, 118.0F), 38.0F},
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
      .appearanceTab = {panelX + 20.0F * settingsScale, settingsTabY,
                        settingsTabWidth, settingsTabHeight},
      .behaviorTab = {
          panelX + 20.0F * settingsScale + settingsTabWidth + settingsTabGap,
          settingsTabY, settingsTabWidth, settingsTabHeight},
      .systemTab = {
          panelX + 20.0F * settingsScale +
              (settingsTabWidth + settingsTabGap) * 2.0F,
          settingsTabY, settingsTabWidth, settingsTabHeight},
      .decreaseFontButton = {panelX + std::max(20.0F, decreaseX),
                             panelY + 100.0F * settingsScale,
                             decrementSize, decrementSize},
      .increaseFontButton = {panelX + increaseX,
                             panelY + 100.0F * settingsScale,
                             decrementSize, decrementSize},
      .themeButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                      panelY + 244.0F * settingsScale,
                      settingsControlWidth, 38.0F * settingsScale},
      .densityButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                        panelY + 110.0F * settingsScale,
                        settingsControlWidth, 38.0F * settingsScale},
      .descriptionsButton = {
          panelX + settingsWidth - settingsControlWidth - 20.0F,
          panelY + 334.0F * settingsScale, settingsControlWidth,
          38.0F * settingsScale},
      .animationButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                          panelY + 278.0F * settingsScale,
                          settingsControlWidth, 38.0F * settingsScale},
      .smoothScrollButton = {
          panelX + settingsWidth - settingsControlWidth - 20.0F,
          panelY + 166.0F * settingsScale, settingsControlWidth,
          38.0F * settingsScale},
      .sparkButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                      panelY + 222.0F * settingsScale, settingsControlWidth,
                      38.0F * settingsScale},
      .fontFamilyButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                           panelY + 148.0F * settingsScale,
                           settingsControlWidth, 38.0F * settingsScale},
      .fontStyleButton = {panelX + settingsWidth - settingsControlWidth - 20.0F,
                          panelY + 196.0F * settingsScale,
                          settingsControlWidth, 38.0F * settingsScale},
      .clearCacheButton = {panelX + 20.0F,
                           panelY + 156.0F * settingsScale,
                           settingsWidth - 40.0F,
                           40.0F * settingsScale},
      .resetSettingsButton = {panelX + 20.0F,
                              panelY + 210.0F * settingsScale,
                              settingsWidth - 40.0F,
                              40.0F * settingsScale},
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
      .infoDialogPanel = {infoX, infoY, infoWidth, infoHeight},
      .infoDialogCloseButton = {
          infoX + infoWidth - infoCloseSize - 16.0F, infoY + 12.0F,
          infoCloseSize, infoCloseSize},
      .headerHeight = headerHeight,
      .horizontalMargin = margin,
      .queryTextX = queryTextX,
      .queryTextBaseline = queryBaseline,
      .queryTextWidth = queryTextWidth,
      .statusBaseline = statusBaseline,
      .packagesTitleBaseline = resultsY + 30.0F,
      .rowContentX = margin + 20.0F,
      .helpButtonX = helpButtonX,
      .helpButtonWidth = helpButtonWidth,
      .actionButtonX = actionButtonX,
      .actionButtonWidth = actionButtonWidth,
      .actionButtonHeight = actionButtonHeight,
      .fontSizeValueX = fontSizeValueX,
      .firstRowY = firstRowY,
      .rowHeight = rowHeight,
      .settingsScale = settingsScale,
      .confirmScale = confirmScale,
      .confirmButtonWidth = confirmButtonWidth,
      .confirmButtonHeight = confirmButtonHeight,
      .visibleRows = std::max(
          0, static_cast<int>((windowHeight - firstRowY - bottomMargin) / rowHeight)),
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
  const std::string sourcePath =
      std::string(FLUX_ASSET_DIR) + "/" + std::string(filename);
  if (std::filesystem::exists(sourcePath)) {
    return sourcePath;
  }
  return std::string(FLUX_INSTALL_ASSET_DIR) + "/" + std::string(filename);
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
  const float inset = 2.0F;
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

  GLFWwindow* window;
  render::Renderer& renderer;
  pm::Manager packageManager;
  bool searchFocused = true;
  bool settingsOpen = false;
  bool cacheConfirmOpen = false;
  bool packageInfoOpen = false;
  bool cacheClearTask = false;
  bool mouseWasDown = false;
  float infoTransition = 0.0F;
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
  int scrollOffset = 0;
  float scrollPosition = 0.0F;
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
  std::string status = "Type a package name and press Enter to search.";
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
  mutable std::unordered_map<std::string, float> hoverAmounts;
  mutable std::unordered_map<std::string, ButtonPress> buttonPressTimes;
  mutable std::unordered_map<std::string, float> switchPositions;
  std::vector<pm::Package> packages;
  std::vector<render::FontFace> fontFaces;
  std::vector<std::string> fontFamilies;
  std::vector<render::FontFace> dropdownChoices;
  std::vector<ThemeMode> dropdownThemeChoices;
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
        packageManager(selectedManager) {
    settings = loadSettings();
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
        settingsStatus = std::string("Could not load saved font: ") + error.what();
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
      status = "Package operations are not supported on this distribution.";
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
    return settings.themeMode == ThemeMode::light ||
           (settings.themeMode == ThemeMode::system && systemLightTheme);
  }

  void updateCursor(const Layout& layout) const {
    bool pointer = false;
    bool text = false;
    if (packageInfoOpen || infoTransition > 0.005F) {
      pointer = contains(layout.infoDialogCloseButton, cursorX, cursorY);
    } else if (cacheConfirmOpen) {
      pointer = contains(layout.cancelCacheButton, cursorX, cursorY) ||
                contains(layout.confirmCacheButton, cursorX, cursorY);
    } else if (settingsOpen) {
      pointer =
          contains(layout.settingsHeaderCloseButton, cursorX, cursorY) ||
          contains(layout.closeSettingsButton, cursorX, cursorY) ||
          contains(layout.appearanceTab, cursorX, cursorY) ||
          contains(layout.behaviorTab, cursorX, cursorY) ||
          contains(layout.systemTab, cursorX, cursorY);
      if (settingsCategory == SettingsCategory::appearance) {
        pointer = pointer || contains(layout.decreaseFontButton, cursorX, cursorY) ||
                  contains(layout.increaseFontButton, cursorX, cursorY) ||
                  contains(layout.fontFamilyButton, cursorX, cursorY) ||
                  contains(layout.fontStyleButton, cursorX, cursorY) ||
                  contains(layout.themeButton, cursorX, cursorY);
      } else if (settingsCategory == SettingsCategory::behavior) {
        pointer = pointer || contains(layout.densityButton, cursorX, cursorY) ||
                  contains(layout.descriptionsButton, cursorX, cursorY) ||
                  contains(layout.smoothScrollButton, cursorX, cursorY) ||
                  contains(layout.sparkButton, cursorX, cursorY) ||
                  contains(layout.animationButton, cursorX, cursorY);
      } else {
        pointer = pointer || contains(layout.clearCacheButton, cursorX, cursorY) ||
                  contains(layout.resetSettingsButton, cursorX, cursorY);
      }
      if (dropdownOpen) {
        const DropdownGeometry geometry = dropdownGeometry(layout);
        pointer = pointer || contains(geometry.panel, cursorX, cursorY);
      }
    } else if (!packageInfoOpen) {
      text = contains(layout.searchField, cursorX, cursorY);
      pointer = contains(layout.settingsButton, cursorX, cursorY) ||
                contains(layout.searchButton, cursorX, cursorY) ||
                (layout.showClearQueryButton &&
                 clearQueryButtonTransition > 0.005F &&
                 contains(layout.clearQueryButton, cursorX, cursorY));
      pointer = pointer ||
                (layout.visibleRows > 0 &&
                 packages.size() > static_cast<std::size_t>(layout.visibleRows) &&
                 contains(layout.packageScrollbarTrack, cursorX, cursorY));
      if (!busy()) {
        const std::size_t first = std::min(
            static_cast<std::size_t>(std::floor(scrollPosition)), packages.size());
        const float fraction = scrollPosition - std::floor(scrollPosition);
        const int rows =
            layout.visibleRows + (fraction > 0.0F ? 1 : 0);
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
    scrollOffset = static_cast<int>(
        std::lround(fraction * static_cast<float>(maxOffset)));
    if (!settings.smoothScroll) {
      scrollPosition = static_cast<float>(scrollOffset);
    }
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
    const std::vector<std::string> target =
        splitCodepoints(query.empty() ? kSearchPlaceholder
                                      : std::string_view(query));
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
    status = "Press Enter or click Search.";
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

  float queryCharacterWidth() const {
    return std::max(1.0F, static_cast<float>(renderer.fontSize()) * 0.55F);
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
    if (self == nullptr || self->busy() || self->settingsOpen || !self->searchFocused ||
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
      } else if (self->settingsOpen) {
        self->settingsOpen = false;
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
        const Layout layout = makeLayout(width, height, self->renderer.fontSize(),
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
    if (self == nullptr || self->busy() || self->packageInfoOpen ||
        self->infoTransition > 0.005F) {
      return;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    const Layout layout =
        makeLayout(width, height, self->renderer.fontSize(), self->settings.compactList);
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
    const int scrollDelta = yOffset > 0.0 ? 1 : yOffset < 0.0 ? -1 : 0;
    self->scrollOffset = std::clamp(
        self->scrollOffset - scrollDelta, 0,
        std::max(0, static_cast<int>(self->packages.size()) - layout.visibleRows));
    if (!self->settings.smoothScroll) {
      self->scrollPosition = static_cast<float>(self->scrollOffset);
    }
  }

  void startSearch() {
    if (busy()) {
      return;
    }
    if (packageManager == pm::Manager::none) {
      status = "Package operations are not supported on this distribution.";
      return;
    }
    if (query.empty()) {
      packages.clear();
      scrollOffset = 0;
      status = "Enter a package name before searching.";
      return;
    }

    const std::string searchTerm = query;
    packages.clear();
    scrollOffset = 0;
    scrollPosition = 0.0F;
    const std::string managerName =
        packageManager == pm::Manager::dnf ? "DNF" : "pacman";
    status = "Searching " + managerName + " repositories...";
    searchProgress.store(0.0F, std::memory_order_relaxed);
    searchTask = std::async(std::launch::async, [searchTerm, this] {
      return pm::searchPackages(packageManager, searchTerm, [this](float progress) {
        searchProgress.store(progress, std::memory_order_relaxed);
      });
    });
  }

  void startAction(const pm::Package& package) {
    if (busy() || packageManager == pm::Manager::none) {
      return;
    }

    const std::string packageName = package.name;
    const bool installed = package.installed;
    cacheClearTask = false;
    activeActionPackage = packageName;
    status = std::string(installed ? "Removing " : "Installing ") + packageName +
             " (Polkit may ask for authentication)...";
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
    status = "Clearing the entire " + managerName + " package cache via Polkit...";
    actionTask = std::async(std::launch::async,
                            [manager] { pm::clearPackageCache(manager); });
  }

  void updateSearch() {
    if (!searchTask.valid() ||
        searchTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return;
    }
    try {
      packages = searchTask.get();
      searchProgress.store(1.0F, std::memory_order_relaxed);
      scrollOffset = 0;
      scrollPosition = 0.0F;
      status = packages.empty() ? "No matching packages found."
                                : "Found " + std::to_string(packages.size()) +
                                      " packages. Select an action.";
    } catch (const std::exception& error) {
      status = std::string("Search failed: ") + error.what();
    }
  }

  void updateAction() {
    if (!actionTask.valid() ||
        actionTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return;
    }
    const bool wasCacheClear = cacheClearTask;
    cacheClearTask = false;
    activeActionPackage.clear();
    try {
      actionTask.get();
      if (wasCacheClear) {
        const std::string managerName =
            packageManager == pm::Manager::dnf ? "DNF" : "Pacman";
        status = managerName + " package cache cleared.";
        return;
      }
      status = "Transaction finished; refreshing results...";
      startSearch();
    } catch (const std::exception& error) {
      status = std::string("Transaction failed: ") + error.what();
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
      settingsStatus = std::string("Could not change font size: ") + error.what();
    }
  }

  void selectFontFace(const render::FontFace& font) {
    if (font.path.empty()) {
      try {
        renderer.useSystemFont();
        settings.fontPath.clear();
        persistSettings();
      } catch (const std::exception& error) {
        settingsStatus = std::string("Could not change font: ") + error.what();
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
      settingsStatus = std::string("Could not change font: ") + error.what();
    }
  }

  std::size_t currentDropdownIndex() const {
    if (dropdownKind == DropdownKind::theme) {
      const auto current = std::find(dropdownThemeChoices.begin(),
                                     dropdownThemeChoices.end(),
                                     settings.themeMode);
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
    return dropdownKind == DropdownKind::theme ? dropdownThemeChoices.size()
                                               : dropdownChoices.size();
  }

  bool dropdownChoiceIsCurrent(std::size_t index) const {
    if (dropdownKind == DropdownKind::theme) {
      return dropdownThemeChoices[index] == settings.themeMode;
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
      switch (dropdownThemeChoices[index]) {
        case ThemeMode::system:
          return "System";
        case ThemeMode::light:
          return "Light";
        case ThemeMode::dark:
          return "Dark";
      }
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
    if (kind == DropdownKind::fontFamily) {
      dropdownChoices.push_back({"", "System default", ""});
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
      dropdownThemeChoices = {ThemeMode::system, ThemeMode::light, ThemeMode::dark};
    }
    dropdownKind = kind;
    if (dropdownChoiceCount() == 0) {
      dropdownKind = DropdownKind::none;
      dropdownOpen = false;
      settingsStatus = "No font choices are available.";
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
        settings.themeMode = dropdownThemeChoices[choiceIndex];
        if (settings.themeMode == ThemeMode::system) {
          systemLightTheme = systemPreferredLightTheme().value_or(false);
          nextSystemThemeCheck =
              std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
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
        makeLayout(width, height, renderer.fontSize(), settings.compactList);
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
      previousFontSize = oldFontSize;
      fontSizeTransition = settings.animations ? 0.0F : 1.0F;
      systemLightTheme = systemPreferredLightTheme().value_or(false);
      nextSystemThemeCheck =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      persistSettings();
    } catch (const std::exception& error) {
      settingsStatus = std::string("Could not reset settings: ") + error.what();
    }
  }

  void persistSettings() {
    try {
      saveSettings(settings);
      settingsStatus = "Settings saved.";
    } catch (const std::exception& error) {
      settingsStatus = std::string("Settings changed, but not saved: ") + error.what();
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
                                     renderer.fontSize(), settings.compactList);

    if (cacheConfirmOpen) {
      if (contains(layout.cancelCacheButton, cursorX, cursorY)) {
        triggerButtonPress(layout.cancelCacheButton);
        cacheConfirmOpen = false;
      } else if (!contains(layout.cacheConfirmPanel, cursorX, cursorY)) {
        cacheConfirmOpen = false;
      } else if (contains(layout.confirmCacheButton, cursorX, cursorY)) {
        triggerButtonPress(layout.confirmCacheButton);
        cacheConfirmOpen = false;
        settingsOpen = false;
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
        settingsCategory = SettingsCategory::appearance;
        dropdownOpen = false;
        settingsStatus.clear();
        return;
      }
      if (contains(layout.behaviorTab, cursorX, cursorY)) {
        settingsCategory = SettingsCategory::behavior;
        dropdownOpen = false;
        settingsStatus.clear();
        return;
      }
      if (contains(layout.systemTab, cursorX, cursorY)) {
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
        } else if (contains(layout.themeButton, cursorX, cursorY)) {
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
            scrollPosition = static_cast<float>(scrollOffset);
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
        settingsOpen = false;
        dropdownOpen = false;
      } else if (contains(layout.settingsHeaderCloseButton, cursorX, cursorY)) {
        triggerButtonPress(layout.settingsHeaderCloseButton);
        settingsOpen = false;
        dropdownOpen = false;
      }
      return;
    }

    if (layout.showClearQueryButton && clearQueryButtonTransition > 0.5F &&
        contains(layout.clearQueryButton, cursorX, cursorY)) {
      triggerButtonPress(layout.clearQueryButton);
      searchFocused = true;
      clearQuery();
      return;
    }
    if (contains(layout.searchField, cursorX, cursorY)) {
      searchFocused = true;
      queryCursor = queryPositionAtCursor(layout.queryTextWidth, layout.queryTextX);
      selectionAnchor = queryCursor;
      draggingSearchSelection = true;
      lastCaretActivity = std::chrono::steady_clock::now();
      return;
    }
    if (contains(layout.searchButton, cursorX, cursorY)) {
      triggerButtonPress(layout.searchButton);
      searchFocused = false;
      startSearch();
      return;
    }
    if (!busy() && layout.visibleRows > 0 &&
        packages.size() > static_cast<std::size_t>(layout.visibleRows) &&
        contains(layout.packageScrollbarTrack, cursorX, cursorY)) {
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
    for (int row = 0; row < layout.visibleRows + (fractionalScroll > 0.0F ? 1 : 0);
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
                                       renderer.fontSize(), settings.compactList);
      const int maxOffset = std::max(
          0, static_cast<int>(packages.size()) - layout.visibleRows);
      scrollOffset = std::clamp(scrollOffset, 0, maxOffset);
      scrollPosition =
          std::min(scrollPosition, static_cast<float>(maxOffset));
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
                                         renderer.fontSize(), settings.compactList);
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
                                         renderer.fontSize(), settings.compactList);
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
        !query.empty() && framebufferWidth > 0 && framebufferHeight > 0 &&
        makeLayout(framebufferWidth, framebufferHeight, renderer.fontSize(),
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
      updateCursor(makeLayout(framebufferWidth, framebufferHeight,
                              renderer.fontSize(), settings.compactList));
    }
    if (settings.smoothScroll) {
      const float scrollBlend = 1.0F - std::exp(-frameDeltaSeconds * 20.0F);
      scrollPosition +=
          (static_cast<float>(scrollOffset) - scrollPosition) * scrollBlend;
      if (std::abs(scrollPosition - static_cast<float>(scrollOffset)) < 0.005F) {
        scrollPosition = static_cast<float>(scrollOffset);
      }
    } else {
      scrollPosition = static_cast<float>(scrollOffset);
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

  static std::string shortened(std::string value, std::size_t maxCharacters) {
    if (maxCharacters == 0) {
      return {};
    }
    std::size_t offset = 0;
    std::size_t characters = 0;
    while (offset < value.size() && characters < maxCharacters) {
      offset = nextCodepointPosition(value, offset);
      ++characters;
    }
    if (offset < value.size()) {
      if (maxCharacters > 3) {
        offset = 0;
        characters = 0;
        while (offset < value.size() && characters < maxCharacters - 3) {
          offset = nextCodepointPosition(value, offset);
          ++characters;
        }
        value.resize(offset);
        value += "...";
      } else {
        value.resize(offset);
      }
    }
    return value;
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
    addButtonSurface(rectangles, bounds, fill, border, hoverAmount, pressAmount);
    if (pressAmount > 0.0F) {
      addRoundedRect(rectangles, bounds, border, 12.0F, pressAmount * 0.12F);
    }
    if (rippleOpacity > 0.0F) {
      addRoundedRect(rectangles, rippleBounds, border,
                     rippleBounds.width * 0.5F, rippleOpacity);
    }

    const float iconScale = label.empty() ? 0.68F : 0.38F;
    const float iconSize =
        icon.empty() ? 0.0F
                     : std::min({24.0F, bounds.height * iconScale,
                                 bounds.width * iconScale});
    const float textWidth = label.empty() ? 0.0F : renderer.measureText(label);
    const float gap = iconSize > 0.0F && textWidth > 0.0F ? 7.0F : 0.0F;
    const float contentWidth = iconSize + gap + textWidth;
    float contentX =
        bounds.x + std::max(0.0F, (bounds.width - contentWidth) * 0.5F);
    if (iconSize > 0.0F) {
      const float iconY = bounds.y + (bounds.height - iconSize) * 0.5F;
      images.push_back(
          {iconPath(icon), contentX, iconY, iconSize, iconSize, 1.0F});
      contentX += iconSize + gap;
    }
    if (!label.empty()) {
      text.push_back({std::move(label), contentX,
                      bounds.y + bounds.height / 2.0F +
                          static_cast<float>(renderer.fontSize()) * 0.34F,
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
      lines.emplace_back("No description provided.");
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
      const float indicatorSize = std::min(18.0F, geometry.rowHeight * 0.58F);
      const float labelWidth =
          std::max(0.0F, rowBounds.width - indicatorSize - 28.0F);
      text.push_back(
          {shortened(label, static_cast<std::size_t>(
                                labelWidth / queryCharacterWidth())),
           rowBounds.x + 12.0F,
           rowBounds.y + rowBounds.height / 2.0F +
               static_cast<float>(renderer.fontSize()) * 0.34F,
           palette.text, dropdownTransition});
      if (currentChoice) {
        images.push_back(
            {iconPath("done.png"),
             rowBounds.x + rowBounds.width - indicatorSize - 10.0F,
             rowBounds.y + (rowBounds.height - indicatorSize) / 2.0F,
             indicatorSize, indicatorSize, dropdownTransition});
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

    if (!cacheConfirmOpen) {
      text.push_back({"Settings", panel.x + 30.0F, panel.y + 44.0F * scale,
                      palette.text});
      drawButton(rectangles, text, images, layout.settingsHeaderCloseButton, "",
                 palette.elevated, palette.outline, palette.text, "close.png");
      const bool narrowPanel = panel.width < 500.0F;
      const std::string appearanceLabel =
          narrowPanel ? "Look" : "Appearance";
      const std::string behaviorLabel =
          narrowPanel ? "Behavior" : "Behavior";
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
      drawButton(rectangles, text, images, layout.systemTab, "System",
                 tabFill(settingsCategory == SettingsCategory::system),
                 tabBorder(settingsCategory == SettingsCategory::system),
                 palette.text);

      if (settingsCategory == SettingsCategory::appearance) {
        text.push_back({narrowPanel ? "Font" : "Font size",
                        panel.x + 24.0F, panel.y + 130.0F * scale,
                        palette.secondaryText});
        const float fontProgress =
            settings.animations ? fontSizeTransition : 1.0F;
        const std::string currentSize =
            std::to_string(renderer.fontSize()) + " px";
        const std::string previousSize =
            std::to_string(previousFontSize) + " px";
        if (fontProgress < 1.0F) {
          text.push_back({previousSize, layout.fontSizeValueX,
                          panel.y + (124.0F - 6.0F * fontProgress) * scale,
                          palette.secondaryText, 1.0F - fontProgress});
        }
        text.push_back({currentSize, layout.fontSizeValueX,
                        panel.y + (130.0F + 6.0F * (1.0F - fontProgress)) * scale,
                        palette.text, fontProgress});
        text.push_back({narrowPanel ? "Face" : "Font family",
                        panel.x + (narrowPanel ? 24.0F : 30.0F),
                        panel.y + 176.0F * scale, palette.secondaryText});
        text.push_back({narrowPanel ? "Style" : "Font style",
                        panel.x + (narrowPanel ? 24.0F : 30.0F),
                        panel.y + 224.0F * scale, palette.secondaryText});
        text.push_back({"Theme", panel.x + 30.0F,
                        panel.y + 272.0F * scale, palette.secondaryText});
      } else if (settingsCategory == SettingsCategory::behavior) {
        text.push_back({"Compact list", panel.x + 30.0F,
                        panel.y + 140.0F * scale, palette.secondaryText});
        text.push_back({"Smooth scrolling", panel.x + 30.0F,
                        panel.y + 196.0F * scale, palette.secondaryText});
        text.push_back({"Click sparks", panel.x + 30.0F,
                        panel.y + 252.0F * scale, palette.secondaryText});
        text.push_back({"Animations", panel.x + 30.0F,
                        panel.y + 308.0F * scale, palette.secondaryText});
        text.push_back({narrowPanel ? "Descriptions" : "Package descriptions",
                        panel.x + 30.0F,
                        panel.y + 364.0F * scale, palette.secondaryText});
      } else {
        text.push_back({shortened("System theme follows the desktop preference.",
                                  static_cast<std::size_t>(
                                      std::max(1.0F, (panel.width - 48.0F) /
                                                        queryCharacterWidth()))),
                        panel.x + 24.0F, panel.y + 100.0F * scale,
                        palette.secondaryText});
        text.push_back({"Package cache", panel.x + 24.0F,
                        panel.y + 142.0F * scale, palette.secondaryText});
        text.push_back({shortened("Reset appearance and behavior settings.",
                                  static_cast<std::size_t>(
                                      std::max(1.0F, (panel.width - 48.0F) /
                                                        queryCharacterWidth()))),
                        panel.x + 24.0F, panel.y + 272.0F * scale,
                        palette.secondaryText});
      }

      if (!settingsStatus.empty()) {
        text.push_back(
            {shortened(settingsStatus,
                       static_cast<std::size_t>((panel.width - 48.0F) /
                                                std::max(1.0F, renderer.fontSize() * 0.55F))),
             panel.x + 24.0F, panel.y + 424.0F * scale, palette.secondaryText});
      }

      if (settingsCategory == SettingsCategory::appearance) {
        const float buttonCharacterWidth =
            std::max(1.0F, static_cast<float>(renderer.fontSize()) * 0.55F);
        const std::size_t familyLabelLimit = static_cast<std::size_t>(
            std::max(1.0F, (layout.fontFamilyButton.width - 24.0F) /
                               buttonCharacterWidth));
        const std::size_t styleLabelLimit = static_cast<std::size_t>(
            std::max(1.0F, (layout.fontStyleButton.width - 24.0F) /
                               buttonCharacterWidth));
        const std::size_t familyNameLimit =
            familyLabelLimit > 2 ? familyLabelLimit - 2 : familyLabelLimit;
        const std::size_t styleNameLimit =
            styleLabelLimit > 2 ? styleLabelLimit - 2 : styleLabelLimit;
        const std::string dropdownArrow = "\xE2\x96\xBE";
        std::string familyLabel =
            shortened(renderer.fontFamily(), familyNameLimit);
        std::string styleLabel = shortened(renderer.fontStyle(), styleNameLimit);
        familyLabel += " " + dropdownArrow;
        styleLabel += " " + dropdownArrow;
        std::string themeLabel(themeModeLabel(settings.themeMode));
        themeLabel += " " + dropdownArrow;

        drawButton(rectangles, text, images, layout.decreaseFontButton, "",
                   palette.elevated, palette.outline, palette.text, "remove.png");
        drawButton(rectangles, text, images, layout.increaseFontButton, "",
                   palette.elevated, palette.outline, palette.text, "add.png");
        drawButton(rectangles, text, images, layout.fontFamilyButton,
                   std::move(familyLabel), palette.elevated, palette.outline,
                   palette.text);
        drawButton(rectangles, text, images, layout.fontStyleButton,
                   std::move(styleLabel), palette.elevated, palette.outline,
                   palette.text);
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
      } else {
        drawButton(rectangles, text, images, layout.clearCacheButton, "Clear",
                   {0.38F, 0.20F, 0.23F}, {0.64F, 0.33F, 0.38F},
                   {1.0F, 0.88F, 0.89F}, "delete.png");
        drawButton(rectangles, text, images, layout.resetSettingsButton,
                   "Reset settings", palette.elevated, palette.outline,
                   palette.text);
      }
      drawButton(rectangles, text, images, layout.closeSettingsButton, "Done",
                 palette.primaryContainer, palette.primary, palette.text,
                 "done.png");
    } else {
      const Bounds& confirmation = layout.cacheConfirmPanel;
      const float confirmationScale = layout.confirmScale;
      addRoundedRect(rectangles,
                     {confirmation.x, confirmation.y + 6.0F, confirmation.width,
                      confirmation.height},
                     themeIsLight() ? std::array<float, 3>{0.76F, 0.74F, 0.79F}
                                    : std::array<float, 3>{0.035F, 0.03F, 0.05F},
                     24.0F);
      addRoundedRect(rectangles, confirmation, palette.outline, 24.0F);
      addRoundedRect(rectangles, {confirmation.x + 1.5F, confirmation.y + 1.5F,
                                  confirmation.width - 3.0F,
                                  confirmation.height - 3.0F},
                     palette.surface, 22.0F);
      const std::size_t confirmationTextLength =
          static_cast<std::size_t>(std::max(
              1.0F, (confirmation.width - 48.0F) / queryCharacterWidth()));
      text.push_back({shortened("Clear the entire package cache?",
                                confirmationTextLength),
                      confirmation.x + 24.0F,
                      confirmation.y + 48.0F * confirmationScale, palette.text});
      const std::string cacheEffect =
          packageManager == pm::Manager::dnf
              ? "Removes cached packages and repository metadata."
              : "Removes downloaded pacman packages.";
      text.push_back({shortened(cacheEffect, confirmationTextLength),
                      confirmation.x + 24.0F, confirmation.y + 94.0F * confirmationScale,
                      palette.secondaryText});
      const std::string cacheLocation =
          packageManager == pm::Manager::dnf ? "Location: /var/cache/dnf"
                                             : "Location: /var/cache/pacman/pkg";
      text.push_back({shortened(cacheLocation,
                                confirmationTextLength),
                      confirmation.x + 24.0F, confirmation.y + 124.0F * confirmationScale,
                      palette.secondaryText});
      drawButton(rectangles, text, images, layout.cancelCacheButton, "Cancel",
                 palette.elevated, palette.outline, palette.text, "close.png");
      drawButton(rectangles, text, images, layout.confirmCacheButton, "Clear",
                 {0.45F, 0.17F, 0.20F}, {0.68F, 0.30F, 0.34F},
                 {1.0F, 0.90F, 0.91F}, "delete.png");
    }
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

    text.push_back({"Package information", panel.x + 24.0F,
                    panel.y + 42.0F, palette.secondaryText});
    drawButton(rectangles, text, images, layout.infoDialogCloseButton, "",
               palette.elevated, palette.outline, palette.text, "close.png");
    const std::size_t titleLimit = static_cast<std::size_t>(
        std::max(1.0F, (panel.width - 64.0F) / queryCharacterWidth()));
    text.push_back({shortened(infoPackage.name, titleLimit), panel.x + 24.0F,
                    panel.y + 82.0F, palette.text});
    const float fontSize = static_cast<float>(renderer.fontSize());
    const float maxVersionWidth = std::max(0.0F, panel.width - 40.0F);
    const std::size_t versionLimit = static_cast<std::size_t>(std::max(
        1.0F, (maxVersionWidth - 24.0F) / queryCharacterWidth()));
    const std::string versionText = shortened(infoPackage.version, versionLimit);
    const float versionBadgeHeight = std::max(28.0F, fontSize + 10.0F);
    const float versionBadgeWidth = std::min(
        maxVersionWidth, renderer.measureText(versionText) + 24.0F);
    const Bounds versionBadge{panel.x + 20.0F, panel.y + 91.0F,
                              versionBadgeWidth, versionBadgeHeight};
    addRoundedRect(rectangles, versionBadge, palette.elevated, 12.0F);
    const float versionBaseline =
        versionBadge.y + (versionBadge.height - fontSize) * 0.5F + fontSize * 0.8F;
    text.push_back({versionText, versionBadge.x + 12.0F, versionBaseline,
                    palette.secondaryText});

    const float lineHeight =
        std::max(18.0F, fontSize * 1.45F);
    const float descriptionHeaderBaseline =
        versionBadge.y + versionBadge.height + 12.0F;
    const float firstDescriptionBaseline =
        descriptionHeaderBaseline + lineHeight;
    const float lastDescriptionBaseline = panel.y + panel.height - 18.0F;
    const int maxLines = std::max(
        1, static_cast<int>(std::floor(
               (lastDescriptionBaseline - firstDescriptionBaseline) /
               lineHeight)) +
               1);
    text.push_back({"Description", panel.x + 24.0F,
                    descriptionHeaderBaseline, palette.primary});
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
      lines.push_back({"No description provided.", false, false});
    }
    const bool truncated = lines.size() > static_cast<std::size_t>(maxLines);
    if (truncated) {
      lines.resize(static_cast<std::size_t>(maxLines));
    }
    const std::size_t descriptionLimit = static_cast<std::size_t>(
        std::max(1.0F, (panel.width - 48.0F) / queryCharacterWidth()));
    if (truncated && !lines.empty()) {
      const std::size_t prefixLimit =
          descriptionLimit > 3 ? descriptionLimit - 3 : descriptionLimit;
      lines.back().text = shortened(std::move(lines.back().text), prefixLimit) + "...";
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const float baseline = firstDescriptionBaseline +
                             static_cast<float>(index) * lineHeight;
      if (lines[index].heading) {
        const float headingWidth =
            std::min(textWidth, renderer.measureText(lines[index].text) + 16.0F);
        addRoundedRect(rectangles,
                       {panel.x + 20.0F, baseline - lineHeight + 2.0F,
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

    for (render::RectItem& item : rectangles) {
      item.opacity *= infoTransition;
    }
    for (render::TextItem& item : text) {
      item.opacity *= infoTransition;
    }
    for (render::ImageItem& item : images) {
      item.opacity *= infoTransition;
    }
    renderer.drawOverlay(width, height, rectangles, text, images);
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
    const float characterWidth = queryCharacterWidth();

    rectangles.push_back(
        {0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height),
         palette.background});
    rectangles.push_back({0.0F, 0.0F, static_cast<float>(width), layout.headerHeight,
                          palette.header});
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
    images.push_back({iconPath("Flux.png"), logoSurface.x + logoInset,
                      logoSurface.y + logoInset,
                      logoSize - logoInset * 2.0F,
                      logoSize - logoInset * 2.0F, 1.0F});
    text.push_back({"Flux " FLUX_VERSION, layout.horizontalMargin + logoSize + 8.0F,
                    layout.headerHeight * 0.67F, palette.text});
    const std::size_t statusLimit = static_cast<std::size_t>(
        std::max(1.0F, (static_cast<float>(width) - layout.horizontalMargin * 2.0F) /
                           queryCharacterWidth()));
    text.push_back({shortened(status, statusLimit), layout.horizontalMargin + 16.0F,
                    layout.statusBaseline, palette.secondaryText});
    text.push_back({"Packages", layout.resultsPanel.x + 20.0F,
                    layout.packagesTitleBaseline, palette.secondaryText});

    drawButton(rectangles, text, images, layout.settingsButton, "",
               palette.elevated, palette.outline, palette.text, "settings.png");
    drawButton(rectangles, text, images, layout.searchButton, "Search",
               palette.primaryContainer, palette.primary, palette.text);
    if (layout.showClearQueryButton && clearQueryButtonTransition > 0.005F) {
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
                 "close.png", {}, clearQueryButtonTransition);
    }

    const float availableTextWidth = layout.queryTextWidth;
    const auto [viewStart, viewEnd] = queryViewRange(availableTextWidth);
    lastQueryViewStartWidth =
        query.empty()
            ? 0.0F
            : renderer.measureText(std::string_view(query).substr(0, viewStart));
    if (hasSelection()) {
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
    if (settings.animations) {
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
    if (query.empty()) {
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
        static_cast<std::size_t>(layout.visibleRows + (fractionalScroll > 0.0F ? 1 : 0)));
    for (std::size_t row = 0; row < count; ++row) {
      const pm::Package& package = packages[first + row];
      const float rowY = layout.firstRowY +
                         (static_cast<float>(row) - fractionalScroll) * layout.rowHeight;
      const float rowHeight = layout.rowHeight - 5.0F;
      const Bounds rowBounds{
          layout.horizontalMargin + 8.0F, rowY,
          std::max(0.0F, static_cast<float>(width) -
                             layout.horizontalMargin * 2.0F - 16.0F),
          rowHeight};
      addRoundedRect(packageRectangles, rowBounds, palette.outline, 16.0F);
      addRoundedRect(packageRectangles,
                     {rowBounds.x + 1.0F, rowBounds.y + 1.0F, rowBounds.width - 2.0F,
                      rowBounds.height - 2.0F},
                     palette.elevated, 15.0F);
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
                 palette.elevated, palette.outline, palette.text, "help.png",
                 "package-help:" + package.name);
      drawButton(packageRectangles, packageText, packageImages, actionButton,
                 installed ? "Remove" : "Install",
                 installed ? std::array<float, 3>{0.38F, 0.20F, 0.23F}
                           : palette.primaryContainer,
                 installed ? std::array<float, 3>{0.64F, 0.33F, 0.38F}
                           : palette.primary,
                 palette.text, {},
                 "package-action:" + package.name);

      std::string packageLabel = package.name + "  " + package.version;
      const float textRight = layout.helpButtonX;
      const float packageTextWidth =
          std::max(0.0F, textRight - layout.rowContentX - 12.0F);
      const std::size_t packageTextLength = static_cast<std::size_t>(
          packageTextWidth / std::max(1.0F, characterWidth));
      packageLabel = shortened(std::move(packageLabel), packageTextLength);
      packageText.push_back({std::move(packageLabel), layout.rowContentX,
                             rowY + layout.rowHeight * 0.40F, palette.text});
      if (settings.showPackageDescriptions && !actionInProgress &&
          !package.description.empty() && width >= 600 &&
          rowHeight >= 54.0F) {
        const std::size_t descriptionLength = static_cast<std::size_t>(
            packageTextWidth / std::max(1.0F, characterWidth));
        packageText.push_back(
            {shortened(package.description, descriptionLength),
             layout.rowContentX, rowY + layout.rowHeight * 0.78F,
             palette.secondaryText});
      }
    }

    if (packages.empty() && !busy() &&
        (status == "Type a package name and press Enter to search." ||
         status == "No matching packages found.")) {
      const bool noMatches = status == "No matching packages found.";
      const float centerX = static_cast<float>(width) * 0.5F;
      const float centerY =
          layout.firstRowY +
          (layout.resultsPanel.y + layout.resultsPanel.height - layout.firstRowY) *
              0.5F;
      const Bounds iconSurface{centerX - 34.0F, centerY - 82.0F, 68.0F, 68.0F};
      addRoundedRect(packageRectangles, iconSurface, palette.elevated, 34.0F);
      packageImages.push_back(
          {iconPath("search.png"), centerX - 16.0F, centerY - 64.0F,
           32.0F, 32.0F, 0.82F});
      const std::size_t detailLimit = static_cast<std::size_t>(std::max(
          1.0F, (layout.resultsPanel.width - 48.0F) /
                    std::max(1.0F, characterWidth)));
      const std::string heading = shortened(
          noMatches ? "No packages found" : "Find your next app", detailLimit);
      const std::string detail = shortened(
          noMatches ? "Try another name or a shorter search."
                    : "Search package names to browse available software.",
          detailLimit);
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

    const Bounds scrollbar = scrollbarThumb(layout);
    if (scrollbar.height > 0.0F) {
      addRoundedRect(packageRectangles, layout.packageScrollbarTrack,
                     palette.outline, 3.0F,
                     0.30F);
      addRoundedRect(packageRectangles, scrollbar, palette.primary, 3.0F, 0.90F);
    }

    if (packages.size() > count) {
      const std::string range =
          "Showing " + std::to_string(first + 1) + "-" +
          std::to_string(first + count) + " of " + std::to_string(packages.size()) +
          " packages - use mouse wheel to scroll";
      const float rangeBaseline =
          static_cast<float>(height) - layout.horizontalMargin * 0.5F;
      const std::size_t rangeLength = static_cast<std::size_t>(
          std::max(1.0F, (static_cast<float>(width) -
                          layout.horizontalMargin * 2.0F) /
                             characterWidth));
      text.push_back({shortened(range, rangeLength), layout.horizontalMargin + 16.0F,
                      rangeBaseline, palette.secondaryText});
    }

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
  }

  void draw(int width, int height) const {
    if (width <= 0 || height <= 0) {
      renderer.draw(width, height, {}, {});
      return;
    }

    const Layout layout =
        makeLayout(width, height, renderer.fontSize(), settings.compactList);
    const Palette& palette = themeIsLight() ? kLightPalette : kDarkPalette;
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
