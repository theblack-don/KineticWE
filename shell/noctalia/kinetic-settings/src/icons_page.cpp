#include "icons_page.h"

#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "ini_config.h"
#include "render/core/renderer.h"
#include "system/process.h"
#include "theme/kde_color_scheme.h"
#include "theme_discovery.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("kinetic-settings");

  constexpr float kTileWidth = 150.0F;
  constexpr float kTileHeight = 118.0F;
  constexpr float kPreviewSize = 56.0F;

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
  // kdeglobals lives. The shell process (which embeds kinetic-settings) keeps
  // the real XDG_CONFIG_HOME, so this resolves to the shared config location.
  [[nodiscard]] std::filesystem::path realConfigHome() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config";
    }
    return std::filesystem::path(".");
  }

  // A preview icon for a theme: tries a few well-known names so themes that
  // don't ship a "folder" still resolve something (SVG preferred).
  [[nodiscard]] std::string resolvePreview(const theme_discovery::ThemeInfo& theme) {
    for (const char* name : {"folder", "start-here", "system-file-manager", "text-x-generic", "application-x-executable"}) {
      if (const std::string path = theme_discovery::resolveIconInTheme(theme.path, name); !path.empty()) {
        return path;
      }
    }
    return {};
  }

  [[nodiscard]] std::unique_ptr<Button> makeResetButton(
      std::string path, bool pendingConfirmation, float scale, const IconsPage::Callbacks& callbacks
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

std::vector<std::string> IconsPage::resettableKeyPaths() const {
  return {keyPath()};
}

void IconsPage::resetKeyPath(std::string_view path) {
  if (path == keyPath()) {
    stage(Value{});
  }
}

void IconsPage::resetAll() {
  stage(Value{});
}

void IconsPage::ensureLoaded() {
  if (!m_loaded) {
    m_config.setPath(appearanceKwePath());
    m_config.load();
    m_loaded = true;
  }
}

IconsPage::Value IconsPage::fileBackedValue() const {
  return m_config.readString("Appearance", "IconTheme").value_or(Value{});
}

IconsPage::Value IconsPage::effectiveValue() const {
  const auto staged = m_pending.find(keyPath());
  if (staged != m_pending.end()) {
    return staged->second;
  }
  return fileBackedValue();
}

bool IconsPage::overridden() const {
  return !effectiveValue().empty();
}

void IconsPage::stage(const Value& value) {
  ensureLoaded();
  if (value == fileBackedValue()) {
    m_pending.erase(keyPath());
  } else {
    m_pending[keyPath()] = value;
  }
}

void IconsPage::stageAndRebuild(const Value& value, const Callbacks& callbacks) {
  stage(value);
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

bool IconsPage::applyTheme(std::string_view theme, std::string* error) {
  // 1. kdeglobals [Icons] Theme=<name> so Qt/KDE apps pick it up.
  const std::filesystem::path schemePath = std::filesystem::temp_directory_path() / "kineticwe-icons.colors";
  {
    std::ofstream out(schemePath);
    if (!out) {
      if (error != nullptr) {
        *error = "failed to write temporary icon scheme";
      }
      return false;
    }
    out << "[Icons]\n";
    out << "Theme=" << theme << "\n";
    if (!out) {
      if (error != nullptr) {
        *error = "failed to write temporary icon scheme";
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

  // 2. Notify Qt/KDE apps (type 4 = IconChanged).
  if (const std::string notifyError = noctalia::theme::notifyKdeChange(4, 0); !notifyError.empty()) {
    kLog.warn("kinetic-settings: failed to notify Qt apps of icon change: {}", notifyError);
  }

  // 3. GTK 3/4 read gtk-icon-theme-name from their settings.ini, which takes
  // precedence over the gsettings/dconf value below. Write it so GTK apps
  // follow the selection even when dconf says otherwise (a fresh profile, or a
  // file seeded by another desktop/tool). Best effort — a read-only or
  // hand-written file must not fail the whole Apply.
  for (const std::string_view tool : {"gtk-3.0", "gtk-4.0"}) {
    const std::filesystem::path config = realConfigHome() / std::string(tool) / "settings.ini";
    std::string iniError;
    if (!kinetic_settings::setKey(config, "Settings", "gtk-icon-theme-name", theme, true, &iniError)) {
      kLog.warn("kinetic-settings: failed to sync the {} icon theme: {}", tool, iniError);
    }
  }

  // 4. GTK apps (and the shell's icon resolver, which reads gsettings): set the
  // icon-theme key directly.
  if (process::commandExists("gsettings")) {
    (void)process::runAsync(
        "gsettings set org.gnome.desktop.interface icon-theme \"" + std::string(theme) + "\""
    );
  } else if (process::commandExists("dconf")) {
    (void)process::runAsync(
        "dconf write /org/gnome/desktop/interface/icon-theme \"'" + std::string(theme) + "'\""
    );
  }

  // 5. qt5ct/qt6ct: non-KDE Qt apps read the icon theme from the platform-theme
  // config, not kdeglobals, so keep [Appearance] icon_theme in step with the
  // choice. Best effort — the KDE/GTK paths above already applied, and a
  // read-only or hand-written tool config must not fail the whole Apply.
  for (const std::string_view tool : {"qt5ct", "qt6ct"}) {
    const std::filesystem::path config = realConfigHome() / std::string(tool) / (std::string(tool) + ".conf");
    std::string iniError;
    if (!kinetic_settings::setKey(config, "Appearance", "icon_theme", theme, false, &iniError)) {
      kLog.warn("kinetic-settings: failed to sync the {} icon theme: {}", tool, iniError);
    }
  }

  return true;
}

bool IconsPage::applyAll() {
  ensureLoaded();
  if (!m_pending.empty()) {
    for (const auto& [path, value] : m_pending) {
      if (path == keyPath()) {
        if (value.empty()) {
          m_config.removeKey("Appearance", "IconTheme");
        } else {
          m_config.setString("Appearance", "IconTheme", value);
        }
      }
    }
    if (!m_config.save()) {
      m_errorNotice = "Failed to write appearance.kwe. Your changes were kept; try Apply again.";
      kLog.warn("kinetic-settings: failed to write {}", m_config.path().string());
      return false;
    }
  }

  // Apply the effective selection system-wide (even when nothing was staged or
  // it equals the default) so pressing Apply also repairs targets that drifted,
  // e.g. a gsettings value reset back to Adwaita.
  const Value current = effectiveValue();
  if (!current.empty()) {
    std::string error;
    if (!applyTheme(current, &error)) {
      m_errorNotice = "Failed to apply the icon theme: " + error;
      kLog.warn("kinetic-settings: failed to apply icon theme: {}", error);
      return false;
    }
  }

  m_pending.clear();
  m_errorNotice.clear();
  return true;
}

std::unique_ptr<Node> IconsPage::buildTile(
    float scale, Renderer& renderer, const std::string& displayName, const std::string& previewPath, bool selected,
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

  if (!previewPath.empty()) {
    auto img = ui::image({
        .fit = ImageFit::Contain,
        .width = previewSize,
        .height = previewSize,
    });
    img->setSourceFile(renderer, previewPath, static_cast<int>(previewSize * 2), true);
    card->addChild(std::move(img));
  } else {
    card->addChild(ui::glyph({
        .glyph = "apps",
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

std::unique_ptr<Node> IconsPage::build(
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
          .glyph = "apps",
          .glyphSize = Style::fontSizeHeader * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeLabel("Icons", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
  ));
  // Opens the icon themes folder so the user can browse/install themes directly.
  headerRow->addChild(ui::button({
      .glyph = "folder-open",
      .glyphSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Ghost,
      .tooltip = "Open the icon themes folder",
      .minHeight = Style::controlHeightSm * scale,
      .padding = Style::spaceXs * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = []() { (void)theme_discovery::openIconRoot(); },
  }));
  headerRow->addChild(ui::spacer());
  if (overridden()) {
    headerRow->addChild(makeResetButton(keyPath(), armedResetKey == keyPath(), scale, callbacks));
  }
  root->addChild(std::move(headerRow));

  root->addChild(makeLabel(
      "Choose the icon theme used for applications and the desktop. "
      "Select \"System default\" to use the distro's default icons.",
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

  const Value current = effectiveValue();
  const bool systemSelected = current.empty();

  auto grid = ui::row({
      .align = FlexAlign::Start,
      .wrap = true,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
  });

  // The "System default" tile (empty value = no override).
  const bool searchActive = !filter.searchQuery.empty();
  const bool showSystem = !filter.showOverriddenOnly
      && (!searchActive || StringUtils::containsInsensitive("System default", filter.searchQuery));
  if (showSystem) {
    grid->addChild(buildTile(
        scale, renderer, "System default", {}, systemSelected,
        [this, callbacks]() { stageAndRebuild(Value{}, callbacks); }
    ));
  }

  bool anyTiles = false;
  for (const auto& theme : theme_discovery::iconThemes()) {
    const bool matchesSearch = filter.searchQuery.empty()
        || StringUtils::containsInsensitive(theme.displayName, filter.searchQuery)
        || StringUtils::containsInsensitive(theme.name, filter.searchQuery);
    const bool matchesOverride = !filter.showOverriddenOnly || theme.name == current;
    if (!matchesSearch || !matchesOverride) {
      continue;
    }
    const bool selected = theme.name == current;
    grid->addChild(buildTile(
        scale, renderer, theme.displayName, resolvePreview(theme), selected,
        [this, callbacks, theme]() { stageAndRebuild(Value{theme.name}, callbacks); }
    ));
    anyTiles = true;
  }

  const bool anyThemes = showSystem || anyTiles;
  if (!anyThemes) {
    if (searchActive) {
      // Global search drops pages with no hits.
      return nullptr;
    }
    root->addChild(makeLabel(
        "No icon themes match the current filters.", Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }

  root->addChild(std::move(grid));
  return root;
}