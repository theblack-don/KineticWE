#include "custom_keybinds.h"

#include "config/config_service.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "keybinds_kwe.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/process.h"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace {

  const Logger kLog("kinetic-settings");

  const sdbus::ServiceName kGlobalAccelBusName{"org.kde.kglobalaccel"};
  const sdbus::ObjectPath kGlobalAccelPath{"/kglobalaccel"};
  constexpr auto kGlobalAccelInterface = "org.kde.KGlobalAccel";
  constexpr auto kComponentInterface = "org.kde.kglobalaccel.Component";

  // Same resolution as the compositor's settings root (see kineticwe_kwe.cpp).
  std::filesystem::path configHome() {
    if (const char* kwe = std::getenv("KWE_CONFIG_HOME"); kwe != nullptr && *kwe != '\0') {
      return kwe;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config" / "kineticwe";
    }
    return ".";
  }

  // The daemon's D-Bus object path for a component replaces every non-ASCII
  // character with '_' (kglobalacceld component.cpp: dbusPath()).
  [[nodiscard]] std::string componentObjectPath(std::string_view component) {
    std::string path = "/component/";
    for (const char c : component) {
      path.push_back(
          (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ? c : '_'
      );
    }
    return path;
  }

  [[nodiscard]] std::string friendlyNameFor(std::string_view component) {
    return component == CustomKeybinds::kAppsComponent
        ? std::string(CustomKeybinds::kAppsFriendly)
        : std::string(CustomKeybinds::kCommandsFriendly);
  }

  // appId may be a relative desktop-file id; the exec needs its field codes
  // (%u, %f, ...) stripped before launching.
  [[nodiscard]] std::string stripExecFieldCodes(const std::string& exec) {
    std::string cleaned = exec;
    std::size_t start = 0;
    while (start < cleaned.size()) {
      const std::size_t percent = cleaned.find('%', start);
      if (percent == std::string::npos) {
        break;
      }
      std::size_t end = percent + 1;
      while (end < cleaned.size() && cleaned[end] != ' ' && cleaned[end] != '\t') {
        ++end;
      }
      cleaned.erase(percent, end - percent);
      start = percent;
    }
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '\t')) {
      cleaned.pop_back();
    }
    return cleaned;
  }

} // namespace

struct CustomKeybinds::Impl {
  std::unique_ptr<sdbus::IProxy> appsProxy;
  std::unique_ptr<sdbus::IProxy> commandsProxy;
};

CustomKeybinds::CustomKeybinds(SessionBus& bus, const ConfigService& config) : m_bus(bus), m_config(config) {}

CustomKeybinds::~CustomKeybinds() = default;

void CustomKeybinds::load() {
  m_definitions.clear();
  const std::filesystem::path path = configHome() / "custom-keybinds.kwe";
  std::ifstream in(path);
  if (!in.is_open()) {
    kLog.warn("custom keybinds: could not open definitions file {}", path.string());
    return;
  }
  kLog.info("custom keybinds: reading definitions from {}", path.string());
  std::string line;
  std::string group;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      group = line.substr(1, line.size() - 2);
      continue;
    }
    if (group.empty()) {
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) {
      continue;
    }
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (group == "applications" || group == "custom-commands") {
      auto& definition = m_definitions.emplace_back();
      definition.component = group;
      definition.action = key;
      definition.friendlyName = value;
    } else if (group == "applications:exec" || group == "custom-commands:exec") {
      const std::string component = group.substr(0, group.find(':'));
      const auto it = std::ranges::find_if(m_definitions, [&](const Definition& d) {
        return d.component == component && d.action == key;
      });
      if (it != m_definitions.end()) {
        it->command = value;
      }
    }
  }
}

bool CustomKeybinds::save() const {
  const std::filesystem::path path = configHome() / "custom-keybinds.kwe";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    kLog.warn("could not create keybinds dir: {}", error.message());
    return false;
  }
  const std::string tmpName = path.string() + ".tmp";
  {
    std::ofstream out(tmpName);
    if (!out.is_open()) {
      kLog.warn("could not open {} for writing", tmpName);
      return false;
    }
    out << "# KineticWE custom keybinds — generated by Kinetic Settings.\n";
    out << "# Each entry adds one action to the Applications / Custom Commands\n";
    out << "# keybinds categories; chords live in keybinds.kwe and the daemon.\n";
    for (const std::string_view component : {CustomKeybinds::kAppsComponent, CustomKeybinds::kCommandsComponent}) {
      const std::string componentStr(component);
      bool wroteNames = false;
      for (const auto& definition : m_definitions) {
        if (definition.component != componentStr) {
          continue;
        }
        if (!wroteNames) {
          out << '[' << componentStr << "]\n";
          wroteNames = true;
        }
        out << definition.action << '=' << definition.friendlyName << '\n';
      }
      if (!wroteNames) {
        continue;
      }
      out << '[' << componentStr << ":exec]\n";
      for (const auto& definition : m_definitions) {
        if (definition.component == componentStr) {
          out << definition.action << '=' << definition.command << '\n';
        }
      }
    }
  }
  std::filesystem::rename(tmpName, path, error);
  if (error) {
    kLog.warn("could not move {} into place: {}", tmpName, error.message());
    return false;
  }
  return true;
}

bool CustomKeybinds::registerAction(const Definition& definition) {
  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kGlobalAccelBusName, kGlobalAccelPath);
    const std::vector<std::string> id{
        definition.component, definition.action, friendlyNameFor(definition.component), definition.friendlyName
    };
    proxy->callMethod("doRegister").onInterface(kGlobalAccelInterface).withArguments(id);
    kLog.info("custom keybind registered {}.{}", definition.component, definition.action);
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("custom keybind register failed for {}.{}: {}", definition.component, definition.action, e.what());
    return false;
  }
}

void CustomKeybinds::subscribeSignals() {
  m_impl = std::make_unique<Impl>();
  const auto subscribe = [this](std::string_view component, std::unique_ptr<sdbus::IProxy>& proxy) {
    try {
      proxy = sdbus::createProxy(
          m_bus.connection(), kGlobalAccelBusName, sdbus::ObjectPath{componentObjectPath(component)}
      );
      proxy->uponSignal("globalShortcutPressed")
          .onInterface(kComponentInterface)
          .call([this](std::string triggeredComponent, std::string action, std::int64_t /*timestamp*/) {
            onTriggered(triggeredComponent, action);
          });
    } catch (const sdbus::Error& e) {
      kLog.warn("custom keybind signal subscription failed for {}: {}", component, e.what());
    }
  };
  subscribe(kAppsComponent, m_impl->appsProxy);
  subscribe(kCommandsComponent, m_impl->commandsProxy);
}

void CustomKeybinds::initialize() {
  if (m_impl != nullptr) {
    return;
  }
  load();
  kLog.info("custom keybinds: loaded {} definition(s) from custom-keybinds.kwe", m_definitions.size());
  for (const auto& definition : m_definitions) {
    if (!registerAction(definition)) {
      kLog.warn("custom keybind startup registration failed for {}.{}", definition.component, definition.action);
    }
  }
  subscribeSignals();
}

bool CustomKeybinds::addApp(const std::string& appId, const std::string& friendlyName, const std::string& exec) {
  const auto already = std::ranges::find_if(m_definitions, [&](const Definition& d) {
    return d.component == kAppsComponent && d.action == appId;
  });
  if (already != m_definitions.end()) {
    return true;
  }
  Definition definition{
      .component = std::string(kAppsComponent),
      .action = appId,
      .friendlyName = friendlyName,
      .command = stripExecFieldCodes(exec),
  };
  if (!registerAction(definition)) {
    return false;
  }
  m_definitions.push_back(std::move(definition));
  return save();
}

bool CustomKeybinds::addCommand(const std::string& command) {
  const std::string base = "cmd-";
  int next = 1;
  for (const auto& definition : m_definitions) {
    if (definition.component == kCommandsComponent && definition.action.starts_with(base)) {
      next = std::max(next, std::stoi(definition.action.substr(base.size())) + 1);
    }
  }
  Definition definition{
      .component = std::string(kCommandsComponent),
      .action = base + std::to_string(next),
      .friendlyName = command,
      .command = command,
  };
  if (!registerAction(definition)) {
    return false;
  }
  m_definitions.push_back(std::move(definition));
  return save();
}

bool CustomKeybinds::updateCommand(std::string_view action, const std::string& command) {
  const auto it = std::ranges::find_if(m_definitions, [&](const Definition& d) {
    return d.component == kCommandsComponent && d.action == action;
  });
  if (it == m_definitions.end() || command.empty()) {
    return false;
  }
  if (it->friendlyName == command && it->command == command) {
    return true;
  }
  // The command line is both what runs and what the page displays (the daemon's
  // action friendly name). Re-register so doRegister updates the existing
  // action's friendly name in place; its bound chords are untouched.
  it->friendlyName = command;
  it->command = command;
  registerAction(*it);
  return save();
}

bool CustomKeybinds::remove(std::string_view component, std::string_view action) {
  const auto it = std::ranges::find_if(m_definitions, [&](const Definition& d) {
    return d.component == component && d.action == action;
  });
  if (it == m_definitions.end()) {
    kLog.warn("custom keybind remove: no definition for {}.{}", component, action);
  }
  const std::string friendlyName = it != m_definitions.end() ? it->friendlyName : std::string(action);
  try {
    auto proxy = sdbus::createProxy(m_bus.connection(), kGlobalAccelBusName, kGlobalAccelPath);
    const std::vector<std::string> id{
        std::string(component), std::string(action), friendlyNameFor(component), friendlyName
    };
    proxy->callMethod("unRegister").onInterface(kGlobalAccelInterface).withArguments(id);
    kLog.info("custom keybind unregistered {}.{}", component, action);
  } catch (const sdbus::Error& e) {
    kLog.warn("custom keybind unregister failed for {}.{}: {}", component, action, e.what());
    return false;
  }
  if (it != m_definitions.end()) {
    m_definitions.erase(it);
    KeybindsKwe::removeEntries(KeybindsKwe::defaultPath(), std::string(component), std::string(action));
    save();
  }
  return true;
}

void CustomKeybinds::onTriggered(const std::string& component, const std::string& action) {
  const auto it = std::ranges::find_if(m_definitions, [&](const Definition& d) {
    return d.component == component && d.action == action;
  });
  if (it == m_definitions.end()) {
    kLog.warn("custom keybind trigger for unknown action {}.{}", component, action);
    return;
  }
  if (component == kAppsComponent) {
    const auto& entries = desktopEntries();
    const auto entry = std::ranges::find_if(entries, [&](const DesktopEntry& e) { return e.id == it->action; });
    if (entry == entries.end()) {
      kLog.warn("custom keybind trigger: app '{}' no longer exists", it->action);
      return;
    }
    // Same app-launch settings as the launcher, dock and taskbar.
    const auto& shell = m_config.config().shell;
    const desktop_entry_launch::LaunchOptions options{
        .runAsSystemdService = shell.launchAppsAsSystemdServices,
        .customCommand = shell.launchAppsCustomCommand,
    };
    if (!desktop_entry_launch::launchEntry(*entry, options)) {
      kLog.warn("custom keybind trigger: failed to launch '{}'", it->action);
    }
    return;
  }
  if (!process::runAsync(it->command)) {
    kLog.warn("custom keybind trigger: failed to run '{}'", it->command);
  }
}