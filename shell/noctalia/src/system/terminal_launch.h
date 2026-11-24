#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terminal_launch {

  struct Options {
    std::vector<std::string> terminalCandidates;
    bool useSystemTerminalDiscovery = true;
  };

  std::optional<std::vector<std::string>> prepareCommand(std::string_view command, const Options& options = {});

  [[nodiscard]] bool launch(std::string_view command, const Options& options = {});

  // The user's configured terminal command from kdeglobals [General]
  // TerminalApplication ("" when unset) — the location KDE's terminal
  // component chooser, KIO and the Default Applications settings page share.
  [[nodiscard]] std::string configuredTerminal();

  // Writes (empty command: deletes) kdeglobals [General] TerminalApplication
  // via kwriteconfig6, so the shared file keeps its exact KConfig formatting.
  // Returns false when kwriteconfig6 is unavailable or fails.
  [[nodiscard]] bool writeConfiguredTerminal(std::string_view command);

} // namespace terminal_launch
