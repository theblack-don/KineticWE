#include "system/terminal_launch.h"

#include "system/process.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  std::vector<std::string> tokenize(std::string_view cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (const char c : cmd) {
      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
        continue;
      }
      if (c == '"' && !inSingle) {
        inDouble = !inDouble;
        continue;
      }
      if (c == ' ' && !inSingle && !inDouble) {
        if (!current.empty()) {
          args.push_back(std::move(current));
          current.clear();
        }
        continue;
      }
      current += c;
    }
    if (!current.empty()) {
      args.push_back(std::move(current));
    }
    return args;
  }

  std::string expandExecutablePath(std::string_view binary) {
    if (binary.empty() || binary.front() != '~') {
      return std::string(binary);
    }
    return FileUtils::expandUserPath(std::string(binary)).string();
  }

  bool isExecutableOnPath(std::string_view binary) {
    if (binary.empty()) {
      return false;
    }
    if (binary.contains('/')) {
      const std::string expanded = expandExecutablePath(binary);
      return access(expanded.c_str(), X_OK) == 0;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      return false;
    }

    std::string_view path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto sep = path.find(':', start);
      const auto segment = sep == std::string_view::npos ? path.substr(start) : path.substr(start, sep - start);
      if (!segment.empty()) {
        std::string candidate(segment);
        candidate.push_back('/');
        candidate.append(binary);
        if (access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      if (sep == std::string_view::npos) {
        break;
      }
      start = sep + 1;
    }
    return false;
  }

  // The user's kdeglobals (shared KDE config) in the real config home.
  [[nodiscard]] std::filesystem::path kdeglobalsPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      return std::filesystem::path(xdg) / "kdeglobals";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config" / "kdeglobals";
    }
    return {};
  }

  std::vector<std::string> discoverTerminal(const terminal_launch::Options& options) {
    if (!options.terminalCandidates.empty()) {
      for (const auto& candidate : options.terminalCandidates) {
        std::vector<std::string> terminal = tokenize(candidate);
        if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
          return terminal;
        }
      }
      return {};
    }
    if (!options.useSystemTerminalDiscovery) {
      return {};
    }

    if (const char* envTerminal = std::getenv("TERMINAL"); envTerminal != nullptr && envTerminal[0] != '\0') {
      std::vector<std::string> terminal = tokenize(envTerminal);
      if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
        return terminal;
      }
    }

    // Honor the terminal configured in kdeglobals [General] TerminalApplication
    // (what KDE's component chooser, KIO and the Default Applications settings
    // page write) ahead of the built-in candidate list, so the user's choice
    // actually launches Terminal=true desktop entries. Falls through when the
    // configured command is missing or not executable.
    if (const std::string configured = terminal_launch::configuredTerminal(); !configured.empty()) {
      std::vector<std::string> terminal = tokenize(configured);
      if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
        return terminal;
      }
    }

    static constexpr std::array<std::string_view, 11> kTerminalCandidates = {
        "x-terminal-emulator", "ghostty", "kitty",  "alacritty", "wezterm", "foot", "konsole",
        "gnome-terminal",      "kgx",     "ptyxis", "xterm",
    };
    for (const auto candidate : kTerminalCandidates) {
      if (isExecutableOnPath(candidate)) {
        return {std::string(candidate)};
      }
    }
    return {};
  }

  bool usesCommandSeparator(std::string_view terminal) {
    return terminal == "gnome-terminal" || terminal == "kgx" || terminal == "ptyxis";
  }

} // namespace

namespace terminal_launch {

  std::optional<std::vector<std::string>> prepareCommand(std::string_view command, const Options& options) {
    if (command.empty()) {
      return std::nullopt;
    }

    std::vector<std::string> terminal = discoverTerminal(options);
    if (terminal.empty()) {
      return std::nullopt;
    }

    if (terminal.front().contains('/')) {
      terminal.front() = expandExecutablePath(terminal.front());
    }

    const std::string termBin = terminal.front();
    if (usesCommandSeparator(termBin)) {
      terminal.emplace_back("--");
    } else {
      terminal.emplace_back("-e");
    }
    terminal.emplace_back("sh");
    terminal.emplace_back("-lc");
    terminal.emplace_back(command);
    return terminal;
  }

  bool launch(std::string_view command, const Options& options) {
    auto prepared = prepareCommand(command, options);
    return prepared.has_value() && process::runAsync(*prepared);
  }

  std::string configuredTerminal() {
    const std::filesystem::path path = kdeglobalsPath();
    if (path.empty()) {
      return {};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
      return {};
    }
    std::ifstream in(path);
    if (!in.is_open()) {
      return {};
    }
    bool inGeneral = false;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
      }
      const std::string trimmed = StringUtils::trim(line);
      if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
        continue;
      }
      if (trimmed.front() == '[' && trimmed.back() == ']') {
        inGeneral = (trimmed == "[General]");
        continue;
      }
      if (!inGeneral) {
        continue;
      }
      const std::size_t eq = trimmed.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      std::string key = StringUtils::trim(trimmed.substr(0, eq));
      // KConfig expansion/immutability suffixes (e.g. "TerminalApplication[$e]").
      if (const std::size_t bracket = key.find('['); bracket != std::string::npos) {
        key.resize(bracket);
      }
      if (key != "TerminalApplication") {
        continue;
      }
      std::string value = trimmed.substr(eq + 1);
      if (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
      }
      return value;
    }
    return {};
  }

  bool writeConfiguredTerminal(std::string_view command) {
    if (!process::commandExists("kwriteconfig6")) {
      return false;
    }
    // Delegate to KDE's own config writer so kdeglobals — a file other KDE
    // components also write — keeps its exact KConfig formatting. The absolute
    // path keeps the write consistent with configuredTerminal()'s read even if
    // XDG_CONFIG_HOME ever gets redirected.
    const std::filesystem::path target = kdeglobalsPath();
    if (target.empty()) {
      return false;
    }
    std::vector<std::string> args{
        "kwriteconfig6", "--file", target.string(), "--group", "General", "--key", "TerminalApplication"};
    if (command.empty()) {
      args.push_back("--delete");
    } else {
      args.push_back(std::string(command));
    }
    return static_cast<bool>(process::runSyncWithTimeout(args, std::chrono::seconds(10)));
  }

} // namespace terminal_launch
