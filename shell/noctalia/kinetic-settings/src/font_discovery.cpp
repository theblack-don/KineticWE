#include "font_discovery.h"

#include "core/log.h"
#include "shell/settings/font_family_catalog.h"
#include "system/process.h"
#include "theme_discovery.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

  constexpr Logger kLog("kinetic-settings");

  // Function-local static cache, dropped by refreshFamilies() so the next
  // families() call re-runs fc-list.
  std::vector<std::string>& cachedFamilies() {
    static std::vector<std::string> families;
    return families;
  }

  bool& familyCacheLoaded() {
    static bool loaded = false;
    return loaded;
  }

  std::vector<std::string> discoverFamilies() {
    if (!process::commandExists("fc-list")) {
      kLog.warn("font discovery: fc-list is not installed");
      return {};
    }

    const auto result = process::runSync({"fc-list", ":", "family"});
    if (!result) {
      kLog.warn("font discovery: fc-list failed (exit {}, timed out {})", result.exitCode, result.timedOut);
      return {};
    }

    return settings::parseFontFamilyList(result.out);
  }

} // namespace

namespace font_discovery {

const std::vector<std::string>& families() {
  if (!familyCacheLoaded()) {
    cachedFamilies() = discoverFamilies();
    familyCacheLoaded() = true;
  }
  return cachedFamilies();
}

void refreshFamilies() {
  cachedFamilies().clear();
  familyCacheLoaded() = false;
}

std::string userFontsPath() {
  if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && dataHome[0] != '\0') {
    return std::string(dataHome) + "/fonts";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::string(home) + "/.local/share/fonts";
  }
  return {};
}

bool openUserFontsDirectory() {
  const std::string path = userFontsPath();
  if (path.empty()) {
    kLog.warn("font discovery: could not resolve the user font directory");
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    kLog.warn("font discovery: failed to create '{}': {}", path, ec.message());
    return false;
  }

  return theme_discovery::openDirectory(path);
}

} // namespace font_discovery
