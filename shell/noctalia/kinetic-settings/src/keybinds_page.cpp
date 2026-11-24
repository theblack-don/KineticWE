#include "keybinds_page.h"

#include "core/input/key_chord.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "keybind_codec.h"
#include "keybinds_kwe.h"
#include "kinetic-settings/src/section_card.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "shell/controls/search_picker.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("kinetic-settings");

  constexpr float kChordRecorderWidth = 260.0F;
  // Per-window shortcuts (Window Close, Move, ...) are per-instance, not
  // rebindable globals; hide them like the KDE Shortcuts KCM.
  constexpr std::string_view kSessionActionPrefix = "_k_session:";
  // Components the page surfaces. The keybinds list is deliberately an
  // allow-list of KineticWE-owned components rather than a deny-list of
  // everything else: the daemon exposes one component per registered shortcut
  // owner, which on a real session includes Plasma services ("Audio Volume",
  // "Display Configuration", "Keyboard Layout Switcher", "Media Controller",
  // "System Monitor", "Touchpad", "ActivityManager", "plasmashell",
  // "ksmserver") and per-application components (Konsole, Dolphin, Ghostty,
  // KCalc, ...). None of those are backed by KineticWE config — they read
  // kwinrc / Plasma configs, not kineticwe.kwe — so editing them here presents
  // binds that KineticWE neither owns nor themes. The compositor ("kineticwe"), the
  // shell ("noctalia"), the shell-backed "audio" / "media-ui" / "brightness"
  // action groups (registered by the compositor, see src/noctaliasettings.cpp), Spectacle and
  // the user's own Applications / Custom Commands actions are shown.
  //
  // Spectacle is the one per-application component surfaced on purpose: it owns
  // screenshots, and KineticWE ships its actions unbound so the user can bind
  // them here (the compositor clears the .desktop defaults; see
  // GlobalShortcutsManager::clearSpectacleDefaults). Its component unique name
  // is the desktop-file storage id, not the friendly "Spectacle" name.
  const std::vector<std::string>& visibleComponents() {
    static const std::vector<std::string> kVisible{
        "kineticwe",
        "noctalia",
        "audio",
        "media-ui",
        "brightness",
        "org.kde.spectacle.desktop",
        std::string(CustomKeybinds::kAppsComponent),
        std::string(CustomKeybinds::kCommandsComponent),
    };
    return kVisible;
  }

  // Upstream compositor actions KineticWE deliberately does not surface on the
  // page: they are not part of the KineticWE workflow. Window Restore, the
  // Present Windows launcher, the titlebar/frame toggle (duplicated by
  // "Toggle Title Bars"), the custom quick-tile slots, and the xkill-style
  // Kill Window. Unlike hidden components these are kept in m_components so
  // conflict detection still knows about chords they own (e.g. Kill Window's
  // Meta+Ctrl+Esc); they are only skipped when rendering and when resetting.
  const std::vector<std::string>& hiddenActions() {
    static const std::vector<std::string> kHidden{
        "Window Restore",
        "ExposeAll",
        "Window No Border",
        "Window Custom Quick Tile Left",
        "Window Custom Quick Tile Right",
        "Window Custom Quick Tile Top",
        "Window Custom Quick Tile Bottom",
        "Kill Window",
    };
    return kHidden;
  }

  [[nodiscard]] bool isHiddenAction(std::string_view component, std::string_view action) {
    if (component != "kineticwe") {
      return false;
    }
    return std::ranges::any_of(hiddenActions(), [action](const std::string& hidden) {
      return std::string_view(hidden) == action;
    });
  }

  [[nodiscard]] bool isHiddenComponent(std::string_view uniqueName) {
    const auto underscoreForm = [&uniqueName]() {
      std::string form(uniqueName);
      std::ranges::replace(form, '.', '_');
      return form;
    };
    return std::ranges::none_of(visibleComponents(), [&](const std::string& visible) {
      return uniqueName == visible || underscoreForm() == visible;
    });
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

  // Status pill ("Override"), same visual as the other pages.
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

  // The compositor is branded "KineticWE"; everything else uses the daemon's
  // friendly name (which for the compositor's own components defaults to the
  // unique name).
  [[nodiscard]] std::string componentDisplayName(const KeybindDbus::Component& component) {
    if (component.uniqueName == "kineticwe") {
      return "KineticWE";
    }
    // Spectacle registers under its desktop-file storage id and the daemon
    // reports no component friendly name, so surface the product name instead
    // of the package id. The underscore form is the id with dots replaced.
    if (component.uniqueName == "org.kde.spectacle.desktop"
        || component.uniqueName == "org_kde_spectacle_desktop") {
      return "Spectacle";
    }
    if (!component.friendlyName.empty()) {
      return component.friendlyName;
    }
    if (component.uniqueName.empty()) {
      return {};
    }
    std::string name = component.uniqueName;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
  }

  // The compositor still registers its virtual-desktop shortcuts with upstream
  // KWin's "Desktop" wording ("Switch to Next Desktop", "Window to Desktop 1",
  // ...), but every other KineticWE settings page calls these workspaces.
  // Rename the displayed label for the compositor's own component so the
  // Kwebinds list reads consistently. This is display-only: action IDs,
  // chords, and the friendly names sent back to the daemon are untouched.
  [[nodiscard]] std::string workspaceWording(std::string_view text) {
    std::string result(text);
    static constexpr std::pair<std::string_view, std::string_view> kTerms[] = {
        {"Desktops", "Workspaces"},
        {"Desktop", "Workspace"},
    };
    for (const auto& [from, to] : kTerms) {
      for (std::size_t pos = result.find(from); pos != std::string::npos; pos = result.find(from, pos + to.size())) {
        result.replace(pos, from.size(), to);
      }
    }
    return result;
  }

  // The label the UI shows for an action row: friendlyName when the daemon
  // has one, otherwise the actionUnique id (matches buildActionRow at the
  // call site). Sorting by this key keeps the rendered list in the same
  // order as the visible labels. The compositor's own component gets the
  // workspace wording applied; "Peek at Desktop" ("Show Desktop") is left
  // alone because it refers to the desktop surface, not a virtual desktop.
  [[nodiscard]] std::string displayActionName(const KeybindDbus::Component& component, const KeybindDbus::Action& action) {
    const std::string_view name = action.friendlyName.empty() ? std::string_view(action.actionUnique) : std::string_view(action.friendlyName);
    if (component.uniqueName != "kineticwe" || action.actionUnique == "Show Desktop") {
      return std::string(name);
    }
    return workspaceWording(name);
  }

  // kineticwe first, then noctalia, then the rest in case-insensitive alphabetical
  // order of the user-visible display name (the daemon's registration
  // order is not a stable UI order, and the user reads the list by display
  // name — sorting by uniqueName would disagree with that for any component
  // whose friendlyName differs from its uniqueName). Each component's
  // actions are sorted the same way so the right-column rows read
  // alphabetically and the keybinds.kwe mirror file stays stable.
  void orderComponents(std::vector<KeybindDbus::Component>& components) {
    const auto rank = [](const std::string& name) {
      if (name == "kineticwe") {
        return 0;
      }
      if (name == "noctalia") {
        return 1;
      }
      return 2;
    };
    // Natural (number-aware) string compare, case-insensitive. Walks both
    // strings and at each step either takes a contiguous run of ASCII
    // digits and compares them as integers, or compares the current
    // characters case-insensitively. Ties fall back to length. This makes
    // "Desktop 1" sort before "Desktop 2" before "Desktop 10" (the KWin
    // actions are numbered, so the plain lexical order is unusable).
    const auto naturalLess = [](std::string_view lhs, std::string_view rhs) {
      std::size_t i = 0;
      std::size_t j = 0;
      while (i < lhs.size() && j < rhs.size()) {
        const bool leftDigit = std::isdigit(static_cast<unsigned char>(lhs[i])) != 0;
        const bool rightDigit = std::isdigit(static_cast<unsigned char>(rhs[j])) != 0;
        if (leftDigit && rightDigit) {
          // Skip leading zeros so "Desktop 01" sorts with "Desktop 1".
          while (i < lhs.size() && lhs[i] == '0') {
            ++i;
          }
          while (j < rhs.size() && rhs[j] == '0') {
            ++j;
          }
          const std::size_t leftStart = i;
          const std::size_t rightStart = j;
          while (i < lhs.size() && std::isdigit(static_cast<unsigned char>(lhs[i])) != 0) {
            ++i;
          }
          while (j < rhs.size() && std::isdigit(static_cast<unsigned char>(rhs[j])) != 0) {
            ++j;
          }
          const std::size_t leftLen = i - leftStart;
          const std::size_t rightLen = j - rightStart;
          if (leftLen != rightLen) {
            return leftLen < rightLen;
          }
          // Same length: compare digit-by-digit (we already stripped the
          // leading zeros so all digits are significant).
          for (std::size_t k = 0; k < leftLen; ++k) {
            if (lhs[leftStart + k] != rhs[rightStart + k]) {
              return lhs[leftStart + k] < rhs[rightStart + k];
            }
          }
          // Numbers are equal; fall through to the next run.
          continue;
        }
        const unsigned char lc = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(lhs[i])));
        const unsigned char rc = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(rhs[j])));
        if (lc != rc) {
          return lc < rc;
        }
        ++i;
        ++j;
      }
      return lhs.size() < rhs.size();
    };
    for (auto& component : components) {
      std::stable_sort(component.actions.begin(), component.actions.end(),
          [&component, &naturalLess](const KeybindDbus::Action& a, const KeybindDbus::Action& b) {
            return naturalLess(displayActionName(component, a), displayActionName(component, b));
          });
    }
    std::stable_sort(components.begin(), components.end(), [&](const auto& a, const auto& b) {
      if (rank(a.uniqueName) != rank(b.uniqueName)) {
        return rank(a.uniqueName) < rank(b.uniqueName);
      }
      return naturalLess(componentDisplayName(a), componentDisplayName(b));
    });
  }

  void filterComponents(std::vector<KeybindDbus::Component>& components) {
    std::erase_if(components, [](const KeybindDbus::Component& component) {
      return isHiddenComponent(component.uniqueName);
    });
  }

} // namespace

KeybindsPage::KeybindsPage(SessionBus& bus, CustomKeybinds& customKeybinds)
    : m_bus(bus), m_customKeybinds(customKeybinds), m_dbus(bus) {}

InputArea* KeybindsPage::takePendingFocusArea() {
  InputArea* area = m_pendingFocusArea;
  m_pendingFocusArea = nullptr;
  return area;
}

std::string KeybindsPage::keyPath(std::string_view component, std::string_view action) {
  // Separator between the component and action halves of the pending-key path.
  // It must never appear in a component or action name: both can contain dots
  // (service components end in ".desktop"; custom-app actions are desktop-file
  // ids like "org.kde.dolphin"), so a dot separator (used in an earlier rev)
  // silently corrupted the round-trip and made every ".desktop" component —
  // including Spectacle — fail to apply. The ASCII unit separator is never
  // produced by any component/action name.
  static constexpr char kSep = '\x1d';
  return std::string(component) + kSep + std::string(action);
}

bool KeybindsPage::splitKeyPath(std::string_view keyPath, std::string& component, std::string& action) {
  static constexpr char kSep = '\x1d';
  const auto sep = keyPath.find(kSep);
  if (sep == std::string_view::npos || sep == 0 || sep + 1 >= keyPath.size()) {
    return false;
  }
  component = keyPath.substr(0, sep);
  action = keyPath.substr(sep + 1);
  return true;
}

void KeybindsPage::ensureLoaded() {
  if (!m_loaded) {
    m_components = m_dbus.components();
    filterComponents(m_components);
    orderComponents(m_components);
    m_loaded = true;
  }
}

const KeybindDbus::Action* KeybindsPage::findAction(std::string_view component, std::string_view action) const {
  const auto componentIt = std::ranges::find_if(
      m_components, [component](const KeybindDbus::Component& c) { return c.uniqueName == component; }
  );
  if (componentIt == m_components.end()) {
    return nullptr;
  }
  const auto actionIt = std::ranges::find_if(
      componentIt->actions, [action](const KeybindDbus::Action& a) { return a.actionUnique == action; }
  );
  return actionIt == componentIt->actions.end() ? nullptr : &*actionIt;
}

std::vector<KeyChord> KeybindsPage::effectiveChords(std::string_view component, std::string_view action) const {
  const auto staged = m_pending.find(keyPath(component, action));
  if (staged != m_pending.end()) {
    return staged->second;
  }
  if (const auto* daemonAction = findAction(component, action)) {
    return daemonAction->active;
  }
  return {};
}

bool KeybindsPage::overridden(const std::string& component, const KeybindDbus::Action& action) const {
  return effectiveChords(component, action.actionUnique) != action.defaults;
}

bool KeybindsPage::matchesFilter(
    const KeybindDbus::Component& component, const KeybindDbus::Action& action, const Filter& filter
) const {
  if (filter.showOverriddenOnly && !overridden(component.uniqueName, action)) {
    return false;
  }
  if (filter.searchQuery.empty()) {
    return true;
  }
  const auto matches = [&filter](std::string_view text) {
    return StringUtils::containsInsensitive(text, filter.searchQuery);
  };
  if (matches(action.friendlyName) || matches(action.actionUnique) || matches(displayActionName(component, action))) {
    return true;
  }
  for (const auto& chord : effectiveChords(component.uniqueName, action.actionUnique)) {
    if (matches(KeybindCodec::toChordString(chord))) {
      return true;
    }
  }
  return false;
}

void KeybindsPage::stage(const std::string& component, const std::string& action, std::vector<KeyChord> chords) {
  ensureLoaded();
  const std::string path = keyPath(component, action);
  // Editing back to what the daemon has un-stages (the row stops being dirty).
  if (const auto* daemonAction = findAction(component, action); daemonAction != nullptr
      && chords == daemonAction->active) {
    kLog.warn("keybinds stage UNSTAGED {}.{} (matches daemon active)", component, action);
    m_pending.erase(path);
  } else {
    m_pending[path] = std::move(chords);
    kLog.warn("keybinds stage STAGED {}.{} (pending={})", component, action, m_pending.size());
  }
}

void KeybindsPage::stageAndRebuild(
    const std::string& component, const std::string& action, std::vector<KeyChord> chords,
    const Callbacks& callbacks
) {
  stage(component, action, std::move(chords));
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

std::vector<std::string> KeybindsPage::resettableKeyPaths() const {
  std::vector<std::string> paths;
  for (const auto& component : m_components) {
    for (const auto& action : component.actions) {
      if (!action.actionUnique.starts_with(kSessionActionPrefix)
          && !isHiddenAction(component.uniqueName, action.actionUnique)) {
        paths.push_back(keyPath(component.uniqueName, action.actionUnique));
      }
    }
  }
  return paths;
}

void KeybindsPage::resetAll() {
  m_pendingRebind.reset();
  for (const auto& component : m_components) {
    for (const auto& action : component.actions) {
      if (action.actionUnique.starts_with(kSessionActionPrefix)
          || isHiddenAction(component.uniqueName, action.actionUnique)) {
        continue;
      }
      if (effectiveChords(component.uniqueName, action.actionUnique) != action.defaults) {
        stage(component.uniqueName, action.actionUnique, action.defaults);
      }
    }
  }
}

bool KeybindsPage::applyAll() {
  ensureLoaded();
  if (m_pending.empty()) {
    m_errorNotice.clear();
    return true;
  }
  for (const auto& [path, chords] : m_pending) {
    std::string component;
    std::string action;
    if (!splitKeyPath(path, component, action)) {
      continue;
    }
    // actionId needs all four entries: [component, action, component
    // friendlyName, action friendlyName]; the daemon rejects shorter ids.
    std::string componentFriendlyName = component;
    std::string actionFriendlyName = action;
    const auto componentIt = std::ranges::find_if(
        m_components, [&component](const KeybindDbus::Component& c) { return c.uniqueName == component; }
    );
    if (componentIt != m_components.end() && !componentIt->friendlyName.empty()) {
      componentFriendlyName = componentIt->friendlyName;
      const auto actionIt = std::ranges::find_if(
          componentIt->actions, [&action](const KeybindDbus::Action& a) { return a.actionUnique == action; }
      );
      if (actionIt != componentIt->actions.end() && !actionIt->friendlyName.empty()) {
        actionFriendlyName = actionIt->friendlyName;
      }
    }
    if (!m_dbus.setActionShortcuts(component, action, actionFriendlyName, componentFriendlyName, chords)) {
      kLog.warn("keybinds apply failed for {}.{}; staged edits kept", component, action);
      // Keep the staged edits for retry, but tell the user why Apply appeared
      // to do nothing instead of leaving the button silently dead.
      m_errorNotice = "Could not bind " + component + "." + action
          + ". The daemon may have rejected the chord (e.g. it conflicts with "
          + "another shortcut that cannot be shown). Your changes were kept; "
          + "try again after clearing the conflicting bind.";
      return false;
    }
  }
  m_pending.clear();
  m_errorNotice.clear();

  // Re-read the daemon (the source of truth) and rewrite the mirror with the
  // binds that actually differ from the defaults. The daemon persists its own
  // state to kglobalshortcutsrc itself; a failed mirror write is only a
  // warning — the binds are already live.
  m_components = m_dbus.components();
  filterComponents(m_components);
  orderComponents(m_components);
  std::vector<KeybindsKwe::Entry> entries;
  std::size_t customEntries = 0;
  for (const auto& component : m_components) {
    for (const auto& action : component.actions) {
      // Custom keybinds (Applications / Custom Commands) are user-owned: persist
      // their active chords unconditionally. The daemon may load a previously
      // saved chord from kglobalaccelrc as BOTH active and default, so the
      // generic `active != defaults` check would treat the user's own bind as
      // stock and silently drop it from the mirror — losing it on the next
      // login. Non-custom actions keep the override-only rule (active !=
      // defaults) so the portable mirror stays minimal.
      if (isCustomComponent(component.uniqueName)) {
        if (!action.active.empty()) {
          entries.push_back(KeybindsKwe::Entry{
              .component = component.uniqueName,
              .action = action.actionUnique,
              .chords = action.active,
          });
          ++customEntries;
        }
      } else if (action.active != action.defaults) {
        entries.push_back(KeybindsKwe::Entry{
            .component = component.uniqueName,
            .action = action.actionUnique,
            .chords = action.active,
        });
      }
    }
  }
  kLog.info("keybinds mirror: writing {} entry(s) ({} custom)", entries.size(), customEntries);
  if (!KeybindsKwe::save(KeybindsKwe::defaultPath(), entries)) {
    kLog.warn("keybinds mirror write failed; binds are live but the mirror is stale");
  }
  return true;
}

void KeybindsPage::commitChord(
    const std::string& component, const std::string& action, std::vector<KeyChord> chords, std::size_t index,
    KeyChord chord, const Callbacks& callbacks
) {
  // Canonicalize through the daemon's integer encoding so staged chords compare
  // equal to what the daemon reports (letters uppercase, consumed Shift gone).
  const auto qt = KeybindCodec::toQtKey(chord);
  const auto canonical = qt.has_value() ? KeybindCodec::fromQtKey(*qt) : std::nullopt;
  if (!canonical.has_value()) {
    kLog.warn("keybinds commit dropped unrepresentable chord for {}.{}", component, action);
    return;
  }
  // Proposed chord list with the new chord in its slot.
  if (index < chords.size()) {
    chords[index] = *canonical;
  } else {
    chords.push_back(*canonical);
  }
  // A chord already bound to another action is a conflict; the action's own
  // current bindings are fine (re-committing a value or moving it between
  // slots of the same action). Because the daemon's globalShortcutsByKey
  // method is unavailable on this fork, conflicts are computed from what the
  // page shows: effectiveChords() so a chord STAGED on another action (but not
  // yet applied) is still reported, instead of only the daemon's on-disk
  // chords.
  const auto conflicts = [&] {
    std::vector<std::pair<std::string, std::string>> owners;
    for (const auto& candidate : m_components) {
      for (const auto& candidateAction : candidate.actions) {
        const auto candidateChords = effectiveChords(candidate.uniqueName, candidateAction.actionUnique);
        if (std::ranges::find(candidateChords, *canonical) != candidateChords.end()) {
          owners.emplace_back(candidate.uniqueName, candidateAction.actionUnique);
        }
      }
    }
    return owners;
  }();
  for (const auto& [ownerComponent, ownerAction] : conflicts) {
    if (ownerComponent == component && ownerAction == action) {
      continue;
    }
    kLog.warn("keybinds commit conflict {}.{} <- {} owned by {}.{}", component, action, KeybindCodec::toChordString(*canonical), ownerComponent, ownerAction);
    // Conflict: hold the edit until the user decides whether to replace the
    // current owner. The row renders a Replace/Cancel prompt while armed.
    m_pendingRebind = PendingRebind{
        .component = component,
        .action = action,
        .ownerComponent = ownerComponent,
        .ownerAction = ownerAction,
        .chord = *canonical,
        .newChords = chords,
        .index = index,
    };
    if (callbacks.requestRebuild) {
      callbacks.requestRebuild();
    }
    return;
  }
  kLog.warn("keybinds commit staged {}.{} -> [{}]", component, action, KeybindCodec::toChordString(*canonical));
  stageAndRebuild(component, action, std::move(chords), callbacks);
}

void KeybindsPage::confirmRebind(const Callbacks& callbacks) {
  if (!m_pendingRebind.has_value()) {
    return;
  }
  PendingRebind pending = std::move(*m_pendingRebind);
  m_pendingRebind.reset();
  // Stage the new chord list for the edited action and an empty list for the
  // previous owner; applyAll() flushes both together.
  stage(pending.component, pending.action, std::move(pending.newChords));
  stage(pending.ownerComponent, pending.ownerAction, {});
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

void KeybindsPage::cancelRebind(const Callbacks& callbacks) {
  if (!m_pendingRebind.has_value()) {
    return;
  }
  m_pendingRebind.reset();
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

void KeybindsPage::removeChord(
    const std::string& component, const std::string& action, std::vector<KeyChord> chords, std::size_t index,
    const Callbacks& callbacks
) {
  if (index >= chords.size()) {
    return;
  }
  chords.erase(chords.begin() + static_cast<std::ptrdiff_t>(index));
  stageAndRebuild(component, action, std::move(chords), callbacks);
}

void KeybindsPage::buildChordRow(
    Flex& parent, std::vector<KeyChord> chords, std::size_t index, const std::string& component,
    const std::string& action, float scale, const Callbacks& callbacks
) {
  // A conflict prompted by this exact row: show Replace/Cancel instead of the
  // recorder so the user decides whether to steal the chord from its owner.
  if (m_pendingRebind.has_value() && m_pendingRebind->component == component
      && m_pendingRebind->action == action && m_pendingRebind->index == index) {
    const auto& pending = *m_pendingRebind;
    std::string ownerActionName = pending.ownerAction;
    std::string ownerComponentName = pending.ownerComponent;
    const auto ownerComponent = std::ranges::find_if(
        m_components, [&pending](const KeybindDbus::Component& c) { return c.uniqueName == pending.ownerComponent; }
    );
    if (ownerComponent != m_components.end()) {
      ownerComponentName = componentDisplayName(*ownerComponent);
      if (const auto* owner = findAction(pending.ownerComponent, pending.ownerAction); owner != nullptr) {
        ownerActionName = displayActionName(*ownerComponent, *owner);
      }
    }
    auto prompt = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    prompt->addChild(makeLabel(
        i18n::tr(
            "settings.keybinds.conflict-prompt", "keybind", KeybindCodec::toChordString(pending.chord),
            "action", ownerActionName, "component", ownerComponentName
        ),
        Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Error)
    ));
    prompt->addChild(ui::spacer());
    prompt->addChild(ui::button({
        .text = i18n::tr("settings.keybinds.conflict-replace"),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Destructive,
        .minHeight = Style::controlHeightSm * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this, callbacks]() { confirmRebind(callbacks); },
    }));
    prompt->addChild(ui::button({
        .text = i18n::tr("settings.keybinds.conflict-cancel"),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Ghost,
        .minHeight = Style::controlHeightSm * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this, callbacks]() { cancelRebind(callbacks); },
    }));
    parent.addChild(std::move(prompt));
    return;
  }

  const bool isAddSlot = index >= chords.size();
  const bool custom = isCustomComponent(component);
  // Custom actions are deleted as a whole; every other bind drops just this
  // chord. The trash sits to the right of the recorder; an unbound custom
  // action still gets one (its only way to delete the action).
  const bool showTrash = !isAddSlot || custom;

  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceXs * scale,
  });
  row->addChild(ui::keybindRecorder({
      .chord = isAddSlot ? std::optional<KeyChord>{} : std::optional<KeyChord>(chords[index]),
      .scale = scale,
      .unsetPlaceholder = i18n::tr("settings.controls.keybind.unset-placeholder"),
      .recordingPlaceholder = i18n::tr("settings.controls.keybind.recording-placeholder"),
      .modifierPolicy = ModifierPolicy::Required,
      .allowSuper = true,
      .escapeCancels = true,
      .width = kChordRecorderWidth * scale,
      .onCommit =
          [this, component, action, chords, index, callbacks](KeyChord chord) {
            commitChord(component, action, chords, index, chord, callbacks);
          },
      .onRecordingChanged = [this, callbacks](bool recording) {
        m_recordingCount += recording ? 1 : -1;
        if (m_recordingCount < 0) {
          m_recordingCount = 0;
        }
        if (callbacks.recordingChanged) {
          callbacks.recordingChanged(recording);
        }
      },
  }));
  if (showTrash) {
    row->addChild(ui::button({
        .glyph = "trash",
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Ghost,
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick =
            [this, component, action, chords, index, callbacks, custom]() {
              if (custom) {
                removeCustomAction(component, action, callbacks);
              } else {
                removeChord(component, action, chords, index, callbacks);
              }
            },
    }));
  }
  parent.addChild(std::move(row));
}

void KeybindsPage::buildChordsColumn(
    Flex& parent, const std::string& component, const std::string& action, std::vector<KeyChord> chords,
    float scale, const Callbacks& callbacks
) {
  // One recorder per bound chord, an unset one when nothing is bound.
  const std::size_t slots = std::max<std::size_t>(1, chords.size());
  for (std::size_t i = 0; i < slots; ++i) {
    buildChordRow(parent, chords, i, component, action, scale, callbacks);
  }
}

void KeybindsPage::buildActionRow(
    Flex& parent, const KeybindDbus::Component& component, const KeybindDbus::Action& action, float scale,
    const Callbacks& callbacks
) {
  const bool isOverridden = overridden(component.uniqueName, action);
  const auto chords = effectiveChords(component.uniqueName, action.actionUnique);

  // One column per action: a title line ([name] ... [Override badge]) and one
  // line per chord ([recorder] [trash]).
  auto column = ui::column({
      .align = FlexAlign::Start,
      .gap = Style::spaceXs * scale,
      .paddingV = 2.0F * scale,
      .fillWidth = true,
  });
  auto titleRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  if (component.uniqueName == CustomKeybinds::kCommandsComponent) {
    // Custom commands are user-owned shell lines: edit the command in place
    // and copy it with one click. The stored definition is the source of truth
    // (its friendly name is what the rest of the page shows); fall back to the
    // daemon's name when the definition is missing.
    const auto* definition = findCustomCommand(action.actionUnique);
    const std::string command = definition != nullptr ? definition->command : displayActionName(component, action);
    Input* commandInput = nullptr;
    titleRow->addChild(ui::input({
        .out = &commandInput,
        .value = command,
        .placeholder = i18n::tr("settings.keybinds.command-placeholder"),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
        .flexGrow = 1.0F,
        .onSubmit =
            [this, actionId = action.actionUnique, callbacks](const std::string& value) {
              updateCustomCommand(actionId, value, callbacks);
            },
        .submitOnFocusLoss = true,
    }));
    titleRow->addChild(ui::button({
        .glyph = "copy",
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("settings.keybinds.copy-command"),
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick =
            [commandInput]() {
              if (commandInput != nullptr) {
                Input::copyToClipboard(commandInput->value());
              }
            },
    }));
  } else {
    titleRow->addChild(makeLabel(
        displayActionName(component, action), Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
    ));
    titleRow->addChild(ui::spacer());
  }
  if (isOverridden) {
    titleRow->addChild(makeOverrideBadge(scale));
  }
  column->addChild(std::move(titleRow));

  buildChordsColumn(*column, component.uniqueName, action.actionUnique, chords, scale, callbacks);
  parent.addChild(std::move(column));
}

bool KeybindsPage::isCustomComponent(std::string_view component) {
  return component == CustomKeybinds::kAppsComponent || component == CustomKeybinds::kCommandsComponent;
}

const CustomKeybinds::Definition* KeybindsPage::findCustomCommand(std::string_view action) const {
  const auto& definitions = m_customKeybinds.definitions();
  const auto it = std::ranges::find_if(definitions, [action](const CustomKeybinds::Definition& definition) {
    return definition.component == CustomKeybinds::kCommandsComponent && definition.action == action;
  });
  return it == definitions.end() ? nullptr : &*it;
}

void KeybindsPage::updateCustomCommand(
    const std::string& action, const std::string& command, const Callbacks& callbacks
) {
  // Persist the edit (re-registering under the new friendly name) and drop the
  // cached component list so the row shows the new text. An empty command is
  // ignored; the rebuild restores the committed value.
  if (!command.empty() && m_customKeybinds.updateCommand(action, command)) {
    m_loaded = false;
  }
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

void KeybindsPage::removeCustomAction(
    const std::string& component, const std::string& action, const Callbacks& callbacks
) {
  // Best effort: unregister from the daemon and drop the definition; failures
  // are logged inside the service. The list is always re-read from the daemon
  // so the UI cannot keep showing an action that is already gone.
  m_customKeybinds.remove(component, action);
  m_pending.erase(keyPath(component, action));
  m_loaded = false;
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

std::unique_ptr<Node> KeybindsPage::buildAddMenu(float scale, const Callbacks& callbacks) {
  auto menu = ui::column({.align = FlexAlign::End, .gap = Style::spaceXs * scale, .fillWidth = true});
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .padding = Style::spaceXs * scale,
      .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.5F),
      .radius = Style::scaledRadiusMd(scale),
      .width = 220.0F * scale,
  });
  const auto menuItem = [scale, callbacks](std::string_view text, auto onPick) {
    return ui::button({
        .text = std::string(text),
        .fontSize = Style::fontSizeCaption * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Ghost,
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = std::move(onPick),
    });
  };
  card->addChild(menuItem(
      i18n::tr("settings.keybinds.add-application"),
      [this, callbacks]() {
        m_addMenuOpen = false;
        m_addMode = AddMode::Apps;
        m_focusAddField = true;
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      }
  ));
  card->addChild(menuItem(
      i18n::tr("settings.keybinds.add-custom-command"),
      [this, callbacks]() {
        m_addMenuOpen = false;
        m_addMode = AddMode::Command;
        m_focusAddField = true;
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      }
  ));
  menu->addChild(std::move(card));
  return menu;
}

std::unique_ptr<Node> KeybindsPage::buildAppPicker(float scale, const Callbacks& callbacks) {
  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  column->addChild(makeLabel(
      i18n::tr("settings.keybinds.app-picker-title"), Style::fontSizeBody * scale,
      colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));

  std::vector<SearchPickerOption> options;
  for (const auto& entry : desktopEntries()) {
    if (entry.exec.empty()) {
      continue;
    }
    options.push_back(SearchPickerOption{
        .value = entry.id,
        .label = entry.name,
        .icon = "app-window",
    });
  }
  auto picker = ui::searchPicker({
      .placeholder = i18n::tr("settings.keybinds.app-picker-placeholder"),
      .emptyText = i18n::tr("settings.keybinds.app-picker-empty"),
      .options = std::move(options),
      .height = 320.0F * scale,
      .onActivated =
          [this, callbacks](const SearchPickerOption& option) {
            const auto& entries = desktopEntries();
            const auto entry = std::ranges::find_if(
                entries, [&option](const DesktopEntry& e) { return e.id == option.value; }
            );
            if (entry == entries.end()) {
              return;
            }
            m_addMode = AddMode::None;
            m_addMenuOpen = false;
            if (m_customKeybinds.addApp(entry->id, entry->name, entry->exec)) {
              m_loaded = false;
              m_expandedGroups.insert(std::string(CustomKeybinds::kAppsComponent));
            }
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
      .onCancel =
          [this, callbacks]() {
            m_addMode = AddMode::None;
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
      .configure =
          [](SearchPicker& picker) {
            picker.clearFill();
            picker.clearBorder();
            picker.setRadius(0.0F);
            picker.setPadding(0.0F);
          },
  });
  if (m_focusAddField) {
    m_pendingFocusArea = picker->filterInputArea();
    m_focusAddField = false;
  }
  column->addChild(std::move(picker));

  column->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::button({
          .text = i18n::tr("settings.keybinds.cancel"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this, callbacks]() {
                m_addMode = AddMode::None;
                if (callbacks.requestRebuild) {
                  callbacks.requestRebuild();
                }
              },
      })
  ));
  return column;
}

std::unique_ptr<Node> KeybindsPage::buildCommandForm(float scale, const Callbacks& callbacks) {
  auto column = ui::column({.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .fillWidth = true});
  column->addChild(makeLabel(
      i18n::tr("settings.keybinds.command-form-title"), Style::fontSizeBody * scale,
      colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));

  auto input = std::make_unique<Input>();
  input->setPlaceholder(i18n::tr("settings.keybinds.command-placeholder"));
  input->setMinLayoutWidth(480.0F * scale);
  if (m_focusAddField) {
    m_pendingFocusArea = input->inputArea();
    m_focusAddField = false;
  }
  Input* inputPtr = input.get();
  input->setOnSubmit(
      [this, inputPtr, callbacks](const std::string& command) {
        if (command.empty()) {
          return;
        }
        m_addMode = AddMode::None;
        m_addMenuOpen = false;
        if (m_customKeybinds.addCommand(command)) {
          m_loaded = false;
          m_expandedGroups.insert(std::string(CustomKeybinds::kCommandsComponent));
        }
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      }
  );
  column->addChild(std::move(input));

  column->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::button({
          .text = i18n::tr("settings.keybinds.add"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this, inputPtr, callbacks]() {
                const std::string command = inputPtr->value();
                if (command.empty()) {
                  return;
                }
                m_addMode = AddMode::None;
                m_addMenuOpen = false;
                if (m_customKeybinds.addCommand(command)) {
                  m_loaded = false;
                  m_expandedGroups.insert(std::string(CustomKeybinds::kCommandsComponent));
                }
                if (callbacks.requestRebuild) {
                  callbacks.requestRebuild();
                }
              },
      }),
      ui::button({
          .text = i18n::tr("settings.keybinds.cancel"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this, callbacks]() {
                m_addMode = AddMode::None;
                if (callbacks.requestRebuild) {
                  callbacks.requestRebuild();
                }
              },
      })
  ));
  return column;
}

std::unique_ptr<Node>
KeybindsPage::build(float scale, const Filter& filter, const Callbacks& callbacks) {
  ensureLoaded();
  // A stale focus request must not survive into a build that does not render the
  // add form (e.g. the user navigated away before it was consumed); the add
  // branches below re-arm it when they build.
  m_pendingFocusArea = nullptr;

  auto root = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceLg * scale,
      .fill = clearColorSpec(),
      .fillWidth = true,
  });

  // Section header: glyph + bold primary title, with the "+" add button on the
  // far right of the title line.
  auto header = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  header->addChild(ui::glyph({
      .glyph = "keyboard",
      .glyphSize = Style::fontSizeHeader * scale,
      .color = colorSpecFromRole(ColorRole::Primary),
  }));
  header->addChild(makeLabel("Kwebinds", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold));
  header->addChild(ui::spacer());
  header->addChild(ui::button({
      .glyph = "plus",
      .fontSize = Style::fontSizeCaption * scale,
      .glyphSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Primary,
      .minWidth = Style::controlHeight * scale,
      .minHeight = Style::controlHeight * scale,
      .padding = Style::spaceXs * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick =
          [this, callbacks]() {
            m_addMenuOpen = !m_addMenuOpen;
            if (m_addMenuOpen) {
              m_addMode = AddMode::None;
            }
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
  }));
  root->addChild(std::move(header));

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

  if (m_addMenuOpen) {
    root->addChild(buildAddMenu(scale, callbacks));
  }

  if (m_components.empty() && m_addMode == AddMode::None) {
    if (!filter.searchQuery.empty()) {
      // No shortcut service means no searchable settings; drop the page from
      // the merged results instead of repeating its error notice everywhere.
      return nullptr;
    }
    root->addChild(makeLabel(
        "The compositor's shortcut service is unavailable.", Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
    return root;
  }

  if (m_addMode == AddMode::Apps) {
    root->addChild(buildAppPicker(scale, callbacks));
    return root;
  }
  if (m_addMode == AddMode::Command) {
    root->addChild(buildCommandForm(scale, callbacks));
    return root;
  }

  // Global search: the normal layout is component-list + selected component's
  // actions, which only surfaces matches in the selected component. Instead
  // scan every component and list the matching actions, grouped by component.
  if (!filter.searchQuery.empty()) {
    bool anyRows = false;
    for (const auto& component : m_components) {
      std::vector<const KeybindDbus::Action*> matching;
      for (const auto& action : component.actions) {
        if (action.actionUnique.starts_with(kSessionActionPrefix)
            || isHiddenAction(component.uniqueName, action.actionUnique)) {
          continue;
        }
        if (matchesFilter(component, action, filter)) {
          matching.push_back(&action);
        }
      }
      if (matching.empty()) {
        continue;
      }
      if (anyRows) {
        root->addChild(ui::separator());
      }
      root->addChild(makeLabel(
          componentDisplayName(component), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold
      ));
      for (const KeybindDbus::Action* action : matching) {
        buildActionRow(*root, component, *action, scale, callbacks);
      }
      anyRows = true;
    }
    if (!anyRows) {
      // Global search drops pages with no hits, so don't render the notice.
      return nullptr;
    }
    return root;
  }

  // One collapsible card per component: the components are the sections, so
  // collapsing folds a whole group of binds. (The search path above stays flat
  // and grouped by component.)
  if (!m_expandedInitialized) {
    m_expandedInitialized = true;
    if (!m_components.empty()) {
      m_expandedGroups.insert(m_components.front().uniqueName);
    }
  }

  bool anyRows = false;
  for (const auto& component : m_components) {
    std::vector<const KeybindDbus::Action*> visible;
    for (const auto& action : component.actions) {
      if (action.actionUnique.starts_with(kSessionActionPrefix)
          || isHiddenAction(component.uniqueName, action.actionUnique)) {
        continue;
      }
      if (matchesFilter(component, action, filter)) {
        visible.push_back(&action);
      }
    }
    if (visible.empty()) {
      continue;
    }
    Flex* body = kinetic_settings::addCollapsibleSection(
        *root, componentDisplayName(component), component.uniqueName, m_expandedGroups, scale, callbacks.jumpRow
    );
    for (const KeybindDbus::Action* action : visible) {
      buildActionRow(*body, component, *action, scale, callbacks);
    }
    anyRows = true;
  }
  if (!anyRows) {
    root->addChild(makeLabel(
        "No keybinds match the current filters.", Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }
  return root;
}
