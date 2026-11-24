#include "keyboard_page.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "layout_catalog.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "shell/controls/search_picker.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

  constexpr Logger kLog("kinetic-settings");

  // Matches upstream's settings description wrapping budget.
  constexpr int kSettingDescriptionMaxLines = 5;

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

  // Status pill ("Override"); module-local re-implementation of
  // SettingsControlFactory::makeStatusBadge.
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

  // Human-readable label for a configured layout: the stored DisplayNames entry
  // when present, otherwise the system catalog, otherwise the raw codes.
  [[nodiscard]] std::string layoutLabel(
      const std::string& layout, const std::string& variant, const std::string& displayName
  ) {
    if (!displayName.empty()) {
      return displayName;
    }
    return layout_catalog::labelFor(layout, variant);
  }

  // The two-stage Reset action shown next to overridden settings.
  [[nodiscard]] std::unique_ptr<Button> makeResetButton(
      std::string path, bool pendingConfirmation, float scale, const KeyboardPage::Callbacks& callbacks
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

KeyboardPage::KeyboardPage(SessionBus& bus) : m_dbus(bus) {}

// ---------------------------------------------------------------------------
// Schema: the key-repeat settings and their compositor defaults. Defaults and
// units mirror src/keyboard_input.cpp ([Keyboard] RepeatDelay=600,
// RepeatRate=25, KeyRepeat="repeat"); ranges mirror the upstream Plasma
// keyboard KCM (delay 100..5000 ms, rate shown in repeats/s).
// ---------------------------------------------------------------------------

const std::vector<KeyboardPage::Field>& KeyboardPage::fields() {
  static const std::vector<Field> table{
      Field{
          .key = "KeyRepeat",
          .title = "Key repeat",
          .subtitle = "When enabled, pressing and holding a key sends it repeatedly",
          .isToggle = true,
          .fallback = Value{true},
      },
      Field{
          .key = "RepeatDelay",
          .title = "Repeat delay",
          .subtitle = "How long a key must be held before it starts repeating",
          .isToggle = false,
          .fallback = Value{600},
          .minValue = 100,
          .maxValue = 5000,
          .step = 50,
          .valueSuffix = " ms",
      },
      Field{
          .key = "RepeatRate",
          .title = "Repeat rate",
          .subtitle = "How many times per second a held key repeats",
          .isToggle = false,
          .fallback = Value{25},
          .minValue = 1,
          .maxValue = 100,
          .step = 1,
          .valueSuffix = " repeats/s",
      },
      Field{
          .key = "NumLock",
          .title = "Num lock",
          .subtitle = "State of the Num Lock key when the session starts",
          .isToggle = false,
          .isSelect = true,
          .selectValues = {0, 1, 2},   // ON, OFF, UNCHANGED (see src/xkb.cpp)
          .selectLabels = {"On", "Off", "Leave unchanged"},
          .fallback = Value{2},
      },
  };
  return table;
}

std::string KeyboardPage::keyPath(const Field& field) {
  return std::string("Keyboard") + '.' + std::string(field.key);
}

const KeyboardPage::Field* KeyboardPage::findField(std::string_view path) {
  for (const auto& field : fields()) {
    if (path == keyPath(field)) {
      return &field;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// State.
// ---------------------------------------------------------------------------

void KeyboardPage::ensureLoaded() {
  if (!m_loaded) {
    m_config.load();
    m_layouts = readLayouts();
    m_switchMode = m_config.readString("Layout", "SwitchMode").value_or("Global");
    m_loaded = true;
  }
}

KeyboardPage::Value KeyboardPage::fileBackedValue(const Field& field) const {
  if (field.isSelect) {
    return Value{m_config.readInt("Keyboard", field.key, std::get<int>(field.fallback))};
  }
  if (field.isToggle) {
    const bool repeat = m_config.readString("Keyboard", field.key).value_or("repeat") == "repeat";
    return Value{repeat};
  }
  if (field.key == "RepeatRate") {
    return Value{static_cast<int>(std::lround(m_config.readDouble("Keyboard", field.key, 25.0)))};
  }
  return Value{m_config.readInt("Keyboard", field.key, std::get<int>(field.fallback))};
}

KeyboardPage::Value KeyboardPage::effectiveValue(const Field& field) const {
  const auto staged = m_pending.find(keyPath(field));
  if (staged != m_pending.end()) {
    return staged->second;
  }
  return fileBackedValue(field);
}

bool KeyboardPage::equalsDefault(const Field& field, const Value& value) const {
  return value == field.fallback;
}

bool KeyboardPage::overridden(const Field& field) const {
  return !equalsDefault(field, effectiveValue(field));
}

// ---------------------------------------------------------------------------
// Staging + persistence.
// ---------------------------------------------------------------------------

void KeyboardPage::stage(const Field& field, const Value& value) {
  ensureLoaded();
  const std::string path = keyPath(field);
  // Editing back to the on-disk value un-stages (the row stops being dirty).
  if (value == fileBackedValue(field)) {
    m_pending.erase(path);
  } else {
    m_pending[path] = value;
  }
}

void KeyboardPage::stageAndRebuild(const Field& field, const Value& value, const Callbacks& callbacks) {
  stage(field, value);
  if (callbacks.requestRebuild) {
    callbacks.requestRebuild();
  }
}

std::vector<std::string> KeyboardPage::resettableKeyPaths() const {
  std::vector<std::string> paths;
  paths.reserve(fields().size());
  for (const auto& field : fields()) {
    paths.push_back(keyPath(field));
  }
  return paths;
}

void KeyboardPage::resetKeyPath(std::string_view path) {
  if (const Field* field = findField(path)) {
    stage(*field, field->fallback);
  }
}

void KeyboardPage::resetAll() {
  for (const auto& field : fields()) {
    stage(field, field.fallback);
  }
  // Layouts reset to the compositor default: no explicit list, Global switching.
  m_pendingLayouts = std::vector<LayoutEntry>{};
  normalizeLayoutsPending();
  m_pendingSwitchMode = std::string("Global");
  normalizeSwitchModePending();
  m_addingLayout = false;
}

bool KeyboardPage::hasPending() const noexcept {
  return !m_pending.empty() || m_pendingLayouts.has_value() || m_pendingSwitchMode.has_value();
}

void KeyboardPage::writeValue(const Field& field, const Value& value) {
  if (field.isSelect) {
    const int setting = std::get<int>(value);
    if (setting == std::get<int>(field.fallback)) {
      // Back to the compositor default ("leave unchanged") keeps the file tidy.
      m_config.removeKey("Keyboard", field.key);
    } else {
      m_config.setInt("Keyboard", field.key, setting);
    }
    return;
  }
  if (field.isToggle) {
    const bool repeat = std::get<bool>(value);
    if (repeat == true) {
      // Back to the compositor default ("repeat") keeps the file tidy.
      m_config.removeKey("Keyboard", field.key);
    } else {
      m_config.setString("Keyboard", field.key, "no-repeat");
    }
    return;
  }
  if (field.key == "RepeatRate") {
    const int number = std::get<int>(value);
    if (number == std::get<int>(field.fallback)) {
      m_config.removeKey("Keyboard", field.key);
    } else {
      m_config.setDouble("Keyboard", field.key, static_cast<double>(number));
    }
    return;
  }
  const int number = std::get<int>(value);
  if (number == std::get<int>(field.fallback)) {
    m_config.removeKey("Keyboard", field.key);
  } else {
    m_config.setInt("Keyboard", field.key, number);
  }
}

bool KeyboardPage::applyAll() {
  ensureLoaded();
  if (!hasPending()) {
    return true;
  }

  for (const auto& [path, value] : m_pending) {
    if (const Field* field = findField(path)) {
      writeValue(*field, value);
    }
  }

  if (m_pendingLayouts) {
    const auto& layouts = *m_pendingLayouts;
    if (layouts.empty()) {
      m_config.removeKey("Layout", "LayoutList");
      m_config.removeKey("Layout", "VariantList");
      m_config.removeKey("Layout", "DisplayNames");
    } else {
      std::vector<std::string> layoutList;
      std::vector<std::string> variantList;
      std::vector<std::string> displayNames;
      layoutList.reserve(layouts.size());
      variantList.reserve(layouts.size());
      displayNames.reserve(layouts.size());
      for (const auto& entry : layouts) {
        layoutList.push_back(entry.layout);
        variantList.push_back(entry.variant);
        displayNames.push_back(layoutLabel(entry.layout, entry.variant, entry.displayName));
      }
      m_config.setStringList("Layout", "LayoutList", layoutList);
      m_config.setStringList("Layout", "VariantList", variantList);
      m_config.setStringList("Layout", "DisplayNames", displayNames);
      // Plasma's KCM uses this bookkeeping key; keep it set for compatibility
      // with tooling that checks whether a layout is configured.
      m_config.setString("Layout", "Use", "true");
    }
    m_layouts = layouts;
  }
  if (m_pendingSwitchMode) {
    if (*m_pendingSwitchMode == "Global") {
      m_config.removeKey("Layout", "SwitchMode");
    } else {
      m_config.setString("Layout", "SwitchMode", *m_pendingSwitchMode);
    }
    m_switchMode = *m_pendingSwitchMode;
  }

  if (!m_config.save()) {
    kLog.warn("kinetic-settings: failed to write {}", KineticweKwe::defaultPath().string());
    return false;
  }

  m_pending.clear();
  m_pendingLayouts.reset();
  m_pendingSwitchMode.reset();
  m_dbus.requestReconfigure();
  return true;
}

// ---------------------------------------------------------------------------
// Controls + layout.
// ---------------------------------------------------------------------------

std::unique_ptr<Node>
KeyboardPage::buildControl(const Field& field, float scale, const Callbacks& callbacks) {
  if (field.isSelect) {
    const int value = std::get<int>(effectiveValue(field));
    const auto it = std::ranges::find(field.selectValues, value);
    const bool known = it != field.selectValues.end();
    const std::optional<std::size_t> selectedIndex =
        known ? std::optional<std::size_t>{static_cast<std::size_t>(it - field.selectValues.begin())}
              : std::nullopt;
    return ui::select({
        .options = field.selectLabels,
        .selectedIndex = selectedIndex,
        .clearSelection = !known,
        .placeholder = known ? std::optional<std::string>{std::nullopt}
                             : std::optional<std::string>{"Unknown value: " + std::to_string(value)},
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .width = 190.0F * scale,
        .height = Style::controlHeight * scale,
        .onSelectionChanged = [this, field, callbacks](std::size_t index, std::string_view /*label*/) {
          if (index < field.selectValues.size()) {
            stageAndRebuild(field, Value{field.selectValues[index]}, callbacks);
          }
        },
    });
  }
  if (field.isToggle) {
    const bool checked = std::get<bool>(effectiveValue(field));
    return ui::toggle({
        .checked = checked,
        .scale = scale,
        .onChange = [this, field, callbacks](bool value) {
          stageAndRebuild(field, Value{value}, callbacks);
        },
    });
  }
  const int value = std::get<int>(effectiveValue(field));
  return ui::stepper({
      .minValue = field.minValue,
      .maxValue = field.maxValue,
      .step = field.step,
      .value = std::clamp(value, field.minValue, field.maxValue),
      .scale = scale,
      .valueSuffix = std::string(field.valueSuffix),
      .onValueCommitted = [this, field, callbacks](int committed) {
        stageAndRebuild(field, Value{std::clamp(committed, field.minValue, field.maxValue)}, callbacks);
      },
  });
}

// One settings row in the exact Noctalia layout: bold title and caption
// description on the left; [Override badge][Reset][control] on the right.
// Badges only appear while the value differs from the default.
void KeyboardPage::addSettingRow(
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
// Keyboard layouts ([Layout] group).
// ---------------------------------------------------------------------------

std::vector<KeyboardPage::LayoutEntry> KeyboardPage::readLayouts() const {
  const auto layoutList = m_config.readStringList("Layout", "LayoutList");
  const auto variantList = m_config.readStringList("Layout", "VariantList");
  const auto displayNames = m_config.readStringList("Layout", "DisplayNames");

  std::vector<LayoutEntry> layouts;
  layouts.reserve(layoutList.size());
  for (std::size_t i = 0; i < layoutList.size(); ++i) {
    if (layoutList[i].empty()) {
      continue;
    }
    layouts.push_back(LayoutEntry{
        .layout = layoutList[i],
        .variant = i < variantList.size() ? variantList[i] : std::string{},
        .displayName = i < displayNames.size() ? displayNames[i] : std::string{},
    });
  }
  return layouts;
}

const std::vector<KeyboardPage::LayoutEntry>& KeyboardPage::effectiveLayouts() const {
  return m_pendingLayouts ? *m_pendingLayouts : m_layouts;
}

const std::string& KeyboardPage::effectiveSwitchMode() const {
  return m_pendingSwitchMode ? *m_pendingSwitchMode : m_switchMode;
}

bool KeyboardPage::layoutsVisible(const Filter& filter) const {
  const auto& layouts = effectiveLayouts();
  if (filter.showOverriddenOnly && layouts.empty() && effectiveSwitchMode() == "Global") {
    return false;
  }
  if (filter.searchQuery.empty()) {
    return true;
  }
  const auto matches = [&filter](std::string_view text) {
    return StringUtils::containsInsensitive(text, filter.searchQuery);
  };
  if (matches("Keyboard layouts") || matches("language") || matches("switch")) {
    return true;
  }
  for (const auto& entry : layouts) {
    if (matches(layoutLabel(entry.layout, entry.variant, entry.displayName)) || matches(entry.layout)
        || matches(entry.variant)) {
      return true;
    }
  }
  return false;
}

void KeyboardPage::addLayout(LayoutEntry entry) {
  ensureLoaded();
  std::vector<LayoutEntry> layouts = effectiveLayouts();
  layouts.push_back(std::move(entry));
  m_pendingLayouts = std::move(layouts);
  normalizeLayoutsPending();
}

void KeyboardPage::removeLayout(std::size_t index) {
  ensureLoaded();
  std::vector<LayoutEntry> layouts = effectiveLayouts();
  if (index >= layouts.size()) {
    return;
  }
  layouts.erase(layouts.begin() + static_cast<std::ptrdiff_t>(index));
  m_pendingLayouts = std::move(layouts);
  normalizeLayoutsPending();
}

void KeyboardPage::moveLayout(std::size_t from, std::size_t to) {
  ensureLoaded();
  std::vector<LayoutEntry> layouts = effectiveLayouts();
  if (from >= layouts.size() || to >= layouts.size()) {
    return;
  }
  std::swap(layouts[from], layouts[to]);
  m_pendingLayouts = std::move(layouts);
  normalizeLayoutsPending();
}

void KeyboardPage::setSwitchMode(std::string mode) {
  ensureLoaded();
  m_pendingSwitchMode = std::move(mode);
  normalizeSwitchModePending();
}

void KeyboardPage::normalizeLayoutsPending() {
  if (m_pendingLayouts && *m_pendingLayouts == m_layouts) {
    m_pendingLayouts.reset();
  }
}

void KeyboardPage::normalizeSwitchModePending() {
  if (m_pendingSwitchMode && *m_pendingSwitchMode == m_switchMode) {
    m_pendingSwitchMode.reset();
  }
}

std::unique_ptr<Node> KeyboardPage::buildLayoutRow(
    const LayoutEntry& entry, std::size_t index, std::size_t count, float scale, const Callbacks& callbacks
) {
  auto row = ui::row(
      {.align = FlexAlign::Center,
       .gap = Style::spaceSm * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  row->addChild(ui::label({
      .text = layoutLabel(entry.layout, entry.variant, entry.displayName),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .flexGrow = 1.0F,
  }));

  const auto iconButton = [scale](std::string glyph, std::function<void()> onClick) {
    return ui::button({
        .glyph = std::move(glyph),
        .glyphSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Ghost,
        .minWidth = Style::controlHeightSm * scale,
        .minHeight = Style::controlHeightSm * scale,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusSm(scale),
        .onClick = std::move(onClick),
    });
  };

  row->addChild(iconButton(
      "close",
      [this, index, callbacks]() {
        removeLayout(index);
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      }
  ));
  if (index > 0) {
    row->addChild(iconButton(
        "chevron-up",
        [this, index, callbacks]() {
          moveLayout(index, index - 1);
          if (callbacks.requestRebuild) {
            callbacks.requestRebuild();
          }
        }
    ));
  }
  if (index + 1 < count) {
    row->addChild(iconButton(
        "chevron-down",
        [this, index, callbacks]() {
          moveLayout(index, index + 1);
          if (callbacks.requestRebuild) {
            callbacks.requestRebuild();
          }
        }
    ));
  }
  return row;
}

std::unique_ptr<Node> KeyboardPage::buildLayoutAddPicker(float scale, const Callbacks& callbacks) {
  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  column->addChild(makeLabel(
      "Pick a layout to add:", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));

  // Exclude the pairs already configured so the same layout is not added twice.
  const auto& configured = effectiveLayouts();
  const auto& catalog = layout_catalog::entries();
  std::vector<SearchPickerOption> options;
  options.reserve(catalog.size());
  for (std::size_t i = 0; i < catalog.size(); ++i) {
    const auto& entry = catalog[i];
    const bool alreadyAdded = std::ranges::any_of(configured, [&entry](const LayoutEntry& configuredEntry) {
      return configuredEntry.layout == entry.layout && configuredEntry.variant == entry.variant;
    });
    if (alreadyAdded) {
      continue;
    }
    options.push_back(SearchPickerOption{
        .value = std::to_string(i),
        .label = layout_catalog::label(entry),
        .description = entry.layout + (entry.variant.empty() ? "" : " (" + entry.variant + ")"),
        .icon = "keyboard",
    });
  }

  column->addChild(ui::searchPicker({
      .placeholder = "Search layouts...",
      .emptyText = options.empty() ? "No more layouts to add." : "No layouts found.",
      .options = std::move(options),
      .height = 320.0F * scale,
      .onActivated =
          [this, callbacks](const SearchPickerOption& option) {
            std::size_t index = 0;
            try {
              index = static_cast<std::size_t>(std::stoul(option.value));
            } catch (...) {
              return;
            }
            const auto& catalogEntries = layout_catalog::entries();
            if (index >= catalogEntries.size()) {
              return;
            }
            const auto& catalogEntry = catalogEntries[index];
            addLayout(LayoutEntry{
                .layout = catalogEntry.layout,
                .variant = catalogEntry.variant,
                .displayName = layout_catalog::label(catalogEntry),
            });
            m_addingLayout = false;
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
      .onCancel =
          [this, callbacks]() {
            m_addingLayout = false;
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
  }));

  column->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::button({
          .text = "Cancel",
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this, callbacks]() {
                m_addingLayout = false;
                if (callbacks.requestRebuild) {
                  callbacks.requestRebuild();
                }
              },
      })
  ));

  return column;
}

std::unique_ptr<Node> KeyboardPage::buildLayoutsSection(float scale, const Callbacks& callbacks) {
  auto section = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});

  section->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .glyph = "keyboard",
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeLabel(
          "Keyboard layouts", Style::fontSizeTitle * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
      )
  ));
  section->addChild(ui::label({
      .text = "Choose the layouts available on this keyboard. Multiple layouts are cycled with Meta+Alt+K.",
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = kSettingDescriptionMaxLines,
  }));

  const auto& layouts = effectiveLayouts();
  if (layouts.empty()) {
    section->addChild(ui::label({
        .text = "No layouts configured - the system default is used.",
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    }));
  } else {
    for (std::size_t i = 0; i < layouts.size(); ++i) {
      section->addChild(buildLayoutRow(layouts[i], i, layouts.size(), scale, callbacks));
    }
  }

  if (m_addingLayout) {
    section->addChild(buildLayoutAddPicker(scale, callbacks));
  } else {
    section->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::button({
            .text = "Add layout",
            .fontSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Primary,
            .minHeight = Style::controlHeight * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick =
                [this, callbacks]() {
                  m_addingLayout = true;
                  if (callbacks.requestRebuild) {
                    callbacks.requestRebuild();
                  }
                },
        })
    ));
  }

  // Switch-with policy; maps to the [Layout] SwitchMode key.
  static const std::vector<std::string> modeKeys{"Global", "Desktop", "Window", "WinClass"};
  static const std::vector<std::string> modeLabels{"Global", "Desktop", "Window", "Application"};
  const std::string& currentMode = effectiveSwitchMode();
  std::optional<std::size_t> selected;
  for (std::size_t i = 0; i < modeKeys.size(); ++i) {
    if (modeKeys[i] == currentMode) {
      selected = i;
      break;
    }
  }

  auto switchRow = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  switchRow->addChild(makeLabel(
      "Switch with", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  switchRow->addChild(ui::select({
      .options = modeLabels,
      .selectedIndex = selected,
      .clearSelection = !selected.has_value(),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .glyphSize = Style::fontSizeBody * scale,
      .width = 190.0F * scale,
      .height = Style::controlHeight * scale,
      .onSelectionChanged =
          [this, callbacks](std::size_t index, std::string_view /*label*/) {
            if (index < modeKeys.size()) {
              setSwitchMode(modeKeys[index]);
              if (callbacks.requestRebuild) {
                callbacks.requestRebuild();
              }
            }
          },
  }));
  section->addChild(std::move(switchRow));

  return section;
}

std::unique_ptr<Node> KeyboardPage::build(
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

  // Section header: glyph + bold primary title.
  root->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .glyph = "keyboard",
          .glyphSize = Style::fontSizeHeader * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeLabel("Kweboard", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
  ));

  bool anyRows = false;

  if (layoutsVisible(filter)) {
    root->addChild(buildLayoutsSection(scale, callbacks));
    anyRows = true;
  }

  for (const auto& field : fields()) {
    if (!filter.searchQuery.empty()) {
      const auto matches = [&filter](std::string_view text) {
        return StringUtils::containsInsensitive(text, filter.searchQuery);
      };
      if (!matches(field.title) && !matches(field.subtitle)) {
        continue;
      }
    }
    if (filter.showOverriddenOnly && !overridden(field)) {
      continue;
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
        "No keyboard settings match the current filters.", Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }

  return root;
}
