#include "autostart.h"

#include "core/log.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/process.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("kinetic-settings");

  constexpr std::string_view kCommandFilePrefix = "kwe-command-";
  // Marks a file written by "Add Custom Command" so launchAll() runs its Exec
  // through a shell (a normal app entry is tokenized and exec'd directly).
  constexpr std::string_view kCommandMarker = "X-KineticWE-Autostart-Command";

  [[nodiscard]] std::filesystem::path configHome() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      return xdg;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config";
    }
    return ".";
  }

  [[nodiscard]] std::filesystem::path runtimeDir() {
    if (const char* dir = std::getenv("XDG_RUNTIME_DIR"); dir != nullptr && *dir != '\0') {
      return dir;
    }
    return std::filesystem::path("/tmp") / ("kineticwe-autostart-" + std::to_string(::getuid()));
  }

  // Identifies the current compositor session. $XDG_RUNTIME_DIR belongs to the
  // systemd user manager, which outlives the compositor across logout/login, so
  // it cannot be used directly to tell sessions apart. The compositor's Wayland
  // socket is recreated on every compositor start, so its device/inode and
  // creation timestamp change per session while a supervised shell restart
  // within the same session keeps them. Empty when it cannot be determined.
  [[nodiscard]] std::string sessionToken() {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || *display == '\0') {
      return {};
    }
    const std::filesystem::path socket =
        std::filesystem::path(display).is_absolute() ? std::filesystem::path(display) : runtimeDir() / display;
    struct stat info {};
    if (::stat(socket.c_str(), &info) != 0) {
      return {};
    }
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":"
        + std::to_string(info.st_mtim.tv_sec) + "." + std::to_string(info.st_mtim.tv_nsec);
  }

  // Single-line value for a desktop-file key (newlines would split the file).
  [[nodiscard]] std::string singleLine(std::string value) {
    std::ranges::replace(value, '\n', ' ');
    std::ranges::replace(value, '\r', ' ');
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.pop_back();
    }
    return value;
  }

  [[nodiscard]] bool writeFileAtomic(const std::filesystem::path& path, const std::string& contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      kLog.warn("autostart: could not create {}: {}", path.parent_path().string(), error.message());
      return false;
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out.is_open()) {
        kLog.warn("autostart: could not open {} for writing", tmp.string());
        return false;
      }
      out << contents;
    }
    std::filesystem::rename(tmp, path, error);
    if (error) {
      kLog.warn("autostart: could not move {} into place: {}", tmp.string(), error.message());
      return false;
    }
    return true;
  }

  // Parses the [Desktop Entry] group of an autostart file. Returns false when
  // the file has no such group (or cannot be opened).
  [[nodiscard]] bool parseFile(const std::filesystem::path& path, autostart::Entry& out) {
    std::ifstream in(path);
    if (!in.is_open()) {
      return false;
    }
    out.filePath = path.string();
    out.fileName = path.filename().string();
    bool inDesktopEntry = false;
    bool sawDesktopEntry = false;
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
      }
      if (line.empty() || line.front() == '#' || line.front() == ';') {
        continue;
      }
      if (line.front() == '[') {
        inDesktopEntry = line == "[Desktop Entry]";
        if (inDesktopEntry) {
          sawDesktopEntry = true;
        }
        continue;
      }
      if (!inDesktopEntry) {
        continue;
      }
      const std::size_t eq = line.find('=');
      if (eq == std::string::npos || eq == 0) {
        continue;
      }
      const std::string key = line.substr(0, eq);
      const std::string value = line.substr(eq + 1);
      if (key == "Name") {
        out.name = value;
      } else if (key == "Exec") {
        out.command = value;
      } else if (key == "Hidden") {
        out.hidden = out.hidden || value == "true";
      } else if (key == "X-GNOME-Autostart-enabled") {
        out.hidden = out.hidden || value == "false";
      } else if (key == kCommandMarker) {
        out.isCommand = value == "true";
      }
    }
    return sawDesktopEntry;
  }

} // namespace

std::filesystem::path autostart::configDir() {
  return configHome() / "autostart";
}

std::vector<autostart::Entry> autostart::entries() {
  std::vector<Entry> result;
  const std::filesystem::path dir = configDir();
  std::error_code error;
  for (const auto& item : std::filesystem::directory_iterator(dir, error)) {
    if (error) {
      break;
    }
    if (!item.is_regular_file(error) || item.path().extension() != ".desktop") {
      continue;
    }
    Entry entry;
    if (!parseFile(item.path(), entry)) {
      continue;
    }
    if (entry.name.empty()) {
      entry.name = item.path().stem().string();
    }
    result.push_back(std::move(entry));
  }
  std::ranges::sort(result, [](const Entry& a, const Entry& b) { return a.fileName < b.fileName; });
  return result;
}

bool autostart::addApplication(const DesktopEntry& app) {
  if (app.id.empty()) {
    return false;
  }
  const std::filesystem::path dest = configDir() / (app.id + ".desktop");
  std::error_code error;
  if (!app.path.empty()) {
    const std::filesystem::path source(app.path);
    if (source.lexically_normal() == dest.lexically_normal()) {
      return true;
    }
    if (std::filesystem::exists(source, error)) {
      std::filesystem::create_directories(dest.parent_path(), error);
      error.clear();
      std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing, error);
      if (!error) {
        kLog.info("autostart: added application '{}' ({})", app.name, dest.string());
        return true;
      }
      kLog.warn("autostart: could not copy {} to {}: {}", source.string(), dest.string(), error.message());
    }
  }
  // Fallback for entries without a readable source file: a minimal stub.
  std::string contents = "[Desktop Entry]\n";
  contents += "Type=Application\n";
  contents += "Name=" + singleLine(app.name.empty() ? app.id : app.name) + "\n";
  contents += "Exec=" + singleLine(app.exec) + "\n";
  if (!app.icon.empty()) {
    contents += "Icon=" + singleLine(app.icon) + "\n";
  }
  contents += "Terminal=false\n";
  if (!writeFileAtomic(dest, contents)) {
    return false;
  }
  kLog.info("autostart: added application '{}' ({})", app.name, dest.string());
  return true;
}

bool autostart::addCommand(const std::string& command) {
  const std::string clean = singleLine(command);
  if (clean.empty()) {
    return false;
  }
  std::filesystem::path dest;
  for (int n = 1; n < 100000; ++n) {
    dest = configDir() / (std::string(kCommandFilePrefix) + std::to_string(n) + ".desktop");
    std::error_code error;
    if (!std::filesystem::exists(dest, error)) {
      break;
    }
  }
  std::string contents = "[Desktop Entry]\n";
  contents += "Type=Application\n";
  contents += "Name=" + clean + "\n";
  contents += "Exec=" + clean + "\n";
  contents += "Terminal=false\n";
  contents += std::string(kCommandMarker) + "=true\n";
  if (!writeFileAtomic(dest, contents)) {
    return false;
  }
  kLog.info("autostart: added command '{}' ({})", clean, dest.string());
  return true;
}

bool autostart::remove(std::string_view fileName) {
  if (fileName.empty() || fileName == "." || fileName == "..") {
    return false;
  }
  const std::filesystem::path name{fileName};
  if (name.filename() != name || name.extension() != ".desktop") {
    kLog.warn("autostart: refusing to remove suspicious path '{}'", fileName);
    return false;
  }
  const std::filesystem::path path = configDir() / name;
  std::error_code error;
  if (!std::filesystem::remove(path, error) || error) {
    kLog.warn("autostart: could not remove {}: {}", path.string(), error.message());
    return false;
  }
  kLog.info("autostart: removed {}", path.string());
  return true;
}

void autostart::launchAll() {
  // Once per compositor session: the shell is supervised and may restart after
  // a crash, which must not relaunch the entries. The marker stores the session
  // token; a new compositor writes a new token and launches again. A missing or
  // undeterminable token means launch every time (never silently skip).
  const std::string token = sessionToken();
  const std::filesystem::path marker = runtimeDir() / "kineticwe-autostart.done";
  if (!token.empty()) {
    std::ifstream in(marker);
    std::string stored;
    std::getline(in, stored);
    if (stored == token) {
      kLog.info("autostart: already launched for this session");
      return;
    }
  }
  std::error_code error;
  std::filesystem::create_directories(marker.parent_path(), error);
  {
    std::ofstream out(marker, std::ios::trunc);
    out << token << "\n";
  }

  std::size_t launched = 0;
  for (const Entry& entry : entries()) {
    if (entry.hidden || entry.command.empty()) {
      continue;
    }
    if (entry.isCommand) {
      // Custom commands are shell lines; runAsync() uses /bin/sh -lc.
      if (process::runAsync(entry.command)) {
        ++launched;
      } else {
        kLog.warn("autostart: failed to run '{}'", entry.command);
      }
      continue;
    }
    const auto prepared = desktop_entry_launch::prepareCommand(entry.command, false);
    if (!prepared.has_value() || prepared->args.empty()) {
      kLog.warn("autostart: could not parse Exec for {}", entry.fileName);
      continue;
    }
    if (process::runAsync(prepared->args)) {
      ++launched;
    } else {
      kLog.warn("autostart: failed to launch {} ({})", entry.name, entry.fileName);
    }
  }
  kLog.info("autostart: launched {} entr{}", launched, launched == 1 ? "y" : "ies");
}
