#include "default_apps_page.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/process.h"
#include "system/terminal_launch.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("kinetic-settings");

  constexpr int kSettingDescriptionMaxLines = 5;
  constexpr float kDefaultControlWidth = 230.0F;

  // The Default Applications page persists to the standard XDG mimeapps.list,
  // in the user's real config home — NOT the compositor-redirected
  // KWE_CONFIG_HOME. The shell process keeps the real XDG_CONFIG_HOME, so this
  // resolves to the shared config location that xdg-mime and KDE apps read.
  std::filesystem::path mimeappsListPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      return std::filesystem::path(xdg) / "mimeapps.list";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config" / "mimeapps.list";
    }
    return std::filesystem::path(".") / "mimeapps.list";
  }

  // The user's real config home, resolved the same way as mimeappsListPath().
  std::filesystem::path userConfigHome() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config";
    }
    return std::filesystem::path(".");
  }

  // The portal configuration this page manages. xdg-desktop-portal reads it
  // from the user's config home, ahead of the system-wide portals.conf.
  std::filesystem::path portalConfigPath() {
    return userConfigHome() / "xdg-desktop-portal" / "kineticwe-portals.conf";
  }

  // Splits an XDG colon-separated directory list, falling back to `fallback`
  // when the variable is unset or empty.
  [[nodiscard]] std::vector<std::filesystem::path> splitPathList(const char* value, std::string_view fallback) {
    std::vector<std::filesystem::path> dirs;
    const std::string list = (value != nullptr && *value != '\0') ? std::string(value) : std::string(fallback);
    for (const std::string_view part : StringUtils::split(list, ':')) {
      const std::string dir = StringUtils::trim(part);
      if (!dir.empty()) {
        dirs.emplace_back(dir);
      }
    }
    return dirs;
  }

  // $XDG_DATA_HOME (default ~/.local/share) followed by $XDG_DATA_DIRS
  // (default /usr/local/share:/usr/share).
  [[nodiscard]] std::vector<std::filesystem::path> xdgDataDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
      dirs.emplace_back(dataHome);
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      dirs.emplace_back(std::filesystem::path(home) / ".local" / "share");
    }
    const auto systemDirs = splitPathList(std::getenv("XDG_DATA_DIRS"), "/usr/local/share:/usr/share");
    dirs.insert(dirs.end(), systemDirs.begin(), systemDirs.end());
    return dirs;
  }

  // The directories xdg-desktop-portal consults for portals.conf, in lookup
  // order: user config, XDG_CONFIG_DIRS, sysconfdir, user data, XDG_DATA_DIRS,
  // then /usr/share.
  [[nodiscard]] std::vector<std::filesystem::path> portalConfigSearchDirs() {
    std::vector<std::filesystem::path> dirs;
    dirs.push_back(userConfigHome());

    const auto configDirs = splitPathList(std::getenv("XDG_CONFIG_DIRS"), "/etc/xdg");
    dirs.insert(dirs.end(), configDirs.begin(), configDirs.end());
    // Build-time sysconfdir (usually /etc): /etc/xdg-desktop-portal.
    dirs.emplace_back("/etc");

    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
      dirs.emplace_back(dataHome);
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      dirs.emplace_back(std::filesystem::path(home) / ".local" / "share");
    }
    const auto dataDirs = splitPathList(std::getenv("XDG_DATA_DIRS"), "/usr/local/share:/usr/share");
    dirs.insert(dirs.end(), dataDirs.begin(), dataDirs.end());
    dirs.emplace_back("/usr/share");
    return dirs;
  }

  // First existing portal configuration file wins, mirroring the portal's own
  // search order (kineticwe-portals.conf before the generic portals.conf).
  [[nodiscard]] std::filesystem::path findPortalConfig() {
    for (const auto& dir : portalConfigSearchDirs()) {
      for (const char* name : {"kineticwe-portals.conf", "portals.conf"}) {
        const auto candidate = dir / "xdg-desktop-portal" / name;
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) {
          return candidate;
        }
      }
    }
    return {};
  }

  [[nodiscard]] std::string firstListToken(const std::string& value) {
    if (value.empty()) {
      return {};
    }
    return StringUtils::trim(StringUtils::split(value, ';').front());
  }

  // The backend xdg-desktop-portal would actually use for file dialogs:
  // "gtk" for the GTK portal, "" for the built-in KineticWE portal (the UI
  // only distinguishes those two; every other backend is treated as built-in).
  [[nodiscard]] std::string effectivePortalBackend() {
    const auto path = findPortalConfig();
    if (path.empty()) {
      return {};
    }
    KineticweKwe config;
    config.setPath(path);
    if (!config.load()) {
      return {};
    }
    const auto chooser = config.readString("preferred", "org.freedesktop.impl.portal.FileChooser");
    const auto fallback = config.readString("preferred", "default");
    std::string value;
    if (chooser.has_value() && !chooser->empty()) {
      value = *chooser;
    } else if (fallback.has_value() && !fallback->empty()) {
      value = *fallback;
    }
    return firstListToken(value) == "gtk" ? std::string("gtk") : std::string{};
  }

  // Package-manager agnostic GTK portal detection: the portal file lives in
  // <data-dir>/xdg-desktop-portal/portals/, with an older layout directly in
  // <data-dir>/xdg-desktop-portal/. Require it to declare the FileChooser
  // interface so an unrelated/stub file doesn't enable the option.
  [[nodiscard]] bool gtkFileChooserAvailable() {
    for (const auto& dir : xdgDataDirs()) {
      // Current layout first, then the older directly-under-the-portal-dir one.
      for (const char* relative : {"portals/gtk.portal", "gtk.portal"}) {
        std::ifstream in(dir / "xdg-desktop-portal" / relative);
        if (!in.is_open()) {
          continue;
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        if (contents.str().find("org.freedesktop.impl.portal.FileChooser") != std::string::npos) {
          return true;
        }
      }
    }
    return false;
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

  [[nodiscard]] std::unique_ptr<Flex>
  makeStatusBadge(std::string_view label, const ColorSpec& fill, const ColorSpec& color, float scale) {
    return ui::row(
        {
            .align = FlexAlign::Center,
            .gap = 0.0F,
            .paddingV = 0.0F,
            .paddingH = Style::spaceXs * scale,
            .fill = fill,
            .radius = Style::scaledRadiusSm(scale),
        },
        makeLabel(label, Style::fontSizeCaption * scale, color, FontWeight::Bold)
    );
  }

  [[nodiscard]] std::unique_ptr<Flex> makeOverrideBadge(float scale) {
    return makeStatusBadge(
        i18n::tr("settings.badges.override"), colorSpecFromRole(ColorRole::Primary, 0.15F),
        colorSpecFromRole(ColorRole::Primary), scale
    );
  }

  [[nodiscard]] std::unique_ptr<Node> makeNotice(std::string_view text, float scale) {
    auto notice = ui::column({
        .align = FlexAlign::Start,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.45F),
        .radius = Style::scaledRadiusMd(scale),
        .fillWidth = true,
    });
    notice->addChild(makeLabel(text, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)));
    return notice;
  }

  // Splits a semicolon-separated .desktop MimeType= value into its tokens.
  [[nodiscard]] std::vector<std::string> splitMimeKeys(std::string_view keys) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= keys.size()) {
      const std::size_t end = keys.find(';', start);
      const std::size_t len = (end == std::string_view::npos) ? keys.size() - start : end - start;
      std::string token = StringUtils::trim(keys.substr(start, len));
      if (!token.empty()) {
        out.push_back(std::move(token));
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return out;
  }

  // Whether `entry` declares support for any of the semicolon-separated keys
  // in `keys`.
  [[nodiscard]] bool entrySupportsMimeKeys(const DesktopEntry& entry, std::string_view keys) {
    if (keys.empty()) {
      return false;
    }
    for (const std::string& want : splitMimeKeys(keys)) {
      for (const std::string& have : splitMimeKeys(entry.mimeTypes)) {
        if (have == want) {
          return true;
        }
      }
    }
    return false;
  }

  // The desktop-file id (e.g. "firefox.desktop") stored in mimeapps.list. The
  // shell's DesktopEntry::id is the file stem, so the ".desktop" suffix is
  // appended to match the id form KDE/xdg-mime use.
  [[nodiscard]] std::string desktopFileId(const DesktopEntry& entry) {
    return entry.id + ".desktop";
  }

  // Basename of a command's first token: "/usr/bin/kitty -e sh" and "kitty"
  // both reduce to "kitty". Used to match a stored terminal command to a
  // desktop entry's Exec program.
  [[nodiscard]] std::string programBasename(std::string_view command) {
    std::string_view token = command.substr(0, command.find(' '));
    token = StringUtils::trimRightView(StringUtils::trimLeftView(token));
    if (const std::size_t slash = token.rfind('/'); slash != std::string_view::npos) {
      token = token.substr(slash + 1);
    }
    return std::string(token);
  }

  // Whether `entry` IS a terminal emulator. The desktop-file Terminal=true key
  // means the opposite — "run this program inside a terminal" — so it selects
  // console tools (htop, btop, yazi, ...), never terminals themselves; kitty,
  // konsole and friends all declare Terminal=false. Terminals are therefore
  // identified by the registered TerminalEmulator desktop-file category, with
  // a bare-exec well-known-program fallback (mirroring terminal_launch's
  // discovery list) for desktop files that lack the category. The fallback
  // deliberately requires a SINGLE-token Exec: launchers that merely run
  // something inside a terminal (custom "kitty --hold --class btop btop"
  // entries, kglobalaccel command shortcuts, ...) carry the payload as extra
  // arguments and must never be offered as terminals — with a loose
  // program-name match they both polluted the option list and hijacked the
  // readback (the first registry match for "kitty" was the btop wrapper, not
  // kitty itself).
  [[nodiscard]] bool isTerminalEmulatorEntry(const DesktopEntry& entry) {
    for (const std::string& category : splitMimeKeys(entry.categoriesLower)) {
      if (category == "terminalemulator") {
        return true;
      }
    }
    // Bare command only: extra arguments mean this is a wrapper around a
    // command, not the terminal itself.
    if (entry.exec.find(' ') != std::string::npos) {
      return false;
    }
    const std::string program = programBasename(entry.exec);
    if (program.empty()) {
      return false;
    }
    static constexpr std::array<std::string_view, 17> kTerminalPrograms = {
        "x-terminal-emulator", "ghostty",        "kitty",          "alacritty",   "wezterm",    "foot",
        "konsole",             "gnome-terminal", "kgx",            "ptyxis",      "xterm",      "terminator",
        "tilix",               "xfce4-terminal", "lxterminal",     "guake",       "yakuake",
    };
    for (const std::string_view candidate : kTerminalPrograms) {
      if (program == candidate) {
        return true;
      }
    }
    return false;
  }

} // namespace

// ---------------------------------------------------------------------------
// Schema: the field descriptor table — the single source of truth for the
// rows, their MIME keys, defaults and presentation.
// ---------------------------------------------------------------------------

const std::vector<DefaultAppsPage::Field>& DefaultAppsPage::fields() {
  static const std::vector<Field> table{
      Field{
          .title = "Web browser",
          .subtitle = "Opens web links and pages",
          .pageGroup = "Internet",
          .mimeKeys = "x-scheme-handler/http;x-scheme-handler/https",
      },
      Field{
          .title = "Email client",
          .subtitle = "Opens mailto: links and email",
          .pageGroup = "Internet",
          .mimeKeys = "x-scheme-handler/mailto",
      },
      Field{
          .title = "Calendar",
          .subtitle = "Opens calendar and subscription links",
          .pageGroup = "Internet",
          .advanced = true,
          .mimeKeys = "x-scheme-handler/webcal;x-scheme-handler/caldav",
      },
      Field{
          .title = "Phone numbers",
          .subtitle = "Opens telephone links",
          .pageGroup = "Internet",
          .advanced = true,
          .mimeKeys = "x-scheme-handler/tel",
      },
      Field{
          .title = "Image viewer",
          .subtitle = "Opens photos and other pictures",
          .pageGroup = "Multimedia",
          .mimeKeys = "image/png;image/jpeg;image/gif;image/webp;image/bmp",
      },
      Field{
          .title = "Music player",
          .subtitle = "Plays audio files",
          .pageGroup = "Multimedia",
          .mimeKeys = "audio/mpeg;audio/x-flac;audio/ogg;audio/wav",
      },
      Field{
          .title = "Video player",
          .subtitle = "Plays video files",
          .pageGroup = "Multimedia",
          .mimeKeys = "video/mp4;video/x-matroska;video/webm;video/ogg",
      },
      Field{
          .title = "Text editor",
          .subtitle = "Opens plain text documents",
          .pageGroup = "Documents",
          .mimeKeys = "text/plain",
      },
      Field{
          .title = "PDF viewer",
          .subtitle = "Opens PDF documents",
          .pageGroup = "Documents",
          .mimeKeys = "application/pdf",
      },
      Field{
          .title = "File manager",
          .subtitle = "Opens folders and the desktop",
          .pageGroup = "Utilities",
          .mimeKeys = "inode/directory",
      },
      Field{
          .title = "Terminal emulator",
          .subtitle = "Opens a command-line terminal",
          .pageGroup = "Utilities",
          .matchByTerminal = true,
      },
      Field{
          .title = "Archive manager",
          .subtitle = "Opens compressed archives",
          .pageGroup = "Utilities",
          .mimeKeys = "application/zip;application/x-tar;application/gzip;application/x-7z-compressed",
      },
      Field{
          .title = "Map",
          .subtitle = "Opens geographic links",
          .pageGroup = "Utilities",
          .advanced = true,
          .mimeKeys = "x-scheme-handler/geo",
      },
      Field{
          .title = "File Picker",
          .subtitle = "Choose which portal backend handles open and save dialogs. Screen sharing and other portal "
                      "features always use KineticWE; the GTK file picker uses xdg-desktop-portal-gtk. Applying "
                      "restarts the portal service and ends active portal sessions such as screen sharing.",
          .pageGroup = "Portals",
          .matchByPortal = true,
      },
  };
  return table;
}

std::string DefaultAppsPage::keyPath(const Field& field) {
  // The path used for reset/override bookkeeping is just the row's title
  // (rows are not config-group keys, so a readable stable token is fine).
  return std::string(field.title);
}

const DefaultAppsPage::Field* DefaultAppsPage::findField(std::string_view path) {
  for (const auto& field : fields()) {
    if (path == keyPath(field)) {
      return &field;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Values: effective state, override detection.
// ---------------------------------------------------------------------------

void DefaultAppsPage::ensureLoaded() {
  if (!m_loaded) {
    m_config.setPath(mimeappsListPath());
    m_config.load();
    m_loaded = true;
  }
}

void DefaultAppsPage::ensurePortalLoaded() {
  if (!m_portalLoaded) {
    m_portalConfig.setPath(portalConfigPath());
    m_portalConfig.load();
    m_portalLoaded = true;
  }
}

DefaultAppsPage::Value DefaultAppsPage::fileBackedValue(const Field& field) const {
  // The file-picker row reflects the backend xdg-desktop-portal would actually
  // use, which may come from a system-wide portals.conf rather than our file.
  if (field.matchByPortal) {
    return effectivePortalBackend();
  }
  // The terminal row is not a MIME default: it persists to kdeglobals
  // ([General] TerminalApplication), the location KIO and this shell resolve
  // the terminal from. Map the stored command back to a terminal desktop-entry
  // id so the dropdown can select it.
  if (field.matchByTerminal) {
    const std::string stored = terminal_launch::configuredTerminal();
    const std::string storedProgram = programBasename(stored);
    kLog.info("default-apps terminal readback: stored='{}' program='{}'", stored, storedProgram);
    if (storedProgram.empty()) {
      return Value{};
    }
    for (const DesktopEntry& entry : desktopEntries()) {
      if (!isTerminalEmulatorEntry(entry)) {
        continue;
      }
      const auto prepared = desktop_entry_launch::prepareCommand(entry.exec, false);
      if (prepared.has_value() && !prepared->args.empty()
          && programBasename(prepared->args.front()) == storedProgram) {
        kLog.info("default-apps terminal readback: matched entry '{}'", desktopFileId(entry));
        return desktopFileId(entry);
      }
    }
    // Configured but not among the offered entries (e.g. uninstalled): surface
    // the raw command so the control shows the placeholder instead of
    // pretending "System" is active.
    return stored;
  }
  // The effective default for a row is the first MIME key that has a default
  // application configured.
  for (const std::string& key : splitMimeKeys(field.mimeKeys)) {
    if (const auto v = m_config.readString("Default Applications", key); v.has_value() && !v->empty()) {
      return *v;
    }
  }
  return Value{};
}

DefaultAppsPage::Value DefaultAppsPage::effectiveValue(const Field& field) const {
  const auto staged = m_pending.find(keyPath(field));
  if (staged != m_pending.end()) {
    return staged->second;
  }
  return fileBackedValue(field);
}

bool DefaultAppsPage::overridden(const Field& field) const {
  return !effectiveValue(field).empty();
}

bool DefaultAppsPage::matchesFilter(const Field& field, const Filter& filter) const {
  if (!filter.showAdvanced && field.advanced) {
    return false;
  }
  if (filter.showOverriddenOnly && !overridden(field)) {
    return false;
  }
  if (!filter.searchQuery.empty()) {
    const auto matches = [&filter](std::string_view text) {
      return StringUtils::containsInsensitive(text, filter.searchQuery);
    };
    if (!matches(field.title) && !matches(field.subtitle) && !matches(field.pageGroup)) {
      return false;
    }
  }
  return true;
}

std::vector<const DesktopEntry*> DefaultAppsPage::candidateApps(const Field& field) const {
  if (field.matchByPortal) {
    return {};
  }
  std::vector<const DesktopEntry*> candidates;
  for (const DesktopEntry& entry : desktopEntries()) {
    if (field.matchByTerminal) {
      if (isTerminalEmulatorEntry(entry)) {
        candidates.push_back(&entry);
      }
    } else if (entrySupportsMimeKeys(entry, field.mimeKeys)) {
      candidates.push_back(&entry);
    }
  }
  std::ranges::sort(
      candidates, [](const DesktopEntry* a, const DesktopEntry* b) { return a->name < b->name; }
  );
  return candidates;
}

// ---------------------------------------------------------------------------
// Persistence: staged edits + Apply.
// ---------------------------------------------------------------------------

void DefaultAppsPage::stage(const Field& field, const Value& value) {
  ensureLoaded();
  const std::string path = keyPath(field);
  if (value == fileBackedValue(field)) {
    m_pending.erase(path);
  } else {
    m_pending[path] = value;
  }
}

void DefaultAppsPage::stageAndRebuild(const Field& field, const Value& value, const Callbacks& callbacks) {
  stage(field, value);
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

bool DefaultAppsPage::applyAll() {
  ensureLoaded();
  if (m_pending.empty()) {
    return true;
  }

  bool mimeDirty = false;
  for (const auto& [path, value] : m_pending) {
    const Field* field = findField(path);
    if (field == nullptr) {
      continue;
    }
    // The terminal row persists to kdeglobals ([General] TerminalApplication)
    // rather than mimeapps.list — terminals have no MIME type.
    if (field->matchByTerminal) {
      if (!writeTerminalApplication(value)) {
        return false;
      }
      continue;
    }
    // The file-picker row persists to kineticwe-portals.conf rather than
    // mimeapps.list — it selects a portal backend, not an application.
    if (field->matchByPortal) {
      if (!writePortalConfig(value)) {
        return false;
      }
      continue;
    }
    // A non-empty value sets EVERY MIME key the row owns: each key resolves
    // independently, so writing only the first (and dropping the rest) left
    // consumers of the other keys (e.g. x-scheme-handler/https) falling back
    // past the user's choice. "System" (empty) removes every key the row owns
    // so the freedesktop default applies.
    const auto keys = splitMimeKeys(field->mimeKeys);
    for (const std::string& key : keys) {
      if (value.empty()) {
        m_config.removeKey("Default Applications", key);
      } else {
        m_config.setString("Default Applications", key, value);
      }
    }
    mimeDirty = true;
  }

  if (mimeDirty && !m_config.save()) {
    m_errorNotice = "Failed to write mimeapps.list. Your changes were kept; try Apply again.";
    kLog.warn("kinetic-settings: failed to write {}", m_config.path().string());
    return false;
  }

  // Refresh the desktop/MIME caches so the new defaults take effect immediately
  // for every consumer, not just xdg-open: update-desktop-database rebuilds
  // mimeinfo.cache (used by some launchers) and kbuildsycoca rebuilds KDE's
  // KApplicationTrader cache (used by KDE apps to resolve default apps). Both
  // are no-ops when the tool isn't installed. kbuildsycoca6 runs synchronously
  // (bounded — it completes in well under a second on typical installs) so the
  // refreshed cache is already visible when Apply returns; a fire-and-forget
  // rebuild previously left a window where lookups still hit the stale cache
  // and launched the old default.
  if (process::commandExists("update-desktop-database")) {
    (void)process::runAsync("update-desktop-database");
  }
  if (process::commandExists("kbuildsycoca6")) {
    const auto result = process::runSyncWithTimeout({"kbuildsycoca6"}, std::chrono::seconds(15));
    if (!result) {
      kLog.warn("kinetic-settings: kbuildsycoca6 failed (exit {})", result.exitCode);
    }
  }

  m_pending.clear();
  m_errorNotice.clear();
  return true;
}

bool DefaultAppsPage::writeTerminalApplication(const Value& value) {
  // "System" (empty) clears the key so KIO's built-in default applies.
  std::string command;
  if (!value.empty()) {
    // Map the desktop-file id ("kitty.desktop") back to its Exec command, with
    // the desktop-entry field codes stripped by prepareCommand.
    const std::string id = value.ends_with(".desktop") ? value.substr(0, value.size() - 8) : value;
    for (const DesktopEntry& entry : desktopEntries()) {
      if (entry.id != id) {
        continue;
      }
      const auto prepared = desktop_entry_launch::prepareCommand(entry.exec, false);
      if (prepared.has_value() && !prepared->args.empty()) {
        command = StringUtils::join(prepared->args, " ");
      }
      break;
    }
    kLog.info("default-apps terminal apply: id='{}' command='{}'", id, command);
    if (command.empty()) {
      m_errorNotice = "The selected terminal could not be resolved. Your changes were kept; try Apply again.";
      return false;
    }
  }
  if (!terminal_launch::writeConfiguredTerminal(command)) {
    m_errorNotice = "Failed to write the terminal setting. Your changes were kept; try Apply again.";
    kLog.warn("kinetic-settings: kwriteconfig6 failed for TerminalApplication");
    return false;
  }
  kLog.info("default-apps terminal apply: wrote TerminalApplication='{}'", command);
  return true;
}

bool DefaultAppsPage::writePortalConfig(const Value& value) {
  ensurePortalLoaded();
  const bool useGtk = value == "gtk";
  if (useGtk && !gtkFileChooserAvailable()) {
    m_errorNotice = "xdg-desktop-portal-gtk is not installed. Install the xdg-desktop-portal-gtk package to enable "
                    "the GTK file picker.";
    return false;
  }

  // Seed `default=kwe` when the user has no usable default: screen sharing and
  // every other interface must keep using the KineticWE portal, while the
  // FileChooser key overrides just the file picker.
  const auto configuredDefault = m_portalConfig.readString("preferred", "default");
  if (!configuredDefault.has_value() || configuredDefault->empty()) {
    m_portalConfig.setString("preferred", "default", "kwe");
  }
  m_portalConfig.setString("preferred", "org.freedesktop.impl.portal.FileChooser", useGtk ? "gtk" : "kwe");
  if (!m_portalConfig.save()) {
    m_errorNotice = "Failed to write the portal configuration. Your changes were kept; try Apply again.";
    kLog.warn("kinetic-settings: failed to write {}", m_portalConfig.path().string());
    return false;
  }
  kLog.info("default-apps portal apply: FileChooser='{}'", useGtk ? "gtk" : "kwe");

  // xdg-desktop-portal reads its configuration at startup, so restart it to
  // make the new file picker take effect immediately. A missing systemctl (or
  // a failed spawn) must not turn a successfully saved config into an error.
  if (process::commandExists("systemctl")) {
    if (!process::runAsync("systemctl --user restart --no-block xdg-desktop-portal.service")) {
      kLog.warn("kinetic-settings: failed to restart xdg-desktop-portal.service");
    }
  } else {
    kLog.warn("kinetic-settings: systemctl not found; xdg-desktop-portal.service was not restarted");
  }
  return true;
}

std::vector<std::string> DefaultAppsPage::resettableKeyPaths() const {
  std::vector<std::string> paths;
  paths.reserve(fields().size());
  for (const auto& field : fields()) {
    paths.push_back(keyPath(field));
  }
  return paths;
}

void DefaultAppsPage::resetKeyPath(std::string_view path) {
  if (const Field* field = findField(path)) {
    stage(*field, Value{});
  }
}

void DefaultAppsPage::resetAll() {
  for (const auto& field : fields()) {
    stage(field, Value{});
  }
}

// ---------------------------------------------------------------------------
// Controls + scene.
// ---------------------------------------------------------------------------

namespace {

  [[nodiscard]] std::unique_ptr<Button> makeResetButton(
      std::string path, bool pendingConfirmation, float scale, const DefaultAppsPage::Callbacks& callbacks
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

std::unique_ptr<Node>
DefaultAppsPage::buildControl(const Field& field, float scale, const Callbacks& callbacks) {
  const Value current = effectiveValue(field);

  // The file-picker row is a portal backend choice, not an application list.
  if (field.matchByPortal) {
    const bool gtkAvailable = gtkFileChooserAvailable();
    const bool useGtk = current == "gtk";
    // Keep the control usable when GTK was selected but its package is now
    // missing, so the user can switch back to the built-in backend.
    std::vector<std::string> options;
    options.emplace_back("KineticWE (built-in)");
    options.push_back(gtkAvailable ? "GTK" : "GTK (not installed)");
    return ui::select({
        .options = std::move(options),
        .selectedIndex = useGtk ? std::size_t{1} : std::size_t{0},
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .enabled = gtkAvailable || useGtk,
        .width = kDefaultControlWidth * scale,
        .height = Style::controlHeight * scale,
        .onSelectionChanged = [this, field, callbacks](std::size_t index, std::string_view /*label*/) {
          stageAndRebuild(field, index == 1 ? Value{"gtk"} : Value{}, callbacks);
        },
    });
  }

  // The first option is always "System" (empty value = use the system/XDG
  // default), followed by every app that can handle the row's MIME types.
  std::vector<std::string> labels;
  labels.emplace_back("System");
  const auto candidates = candidateApps(field);
  for (const DesktopEntry* app : candidates) {
    labels.push_back(app->name);
  }

  if (field.matchByTerminal) {
    kLog.info(
        "default-apps terminal buildControl: current='{}' staged={} candidates={}",
        current, m_pending.contains(keyPath(field)), candidates.size()
    );
  }
  std::optional<std::size_t> selectedIndex;
  if (!current.empty()) {
    // Match the configured desktop-file id (e.g. "firefox.desktop") to a
    // candidate by its full id (id + ".desktop"). A name match covers entries
    // whose stored id no longer resolves to a candidate.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (desktopFileId(*candidates[i]) == current) {
        selectedIndex = i + 1; // +1 because "System" is option 0
        break;
      }
    }
    if (!selectedIndex.has_value()) {
      for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i]->name == current) {
          selectedIndex = i + 1;
          break;
        }
      }
    }
  }

  const bool known = selectedIndex.has_value();
  // Placeholder shown when a configured default doesn't resolve to an offered
  // app (e.g. the app was uninstalled). "System" (empty) never shows one.
  std::optional<std::string> placeholder;
  if (!known && !current.empty()) {
    placeholder = "Unknown app";
  }
  return ui::select({
      .options = labels,
      .selectedIndex = known ? std::optional<std::size_t>{*selectedIndex} : std::nullopt,
      .clearSelection = !known,
      .placeholder = placeholder,
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .glyphSize = Style::fontSizeBody * scale,
      .width = kDefaultControlWidth * scale,
      .height = Style::controlHeight * scale,
      .onSelectionChanged = [this, field, callbacks, candidates](std::size_t index, std::string_view /*label*/) {
        // index 0 == "System" (empty value).
        const Value value = index == 0 ? Value{} : Value{desktopFileId(*candidates[index - 1])};
        stageAndRebuild(field, value, callbacks);
      },
  });
}

void DefaultAppsPage::addSettingRow(
    Flex& parent, const Field& field, float scale, std::string_view armedResetKey, const Callbacks& callbacks
) {
  if (field.matchByPortal && !gtkFileChooserAvailable()) {
    parent.addChild(makeNotice(
        "xdg-desktop-portal-gtk is not installed. Install the xdg-desktop-portal-gtk package to enable the GTK "
        "file picker.",
        scale
    ));
  }

  const std::string path = keyPath(field);
  const bool isOverridden = overridden(field);

  auto titleRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  titleRow->addChild(makeLabel(
      field.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  titleRow->addChild(ui::spacer());

  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(std::move(titleRow));
  if (!field.subtitle.empty()) {
    copy->addChild(ui::label({
        .text = std::string(field.subtitle),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = kSettingDescriptionMaxLines,
    }));
  }

  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (isOverridden) {
    actions->addChild(makeOverrideBadge(scale));
    actions->addChild(makeResetButton(path, armedResetKey == path, scale, callbacks));
  }
  actions->addChild(buildControl(field, scale, callbacks));

  auto row = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  row->addChild(std::move(copy));
  row->addChild(std::move(actions));
  parent.addChild(std::move(row));
}

std::unique_ptr<Node> DefaultAppsPage::build(
    float scale, const Filter& filter, std::string_view armedResetKey, const Callbacks& callbacks
) {
  ensureLoaded();

  auto root = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceLg * scale,
      .fill = clearColorSpec(),
      .fillWidth = true,
  });

  root->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .glyph = "apps",
          .glyphSize = Style::fontSizeHeader * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeLabel("Default Applications", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
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

  bool anyRows = false;
  bool firstGroup = true;
  std::string_view lastGroup;
  const auto addGroupHeader = [&](std::string_view title) {
    if (title.empty()) {
      return;
    }
    if (!firstGroup) {
      root->addChild(ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceSm * scale,
           .configure = [scale](Flex& flex) { flex.setPadding(Style::spaceSm * scale, 0.0F, 0.0F, 0.0F); }},
          ui::separator(),
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold)
      ));
    } else {
      root->addChild(
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold)
      );
    }
    firstGroup = false;
  };

  for (const auto& field : fields()) {
    if (!matchesFilter(field, filter)) {
      continue;
    }
    if (field.pageGroup != lastGroup) {
      lastGroup = field.pageGroup;
      addGroupHeader(lastGroup);
    }
    addSettingRow(*root, field, scale, armedResetKey, callbacks);
    anyRows = true;
  }

  if (!anyRows) {
    // Global search merges hits from every page and drops pages with none, so
    // an empty page must not render its own empty-state notice.
    if (!filter.searchQuery.empty()) {
      return nullptr;
    }
    root->addChild(makeLabel(
        "No settings match the current filters.", Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }

  return root;
}
