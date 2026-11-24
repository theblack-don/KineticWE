#include "cursors_page.h"

#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "ini_config.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "system/process.h"
#include "theme/kde_color_scheme.h"
#include "theme_discovery.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

  constexpr Logger kLog("kinetic-settings");

  constexpr float kTileWidth = 150.0F;
  constexpr float kTileHeight = 118.0F;
  constexpr float kPreviewSize = 56.0F;
  constexpr int kDefaultCursorSize = 24;

  constexpr int kCursorSizes[] = {16, 20, 24, 28, 32, 40, 48, 56, 64};

  std::filesystem::path appearanceKwePath() {
    const auto dir = KineticweKwe::defaultPath().parent_path();
    return dir / "appearance.kwe";
  }

  [[nodiscard]] std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal) {
    return ui::label({
        .text = std::string(text),
        .fontSize = fontSize,
        .fontWeight = fontWeight,
        .color = color,
    });
  }

  // The user's real config home ($XDG_CONFIG_HOME, else ~/.config) — where
  // kdeglobals lives. The shell process keeps the real XDG_CONFIG_HOME.
  [[nodiscard]] std::filesystem::path realConfigHome() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config";
    }
    return std::filesystem::path(".");
  }

  // The user's real home ($HOME), used for paths XDG does not cover (the
  // traditional ~/.icons cursor theme root).
  [[nodiscard]] std::filesystem::path realHome() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home);
    }
    return std::filesystem::path(".");
  }

  // X11/Xwayland (libXcursor), GTK and xsettingsd each keep their own copy of
  // the cursor theme; update them all so apps that do not consult the
  // compositor or gsettings (e.g. Steam) follow the selection. Best effort: a
  // read-only or hand-edited file must not fail the whole Apply.
  void syncCursorFallbackFiles(std::string_view theme, int size, const std::filesystem::path& configHome) {
    const std::string themeValue(theme);
    const std::string sizeValue = std::to_string(size);
    const auto report = [](const char* label, const std::string& message) {
      kLog.warn("kinetic-settings: failed to update {} cursor setting: {}", label, message);
    };

    // kcminputrc: the X11/KDE input config (Xwayland and KDE consumers read it).
    {
      const auto path = configHome / "kcminputrc";
      std::string err;
      if (themeValue.empty()) {
        if (!kinetic_settings::setKey(path, "Mouse", "cursorTheme", "", false, &err)) {
          report("kcminputrc", err);
        }
      } else if (!kinetic_settings::setKey(path, "Mouse", "cursorTheme", themeValue, true, &err)) {
        report("kcminputrc", err);
      }
      err.clear();
      if (!kinetic_settings::setKey(path, "Mouse", "cursorSize", sizeValue, true, &err)) {
        report("kcminputrc", err);
      }
    }

    // ~/.icons/default/index.theme: libXcursor's "default" theme — what Xwayland
    // apps (Steam) resolve when XCURSOR_THEME is unset.
    {
      const auto path = realHome() / ".icons" / "default" / "index.theme";
      std::string err;
      if (themeValue.empty()) {
        if (!kinetic_settings::setKey(path, "Icon Theme", "Inherits", "", false, &err)) {
          report("~/.icons/default/index.theme", err);
        }
      } else if (!kinetic_settings::setKey(path, "Icon Theme", "Inherits", themeValue, true, &err)) {
        report("~/.icons/default/index.theme", err);
      }
    }

    // GTK 3/4 read the cursor from their settings.ini; gsettings covers distros
    // with the schema installed, the ini is the reliable fallback.
    for (const char* tool : {"gtk-3.0", "gtk-4.0"}) {
      const auto path = configHome / tool / "settings.ini";
      std::string err;
      if (themeValue.empty()) {
        if (!kinetic_settings::setKey(path, "Settings", "gtk-cursor-theme-name", "", false, &err)) {
          report(tool, err);
        }
      } else if (!kinetic_settings::setKey(path, "Settings", "gtk-cursor-theme-name", themeValue, true, &err)) {
        report(tool, err);
      }
      err.clear();
      if (!kinetic_settings::setKey(path, "Settings", "gtk-cursor-theme-size", sizeValue, true, &err)) {
        report(tool, err);
      }
    }

    // xsettingsd: the XSETTINGS properties X11 GTK apps read.
    {
      const auto path = configHome / "xsettingsd" / "xsettingsd.conf";
      std::string err;
      if (themeValue.empty()) {
        if (!kinetic_settings::setFlatKey(path, "Gtk/CursorThemeName", "", " ", true, false, &err)) {
          report("xsettingsd.conf", err);
        }
      } else if (!kinetic_settings::setFlatKey(path, "Gtk/CursorThemeName", themeValue, " ", true, true, &err)) {
        report("xsettingsd.conf", err);
      }
      err.clear();
      if (!kinetic_settings::setFlatKey(path, "Gtk/CursorThemeSize", sizeValue, " ", false, true, &err)) {
        report("xsettingsd.conf", err);
      }
    }

    // environment.d: XCURSOR_THEME/XCURSOR_SIZE for GUI apps started by the
    // systemd user session (takes effect at next login).
    {
      const auto path = configHome / "environment.d" / "90-cursor.conf";
      std::string err;
      if (themeValue.empty()) {
        if (!kinetic_settings::setFlatKey(path, "XCURSOR_THEME", "", "=", false, false, &err)) {
          report("environment.d", err);
        }
      } else if (!kinetic_settings::setFlatKey(path, "XCURSOR_THEME", themeValue, "=", false, true, &err)) {
        report("environment.d", err);
      }
      err.clear();
      if (!kinetic_settings::setFlatKey(path, "XCURSOR_SIZE", sizeValue, "=", false, true, &err)) {
        report("environment.d", err);
      }
    }
  }

  [[nodiscard]] std::unique_ptr<Button> makeResetButton(
      std::string path, bool pendingConfirmation, float scale, const CursorsPage::Callbacks& callbacks
  ) {
    return ui::button({
        .text = i18n::tr(pendingConfirmation ? "settings.actions.confirm-reset" : "settings.actions.reset"),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = pendingConfirmation ? ButtonVariant::Destructive : ButtonVariant::Ghost,
        .minHeight = Style::controlHeightSm * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [path = std::move(path), pendingConfirmation, callbacks]() mutable {
          if (!pendingConfirmation) {
            if (callbacks.armReset) {
              callbacks.armReset(std::move(path));
            }
            return;
          }
          if (callbacks.confirmReset) {
            callbacks.confirmReset(std::move(path));
          }
        },
    });
  }

} // namespace

std::vector<std::string> CursorsPage::resettableKeyPaths() const {
  return {themeKeyPath(), sizeKeyPath()};
}

void CursorsPage::resetKeyPath(std::string_view path) {
  if (path == themeKeyPath()) {
    stageTheme(ThemeValue{});
  } else if (path == sizeKeyPath()) {
    stageSize(kDefaultCursorSize);
  }
}

void CursorsPage::resetAll() {
  stageTheme(ThemeValue{});
  stageSize(kDefaultCursorSize);
}

void CursorsPage::ensureLoaded() {
  if (!m_loaded) {
    m_config.setPath(appearanceKwePath());
    m_config.load();
    m_loaded = true;
  }
}

CursorsPage::ThemeValue CursorsPage::fileBackedTheme() const {
  return m_config.readString("Appearance", themeGroupKey()).value_or(ThemeValue{});
}

CursorsPage::SizeValue CursorsPage::fileBackedSize() const {
  return m_config.readInt("Appearance", sizeGroupKey(), kDefaultCursorSize);
}

CursorsPage::ThemeValue CursorsPage::effectiveTheme() const {
  const auto staged = m_pending.find(themeKeyPath());
  if (staged != m_pending.end()) {
    return staged->second;
  }
  return fileBackedTheme();
}

CursorsPage::SizeValue CursorsPage::effectiveSize() const {
  const auto staged = m_pending.find(sizeKeyPath());
  if (staged != m_pending.end()) {
    int parsed = 0;
    const auto [end, ec] = std::from_chars(staged->second.data(), staged->second.data() + staged->second.size(), parsed);
    if (ec == std::errc{} && end == staged->second.data() + staged->second.size()) {
      return parsed;
    }
  }
  return fileBackedSize();
}

void CursorsPage::stageTheme(const ThemeValue& value) {
  ensureLoaded();
  if (value == fileBackedTheme()) {
    m_pending.erase(themeKeyPath());
  } else {
    m_pending[themeKeyPath()] = value;
  }
}

void CursorsPage::stageSize(SizeValue value) {
  ensureLoaded();
  const std::string encoded = std::to_string(value);
  if (value == fileBackedSize()) {
    m_pending.erase(sizeKeyPath());
  } else {
    m_pending[sizeKeyPath()] = encoded;
  }
}

void CursorsPage::stageAndRebuild(const std::string& keyPath, const std::string& value, const Callbacks& callbacks) {
  ensureLoaded();
  if (keyPath == themeKeyPath()) {
    stageTheme(value);
  } else if (keyPath == sizeKeyPath()) {
    int parsed = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec == std::errc{} && end == value.data() + value.size()) {
      stageSize(parsed);
    }
  }
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

bool CursorsPage::applyTheme(std::string_view theme, int size, std::string* error) {
  // 1. Compositor config: kineticwe.kwe [Mouse] cursorTheme / cursorSize. KWin
  // reads this group; a KGlobalSettings CursorChanged notify triggers a reload.
  {
    KineticweKwe compositor;
    compositor.setPath(KineticweKwe::defaultPath());
    compositor.load();
    if (theme.empty()) {
      compositor.removeKey("Mouse", "cursorTheme");
    } else {
      compositor.setString("Mouse", "cursorTheme", theme);
    }
    compositor.setInt("Mouse", "cursorSize", size);
    if (!compositor.save()) {
      if (error != nullptr) {
        *error = "failed to write " + compositor.path().string();
      }
      return false;
    }
  }

  // 2. kdeglobals so Qt/KDE apps read the theme too ([KDE] cursorTheme/cursorSize).
  const std::filesystem::path schemePath = std::filesystem::temp_directory_path() / "kineticwe-cursors.colors";
  {
    std::ofstream out(schemePath);
    if (!out) {
      if (error != nullptr) {
        *error = "failed to write temporary cursor scheme";
      }
      return false;
    }
    out << "[KDE]\n";
    if (!theme.empty()) {
      out << "cursorTheme=" << theme << "\n";
    }
    out << "cursorSize=" << size << "\n";
    if (!out) {
      if (error != nullptr) {
        *error = "failed to write temporary cursor scheme";
      }
      return false;
    }
  }
  const std::filesystem::path kdeGlobals = realConfigHome() / "kdeglobals";
  std::string mergeError;
  if (!noctalia::theme::mergeKdeColorScheme(schemePath, kdeGlobals, &mergeError)) {
    if (error != nullptr) {
      *error = std::move(mergeError);
    }
    return false;
  }

  // The merge above only ever writes keys; clear a stale cursorTheme override
  // when the user reverts to the system default.
  if (theme.empty()) {
    std::string removeError;
    if (!kinetic_settings::setKey(kdeGlobals, "KDE", "cursorTheme", "", false, &removeError)) {
      kLog.warn("kinetic-settings: failed to clear the kdeglobals cursor theme: {}", removeError);
    }
  }

  // 3. Notify KGlobalSettings (type 5 = CursorChanged) so the compositor
  // re-reads its config and swaps the cursor live.
  if (const std::string notifyError = noctalia::theme::notifyKdeChange(5, 0); !notifyError.empty()) {
    kLog.warn("kinetic-settings: failed to notify compositor of cursor change: {}", notifyError);
  }

  // 4. GTK apps read cursor-theme/cursor-size from gsettings.
  if (process::commandExists("gsettings")) {
    if (!theme.empty()) {
      (void)process::runAsync(
          "gsettings set org.gnome.desktop.interface cursor-theme \"" + std::string(theme) + "\""
      );
    }
    (void)process::runAsync("gsettings set org.gnome.desktop.interface cursor-size " + std::to_string(size));
  } else if (process::commandExists("dconf")) {
    if (!theme.empty()) {
      (void)process::runAsync(
          "dconf write /org/gnome/desktop/interface/cursor-theme \"'" + std::string(theme) + "'\""
      );
    }
    (void)process::runAsync("dconf write /org/gnome/desktop/interface/cursor-size " + std::to_string(size));
  }

  // 5. X11/Xwayland, GTK ini and xsettingsd copies, so apps that bypass the
  // compositor/gsettings (notably Steam under Xwayland) follow the selection.
  syncCursorFallbackFiles(theme, size, realConfigHome());

  return true;
}

bool CursorsPage::applyAll() {
  ensureLoaded();
  if (m_pending.empty()) {
    return true;
  }
  for (const auto& [path, value] : m_pending) {
    if (path == themeKeyPath()) {
      if (value.empty()) {
        m_config.removeKey("Appearance", themeGroupKey());
      } else {
        m_config.setString("Appearance", themeGroupKey(), value);
      }
    } else if (path == sizeKeyPath()) {
      int parsed = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (ec == std::errc{} && end == value.data() + value.size()) {
        m_config.setInt("Appearance", sizeGroupKey(), parsed);
      }
    }
  }
  if (!m_config.save()) {
    m_errorNotice = "Failed to write appearance.kwe. Your changes were kept; try Apply again.";
    kLog.warn("kinetic-settings: failed to write {}", m_config.path().string());
    return false;
  }

  // Apply the effective selection system-wide.
  const ThemeValue theme = effectiveTheme();
  const SizeValue size = effectiveSize();
  std::string error;
  if (!applyTheme(theme, size, &error)) {
    m_errorNotice = "Failed to apply the cursor theme: " + error;
    kLog.warn("kinetic-settings: failed to apply cursor theme: {}", error);
    return false;
  }

  m_pending.clear();
  m_errorNotice.clear();
  return true;
}

std::unique_ptr<Node> CursorsPage::buildTile(
    float scale, Renderer& renderer, const std::string& displayName, const std::string& cursorFile, bool selected,
    std::function<void()> onClick
) {
  const float tileW = kTileWidth * scale;
  const float tileH = kTileHeight * scale;
  const float previewSize = kPreviewSize * scale;

  Flex* cardBody = nullptr;
  auto card = ui::column({
      .out = &cardBody,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs * scale,
      .padding = Style::spaceXs * scale,
      .fill = selected ? colorSpecFromRole(ColorRole::Primary, 0.15F) : colorSpecFromRole(ColorRole::SurfaceContainer),
      .radius = Style::scaledRadiusMd(scale),
      .border = selected ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec(),
      .borderWidth = selected ? Style::emphasizedBorderWidth : 0.0F,
      .width = tileW,
      .height = tileH,
  });

  std::vector<std::uint8_t> rgba;
  int previewW = 0;
  int previewH = 0;
  if (!cursorFile.empty() && theme_discovery::decodeXcursorImage(cursorFile, rgba, previewW, previewH)) {
    auto img = ui::image({
        .fit = ImageFit::Contain,
        .width = previewSize,
        .height = previewSize,
    });
    img->setSourceRaw(
        renderer, rgba.data(), rgba.size(), previewW, previewH, previewW * 4, PixmapFormat::RGBA, false
    );
    card->addChild(std::move(img));
  } else {
    card->addChild(ui::glyph({
        .glyph = "mouse-2",
        .glyphSize = previewSize,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    }));
  }

  card->addChild(ui::label({
      .text = displayName,
      .fontSize = Style::fontSizeCaption * scale,
      .fontWeight = selected ? FontWeight::Bold : FontWeight::Normal,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxWidth = (kTileWidth - Style::spaceSm * 2.0F) * scale,
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));

  auto tile = ui::inputArea({
      .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
      .width = tileW,
      .height = tileH,
      .onEnter = [cardBody](const InputArea::PointerData&) {
        if (cardBody != nullptr) {
          cardBody->setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
        }
      },
      .onLeave = [cardBody]() {
        if (cardBody != nullptr) {
          cardBody->clearBorder();
        }
      },
      .onClick = [onClick = std::move(onClick)](const InputArea::PointerData&) { onClick(); },
  });
  tile->addChild(std::move(card));
  return tile;
}

std::unique_ptr<Node> CursorsPage::build(
    float scale, Renderer& renderer, const Filter& filter, std::string_view armedResetKey, const Callbacks& callbacks
) {
  ensureLoaded();

  auto root = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
      .fill = clearColorSpec(),
      .fillWidth = true,
  });

  auto headerRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
  headerRow->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .glyph = "mouse-2",
          .glyphSize = Style::fontSizeHeader * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeLabel("Cursors", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
  ));
  // Opens the cursor themes folder so the user can browse/install themes directly.
  headerRow->addChild(ui::button({
      .glyph = "folder-open",
      .glyphSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Ghost,
      .tooltip = "Open the cursor themes folder",
      .minHeight = Style::controlHeightSm * scale,
      .padding = Style::spaceXs * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = []() { (void)theme_discovery::openIconRoot(); },
  }));
  headerRow->addChild(ui::spacer());
  if (!effectiveTheme().empty() || effectiveSize() != kDefaultCursorSize) {
    headerRow->addChild(makeResetButton(sizeKeyPath(), armedResetKey == sizeKeyPath(), scale, callbacks));
  }
  root->addChild(std::move(headerRow));

  root->addChild(makeLabel(
      "Choose the cursor theme used across the desktop. The size applies to the "
      "selected theme (or the system default).",
      Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));

  if (!m_errorNotice.empty()) {
    root->addChild(ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .paddingV = Style::spaceXs * scale,
         .paddingH = Style::spaceMd * scale,
         .fill = colorSpecFromRole(ColorRole::Error, 0.15F),
         .radius = Style::scaledRadiusMd(scale)},
        makeLabel(m_errorNotice, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Error))
    ));
  }

  const ThemeValue currentTheme = effectiveTheme();
  const bool systemSelected = currentTheme.empty();

  auto grid = ui::row({
      .align = FlexAlign::Start,
      .wrap = true,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
  });

  const bool searchActive = !filter.searchQuery.empty();
  const bool showSystem = !filter.showOverriddenOnly
      && (!searchActive || StringUtils::containsInsensitive("System default", filter.searchQuery));
  if (showSystem) {
    grid->addChild(buildTile(
        scale, renderer, "System default", {}, systemSelected,
        [this, callbacks]() { stageAndRebuild(themeKeyPath(), ThemeValue{}, callbacks); }
    ));
  }

  bool anyTiles = false;
  for (const auto& theme : theme_discovery::cursorThemes()) {
    const bool matchesSearch = filter.searchQuery.empty()
        || StringUtils::containsInsensitive(theme.displayName, filter.searchQuery)
        || StringUtils::containsInsensitive(theme.name, filter.searchQuery);
    const bool matchesOverride = !filter.showOverriddenOnly || theme.name == currentTheme;
    if (!matchesSearch || !matchesOverride) {
      continue;
    }
    const bool selected = theme.name == currentTheme;
    grid->addChild(buildTile(
        scale, renderer, theme.displayName, theme_discovery::cursorPreviewFile(theme.path), selected,
        [this, callbacks, theme]() { stageAndRebuild(themeKeyPath(), ThemeValue{theme.name}, callbacks); }
    ));
    anyTiles = true;
  }

  const bool anyThemes = showSystem || anyTiles;
  const bool matchSize = !searchActive
      || StringUtils::containsInsensitive("Cursor size", filter.searchQuery)
      || StringUtils::containsInsensitive("size", filter.searchQuery);
  if (!searchActive && !anyThemes) {
    root->addChild(makeLabel(
        "No cursor themes match the current filters.", Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }
  if (searchActive && !anyThemes && !matchSize) {
    // Global search drops pages with no hits.
    return nullptr;
  }

  root->addChild(std::move(grid));

  // Cursor size selector.
  if (!searchActive || matchSize) {
    root->addChild(ui::separator());
  }

  auto sizeRow = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceXs * scale,
      .paddingV = 2.0F * scale,
      .fillWidth = true,
  });
  auto sizeCopy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  sizeCopy->addChild(makeLabel(
      "Cursor size", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  sizeCopy->addChild(makeLabel(
      "The size of the pointer in pixels.", Style::fontSizeCaption * scale,
      colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));
  sizeRow->addChild(std::move(sizeCopy));

  std::vector<std::string> sizeLabels;
  sizeLabels.reserve(std::size(kCursorSizes));
  for (const int size : kCursorSizes) {
    sizeLabels.push_back(std::to_string(size));
  }
  const int currentSize = effectiveSize();
  std::optional<std::size_t> selectedIndex;
  for (std::size_t i = 0; i < std::size(kCursorSizes); ++i) {
    if (kCursorSizes[i] == currentSize) {
      selectedIndex = i;
      break;
    }
  }
  sizeRow->addChild(ui::select({
      .options = sizeLabels,
      .selectedIndex = selectedIndex,
      .clearSelection = !selectedIndex.has_value(),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .glyphSize = Style::fontSizeBody * scale,
      .width = 110.0F * scale,
      .height = Style::controlHeight * scale,
      .onSelectionChanged = [this, callbacks](std::size_t index, std::string_view /*label*/) {
        if (index < std::size(kCursorSizes)) {
          stageAndRebuild(sizeKeyPath(), std::to_string(kCursorSizes[index]), callbacks);
        }
      },
  }));
  if (!searchActive || matchSize) {
    root->addChild(std::move(sizeRow));
  }

  return root;
}