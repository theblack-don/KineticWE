#include "autostart_page.h"

#include "autostart.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "shell/controls/search_picker.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("kinetic-settings");

  [[nodiscard]] std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal) {
    return ui::label({
        .text = std::string(text),
        .fontSize = fontSize,
        .fontWeight = fontWeight,
        .color = color,
    });
  }

  [[nodiscard]] bool matches(const autostart::Entry& entry, const AutostartPage::Filter& filter) {
    if (filter.searchQuery.empty()) {
      return true;
    }
    return StringUtils::containsInsensitive(entry.name, filter.searchQuery)
        || StringUtils::containsInsensitive(entry.command, filter.searchQuery);
  }

} // namespace

std::unique_ptr<Node> AutostartPage::buildAddMenu(float scale, const Callbacks& callbacks) {
  auto menu = ui::column({.align = FlexAlign::End, .gap = Style::spaceXs * scale, .fillWidth = true});
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .padding = Style::spaceXs * scale,
      .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.5F),
      .radius = Style::scaledRadiusMd(scale),
      .width = 220.0F * scale,
  });
  const auto menuItem = [scale](std::string_view text, auto onPick) {
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
      i18n::tr("settings.autostart.add-application"),
      [this, callbacks]() {
        m_addMenuOpen = false;
        m_addMode = AddMode::Apps;
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      }
  ));
  card->addChild(menuItem(
      i18n::tr("settings.autostart.add-custom-command"),
      [this, callbacks]() {
        m_addMenuOpen = false;
        m_addMode = AddMode::Command;
        if (callbacks.requestRebuild) {
          callbacks.requestRebuild();
        }
      }
  ));
  menu->addChild(std::move(card));
  return menu;
}

std::unique_ptr<Node> AutostartPage::buildAppPicker(float scale, const Callbacks& callbacks) {
  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  column->addChild(makeLabel(
      i18n::tr("settings.autostart.app-picker-title"), Style::fontSizeBody * scale,
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
  column->addChild(ui::searchPicker({
      .placeholder = i18n::tr("settings.autostart.app-picker-placeholder"),
      .emptyText = i18n::tr("settings.autostart.app-picker-empty"),
      .options = std::move(options),
      .height = 320.0F * scale,
      .onActivated =
          [this, callbacks](const SearchPickerOption& option) {
            const auto& entries = desktopEntries();
            const auto entry = std::ranges::find_if(
                entries, [&option](const DesktopEntry& e) { return e.id == option.value; }
            );
            if (entry != entries.end() && !autostart::addApplication(*entry)) {
              kLog.warn("autostart: failed to add application '{}'", option.value);
            }
            m_addMode = AddMode::None;
            m_addMenuOpen = false;
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
  }));

  column->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::button({
          .text = i18n::tr("settings.autostart.cancel"),
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

std::unique_ptr<Node> AutostartPage::buildCommandForm(float scale, const Callbacks& callbacks) {
  auto column = ui::column({.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .fillWidth = true});
  column->addChild(makeLabel(
      i18n::tr("settings.autostart.command-form-title"), Style::fontSizeBody * scale,
      colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));

  auto input = std::make_unique<Input>();
  input->setPlaceholder(i18n::tr("settings.autostart.command-placeholder"));
  input->setMinLayoutWidth(480.0F * scale);
  Input* inputPtr = input.get();
  const auto submit = [this, inputPtr, callbacks]() {
    const std::string command = inputPtr->value();
    if (command.empty()) {
      return;
    }
    m_addMode = AddMode::None;
    m_addMenuOpen = false;
    if (!autostart::addCommand(command)) {
      kLog.warn("autostart: failed to add command '{}'", command);
    }
    if (callbacks.requestRebuild) {
      callbacks.requestRebuild();
    }
  };
  input->setOnSubmit([submit](const std::string&) { submit(); });
  column->addChild(std::move(input));

  column->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::button({
          .text = i18n::tr("settings.autostart.add"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Primary,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = submit,
      }),
      ui::button({
          .text = i18n::tr("settings.autostart.cancel"),
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

std::unique_ptr<Node> AutostartPage::build(float scale, const Filter& filter, const Callbacks& callbacks) {
  auto root = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceLg * scale,
      .fill = clearColorSpec(),
      .fillWidth = true,
  });

  auto header = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  header->addChild(ui::glyph({
      .glyph = "rocket",
      .glyphSize = Style::fontSizeHeader * scale,
      .color = colorSpecFromRole(ColorRole::Primary),
  }));
  header->addChild(
      makeLabel("Autostart", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
  );
  header->addChild(ui::spacer());
  header->addChild(ui::button({
      .glyph = "plus",
      .fontSize = Style::fontSizeCaption * scale,
      .glyphSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Ghost,
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

  if (m_addMenuOpen) {
    root->addChild(buildAddMenu(scale, callbacks));
  }
  if (m_addMode == AddMode::Apps) {
    root->addChild(buildAppPicker(scale, callbacks));
    return root;
  }
  if (m_addMode == AddMode::Command) {
    root->addChild(buildCommandForm(scale, callbacks));
    return root;
  }

  bool any = false;
  for (const autostart::Entry& entry : autostart::entries()) {
    if (!matches(entry, filter)) {
      continue;
    }
    auto row = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
        .paddingV = 2.0F * scale,
        .fillWidth = true,
    });
    auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
    copy->addChild(makeLabel(
        entry.name, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
    ));
    if (!entry.command.empty() && entry.command != entry.name) {
      copy->addChild(ui::label({
          .text = entry.command,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      }));
    }
    row->addChild(std::move(copy));
    row->addChild(ui::button({
        .glyph = "close",
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("settings.autostart.remove"),
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick =
            [fileName = entry.fileName, callbacks]() {
              if (!autostart::remove(fileName)) {
                kLog.warn("autostart: failed to remove '{}'", fileName);
              }
              if (callbacks.requestRebuild) {
                callbacks.requestRebuild();
              }
            },
    }));
    root->addChild(std::move(row));
    any = true;
  }

  if (!any) {
    if (!filter.searchQuery.empty()) {
      // Global search drops pages with no hits, so don't render an empty notice.
      return nullptr;
    }
    root->addChild(makeLabel(
        i18n::tr("settings.autostart.empty"), Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
  }

  return root;
}
