#pragma once

#include <filesystem>
#include <string>

namespace noctalia::theme {

  struct [[nodiscard]] KdeColorSchemeApplyResult {
    bool success = false;
    std::string error;
    std::string notificationError;
  };

  [[nodiscard]] bool mergeKdeColorScheme(
      const std::filesystem::path& schemePath, const std::filesystem::path& kdeGlobalsPath, std::string* error
  );

  KdeColorSchemeApplyResult applyKdeColorScheme(const std::filesystem::path& schemePath);

  // Emits org.kde.KGlobalSettings.notifyChange(0,0) on the session bus so Qt/KDE
  // apps (including qt5ct) reload their palette. Returns an error string on
  // failure, empty on success. Safe to call after rewriting the color scheme.
  [[nodiscard]] std::string notifyKdeColorSchemeChanged();

  // Emits org.kde.KGlobalSettings.notifyChange(type, arg) on the session bus.
  // Used for non-palette settings the compositor/KDE apps react to live:
  //   type 4 = IconChanged (icons)
  //   type 5 = CursorChanged (cursor theme — the compositor re-reads its config)
  // Returns an error string on failure, empty on success.
  [[nodiscard]] std::string notifyKdeChange(std::int32_t type, std::int32_t arg = 0);

} // namespace noctalia::theme
