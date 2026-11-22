#include "tiling_page.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "kinetic-settings/src/section_card.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <ranges>
#include <utility>

namespace {

  constexpr Logger kLog("kinetic-settings");

  // Gap keys the compositor lets a [Tiling][Output <name>] section shadow
  // (TilingController::applyGapSettingsToOutput).
  constexpr std::array<std::string_view, 5> kGapOverrideKeys{
      "GapBetween", "GapLeft", "GapRight", "GapTop", "GapBottom",
  };

  // Matches upstream's settings description wrapping budget.
  constexpr int kSettingDescriptionMaxLines = 5;
  // Matches the control widths used by Noctalia's SettingsControlFactory.
  constexpr float kDefaultControlWidth = 190.0F;
  constexpr float kSliderValueInputWidth = 50.0F;
  constexpr float kSuffixSlotWidth = 36.0F;

  // Markup-free label helper mirroring settings::makeLabel.
  [[nodiscard]] std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal) {
    return ui::label({
        .text = std::string(text),
        .fontSize = fontSize,
        .fontWeight = fontWeight,
        .color = color,
    });
  }

  // Status pill ("Override" / "Advanced"); a module-local re-implementation of
  // SettingsControlFactory::makeStatusBadge so visuals match without depending
  // on Noctalia's settings internals.
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

  [[nodiscard]] std::unique_ptr<Flex> makeAdvancedBadge(float scale) {
    return makeStatusBadge(
        i18n::tr("settings.badges.advanced"), colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.12F),
        colorSpecFromRole(ColorRole::OnSurfaceVariant), scale
    );
  }

  [[nodiscard]] std::string formatSliderValue(double value, bool integerValue) {
    if (integerValue) {
      return std::format("{}", static_cast<int>(std::llround(value)));
    }
    return StringUtils::formatFixedDotDecimal(value, 2);
  }

  // Accepts the QColor text forms KConfig round-trips: #rgb, #rrggbb, #aarrggbb.
  [[nodiscard]] bool isHexColorText(std::string_view text) {
    const auto trimmed = StringUtils::trim(text);
    if (trimmed.size() != 4 && trimmed.size() != 7 && trimmed.size() != 9) {
      return false;
    }
    return trimmed.front() == '#'
           && std::ranges::all_of(trimmed.substr(1), [](unsigned char c) { return std::isxdigit(c) != 0; });
  }

  [[nodiscard]] std::string normalizedColorText(std::string_view text) {
    return StringUtils::toLower(StringUtils::trim(text));
  }

  [[nodiscard]] std::vector<std::string> splitListText(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& item : StringUtils::split(text, ';')) {
      std::string trimmed = StringUtils::trim(item);
      if (!trimmed.empty()) {
        out.push_back(std::move(trimmed));
      }
    }
    return out;
  }

  [[nodiscard]] std::string joinListText(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
      if (!out.empty()) {
        out.push_back(';');
      }
      out += item;
    }
    return out;
  }

  [[nodiscard]] std::string joinKeyPath(std::string_view group, std::string_view key) {
    return std::string(group) + '.' + std::string(key);
  }

} // namespace

// ---------------------------------------------------------------------------
// Schema: option catalogs + the field descriptor table — the single source of
// truth for file keys, defaults, ranges and presentation. Defaults mirror
// src/kcms/tiling/tilingsettings.kcfg; enum settings store their kcfg choice
// names as strings, which is what the compositor parses natively. (The active
// custom border color's true runtime default is the system palette's highlight
// color; the UI treats the kcfg's #3daee6 as the default.)
// ---------------------------------------------------------------------------

TilingPage::TilingPage(SessionBus& bus) : m_bus(bus), m_dbus(bus) {}


const std::vector<std::string>& TilingPage::layoutKindValues() {
  static const std::vector<std::string> values{
      "MasterStack", "Stacked", "CenterTile", "Columns", "Monocle", "AutoGrid", "Emerge",
  };
  return values;
}

const std::vector<std::string>& TilingPage::layoutKindLabels() {
  static const std::vector<std::string> labels{
      "MasterStack", "Stacked", "Center Tile", "Columns", "Monocle", "Auto Grid", "Emerge",
  };
  return labels;
}

const std::vector<std::string>& TilingPage::layoutKindSubtitles() {
  static const std::vector<std::string> subtitles{
      "One large primary area with the remaining windows stacked beside it",
      "Every window gets an equal slot in a single stack",
      "The focused window stays centered while others tile around the edges",
      "Windows are laid out side by side in a bounded number of columns",
      "One window fills the screen at a time; switch with shortcuts",
      "Windows are arranged in an even grid automatically",
      "Windows form an endless strip that rises into view; open a window, it never resizes others",
  };
  return subtitles;
}

const std::vector<std::string>& TilingPage::borderModeValues() {
  static const std::vector<std::string> values{"None", "AllTiled", "ActiveOnly", "AllWindows"};
  return values;
}

const std::vector<std::string>& TilingPage::borderModeLabels() {
  static const std::vector<std::string> labels{"None", "All tiled windows", "Active window only", "All windows"};
  return labels;
}

const std::vector<std::string>& TilingPage::colorSourceActiveValues() {
  static const std::vector<std::string> values{"Custom", "SystemAccent"};
  return values;
}

const std::vector<std::string>& TilingPage::colorSourceActiveLabels() {
  static const std::vector<std::string> labels{"Custom", "System accent"};
  return labels;
}

const std::vector<std::string>& TilingPage::colorSourceInactiveValues() {
  static const std::vector<std::string> values{
      "Custom", "SystemAccent", "SystemAccentFaded",
  };
  return values;
}

const std::vector<std::string>& TilingPage::colorSourceInactiveLabels() {
  static const std::vector<std::string> labels{
      "Custom", "System accent", "System accent (faded)",
  };
  return labels;
}

const std::vector<std::string>& TilingPage::emergeAxisValues() {
  static const std::vector<std::string> values{"Auto", "Horizontal", "Vertical"};
  return values;
}

const std::vector<std::string>& TilingPage::emergeAxisLabels() {
  static const std::vector<std::string> labels{
      "Auto (from monitor shape)", "Horizontal", "Vertical",
  };
  return labels;
}


const std::vector<TilingPage::Field>& TilingPage::fields() {
  static const std::vector<Field> table = [] {
    std::vector<Field> f;
    // General ----------------------------------------------------------------
    f.push_back(Field{
        .group = "Tiling",
        .key = "Enabled",
        .control = ControlKind::Toggle,
        .title = "Enable tiling",
        .subtitle = "Whether native tiling is enabled",
        .pageGroup = "General",
        .fallback = Value{true},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "DefaultLayout",
        .control = ControlKind::Select,
        .title = "Default layout",
        .subtitle = "Default layout used for new (output, desktop) pairs that do "
                    "not have a per-monitor override",
        .pageGroup = "General",
        .optionValues = &layoutKindValues(),
        .optionLabels = &layoutKindLabels(),
        .fallback = Value{std::string("MasterStack")},
    });
    // Layouts ----------------------------------------------------------------
    f.push_back(Field{
        .group = "Tiling",
        .key = "EnabledLayouts",
        .control = ControlKind::LayoutToggles,
        .title = "Enabled layouts",
        .subtitle = "Layouts available for cycling and for the default layout pick",
        .pageGroup = "Layouts",
        .fallback = Value{layoutKindValues()},
    });
    // Gaps -------------------------------------------------------------------
    for (const auto& [key, title, subtitle] : std::initializer_list<
             std::tuple<std::string_view, std::string_view, std::string_view>>{
             {"GapLeft", "Gap left", "Gap between the left screen edge and tiles"},
             {"GapRight", "Gap right", "Gap between the right screen edge and tiles"},
             {"GapTop", "Gap top", "Gap between the top screen edge and tiles"},
             {"GapBottom", "Gap bottom", "Gap between the bottom screen edge and tiles"},
         }) {
      f.push_back(Field{
          .group = "Tiling",
          .key = key,
          .control = ControlKind::Slider,
          .title = title,
          .subtitle = subtitle,
          .pageGroup = "Gaps",
          .advanced = true,
          .maxValue = 1000.0,
          .integerValue = true,
          .fallback = Value{10},
      });
    }
    f.push_back(Field{
        .group = "Tiling",
        .key = "GapBetween",
        .control = ControlKind::Slider,
        .title = "Gap between tiles",
        .subtitle = "Gap between adjacent tiles",
        .pageGroup = "Gaps",
        .maxValue = 30.0,
        .integerValue = true,
        .fallback = Value{10},
    });

    // Gap Toggle ---------------------------------------------------------------
    // The two values the "Tiling Toggle Outer Gaps" shortcut (and the Toggle
    // now button) flips the active monitor's outer left/right gaps between.
    f.push_back(Field{
        .group = "Tiling",
        .key = "GapToggleWide",
        .control = ControlKind::Slider,
        .title = "Full-width gap",
        .subtitle = "Outer gap used when the tiling area should span the full "
                    "width of the screen",
        .pageGroup = "Gap Toggle",
        .maxValue = 1000.0,
        .integerValue = true,
        .fallback = Value{15},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "GapToggleCentered",
        .control = ControlKind::Slider,
        .title = "Centered gap",
        .subtitle = "Outer gap used when the tiling area should be centered "
                    "(e.g. an ultrawide workflow with a narrow working column)",
        .pageGroup = "Gap Toggle",
        .maxValue = 1000.0,
        .integerValue = true,
        .fallback = Value{390},
    });

    // Borders ----------------------------------------------------------------
    f.push_back(Field{
        .group = "Tiling",
        .key = "TilingBorderMode",
        .control = ControlKind::Select,
        .title = "Border mode",
        .subtitle = "Border display mode for windows",
        .pageGroup = "Borders",
        .optionValues = &borderModeValues(),
        .optionLabels = &borderModeLabels(),
        .fallback = Value{std::string("None")},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "TilingBorderThickness",
        .control = ControlKind::Slider,
        .title = "Border thickness",
        .subtitle = "Thickness of the tiling border",
        .pageGroup = "Borders",
        .maxValue = 20.0,
        .integerValue = true,
        .fallback = Value{2},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "TilingBorderColorSourceActive",
        .control = ControlKind::Select,
        .title = "Active border color source",
        .subtitle = "Source for the active border color",
        .pageGroup = "Borders",
        .optionValues = &colorSourceActiveValues(),
        .optionLabels = &colorSourceActiveLabels(),
        .fallback = Value{std::string("SystemAccent")},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "TilingBorderColorActive",
        .control = ControlKind::Text,
        .title = "Custom active border color",
        .subtitle = "Color of the border around the active tiled window, used "
                    "when the active color source is Custom (#rrggbb)",
        .pageGroup = "Borders",
        .placeholder = "#3daee6",
        .fallback = Value{std::string("#3daee6")},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "TilingBorderColorSourceInactive",
        .control = ControlKind::Select,
        .title = "Inactive border color source",
        .subtitle = "Source for the inactive border color",
        .pageGroup = "Borders",
        .optionValues = &colorSourceInactiveValues(),
        .optionLabels = &colorSourceInactiveLabels(),
        .fallback = Value{std::string("SystemAccentFaded")},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "TilingBorderColorInactive",
        .control = ControlKind::Text,
        .title = "Custom inactive border color",
        .subtitle = "Color of the border around inactive tiled windows, used "
                    "when the inactive color source is Custom (#aarrggbb or #rrggbb)",
        .pageGroup = "Borders",
        .placeholder = "#c8808080",
        .fallback = Value{std::string("#c8808080")},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "TilingCornerRadius",
        .control = ControlKind::Slider,
        .title = "Corner radius",
        .subtitle = "Corner radius applied to tiled windows (in pixels)",
        .pageGroup = "Borders",
        .advanced = true,
        .maxValue = 50.0,
        .integerValue = true,
        .fallback = Value{0},
    });

    // Behavior ---------------------------------------------------------------
    f.push_back(Field{
        .group = "Tiling",
        .key = "ResizeStep",
        .control = ControlKind::Slider,
        .title = "Resize step",
        .subtitle = "How much each Grow/Shrink shortcut press changes a tiled "
                    "window's share of its column, or the column divider in a "
                    "master/stack layout (0.01 to 1.0)",
        .pageGroup = "Behavior",
        .advanced = true,
        .minValue = 0.01,
        .maxValue = 1.0,
        .step = 0.01,
        .fallback = Value{0.05},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "ColumnsMaxColumns",
        .control = ControlKind::Slider,
        .title = "Columns layout: max columns",
        .subtitle = "Maximum number of columns the Columns layout will create "
                    "before stacking windows vertically within a column",
        .pageGroup = "Behavior",
        .minValue = 3.0,
        .maxValue = 5.0,
        .integerValue = true,
        .fallback = Value{3},
    });

    // Emerge ---------------------------------------------------------------
    f.push_back(Field{
        .group = "Tiling",
        .key = "EmergeAxis",
        .control = ControlKind::Select,
        .title = "Emerge: strip direction",
        .subtitle = "Direction the Emerge layout's strip runs; Auto follows the monitor's shape",
        .pageGroup = "Emerge",
        .optionValues = &emergeAxisValues(),
        .optionLabels = &emergeAxisLabels(),
        .fallback = Value{std::string("Auto")},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "EmergePresetColumnWidths",
        .control = ControlKind::Text,
        .title = "Emerge: column width presets",
        .subtitle = "Fractions of the work area the column-width preset shortcut cycles through, "
                    "separated by ; (e.g. 0.333;0.5;0.667;0.75;1)",
        .pageGroup = "Emerge",
        .placeholder = "0.333;0.5;0.667;0.75;1",
        .controlWidth = 260.0F,
        .fallback = Value{std::vector<std::string>{"0.333", "0.5", "0.667", "0.75", "1"}},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "EmergePresetWindowHeights",
        .control = ControlKind::Text,
        .title = "Emerge: window height presets",
        .subtitle = "Fractions of the work area used for stacked-tile heights, separated by ; "
                    "(e.g. 0.333;0.5;0.667;0.75;1)",
        .pageGroup = "Emerge",
        .placeholder = "0.333;0.5;0.667;0.75;1",
        .controlWidth = 260.0F,
        .fallback = Value{std::vector<std::string>{"0.333", "0.5", "0.667", "0.75", "1"}},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "EmergePeek",
        .control = ControlKind::Toggle,
        .title = "Emerge: peek off-screen windows",
        .subtitle = "Show a sliver of the columns scrolled off the strip in a band at the bottom "
                    "instead of hiding them; suppressed when a monitor sits directly below",
        .pageGroup = "Emerge",
        .fallback = Value{false},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "EmergePeekHeight",
        .control = ControlKind::Slider,
        .title = "Emerge: peek band height",
        .subtitle = "Height of the bottom peek band in pixels",
        .pageGroup = "Emerge",
        .maxValue = 1000.0,
        .integerValue = true,
        .valueSuffix = " px",
        .fallback = Value{48},
    });
    f.push_back(Field{
        .group = "Tiling",
        .key = "EmergeAppWidths",
        .control = ControlKind::EmergeAppWidths,
        .title = "Emerge: app column widths",
        .subtitle = "Open a matching app at a fixed column width. The window class is matched exactly "
                    "(case-insensitive); applies to open and newly opened windows, and a manual resize "
                    "overrides it.",
        .pageGroup = "Emerge",
        .fallback = Value{std::vector<std::string>{}},
    });
    // Window Rules -------------------------------------------------------------
    f.push_back(Field{
        .group = "TilingRules",
        .key = "FloatingClass",
        .control = ControlKind::Text,
        .title = "Always float (class patterns)",
        .subtitle = "Window class patterns that should float by default "
                    "(separated by ;)",
        .pageGroup = "Window Rules",
        .advanced = true,
        .placeholder = "spotify;firefox;org.kde.*",
        .controlWidth = 260.0F,
        .fallback = Value{std::vector<std::string>{}},
    });
    f.push_back(Field{
        .group = "TilingRules",
        .key = "IgnoreClass",
        .control = ControlKind::Text,
        .title = "Never tile (class patterns)",
        .subtitle = "Window class patterns that should be ignored by tiling "
                    "(separated by ;)",
        .pageGroup = "Window Rules",
        .advanced = true,
        .placeholder = "mpv;vlc",
        .controlWidth = 260.0F,
        .fallback = Value{std::vector<std::string>{}},
    });
    for (const auto& [key, title, subtitle] : std::initializer_list<
             std::tuple<std::string_view, std::string_view, std::string_view>>{
             {"FloatUtility", "Float utility windows", "Whether utility windows should float"},
             {"FloatDialog", "Float dialogs", "Whether dialog windows should float"},
             {"FloatTransient", "Float transient windows", "Whether transient windows should float"},
         }) {
      f.push_back(Field{
          .group = "TilingRules",
          .key = key,
          .control = ControlKind::Toggle,
          .title = title,
          .subtitle = subtitle,
          .pageGroup = "Window Rules",
          .fallback = Value{true},
      });
    }
    return f;
  }();
  return table;
}

// ---------------------------------------------------------------------------
// Values: defaults, effective (config-backed) state, override detection.
// ---------------------------------------------------------------------------

TilingPage::Value TilingPage::defaultValue(const Field& field) {
  return field.fallback;
}

std::string TilingPage::keyPath(const Field& field) {
  return joinKeyPath(field.group, field.key);
}

const TilingPage::Field* TilingPage::findField(std::string_view path) {
  for (const auto& field : fields()) {
    if (path == keyPath(field)) {
      return &field;
    }
  }
  return nullptr;
}

void TilingPage::ensureLoaded() {
  if (!m_loaded) {
    m_config.load();
    m_loaded = true;
  }
}


TilingPage::Value TilingPage::fileBackedValue(const Field& field) const {
  const Value fallback = defaultValue(field);
  return std::visit(
      [this, &field](const auto& fallbackValue) -> Value {
        using T = std::decay_t<decltype(fallbackValue)>;
        if constexpr (std::is_same_v<T, bool>) {
          return m_config.readBool(field.group, field.key, fallbackValue);
        } else if constexpr (std::is_same_v<T, int>) {
          const auto parsed = m_config.readInt(field.group, field.key, fallbackValue);
          return std::clamp(parsed, static_cast<int>(field.minValue), static_cast<int>(field.maxValue));
        } else if constexpr (std::is_same_v<T, double>) {
          const auto parsed = m_config.readDouble(field.group, field.key, fallbackValue);
          return std::clamp(parsed, field.minValue, field.maxValue);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return m_config.readString(field.group, field.key).value_or(fallbackValue);
        } else {
          // StringLists: a missing key (or an explicitly empty one) means default.
          const auto values = m_config.readStringList(field.group, field.key);
          if (!m_config.hasKey(field.group, field.key) || values.empty()) {
            return fallbackValue;
          }
          return values;
        }
      },
      fallback
  );
}

TilingPage::Value TilingPage::effectiveValue(const Field& field) const {
  const auto staged = m_pending.find(keyPath(field));
  if (staged != m_pending.end()) {
    return staged->second;
  }
  return fileBackedValue(field);
}

bool TilingPage::equalsDefault(const Field& field, const Value& value) {
  return valuesEqual(field, value, defaultValue(field));
}

bool TilingPage::valuesEqual(const Field& field, const Value& a, const Value& b) {
  if (a.index() != b.index()) {
    return false;
  }
  if (const auto* text = std::get_if<std::string>(&a)) {
    const bool colorKey = field.key == "TilingBorderColorActive" || field.key == "TilingBorderColorInactive";
    if (colorKey) {
      return normalizedColorText(*text) == normalizedColorText(std::get<std::string>(b));
    }
  }
  return a == b;
}

bool TilingPage::overridden(const Field& field) const {
  return !equalsDefault(field, effectiveValue(field));
}

bool TilingPage::matchesFilter(const Field& field, const Filter& filter) const {
  if (!filter.showAdvanced && field.advanced) {
    return false;
  }
  if (filter.showOverriddenOnly && equalsDefault(field, effectiveValue(field))) {
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

// ---------------------------------------------------------------------------
// Persistence: staged edits + Apply. Values set back to the default remove the
// key once applied, mirroring Noctalia's setOverride/clearOverride behavior.
// ---------------------------------------------------------------------------

void TilingPage::writeValue(const Field& field, const Value& value) {
  if (equalsDefault(field, value)) {
    m_config.removeKey(field.group, field.key);
  } else {
    std::visit(
        [this, &field](const auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, bool>) {
            m_config.setBool(field.group, field.key, v);
          } else if constexpr (std::is_same_v<T, int>) {
            m_config.setInt(field.group, field.key, v);
          } else if constexpr (std::is_same_v<T, double>) {
            m_config.setDouble(field.group, field.key, v);
          } else if constexpr (std::is_same_v<T, std::string>) {
            m_config.setString(field.group, field.key, v);
          } else {
            m_config.setStringList(field.group, field.key, v);
          }
        },
        value
    );
  }

}

void TilingPage::stage(const Field& field, const Value& value) {
  ensureLoaded();
  const std::string path = keyPath(field);
  // Editing back to the on-disk value un-stages (the row stops being dirty).
  if (valuesEqual(field, value, fileBackedValue(field))) {
    m_pending.erase(path);
  } else {
    m_pending[path] = value;
  }
}

void TilingPage::stageAndRebuild(const Field& field, const Value& value, const Callbacks& callbacks) {
  stage(field, value);
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

bool TilingPage::applyAll() {
  if (m_pending.empty() && m_pendingDesktopOverrides.empty()) {
    return true;
  }
  ensureLoaded();
  for (const auto& [path, value] : m_pending) {
    if (const Field* field = findField(path)) {
      writeValue(*field, value);
    }
  }
  // Per-desktop layout overrides land in the same file + same reload. "Float
  // only" sets the FloatOnly key (leaving any DefaultLayout choice intact so
  // toggling back on restores it); an empty value removes both keys.
  for (const auto& [group, layout] : m_pendingDesktopOverrides) {
    if (layout == "FloatOnly") {
      m_config.setBool(group, "FloatOnly", true);
    } else if (layout.empty()) {
      m_config.removeKey(group, "DefaultLayout");
      m_config.removeKey(group, "FloatOnly");
    } else {
      m_config.setString(group, "DefaultLayout", layout);
      m_config.removeKey(group, "FloatOnly");
    }
  }
  if (!m_config.save()) {
    kLog.warn("kinetic-settings: failed to write {}", KineticweKwe::defaultPath().string());
    return false; // keep the staged edits so the user can retry Apply
  }
  m_pending.clear();
  m_pendingDesktopOverrides.clear();
  m_dbus.requestReconfigure();
  return true;
}

std::vector<std::string> TilingPage::resettableKeyPaths() const {
  std::vector<std::string> paths;
  paths.reserve(fields().size());
  for (const auto& field : fields()) {
    paths.push_back(keyPath(field));
  }
  return paths;
}

void TilingPage::resetKeyPath(std::string_view path) {
  if (path.starts_with(kDesktopOverridePathPrefix)) {
    // Per-desktop override rows reset back to "use default" (stage removal).
    stageDesktopOverride(std::string(path.substr(kDesktopOverridePathPrefix.size())), "");
    return;
  }
  if (const Field* field = findField(path)) {
    stage(*field, defaultValue(*field));
  }
}

void TilingPage::resetAll() {
  for (const auto& field : fields()) {
    stage(field, defaultValue(field));
  }
  // Clear every per-desktop override group too (KDE's per-desktop model removes
  // the DesktopOutput groups on Defaults). Stage a removal for each existing
  // override; Apply writes it out.
  ensureLoaded();
  m_pendingDesktopOverrides.clear();
  for (const auto& group : existingDesktopOverrideGroups()) {
    m_pendingDesktopOverrides[group] = {};
  }
}


// ---------------------------------------------------------------------------
// Controls (Noctalia settings visuals, re-implemented module-locally).
// ---------------------------------------------------------------------------

namespace {

  // The two-stage Reset action shown next to overridden settings: the first
  // click arms it, the second confirms (upstream makeGroupedResetButton).
  [[nodiscard]] std::unique_ptr<Button> makeResetButton(
      std::string path, bool pendingConfirmation, float scale, const TilingPage::Callbacks& callbacks
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
TilingPage::buildControl(const Field& field, float scale, const Callbacks& callbacks) {
  switch (field.control) {
  case ControlKind::Toggle: {
    return ui::toggle({
        .checked = std::get<bool>(effectiveValue(field)),
        .scale = scale,
        .onChange = [this, field, callbacks](bool value) { stageAndRebuild(field, Value{value}, callbacks); },
    });
  }
  case ControlKind::Select: {
    const auto& values = *field.optionValues;
    const auto& labels = *field.optionLabels;
    const auto selected = std::ranges::find(values, std::get<std::string>(effectiveValue(field)));
    const bool known = selected != values.end();
    const std::optional<std::size_t> selectedIndex =
        known ? std::optional<std::size_t>{static_cast<std::size_t>(selected - values.begin())} : std::nullopt;
    return ui::select({
        .options = labels,
        .selectedIndex = selectedIndex,
        .clearSelection = !known,
        .placeholder = known ? std::optional<std::string>{std::nullopt}
                             : std::optional<std::string>{"Unknown value: "
                                                          + std::get<std::string>(effectiveValue(field))},
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .width = kDefaultControlWidth * scale,
        .height = Style::controlHeight * scale,
        .onSelectionChanged = [this, field, callbacks, values](std::size_t index, std::string_view /*label*/) {
          if (index < values.size()) {
            stageAndRebuild(field, Value{values[index]}, callbacks);
          }
        },
    });
  }

  case ControlKind::Slider: {
    const double value = field.integerValue ? static_cast<double>(std::get<int>(effectiveValue(field)))
                                            : std::get<double>(effectiveValue(field));
    auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

    Input* valueInputPtr = nullptr;
    auto valueInput = ui::input({
        .out = &valueInputPtr,
        .value = formatSliderValue(value, field.integerValue),
        .fontSize = Style::fontSizeCaption * scale,
        .controlHeight = Style::controlHeightSm * scale,
        .horizontalPadding = Style::spaceXs * scale,
        .width = kSliderValueInputWidth * scale,
        .height = Style::controlHeightSm * scale,
    });

    Slider* sliderPtr = nullptr;
    auto slider = ui::slider({
        .out = &sliderPtr,
        .minValue = field.minValue,
        .maxValue = field.maxValue,
        .step = field.step,
        .value = value,
        .width = Style::sliderDefaultWidth * scale,
        .onValueChanged = [valueInputPtr, integerValue = field.integerValue](double v) {
          valueInputPtr->setInvalid(false);
          valueInputPtr->setValue(formatSliderValue(v, integerValue));
        },
    });
    // Wire drag-end AFTER construction: a lambda in the props struct would
    // capture sliderPtr while it is still null (out is assigned by the builder).
    slider->setOnDragEnd([this, field, callbacks, sliderPtr]() {
      const double v = sliderPtr->value();
      stageAndRebuild(
          field, field.integerValue ? Value{static_cast<int>(std::llround(v))} : Value{v}, callbacks
      );
    });

    const auto commitInputText = [this, field, callbacks, sliderPtr, valueInputPtr](const std::string& text) {
      const auto parsed = StringUtils::parseDotDecimal<double>(text);
      if (!parsed.has_value() || *parsed < field.minValue || *parsed > field.maxValue) {
        valueInputPtr->setInvalid(true);
        return false;
      }
      valueInputPtr->setInvalid(false);
      sliderPtr->setValue(*parsed);
      const double snapped = sliderPtr->value();
      valueInputPtr->setValue(formatSliderValue(snapped, field.integerValue));
      stageAndRebuild(
          field, field.integerValue ? Value{static_cast<int>(std::llround(snapped))} : Value{snapped}, callbacks
      );
      return true;
    };

    valueInput->setOnChange([valueInputPtr](const std::string& /*text*/) { valueInputPtr->setInvalid(false); });
    valueInput->setOnSubmit([commitInputText](const std::string& text) { (void)commitInputText(text); });
    valueInput->setOnFocusLoss([commitInputText, valueInputPtr]() { (void)commitInputText(valueInputPtr->value()); });

    wrap->addChild(std::move(slider));
    wrap->addChild(std::move(valueInput));
    if (!field.valueSuffix.empty()) {
      wrap->addChild(ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::Start, .width = kSuffixSlotWidth * scale},
          makeLabel(field.valueSuffix, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
      ));
    }
    return wrap;
  }
  case ControlKind::Text: {
    const bool isColorKey = field.key == "TilingBorderColorActive" || field.key == "TilingBorderColorInactive";
    const bool isListKey = std::holds_alternative<std::vector<std::string>>(defaultValue(field));
    std::string text;
    if (isListKey) {
      text = joinListText(std::get<std::vector<std::string>>(effectiveValue(field)));
    } else {
      text = std::get<std::string>(effectiveValue(field));
    }
    const float inputWidth = (field.controlWidth > 0.0F ? field.controlWidth : kDefaultControlWidth) * scale;
    Input* inputPtr = nullptr;
    auto input = ui::input({
        .out = &inputPtr,
        .value = text,
        .placeholder = std::string(field.placeholder),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
        .width = inputWidth,
        .height = Style::controlHeight * scale,
        .submitOnFocusLoss = true,
    });
    input->setOnChange([inputPtr](const std::string& /*text*/) { inputPtr->setInvalid(false); });
    input->setOnSubmit([this, field, callbacks, isColorKey, isListKey, inputPtr](const std::string& value) {
      const std::string trimmed = StringUtils::trim(value);
      if (isColorKey && !isHexColorText(trimmed)) {
        inputPtr->setInvalid(true);
        return;
      }
      inputPtr->setInvalid(false);
      if (isListKey) {
        stageAndRebuild(field, Value{splitListText(trimmed)}, callbacks);
      } else {
        stageAndRebuild(field, Value{trimmed}, callbacks);
      }
    });
    return input;
  }
  case ControlKind::LayoutToggles:
    // Rendered by buildLayoutTogglesBlock() instead of the generic row path.
    return ui::node({});
  case ControlKind::EmergeAppWidths:
    // Rendered by buildEmergeAppWidthsBlock() instead of the generic row path.
    return ui::node({});
  }
  return ui::node({});
}


// The EnabledLayouts StringList as a block: a regular header row (title,
// description, optional Override badge + Reset) followed by one compact toggle
// row per layout, so the multi-value key reads like a setting, not six.
std::unique_ptr<Node> TilingPage::buildLayoutTogglesBlock(
    const Field& field, float scale, std::string_view armedResetKey, const Callbacks& callbacks
) {
  const auto enabled = std::get<std::vector<std::string>>(effectiveValue(field));
  const std::string path = keyPath(field);

  auto block = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });

  auto titleRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  titleRow->addChild(makeLabel(
      field.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  titleRow->addChild(ui::spacer());
  if (overridden(field)) {
    titleRow->addChild(makeOverrideBadge(scale));
    titleRow->addChild(makeResetButton(path, armedResetKey == path, scale, callbacks));
  }

  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(std::move(titleRow));
  if (!field.subtitle.empty()) {
    copy->addChild(makeLabel(
        field.subtitle, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }
  auto headRow = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  headRow->addChild(std::move(copy));
  block->addChild(std::move(headRow));

  // One toggle per layout; committing re-canonicalizes to the declared order.
  auto list = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .paddingH = Style::spaceLg * scale,
      .fillWidth = true,
  });
  const auto& kinds = layoutKindValues();
  const auto& labels = layoutKindLabels();
  const auto& subtitles = layoutKindSubtitles();
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
        .paddingV = 2.0F * scale,
        .fillWidth = true,
    });
    auto labelColumn = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    labelColumn->addChild(
        makeLabel(i < labels.size() ? labels[i] : kinds[i], Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface))
    );
    if (i < subtitles.size()) {
      labelColumn->addChild(
          makeLabel(subtitles[i], Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant))
      );
    }
    row->addChild(std::move(labelColumn));
    row->addChild(ui::toggle({
        .checked = std::ranges::contains(enabled, kinds[i]),
        .scale = scale,
        .onChange = [this, field, enabled, kind = kinds[i], callbacks](bool checked) {
          std::vector<std::string> next = enabled;
          if (checked && !std::ranges::contains(next, kind)) {
            next.push_back(kind);
          } else if (!checked) {
            std::erase(next, kind);
          }
          // Refuse to remove the last enabled layout; snap back instead.
          if (next.empty()) {
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
            return;
          }
          std::vector<std::string> ordered;
          ordered.reserve(next.size());
          for (const auto& candidate : layoutKindValues()) {
            if (std::ranges::contains(next, candidate)) {
              ordered.push_back(candidate);
            }
          }
          stageAndRebuild(field, Value{std::move(ordered)}, callbacks);
        },
    }));
    list->addChild(std::move(row));
  }
  block->addChild(std::move(list));
  return block;
}

// The EmergeAppWidths StringList as a block: header with an interactive window
// detect, then one row per app (class label, width select, delete). Modeled on
// buildLayoutTogglesBlock().
std::unique_ptr<Node> TilingPage::buildEmergeAppWidthsBlock(
    const Field& field, float scale, std::string_view armedResetKey, const Callbacks& callbacks
) {
  const auto entries = std::get<std::vector<std::string>>(effectiveValue(field));
  const std::string path = keyPath(field);

  auto block = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
  });

  auto titleRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
  titleRow->addChild(makeLabel(
      field.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  titleRow->addChild(ui::spacer());
  if (overridden(field)) {
    titleRow->addChild(makeOverrideBadge(scale));
    titleRow->addChild(makeResetButton(path, armedResetKey == path, scale, callbacks));
  }
  titleRow->addChild(ui::button({
      .text = "Detect window",
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Primary,
      .minHeight = Style::controlHeight * scale,
      .paddingV = Style::spaceSm * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = [this, field, callbacks]() {
        // Blocks until the compositor finishes the interactive window pick.
        const auto detected = m_dbus.queryWindowInfo();
        if (!detected.valid) {
          m_detectNotice = "Window detection cancelled or failed.";
          if (callbacks.requestRebuild) {
            callbacks.requestRebuild();
          }
          return;
        }
        if (detected.resourceClass.empty()) {
          m_detectNotice = "That window has no window class.";
          if (callbacks.requestRebuild) {
            callbacks.requestRebuild();
          }
          return;
        }
        std::vector<std::string> next = std::get<std::vector<std::string>>(effectiveValue(field));
        const bool present = std::ranges::any_of(next, [&detected](const std::string& entry) {
          const auto separator = entry.rfind('=');
          const std::string windowClass = StringUtils::trim(
              separator == std::string::npos ? std::string_view(entry) : std::string_view(entry).substr(0, separator)
          );
          return StringUtils::equalsInsensitive(windowClass, detected.resourceClass);
        });
        if (!present) {
          next.push_back(detected.resourceClass + "=0.5");
        }
        m_detectNotice.clear();
        stageAndRebuild(field, Value{std::move(next)}, callbacks);
      },
  }));

  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(std::move(titleRow));
  if (!field.subtitle.empty()) {
    copy->addChild(makeLabel(
        field.subtitle, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }
  auto headRow = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  headRow->addChild(std::move(copy));
  block->addChild(std::move(headRow));

  if (entries.empty()) {
    block->addChild(makeLabel(
        "No per-app widths yet. Use Detect window and click the app.", Style::fontSizeCaption * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }

  static const std::vector<std::string> kWidthValues{"0.333", "0.5", "0.667", "1"};
  static const std::vector<std::string> kWidthLabels{"33%", "50%", "67%", "100%"};

  auto list = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .paddingH = Style::spaceLg * scale,
      .fillWidth = true,
  });
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const std::string& entry = entries[i];
    const auto separator = entry.rfind('=');
    const std::string windowClass = StringUtils::trim(
        separator == std::string::npos ? std::string_view(entry) : std::string_view(entry).substr(0, separator)
    );
    const std::string fraction = separator == std::string::npos
        ? std::string()
        : StringUtils::trim(std::string_view(entry).substr(separator + 1));

    std::vector<std::string> options = kWidthValues;
    std::vector<std::string> labels = kWidthLabels;
    std::size_t selectedIndex = options.size();
    const auto known = std::ranges::find(options, fraction);
    if (known != options.end()) {
      selectedIndex = static_cast<std::size_t>(known - options.begin());
    } else {
      // Keep a hand-edited value selectable instead of snapping it to a preset.
      options.push_back(fraction);
      labels.push_back(fraction.empty() ? std::string("custom") : fraction + " (custom)");
      selectedIndex = options.size() - 1;
    }

    auto row = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
        .paddingV = 2.0F * scale,
        .fillWidth = true,
    });
    auto labelColumn = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    labelColumn->addChild(
        makeLabel(windowClass, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface))
    );
    row->addChild(std::move(labelColumn));

    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    actions->addChild(ui::select({
        .options = labels,
        .selectedIndex = selectedIndex,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .width = kDefaultControlWidth * scale,
        .height = Style::controlHeight * scale,
        .onSelectionChanged = [this, field, entries, i, windowClass, options, callbacks](
                                  std::size_t index, std::string_view /*label*/
                              ) {
          if (index >= options.size()) {
            return;
          }
          std::vector<std::string> next = entries;
          next[i] = windowClass + "=" + options[index];
          stageAndRebuild(field, Value{std::move(next)}, callbacks);
        },
    }));
    actions->addChild(ui::button({
        .glyph = "trash",
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Ghost,
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this, field, entries, i, callbacks]() {
          std::vector<std::string> next = entries;
          next.erase(next.begin() + static_cast<std::ptrdiff_t>(i));
          stageAndRebuild(field, Value{std::move(next)}, callbacks);
        },
    }));
    row->addChild(std::move(actions));
    list->addChild(std::move(row));
  }
  block->addChild(std::move(list));

  if (!m_detectNotice.empty()) {
    block->addChild(makeLabel(
        m_detectNotice, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Error)
    ));
  }
  return block;
}


// ---------------------------------------------------------------------------
// Per-monitor gap overrides. The compositor lets [Tiling][Output <name>] gap
// keys shadow the [Tiling] defaults this page edits
// (TilingController::applyGapSettingsToOutput). This page deliberately edits
// defaults only, so when the active monitor carries such overrides — created
// by the tiling KCM or migrated from a legacy kwinrc — changing gaps here has
// no visible effect. Surface that as a notice row above the Gaps section with
// a one-click Clear that deletes the shadowing keys and reloads the
// compositor, instead of silently editing values that never apply.
// ---------------------------------------------------------------------------

std::string TilingPage::monitorGapOverrideGroup() {
  const std::string output = m_dbus.activeOutputName();
  if (output.empty()) {
    return {};
  }
  // The flat INI model stores the KConfig subgroup "[Tiling][Output X]" as the
  // group name with the outer brackets stripped (KineticweKwe::load).
  return "Tiling][Output " + output;
}

std::string TilingPage::monitorGapOverrideSummary() {
  ensureLoaded();
  const std::string group = monitorGapOverrideGroup();
  if (group.empty()) {
    return {};
  }
  std::string summary;
  for (const std::string_view key : kGapOverrideKeys) {
    const auto value = m_config.readString(group, key);
    if (!value.has_value()) {
      continue;
    }
    if (!summary.empty()) {
      summary += ", ";
    }
    summary += std::string(key) + "=" + *value;
  }
  return summary;
}

void TilingPage::clearMonitorGapOverrides() {
  ensureLoaded();
  const std::string group = monitorGapOverrideGroup();
  if (group.empty()) {
    return;
  }
  for (const std::string_view key : kGapOverrideKeys) {
    m_config.removeKey(group, key);
  }
  if (m_config.save()) {
    m_dbus.requestReconfigure();
  } else {
    kLog.warn("kinetic-settings: failed to write {}", KineticweKwe::defaultPath().string());
  }
}

void TilingPage::addMonitorGapOverrideNotice(Flex& parent, float scale, const Callbacks& callbacks) {
  const std::string summary = monitorGapOverrideSummary();
  if (summary.empty()) {
    return;
  }
  const std::string output = m_dbus.activeOutputName();

  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(makeLabel(
      "This monitor overrides gap settings", Style::fontSizeBody * scale,
      colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  copy->addChild(ui::label({
      .text = std::format(
          "{} has its own gap values ({}) that take precedence over the ones below.",
          output.empty() ? std::string("The active monitor") : output, summary
      ),
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = kSettingDescriptionMaxLines,
  }));

  auto row = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  row->addChild(std::move(copy));
  row->addChild(ui::button({
      .text = "Clear monitor overrides",
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Default,
      .minHeight = Style::controlHeight * scale,
      .paddingV = Style::spaceSm * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = [this, callbacks]() {
        clearMonitorGapOverrides();
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      },
  }));
  parent.addChild(std::move(row));
}
// One-shot action row at the top of the Gap Toggle group: fires the same
// toggleOuterGaps() path as the "Tiling Toggle Outer Gaps" shortcut, so the
// presets can be tried out without binding a key first. Like the shortcut it
// targets the active monitor and writes a [Tiling][Output <name>] override.
void TilingPage::addGapToggleActionRow(Flex& parent, float scale, const Callbacks& callbacks) {
  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(makeLabel(
      "Toggle now", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  copy->addChild(ui::label({
      .text = "Flips the active monitor's outer (left/right) gaps between the "
              "two presets below — the same action as the \"Tiling Toggle Outer "
              "Gaps\" shortcut, which you can bind in the Shortcuts settings.",
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = kSettingDescriptionMaxLines,
  }));

  auto row = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  row->addChild(std::move(copy));
  row->addChild(ui::button({
      .text = "Toggle outer gaps",
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Default,
      .minHeight = Style::controlHeight * scale,
      .paddingV = Style::spaceSm * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = [this, callbacks]() {
        m_dbus.toggleOuterGaps();
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      },
  }));
  parent.addChild(std::move(row));
}
// ---------------------------------------------------------------------------
// Per-(desktop, monitor) layout overrides: [Tiling][DesktopOutput <n>:<out>]
// DefaultLayout keys. The desktop list comes from the compositor's D-Bus and
// the monitor list from the shell's Wayland connection (passed in by the
// window). Like every other control here, edits are staged until Apply.
// ---------------------------------------------------------------------------

std::string TilingPage::desktopOverrideGroup(std::size_t desktopNumber, std::string_view outputName) {
  return std::format("Tiling][DesktopOutput {}:{}", desktopNumber, outputName);
}

std::string TilingPage::desktopOverrideValue(std::string_view group) const {
  const auto staged = m_pendingDesktopOverrides.find(std::string(group));
  if (staged != m_pendingDesktopOverrides.end()) {
    return staged->second;
  }
  if (m_config.readBool(group, "FloatOnly", false)) {
    return "FloatOnly";
  }
  return m_config.readString(group, "DefaultLayout").value_or("");
}

void TilingPage::stageDesktopOverride(const std::string& group, std::string value) {
  ensureLoaded();
  // Editing back to the file value un-stages (the row stops being dirty).
  const std::string fileValue = m_config.readBool(group, "FloatOnly", false)
      ? std::string("FloatOnly")
      : m_config.readString(group, "DefaultLayout").value_or("");
  if (value == fileValue) {
    m_pendingDesktopOverrides.erase(group);
  } else {
    m_pendingDesktopOverrides[group] = std::move(value);
  }
}

std::vector<std::string> TilingPage::existingDesktopOverrideGroups() const {
  std::vector<std::string> groups;
  for (const auto& name : m_config.groupNames()) {
    if (name.starts_with("Tiling][DesktopOutput ")) {
      groups.push_back(name);
    }
  }
  return groups;
}

bool TilingPage::desktopOverrideRowMatches(
    const Filter& filter, std::string_view desktopName, std::string_view outputName, bool overridden
) const {
  if (filter.showOverriddenOnly && !overridden) {
    return false;
  }
  if (!filter.searchQuery.empty()) {
    const auto matches = [&filter](std::string_view text) {
      return StringUtils::containsInsensitive(text, filter.searchQuery);
    };
    if (!matches(desktopName) && !matches(outputName) && !matches("per-desktop")
        && !matches("layout override") && !matches("float only") && !matches("override")) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<Node> TilingPage::buildDesktopOverridesContent(
    float scale, const Filter& filter, std::string_view armedResetKey, const Callbacks& callbacks
) {
  const auto desktops = m_dbus.virtualDesktops();
  if (desktops.empty()) {
    return nullptr; // compositor unreachable — nothing to override
  }
  std::vector<std::string> outputs = m_outputNames;
  if (outputs.empty()) {
    // Fall back to the active output so a single-monitor setup still works.
    const std::string active = m_dbus.activeOutputName();
    if (!active.empty()) {
      outputs.push_back(active);
    }
  }
  if (outputs.empty()) {
    return nullptr;
  }

  // Compute the visible rows first so the group header only appears when at
  // least one (desktop, monitor) pair matches the filter.
  struct VisibleRow {
    std::size_t desktopNumber;
    std::string desktopName;
    std::string output;
    std::string group;
    std::string value;
  };
  std::vector<VisibleRow> rows;
  for (std::size_t di = 0; di < desktops.size(); ++di) {
    const std::size_t desktopNumber = di + 1; // 1-based, matching the KCM
    std::string desktopName = desktops[di].name;
    if (desktopName.empty()) {
      desktopName = std::format("Workspace {}", desktopNumber);
    }
    for (const auto& output : outputs) {
      const std::string group = desktopOverrideGroup(desktopNumber, output);
      const std::string value = desktopOverrideValue(group);
      if (desktopOverrideRowMatches(filter, desktopName, output, /*overridden=*/!value.empty())) {
        rows.push_back(VisibleRow{desktopNumber, desktopName, output, group, value});
      }
    }
  }
  if (rows.empty()) {
    return nullptr;
  }

  // Content column (the section title is provided by the caller's collapsible
  // card) + info caption.
  auto content = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  content->addChild(ui::label({
      .text =
          "Each row is a workspace and a connected monitor. Pick a layout to "
          "override the global default for that pair, or \"Float only\" to "
          "disable tiling there; leave it as \"Use default\" to fall back to "
          "the per-monitor or global setting.",
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = kSettingDescriptionMaxLines,
  }));

  // Options: "Use default" + "Float only" + every layout kind.
  std::vector<std::string> labels{"Use default", "Float only"};
  std::vector<std::string> values{"", "FloatOnly"};
  for (std::size_t i = 0; i < layoutKindValues().size(); ++i) {
    labels.push_back(layoutKindLabels()[i]);
    values.push_back(layoutKindValues()[i]);
  }

  std::string_view lastDesktop;
  for (const auto& row : rows) {
    if (row.desktopName != lastDesktop) {
      lastDesktop = row.desktopName;
      content->addChild(ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingV = Style::spaceXs * scale},
          makeLabel(
              row.desktopName, Style::fontSizeBody * scale,
              colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
          )
      ));
    }

    const bool overriddenNow = !row.value.empty();
    const std::string resetPath = std::string(kDesktopOverridePathPrefix) + row.group;

    std::size_t selectedIndex = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (values[i] == row.value) {
        selectedIndex = i;
        break;
      }
    }

    auto rowNode = ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::SpaceBetween,
         .gap = Style::spaceSm * scale,
         .paddingV = 2.0F * scale,
         .fillWidth = true}
    );
    rowNode->addChild(makeLabel(
        row.output, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));

    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    if (overriddenNow) {
      actions->addChild(makeOverrideBadge(scale));
      actions->addChild(makeResetButton(resetPath, armedResetKey == resetPath, scale, callbacks));
    }
    actions->addChild(ui::select({
        .options = labels,
        .selectedIndex = selectedIndex,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .width = kDefaultControlWidth * scale,
        .height = Style::controlHeight * scale,
        .onSelectionChanged = [this, group = row.group, values, callbacks](std::size_t index, std::string_view /*label*/) {
          if (index < values.size()) {
            stageDesktopOverride(group, values[index]);
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          }
        },
    }));
    rowNode->addChild(std::move(actions));
    content->addChild(std::move(rowNode));
  }
  return content;
}
// Runtime-only group (org.kde.KWin.Tiling over D-Bus): live state and actions;
// nothing here persists, so these rows never carry Override/Reset controls.
void TilingPage::buildRuntimeGroup(Flex& parent, float scale, const Callbacks& callbacks) {
  const auto makeRowShell = [](float s) {
    return ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::SpaceBetween,
         .gap = Style::spaceXs * s,
         .paddingV = 2.0F * s,
         .fillWidth = true}
    );
  };
  const auto makeCopy = [scale](std::string_view title, std::string_view subtitle) {
    auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
    copy->addChild(makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold));
    copy->addChild(makeLabel(subtitle, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)));
    return copy;
  };

  auto currentRow = makeRowShell(scale);
  currentRow->addChild(makeCopy(
      "Current layout",
      "Live value from the compositor's org.kde.KWin.Tiling D-Bus interface; "
      "not stored in the config file"
  ));
  currentRow->addChild(makeLabel(
      m_dbus.currentLayout(), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));
  parent.addChild(std::move(currentRow));

  auto actionsRow = makeRowShell(scale);
  actionsRow->addChild(makeCopy("Layout actions", "Switch the live layout right away (session only)"));
  auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  buttons->addChild(ui::button({
      .text = "Use MasterStack",
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Default,
      .minHeight = Style::controlHeight * scale,
      .paddingV = Style::spaceSm * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = [this, callbacks]() {
        m_dbus.setLayout("MasterStack");
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      },
  }));
  buttons->addChild(ui::button({
      .text = "Cycle layout",
      .fontSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Default,
      .minHeight = Style::controlHeight * scale,
      .paddingV = Style::spaceSm * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = [this, callbacks]() {
        m_dbus.cycleLayout();
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      },
  }));
  actionsRow->addChild(std::move(buttons));
  parent.addChild(std::move(actionsRow));
}


// One settings row in the exact Noctalia layout: bold title (+ Advanced badge)
// and caption description on the left; [Override badge][Reset][control] on the
// right. Badges only appear while the value differs from the default.
void TilingPage::addSettingRow(
    Flex& parent, const Field& field, float scale, std::string_view armedResetKey, const Callbacks& callbacks
) {
  const std::string path = keyPath(field);
  const bool isOverridden = overridden(field);

  auto titleRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  titleRow->addChild(makeLabel(
      field.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  if (field.advanced) {
    titleRow->addChild(makeAdvancedBadge(scale));
  }
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

// ---------------------------------------------------------------------------

std::unique_ptr<Node>
TilingPage::build(float scale, const Filter& filter, std::string_view armedResetKey, const Callbacks& callbacks) {
  ensureLoaded();

  auto root = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceLg * scale,
      .fill = clearColorSpec(),
      .fillWidth = true,
  });

  // Section header: glyph + bold primary title (upstream makeSection).
  root->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .glyph = "layout-grid",
          .glyphSize = Style::fontSizeHeader * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeLabel("Tiling", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
  ));

  // Each page group renders as a collapsible card. During a global search the
  // groups flatten to plain headers so every match stays visible (mirrors
  // Noctalia's settings behaviour).
  const bool collapsible = filter.searchQuery.empty();
  if (collapsible && !m_expandedInitialized) {
    m_expandedInitialized = true;
    for (const auto& field : fields()) {
      if (!field.pageGroup.empty()) {
        m_expandedGroups.insert(std::string(field.pageGroup));
        break;
      }
    }
  }

  bool anyRows = false;
  bool firstGroup = true;
  std::string_view lastGroup;
  Flex* currentBody = root.get();
  const auto beginGroup = [&](std::string_view title) {
    if (title.empty()) {
      currentBody = root.get();
      return;
    }
    if (collapsible) {
      currentBody = kinetic_settings::addCollapsibleSection(
          *root, title, title, m_expandedGroups, scale, callbacks.jumpRow
      );
      firstGroup = false;
      return;
    }
    // Search mode: flat section header.
    if (!firstGroup) {
      // Non-first groups get a separator above the (bold, secondary) label.
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
    currentBody = root.get();
    firstGroup = false;
  };

  for (const auto& field : fields()) {
    if (!matchesFilter(field, filter)) {
      continue;
    }
    if (field.pageGroup != lastGroup) {
      lastGroup = field.pageGroup;
      beginGroup(lastGroup);
      if (lastGroup == "Gaps") {
        addMonitorGapOverrideNotice(*currentBody, scale, callbacks);
      }
      if (lastGroup == "Gap Toggle") {
        addGapToggleActionRow(*currentBody, scale, callbacks);
      }
    }
    switch (field.control) {
    case ControlKind::LayoutToggles:
      currentBody->addChild(buildLayoutTogglesBlock(field, scale, armedResetKey, callbacks));
      break;
    case ControlKind::EmergeAppWidths:
      currentBody->addChild(buildEmergeAppWidthsBlock(field, scale, armedResetKey, callbacks));
      break;
    default:
      addSettingRow(*currentBody, field, scale, armedResetKey, callbacks);
      break;
    }
    anyRows = true;
  }

  // Per-desktop layout overrides: one row per (workspace, connected monitor)
  // pair, in its own collapsible section, after the persisted fields and before
  // the Runtime group. Skipped entirely when nothing matches the filter.
  if (auto desktopOverrides = buildDesktopOverridesContent(scale, filter, armedResetKey, callbacks)) {
    beginGroup("Per-desktop layout overrides");
    currentBody->addChild(std::move(desktopOverrides));
    anyRows = true;
  }

  // Runtime group: live D-Bus state/actions; hidden by the Overridden filter
  // (it holds no persisted settings) but reachable through search.
  if (!filter.showOverriddenOnly) {
    const auto runtimeMatches = [&filter]() {
      if (filter.searchQuery.empty()) {
        return true;
      }
      return StringUtils::containsInsensitive("Runtime", filter.searchQuery)
             || StringUtils::containsInsensitive("Current layout", filter.searchQuery)
             || StringUtils::containsInsensitive("Layout actions", filter.searchQuery);
    };
    if (runtimeMatches()) {
      beginGroup("Runtime");
      buildRuntimeGroup(*currentBody, scale, callbacks);
      anyRows = true;
    }
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

