#pragma once

#include "kinetic-settings/src/compositor_dbus.h"
#include "kinetic-settings/src/kineticwe_kwe.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

class Flex;
class Node;

// The Tiling settings page: edits [Tiling] + [TilingRules] + the per-desktop
// [Tiling][DesktopOutput <n>:<output>] override groups in
// ~/.config/kineticwe/kineticwe.kwe and applies them via the compositor's
// org.kde.KWin reconfigure() (schema mirror of src/kcms/tiling/tilingsettings.kcfg
// and the tiling KCM's per-desktop model).
//
// The page mirrors the Noctalia settings content layout: a glyph + title
// header, group headers, and rows with a bold title, a caption description and
// the control on the right. Rows whose effective value differs from the
// compiled-in default carry an "Override" badge and a per-setting Reset button.
//
// Edits are STAGED, not instant: controls update an in-memory pending map and
// nothing touches the disk (or the compositor) until the window's Apply button
// flushes every staged value with a single file write and one reconfigure() —
// the compositor visibly reloads, so batching keeps one reload per Apply
// instead of one per control change. Closing the window discards staged edits.
// Staging a value equal to the file value simply un-stages it; staged values
// equal to the default end up removing the key when applied.
//
// The module deliberately re-implements these visuals locally instead of
// including Noctalia's settings internals, so upstream merges stay clean.
class TilingPage {
public:
  // Persisted values are typed so override detection compares semantic values,
  // not file text.
  using Value = std::variant<bool, int, double, std::string, std::vector<std::string>>;

  // Filters driven by the window chrome (search field, Advanced toggle,
  // Overridden toggle).
  struct Filter {
    std::string searchQuery;
    bool showAdvanced = false;
    bool showOverriddenOnly = false;
  };

  // Glue back to the owning window. `requestRebuild` must be called after any
  // state change so the page is rebuilt; the window's implementation also
  // clears any armed (one-click-in) reset confirmation. `armReset` /
  // `confirmReset` implement the two-stage per-setting Reset button.
  struct Callbacks {
    std::function<void()> requestRebuild;
    std::function<void(std::string keyPath)> armReset;
    std::function<void(std::string keyPath)> confirmReset;
    // Window-level fixed section jump row; when non-null each collapsible
    // section adds a pill to it. Owned by the window.
    Flex* jumpRow = nullptr;
  };

  explicit TilingPage(class SessionBus& bus);

  // Rebuilds the page node. `armedResetKey` is the "group.key" path whose Reset
  // button was clicked once and is waiting for the confirming second click.
  std::unique_ptr<Node>
  build(float scale, const Filter& filter, std::string_view armedResetKey, const Callbacks& callbacks);

  // "group.key" paths of every resettable setting on the page (Reset Page scope).
  [[nodiscard]] std::vector<std::string> resettableKeyPaths() const;

  // Stages one setting back at its default (nothing is written until Apply).
  void resetKeyPath(std::string_view keyPath);
  // Stages every Tiling setting back at its default (Reset Page), including
  // clearing every per-desktop override group.
  void resetAll();

  // True while unapplied (staged) edits exist — drives the window's Apply button.
  [[nodiscard]] bool hasPending() const noexcept {
    return !m_pending.empty() || !m_pendingDesktopOverrides.empty();
  }
  // Writes every staged edit to the .kwe file and reloads the compositor ONCE.
  // Returns false when the write failed (the staged edits are kept for retry).
  bool applyAll();

  // Connected monitor (output) connector names, e.g. "DP-1"; supplied by the
  // owning window each time the page is built (the compositor's D-Bus only
  // exposes the active output). Drives the per-desktop layout override grid.
  void setConnectedOutputs(std::vector<std::string> outputs) { m_outputNames = std::move(outputs); }

  // Force the next build() to re-read the config file from disk, discarding
  // any staged (unapplied) edits.
  void reloadFromDisk() noexcept {
    m_loaded = false;
    m_pending.clear();
    m_pendingDesktopOverrides.clear();
    m_detectNotice.clear();
  }

private:
  enum class ControlKind {
    Toggle,
    Select,
    Slider,
    Text,
    // The EnabledLayouts StringList rendered as one toggle per layout.
    LayoutToggles,
    // The EmergeAppWidths StringList rendered as per-app rows with Detect.
    EmergeAppWidths,
  };

  struct Field {
    std::string_view group;      // .kwe group: "Tiling" / "TilingRules"
    std::string_view key;        // .kwe key
    ControlKind control = ControlKind::Toggle;
    std::string_view title;
    std::string_view subtitle;
    std::string_view pageGroup;  // group header shown on the page
    bool advanced = false;
    double minValue = 0.0;       // sliders
    double maxValue = 1.0;
    double step = 1.0;
    bool integerValue = false;
    std::string_view valueSuffix;
    const std::vector<std::string>* optionValues = nullptr; // selects: values stored in the file
    const std::vector<std::string>* optionLabels = nullptr; // selects: labels shown
    std::string_view placeholder;                           // text inputs
    float controlWidth = 0.0F;                              // text inputs (0 = default 190)
    Value fallback;                                         // compiled-in default (override detection)
  };

  static const std::vector<Field>& fields();
  static const std::vector<std::string>& layoutKindValues();
  static const std::vector<std::string>& layoutKindLabels();
  static const std::vector<std::string>& layoutKindSubtitles();
  static const std::vector<std::string>& borderModeValues();
  static const std::vector<std::string>& borderModeLabels();
  static const std::vector<std::string>& colorSourceActiveValues();
  static const std::vector<std::string>& colorSourceActiveLabels();
  static const std::vector<std::string>& colorSourceInactiveValues();
  static const std::vector<std::string>& colorSourceInactiveLabels();
  static const std::vector<std::string>& emergeAxisValues();
  static const std::vector<std::string>& emergeAxisLabels();
  [[nodiscard]] static Value defaultValue(const Field& field);
  [[nodiscard]] static std::string keyPath(const Field& field);
  [[nodiscard]] static const Field* findField(std::string_view keyPath);
  // Typed value equality with per-key normalization (custom colors compare
  // case-insensitively); equalsDefault() is valuesEqual() against the fallback.
  [[nodiscard]] static bool valuesEqual(const Field& field, const Value& a, const Value& b);
  [[nodiscard]] static bool equalsDefault(const Field& field, const Value& value);

  void ensureLoaded();
  // Value read from the config file (ignoring staged edits).
  [[nodiscard]] Value fileBackedValue(const Field& field) const;
  // What the UI shows: the staged value when present, otherwise the file value.
  [[nodiscard]] Value effectiveValue(const Field& field) const;
  [[nodiscard]] bool overridden(const Field& field) const;
  [[nodiscard]] bool matchesFilter(const Field& field, const Filter& filter) const;

  // Writes `value` into the config model (or removes the key when it equals
  // the default). No save / no compositor reload — applyAll() owns those.
  void writeValue(const Field& field, const Value& value);
  // Stage an edit; un-stages when it matches the value already in the file.
  void stage(const Field& field, const Value& value);
  void stageAndRebuild(const Field& field, const Value& value, const Callbacks& callbacks);

  [[nodiscard]] std::unique_ptr<Node>
  buildControl(const Field& field, float scale, const Callbacks& callbacks);
  [[nodiscard]] std::unique_ptr<Node>
  buildLayoutTogglesBlock(const Field& field, float scale, std::string_view armedResetKey, const Callbacks& callbacks);
  [[nodiscard]] std::unique_ptr<Node>
  buildEmergeAppWidthsBlock(
      const Field& field, float scale, std::string_view armedResetKey, const Callbacks& callbacks
  );
  void buildRuntimeGroup(Flex& parent, float scale, const Callbacks& callbacks);

  // Per-monitor gap overrides ([Tiling][Output <active>]). The compositor lets
  // these shadow the [Tiling] defaults this page edits
  // (TilingController::applyGapSettingsToOutput). The page edits defaults
  // only, so shadowing overrides — left behind by the tiling KCM or a legacy
  // kwinrc migration — are surfaced as a notice above the Gaps section with a
  // one-click Clear that removes the shadowing keys and reloads the
  // compositor.
  // Flat-INI group name of the active output's override section
  // ("Tiling][Output DP-1"), empty when the output can't be determined.
  [[nodiscard]] std::string monitorGapOverrideGroup();
  // "Key=value" list of the gap keys the active monitor overrides; empty when
  // none are overridden (also the notice's show/hide predicate).
  [[nodiscard]] std::string monitorGapOverrideSummary();
  void clearMonitorGapOverrides();
  void addMonitorGapOverrideNotice(Flex& parent, float scale, const Callbacks& callbacks);
  void addGapToggleActionRow(Flex& parent, float scale, const Callbacks& callbacks);
  void addSettingRow(
      Flex& parent, const Field& field, float scale, std::string_view armedResetKey, const Callbacks& callbacks
  );

  // Per-(desktop, monitor) layout overrides: [Tiling][DesktopOutput <n>:<out>]
  // groups with a DefaultLayout key (absent/empty = "use default"), resolved by
  // TilingController::resolveLayoutKind before the per-output and global
  // defaults. Desktop numbers are 1-based positions (matching the KCM, which
  // numbers by position), so removing a workspace renumbers the overrides.
  // Override-row reset paths use the "desktopOverride:<group>" scheme.
  static constexpr std::string_view kDesktopOverridePathPrefix = "desktopOverride:";
  [[nodiscard]] static std::string desktopOverrideGroup(std::size_t desktopNumber, std::string_view outputName);
  // Effective override for `group`: staged value, else file value; empty means
  // "use default" (no override).
  [[nodiscard]] std::string desktopOverrideValue(std::string_view group) const;
  void stageDesktopOverride(const std::string& group, std::string value);
  // Flat group names of every override group currently present in the config
  // file (prefix "Tiling][DesktopOutput "), for Reset Page cleanup.
  [[nodiscard]] std::vector<std::string> existingDesktopOverrideGroups() const;
  // Whether one (desktop, monitor) override row passes the filter; search
  // matches workspace/monitor names and the section keywords.
  [[nodiscard]] bool desktopOverrideRowMatches(
      const Filter& filter, std::string_view desktopName, std::string_view outputName, bool overridden
  ) const;
  // Builds the per-desktop override rows (caption + workspace/monitor rows) as a
  // standalone node so the caller can place it inside a collapsible section card;
  // returns nullptr when there is nothing to show.
  [[nodiscard]] std::unique_ptr<Node> buildDesktopOverridesContent(
      float scale, const Filter& filter, std::string_view armedResetKey, const Callbacks& callbacks
  );

  SessionBus& m_bus;
  CompositorDbus m_dbus;
  KineticweKwe m_config;
  bool m_loaded = false;
  // Edits staged by the controls, keyed by "group.key"; flushed by applyAll().
  std::map<std::string, Value> m_pending;
  // Per-desktop override edits staged by the override grid, keyed by the flat
  // group name ("Tiling][DesktopOutput 1:DP-1"); "" stages a removal.
  std::map<std::string, std::string> m_pendingDesktopOverrides;
  // Connected monitor connector names, refreshed by the window before build().
  std::vector<std::string> m_outputNames;
  // One-shot notice from the last window-detect attempt on the Emerge app widths
  // block (cancelled pick, or a picked window without a class).
  std::string m_detectNotice;
  // Page-group keys whose collapsible card is expanded. Owned here so the state
  // survives the window's per-change scene rebuilds; the first group is
  // expanded on the first non-search build.
  std::unordered_set<std::string> m_expandedGroups;
  bool m_expandedInitialized = false;
};
