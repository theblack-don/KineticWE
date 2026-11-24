#include "theme_discovery.h"

#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/process.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

  void pushUniqueDir(std::vector<std::string>& dirs, std::string dir) {
    if (dir.empty()) {
      return;
    }
    if (!std::ranges::contains(dirs, dir)) {
      dirs.push_back(std::move(dir));
    }
  }

  std::vector<std::string> xdgDataDirs() {
    std::vector<std::string> dirs;
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && dataHome[0] != '\0') {
      pushUniqueDir(dirs, dataHome);
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      pushUniqueDir(dirs, std::string(home) + "/.local/share");
    }
    const char* dataDirs = std::getenv("XDG_DATA_DIRS");
    if (dataDirs != nullptr && dataDirs[0] != '\0') {
      std::size_t start = 0;
      while (start <= std::strlen(dataDirs)) {
        const std::size_t end = std::string_view(dataDirs).find(':', start);
        const std::size_t len = end == std::string_view::npos ? std::string_view(dataDirs).size() - start : end - start;
        const std::string part = StringUtils::trim(std::string_view(dataDirs + start, len));
        pushUniqueDir(dirs, part);
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
    } else {
      pushUniqueDir(dirs, "/usr/local/share");
    }
    // Always include the system data dir so icons/cursors installed in
    // /usr/share are discovered even when XDG_DATA_DIRS is unset or omits
    // it (minimal fresh session, some atomic distros, etc.). The user dirs
    // are enumerated first above, so user-installed themes still win.
    pushUniqueDir(dirs, "/usr/share");
    return dirs;
  }

  // The XDG icon theme roots, in priority order (user dirs first).
  std::vector<std::string> iconBaseDirs() {
    std::vector<std::string> roots;
    const auto data = xdgDataDirs();
    if (!data.empty()) {
      pushUniqueDir(roots, data.front() + "/icons");
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      pushUniqueDir(roots, std::string(home) + "/.icons");
    }
    for (std::size_t i = data.empty() ? 0 : 1; i < data.size(); ++i) {
      pushUniqueDir(roots, data[i] + "/icons");
    }
    return roots;
  }

  // Reads Name= from the [Icon Theme] group of an index.theme file.
  std::string indexThemeName(const fs::path& indexTheme) {
    std::ifstream in(indexTheme);
    if (!in.is_open()) {
      return {};
    }
    bool inIconTheme = false;
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      if (line.empty() || line.front() == '#') {
        continue;
      }
      if (line.front() == '[') {
        inIconTheme = (line == "[Icon Theme]");
        continue;
      }
      if (!inIconTheme) {
        continue;
      }
      if (line.starts_with("Name=")) {
        std::string value = StringUtils::trim(std::string_view(line).substr(5));
        return StringUtils::unquote(value);
      }
    }
    return {};
  }

  // A subdirectory name like "scalable", "48x48", "64x64@2x" denotes an icon
  // size directory; "cursors"/"cursors_scalable" denote cursor content.
  bool isIconSizeDir(std::string_view name) {
    if (name == "scalable" || name == "symbolic") {
      return true;
    }
    bool digit = false;
    for (const char c : name) {
      if (c == 'x' || c == '@') {
        return true;
      }
      if (c >= '0' && c <= '9') {
        digit = true;
        continue;
      }
      if (c == '-' || c == '_') {
        continue;
      }
      return false;
    }
    return digit;
  }

  // Counts image files (svg/png) in a directory tree, bounded to keep discovery
  // cheap on large themes. Used to reject themes whose size directories are
  // empty placeholders (e.g. legacy Bluecurve).
  int countIconFiles(const fs::path& dir, int limit) {
    int count = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (count >= limit) {
        break;
      }
      if (entry.is_directory(ec)) {
        count += countIconFiles(entry.path(), limit);
        continue;
      }
      const std::string ext = entry.path().extension().string();
      if (ext == ".svg" || ext == ".png") {
        ++count;
      }
    }
    return count;
  }

  // Minimum number of image files a theme must provide (across its icon size
  // directories) before it counts as a real icon theme. Filters out legacy or
  // placeholder themes that only ship empty size directories.
  constexpr int kMinIconFiles = 10;

  // Whether a theme directory contains icon content (vs only cursor content).
  // Icon themes come in two layouts:
  //   * size-first (Breeze, Adwaita): <size>/<context>/<file>, e.g. 48x48/apps/
  //   * context-first (Gruvbox Plus, Suru-based): <context>/<size>/<file>,
  //     e.g. apps/16/ or places/scalable/
  // Both must be recognized, or valid themes are filtered out of the Icons page.
  bool hasIconContent(const fs::path& themeDir) {
    std::error_code ec;
    int totalFiles = 0;
    const auto countSizeDir = [&](const fs::path& dir) {
      totalFiles += countIconFiles(dir, kMinIconFiles);
      return totalFiles >= kMinIconFiles;
    };
    for (const auto& entry : fs::directory_iterator(themeDir, ec)) {
      if (!entry.is_directory(ec)) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (name == "cursors" || name == "cursors_scalable" || name.starts_with('.')) {
        continue;
      }
      if (isIconSizeDir(name)) {
        if (countSizeDir(entry.path())) {
          return true;
        }
        continue;
      }
      // Context-first layout: probe the context's size subdirectories. Names
      // that do not look like a context simply contribute nothing.
      for (const auto& contextEntry : fs::directory_iterator(entry.path(), ec)) {
        if (!contextEntry.is_directory(ec)) {
          continue;
        }
        if (!isIconSizeDir(contextEntry.path().filename().string())) {
          continue;
        }
        if (countSizeDir(contextEntry.path())) {
          return true;
        }
      }
    }
    return false;
  }

  bool hasCursorContent(const fs::path& themeDir) {
    std::error_code ec;
    return fs::is_directory(themeDir / "cursors", ec) || fs::is_directory(themeDir / "cursors_scalable", ec);
  }

  std::vector<theme_discovery::ThemeInfo> discover(bool cursors) {
    std::vector<theme_discovery::ThemeInfo> out;
    std::vector<std::string> seen; // first root wins (user dirs are enumerated first)
    for (const auto& base : iconBaseDirs()) {
      std::error_code ec;
      if (!fs::is_directory(base, ec)) {
        continue;
      }
      for (const auto& entry : fs::directory_iterator(base, ec)) {
        if (!entry.is_directory(ec)) {
          continue;
        }
        const fs::path dir = entry.path();
        const std::string name = dir.filename().string();
        if (name.empty() || name.front() == '.' || std::ranges::contains(seen, name)) {
          continue;
        }
        const bool match = cursors ? hasCursorContent(dir) : hasIconContent(dir);
        if (!match) {
          continue;
        }
        seen.push_back(name);
        std::string display = indexThemeName(dir / "index.theme");
        if (display.empty()) {
          display = name;
        }
        out.push_back(theme_discovery::ThemeInfo{.name = name, .displayName = std::move(display), .path = dir.string()});
      }
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) {
      return StringUtils::naturalCaseInsensitiveLess(a.displayName, b.displayName);
    });
    return out;
  }

  std::uint32_t readU32(const std::vector<std::uint8_t>& data, std::size_t& pos) {
    if (pos + 4 > data.size()) {
      return 0;
    }
    const std::uint32_t value = static_cast<std::uint32_t>(data[pos]) | (static_cast<std::uint32_t>(data[pos + 1]) << 8)
                                | (static_cast<std::uint32_t>(data[pos + 2]) << 16)
                                | (static_cast<std::uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return value;
  }

  constexpr std::uint32_t kXcursorMagic = 0x72756358;  // "Xcur"
  constexpr std::uint32_t kXcursorImageType = 0xfffd0002;

  // Launch `dir` in the user's default file manager. Resolves the handler for
  // inode/directory from mimeapps.list (the file the Default Apps settings page
  // writes) and launches it with the directory as argument, falling back to
  // xdg-open. Shared by the Icons and Window Decorations folder buttons.
  bool openDirectoryInFileManager(const std::string& dir) {
    // The user's real config home — where mimeapps.list lives. The shell process
    // keeps the real XDG_CONFIG_HOME, so this resolves to the shared location.
    std::filesystem::path mimeapps;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      mimeapps = std::filesystem::path(xdg) / "mimeapps.list";
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      mimeapps = std::filesystem::path(home) / ".config" / "mimeapps.list";
    }

    // Resolve the default handler id for inode/directory (e.g. "org.kde.dolphin").
    std::string handlerId;
    {
      std::error_code ec;
      std::ifstream in(mimeapps);
      if (in.is_open()) {
        bool inDefaultApps = false;
        std::string line;
        while (std::getline(in, line)) {
          const std::string trimmed = StringUtils::trim(line);
          if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
          }
          if (trimmed.front() == '[') {
            inDefaultApps = (trimmed == "[Default Applications]");
            continue;
          }
          if (!inDefaultApps || !trimmed.starts_with("inode/directory=")) {
            continue;
          }
          const std::string value = StringUtils::trim(std::string_view(trimmed).substr(16));
          if (const std::size_t sep = value.find(';'); sep != std::string::npos) {
            handlerId = value.substr(0, sep);
          } else {
            handlerId = value;
          }
          // mimeapps stores the id with the ".desktop" suffix; DesktopEntry::id is the stem.
          if (handlerId.ends_with(".desktop")) {
            handlerId.resize(handlerId.size() - 8);
          }
          break;
        }
      }
    }

    // Launch the resolved file manager with the directory as argument. The Exec
    // field's %F/%U field codes are stripped by prepareCommand, so the directory
    // is appended explicitly.
    if (!handlerId.empty()) {
      for (const DesktopEntry& entry : desktopEntries()) {
        if (entry.id != handlerId) {
          continue;
        }
        auto prepared = desktop_entry_launch::prepareCommand(entry.exec, entry.terminal);
        if (prepared.has_value() && !prepared->args.empty()) {
          std::vector<std::string> args = std::move(prepared->args);
          args.push_back(dir);
          return process::runAsync(args);
        }
        break;
      }
    }

    // Fallback: let xdg-open resolve the handler (only useful when the running
    // portal actually implements OpenURI; xdg-desktop-portal-kwe does not, so on
    // KineticWE this path is effectively dead — kept for other environments).
    if (process::commandExists("xdg-open")) {
      return process::runAsync(std::vector<std::string>{"xdg-open", dir});
    }
    return false;
  }

} // namespace

namespace theme_discovery {

std::vector<ThemeInfo> iconThemes() {
  return discover(false);
}

std::vector<ThemeInfo> cursorThemes() {
  return discover(true);
}

std::string iconRootPath() {
  // User-owned dirs first (that's where themes get installed and managed).
  const std::vector<std::string> roots = iconBaseDirs();
  std::error_code ec;
  for (const std::string& root : roots) {
    if (fs::is_directory(root, ec)) {
      return root;
    }
  }
  if (!roots.empty()) {
    return roots.front();
  }
  return {};
}

bool openIconRoot() {
  const std::string dir = iconRootPath();
  if (dir.empty()) {
    return false;
  }
  return openDirectoryInFileManager(dir);
}

bool openDirectory(const std::string& dir) {
  return openDirectoryInFileManager(dir);
}

std::string resolveIconInTheme(const std::string& themePath, const std::string& iconName) {
  std::string best;
  bool bestScalable = false;
  int bestSize = 0;
  std::error_code ec;
  const auto consider = [&](const fs::path& dir, const std::string& stem) {
    for (const char* ext : {".svg", ".png"}) {
      const fs::path candidate = dir / (stem + ext);
      if (fs::is_regular_file(candidate, ec)) {
        const bool scalable = dir.filename() == "scalable" || dir.string().contains("scalable");
        const std::string dirName = dir.filename().string();
        int size = 0;
        for (const char c : dirName) {
          if (c >= '0' && c <= '9') {
            size = size * 10 + (c - '0');
          } else if (c == 'x' || c == '@') {
            break;
          }
        }
        if (best.empty() || (scalable && !bestScalable) || (scalable == bestScalable && size > bestSize)) {
          best = candidate.string();
          bestScalable = scalable;
          bestSize = size;
        }
        return;
      }
    }
  };
  // Search the theme's own size dirs first (scalable/ + NNxNN/), then the
  // inherited candidates are not explored here — the caller already resolved
  // per-theme. If the theme lacks the icon at top level, check common
  // sub-contexts (apps/, places/, mimetypes/, ...) inside each size dir.
  std::error_code sec;
  for (const auto& sizeEntry : fs::directory_iterator(themePath, sec)) {
    if (!sizeEntry.is_directory(sec)) {
      continue;
    }
    const std::string sizeName = sizeEntry.path().filename().string();
    if (sizeName == "cursors" || sizeName == "cursors_scalable") {
      continue;
    }
    consider(sizeEntry.path(), iconName);
    std::error_code cec;
    for (const auto& contextEntry : fs::directory_iterator(sizeEntry.path(), cec)) {
      if (!contextEntry.is_directory(cec)) {
        continue;
      }
      consider(contextEntry.path(), iconName);
    }
  }
  return best;
}

std::string cursorPreviewFile(const std::string& themePath) {
  const fs::path cursorsDir = fs::path(themePath) / "cursors";
  std::error_code ec;
  if (!fs::is_directory(cursorsDir, ec)) {
    return {};
  }
  for (const char* name : {"left_ptr", "default", "arrow"}) {
    const fs::path candidate = cursorsDir / name;
    if (fs::is_regular_file(candidate, ec) || fs::is_symlink(candidate, ec)) {
      return candidate.string();
    }
  }
  // Fall back to any regular file in cursors/.
  for (const auto& entry : fs::directory_iterator(cursorsDir, ec)) {
    if (entry.is_regular_file(ec) || entry.is_symlink(ec)) {
      return entry.path().string();
    }
  }
  return {};
}

bool decodeXcursorImage(const std::string& filePath, std::vector<std::uint8_t>& rgba, int& width, int& height) {
  const std::vector<std::uint8_t> data = FileUtils::readBinaryFile(filePath);
  if (data.size() < 16) {
    return false;
  }

  std::size_t pos = 0;
  const std::uint32_t magic = readU32(data, pos);
  if (magic != kXcursorMagic) {
    return false;
  }
  (void)readU32(data, pos); // header size
  (void)readU32(data, pos); // version
  const std::uint32_t ntoc = readU32(data, pos);
  if (ntoc == 0 || ntoc > 4096) {
    return false;
  }

  struct TocEntry {
    std::uint32_t type;
    std::uint32_t subtype;  // nominal size for images
    std::uint32_t position; // absolute file position
  };
  std::vector<TocEntry> tocs;
  tocs.reserve(ntoc);
  for (std::uint32_t i = 0; i < ntoc; ++i) {
    if (pos + 12 > data.size()) {
      return false;
    }
    TocEntry entry;
    entry.type = readU32(data, pos);
    entry.subtype = readU32(data, pos);
    entry.position = readU32(data, pos);
    tocs.push_back(entry);
  }

  // Pick the largest image chunk (by width) as the preview.
  std::size_t bestToc = std::size_t(-1);
  int bestArea = 0;
  for (std::size_t i = 0; i < tocs.size(); ++i) {
    if (tocs[i].type != kXcursorImageType) {
      continue;
    }
    std::size_t p = tocs[i].position;
    if (p + 28 > data.size()) {
      continue;
    }
    p += 16; // skip chunk header (header,type,subtype,version)
    const std::uint32_t w = readU32(data, p);
    const std::uint32_t h = readU32(data, p);
    const int area = static_cast<int>(w) * static_cast<int>(h);
    if (w == 0 || h == 0 || w > 2048 || h > 2048) {
      continue;
    }
    if (bestToc == std::size_t(-1) || area > bestArea) {
      bestToc = i;
      bestArea = area;
    }
  }
  if (bestToc == std::size_t(-1)) {
    return false;
  }

  std::size_t p = tocs[bestToc].position;
  p += 16; // chunk header
  const std::uint32_t w = readU32(data, p);
  const std::uint32_t h = readU32(data, p);
  (void)readU32(data, p); // xhot
  (void)readU32(data, p); // yhot
  (void)readU32(data, p); // delay
  const std::uint32_t pixelCount = w * h;
  if (pixelCount == 0 || p + static_cast<std::size_t>(pixelCount) * 4 > data.size()) {
    return false;
  }

  rgba.resize(static_cast<std::size_t>(pixelCount) * 4);
  for (std::uint32_t i = 0; i < pixelCount; ++i) {
    // Pixels are stored as 0xAARRGGBB.
    const std::uint32_t pixel = readU32(data, p);
    const std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xFF);
    const std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xFF);
    const std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xFF);
    const std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xFF);
    rgba[i * 4 + 0] = r;
    rgba[i * 4 + 1] = g;
    rgba[i * 4 + 2] = b;
    rgba[i * 4 + 3] = a;
  }
  width = static_cast<int>(w);
  height = static_cast<int>(h);
  return true;
}

} // namespace theme_discovery