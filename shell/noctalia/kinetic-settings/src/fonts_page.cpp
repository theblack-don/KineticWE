#include "fonts_page.h"

#include "config/config_service.h"
#include "core/log.h"
#include "font_discovery.h"
#include "i18n/i18n.h"
#include "shell/controls/search_picker.h"
#include "system/process.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/stepper.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("kinetic-settings");

  constexpr int kSettingDescriptionMaxLines = 5;
  constexpr float kValueButtonWidth = 230.0F;

  constexpr std::string_view kInterfaceKeyPath = "Fonts.Interface";
  constexpr std::string_view kFontSizeKeyPath = "Fonts.FontSize";
  constexpr const char* kPickerInterface = "interface";
  constexpr int kDefaultFontSize = static_cast<int>(Style::fontSizeBody);
  constexpr int kMinFontSize = 10;
  constexpr int kMaxFontSize = 20;
  constexpr std::string_view kInterfaceSubtitle =
      "Typeface used by the shell and, when enabled, window title bars. Takes effect on Apply.";
  constexpr std::string_view kFontSizeSubtitle =
      "Base size of interface text. Scales shell text only; apps and window title bars keep their own font sizes. "
      "The default is 14 px.";
  constexpr std::string_view kPageDescription =
      "Applying the interface font and size takes effect immediately; system font preferences apply to newly "
      "started applications. Fonts installed during this session may need a shell restart.";
  constexpr std::string_view kSystemDescription =
      "Preferred families for generic font requests in GTK, Qt, and other fontconfig applications. Applies to "
      "newly started applications; restart running apps to update them.";
  constexpr std::string_view kEmptyState = "No settings match the current filters.";
  constexpr std::string_view kNoFamilies =
      "No installed font families were found. Install fontconfig or use Refresh font list.";
  constexpr std::string_view kAliasSample = "The quick brown fox 0123456789";
  constexpr std::string_view kInterfaceSample = "The quick brown fox jumps over the lazy dog";

  // Kinetic Settings presents accessibility.font_scale as pixels of the body text size.
  int fontSizePxFromScale(float fontScale) {
    return std::clamp(static_cast<int>(std::lround(fontScale * Style::fontSizeBody)), kMinFontSize, kMaxFontSize);
  }

  float fontSizeScaleFromPx(int px) { return static_cast<float>(px) / Style::fontSizeBody; }

  // The generic fontconfig families this page manages. keyPath is the stable
  // reset/override token; the title is only the visible row label.
  struct AliasField {
    std::string_view keyPath;
    std::string_view generic;
    std::string_view title;
    std::string_view subtitle;
  };

  const std::vector<AliasField>& aliasFields() {
    static const std::vector<AliasField> fields{
        {
            .keyPath = "Fonts.SansSerif",
            .generic = "sans-serif",
            .title = "Sans-serif",
            .subtitle = "Generic family used when an application requests a sans-serif font.",
        },
        {
            .keyPath = "Fonts.Serif",
            .generic = "serif",
            .title = "Serif",
            .subtitle = "Generic family used when an application requests a serif font.",
        },
        {
            .keyPath = "Fonts.Monospace",
            .generic = "monospace",
            .title = "Monospace",
            .subtitle = "Generic family used when an application requests a monospace font.",
        },
    };
    return fields;
  }

  const AliasField* findAliasField(std::string_view generic) {
    for (const AliasField& field : aliasFields()) {
      if (field.generic == generic) {
        return &field;
      }
    }
    return nullptr;
  }

  const AliasField* findAliasFieldByKeyPath(std::string_view keyPath) {
    for (const AliasField& field : aliasFields()) {
      if (field.keyPath == keyPath) {
        return &field;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool
  rowMatches(const FontsPage::Filter& filter, bool overridden, std::initializer_list<std::string_view> texts) {
    if (filter.showOverriddenOnly && !overridden) {
      return false;
    }
    if (filter.searchQuery.empty()) {
      return true;
    }
    for (const std::string_view text : texts) {
      if (StringUtils::containsInsensitive(text, filter.searchQuery)) {
        return true;
      }
    }
    return false;
  }

  // The user's real config home ($XDG_CONFIG_HOME, else ~/.config). The shell
  // process (which embeds kinetic-settings) keeps the real XDG_CONFIG_HOME.
  [[nodiscard]] std::filesystem::path userConfigHome() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
      return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".config";
    }
    return {};
  }

  // fontconfig's standard user include (/etc/fonts/conf.d/50-user.conf) loads
  // every file in ~/.config/fontconfig/conf.d, so the managed aliases belong
  // there rather than in the hand-editable fonts.conf.
  [[nodiscard]] std::filesystem::path managedAliasesPath() {
    const std::filesystem::path configHome = userConfigHome();
    if (configHome.empty()) {
      return {};
    }
    return configHome / "fontconfig" / "conf.d" / "99-kineticwe.conf";
  }

  constexpr std::string_view kManagedMarker = "Managed by Kinetic Settings";

  [[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
      return {};
    }
    std::string content;
    std::string line;
    while (std::getline(in, line)) {
      content += line;
      content += '\n';
    }
    return content;
  }

  [[nodiscard]] std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&apos;";
        break;
      default:
        out += c;
        break;
      }
    }
    return out;
  }

  [[nodiscard]] std::string xmlUnescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
      if (text[i] != '&') {
        out += text[i++];
        continue;
      }
      if (text.substr(i).starts_with("&amp;")) {
        out += '&';
        i += 5;
      } else if (text.substr(i).starts_with("&lt;")) {
        out += '<';
        i += 4;
      } else if (text.substr(i).starts_with("&gt;")) {
        out += '>';
        i += 4;
      } else if (text.substr(i).starts_with("&quot;")) {
        out += '"';
        i += 6;
      } else if (text.substr(i).starts_with("&apos;")) {
        out += '\'';
        i += 6;
      } else {
        out += text[i++];
      }
    }
    return out;
  }

  // Reads the aliases back from the managed file. Each <match> rule holds the
  // generic family in the first <string> and the preferred family in the
  // second; parsing whole match blocks keeps the read-back working even if the
  // file is pretty-printed or reformatted. Only rules for the three managed
  // generic families are accepted.
  [[nodiscard]] std::map<std::string, std::string> readManagedAliases() {
    std::map<std::string, std::string> aliases;
    const std::filesystem::path path = managedAliasesPath();
    if (path.empty()) {
      return aliases;
    }
    const std::string content = readTextFile(path);
    if (content.empty() || content.find(kManagedMarker) == std::string::npos) {
      return aliases;
    }

    constexpr std::string_view kMatchOpen = "<match";
    constexpr std::string_view kMatchClose = "</match>";
    constexpr std::string_view kFamilyTest = "name=\"family\"";
    constexpr std::string_view kFamilyTestSingle = "name='family'";
    constexpr std::string_view kOpen = "<string>";
    constexpr std::string_view kClose = "</string>";
    const std::string_view view(content);
    std::size_t pos = 0;
    while ((pos = view.find(kMatchOpen, pos)) != std::string_view::npos) {
      const std::size_t blockEnd = view.find(kMatchClose, pos + kMatchOpen.size());
      if (blockEnd == std::string_view::npos) {
        break;
      }
      const std::string_view block = view.substr(pos, blockEnd - pos);
      pos = blockEnd + kMatchClose.size();
      if (block.find(kFamilyTest) == std::string_view::npos
          && block.find(kFamilyTestSingle) == std::string_view::npos) {
        continue;
      }

      const std::size_t firstStart = block.find(kOpen);
      if (firstStart == std::string_view::npos) {
        continue;
      }
      const std::size_t firstEnd = block.find(kClose, firstStart + kOpen.size());
      if (firstEnd == std::string_view::npos) {
        continue;
      }
      const std::size_t secondStart = block.find(kOpen, firstEnd + kClose.size());
      if (secondStart == std::string_view::npos) {
        continue;
      }
      const std::size_t secondEnd = block.find(kClose, secondStart + kOpen.size());
      if (secondEnd == std::string_view::npos) {
        continue;
      }

      const std::string generic = xmlUnescape(
          block.substr(firstStart + kOpen.size(), firstEnd - firstStart - kOpen.size())
      );
      const std::string family = xmlUnescape(
          block.substr(secondStart + kOpen.size(), secondEnd - secondStart - kOpen.size())
      );
      if (generic == "sans-serif" || generic == "serif" || generic == "monospace") {
        aliases[generic] = family;
      }
    }
    return aliases;
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

  [[nodiscard]] std::unique_ptr<Node> makeInfoNotice(std::string_view text, float scale) {
    auto notice = ui::column({
        .align = FlexAlign::Start,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.45F),
        .radius = Style::scaledRadiusMd(scale),
        .fillWidth = true,
    });
    notice->addChild(makeLabel(text, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)));
    return notice;
  }

  [[nodiscard]] std::unique_ptr<Node> makeErrorNotice(std::string_view text, float scale) {
    return ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .paddingV = Style::spaceXs * scale,
         .paddingH = Style::spaceMd * scale,
         .fill = colorSpecFromRole(ColorRole::Error, 0.15F),
         .radius = Style::scaledRadiusMd(scale)},
        makeLabel(text, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Error))
    );
  }

  [[nodiscard]] std::unique_ptr<Button> makeResetButton(
      std::string path, bool pendingConfirmation, float scale, const FontsPage::Callbacks& callbacks
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

  [[nodiscard]] std::unique_ptr<Node> makeSectionTitle(std::string_view title, float scale) {
    return makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold);
  }

  [[nodiscard]] std::unique_ptr<Button> makeValueButton(
      std::string text, float scale, std::function<void()> requestRebuild, std::function<void()> openPicker
  ) {
    // The leading chevron mirrors the other font selectors; clicking the value
    // toggles the inline picker.
    return ui::button({
        .text = std::move(text),
        .glyph = "chevron-down",
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .contentAlign = ButtonContentAlign::Start,
        .width = kValueButtonWidth * scale,
        .onClick = [requestRebuild = std::move(requestRebuild), openPicker = std::move(openPicker)]() {
          openPicker();
          if (requestRebuild) {
            requestRebuild();
          }
        },
    });
  }

} // namespace

FontsPage::FontsPage(ConfigService& config) : m_config(config) {}

// ---------------------------------------------------------------------------
// Effective values.
// ---------------------------------------------------------------------------

std::string FontsPage::interfaceValue() const {
  if (m_pendingInterface.has_value()) {
    return *m_pendingInterface;
  }
  const std::string configured = m_config.config().shell.fontFamily;
  if (configured.empty() || configured == "sans-serif") {
    return {};
  }
  return configured;
}

std::string FontsPage::interfaceFamily() const {
  const std::string value = interfaceValue();
  return value.empty() ? std::string("sans-serif") : value;
}

std::string FontsPage::interfaceButtonLabel() const {
  const std::string value = interfaceValue();
  return value.empty() ? std::string("System default (sans-serif)") : value;
}

bool FontsPage::interfaceOverridden() const {
  return !interfaceValue().empty();
}

int FontsPage::fontSizeValue() const {
  if (m_pendingFontSize.has_value()) {
    return *m_pendingFontSize;
  }
  return fontSizePxFromScale(m_config.config().accessibility.fontScale);
}

bool FontsPage::fontSizeOverridden() const {
  return fontSizeValue() != kDefaultFontSize;
}

std::string FontsPage::aliasValue(std::string_view generic) const {
  const std::string key(generic);
  const auto pending = m_pendingAliases.find(key);
  if (pending != m_pendingAliases.end()) {
    return pending->second;
  }
  const auto stored = m_aliases.find(key);
  return stored != m_aliases.end() ? stored->second : std::string{};
}

std::string FontsPage::aliasFamily(std::string_view generic) const {
  const std::string value = aliasValue(generic);
  return value.empty() ? std::string(generic) : value;
}

std::string FontsPage::aliasButtonLabel(std::string_view generic) const {
  const std::string value = aliasValue(generic);
  return value.empty() ? std::string("System default") : value;
}

bool FontsPage::aliasOverridden(std::string_view generic) const {
  return !aliasValue(generic).empty();
}

void FontsPage::ensureAliasesLoaded() {
  if (!m_aliasesLoaded) {
    m_aliases = readManagedAliases();
    m_aliasesLoaded = true;
  }
}

// ---------------------------------------------------------------------------
// Staging + reset.
// ---------------------------------------------------------------------------

void FontsPage::stageInterface(std::string value) {
  if (value == "sans-serif") {
    // "sans-serif" is the built-in default, not an override.
    value.clear();
  }
  const std::string configured = m_config.config().shell.fontFamily;
  const std::string fileValue = (configured.empty() || configured == "sans-serif") ? std::string{} : configured;
  if (value == fileValue) {
    m_pendingInterface.reset();
  } else {
    m_pendingInterface = std::move(value);
  }
}

void FontsPage::stageFontSize(int value) {
  const int clamped = std::clamp(value, kMinFontSize, kMaxFontSize);
  const int fileValue = fontSizePxFromScale(m_config.config().accessibility.fontScale);
  if (clamped == fileValue) {
    m_pendingFontSize.reset();
  } else {
    m_pendingFontSize = clamped;
  }
}

void FontsPage::stageAlias(std::string generic, std::string value) {
  ensureAliasesLoaded();
  const auto stored = m_aliases.find(generic);
  const std::string fileValue = stored != m_aliases.end() ? stored->second : std::string{};
  if (value == fileValue) {
    m_pendingAliases.erase(generic);
  } else {
    m_pendingAliases[std::move(generic)] = std::move(value);
  }
}

std::vector<std::string> FontsPage::resettableKeyPaths() const {
  std::vector<std::string> paths;
  paths.reserve(aliasFields().size() + 2);
  paths.emplace_back(kInterfaceKeyPath);
  paths.emplace_back(kFontSizeKeyPath);
  for (const AliasField& field : aliasFields()) {
    paths.emplace_back(field.keyPath);
  }
  return paths;
}

void FontsPage::resetKeyPath(std::string_view keyPath) {
  if (keyPath == kInterfaceKeyPath) {
    stageInterface({});
    return;
  }
  if (keyPath == kFontSizeKeyPath) {
    stageFontSize(kDefaultFontSize);
    return;
  }
  if (const AliasField* field = findAliasFieldByKeyPath(keyPath); field != nullptr) {
    stageAlias(std::string(field->generic), {});
  }
}

void FontsPage::resetAll() {
  stageInterface({});
  stageFontSize(kDefaultFontSize);
  for (const AliasField& field : aliasFields()) {
    stageAlias(std::string(field.generic), {});
  }
}

bool FontsPage::hasPending() const noexcept {
  return m_pendingInterface.has_value() || m_pendingFontSize.has_value() || !m_pendingAliases.empty();
}

// ---------------------------------------------------------------------------
// Persistence: ConfigService for the shell font, fontconfig file for aliases.
// ---------------------------------------------------------------------------

bool FontsPage::writeAliases(const std::map<std::string, std::string>& aliases) {
  const std::filesystem::path path = managedAliasesPath();
  if (path.empty()) {
    m_errorNotice = "Could not resolve the fontconfig configuration directory.";
    return false;
  }

  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory) {
    m_errorNotice = "Failed to inspect the fontconfig configuration: " + ec.message()
        + ". Your changes were kept; try Apply again.";
    kLog.warn("kinetic-settings: failed to inspect {}: {}", path.string(), ec.message());
    return false;
  }
  if (exists) {
    const std::string existing = readTextFile(path);
    if (!existing.empty() && existing.find(kManagedMarker) == std::string::npos) {
      m_errorNotice = "The fontconfig file " + path.string()
          + " is not managed by Kinetic Settings. Remove or rename it, then try Apply again.";
      kLog.warn("kinetic-settings: refusing to overwrite unmanaged file {}", path.string());
      return false;
    }
  }

  bool anyAlias = false;
  for (const AliasField& field : aliasFields()) {
    const auto alias = aliases.find(std::string(field.generic));
    if (alias != aliases.end() && !alias->second.empty()) {
      anyAlias = true;
      break;
    }
  }

  if (!anyAlias) {
    // All three aliases are back to the system defaults; drop the managed file
    // instead of leaving an empty configuration behind.
    if (!exists) {
      return true;
    }
    std::filesystem::remove(path, ec);
    if (ec) {
      m_errorNotice = "Failed to remove the fontconfig configuration: " + ec.message()
          + ". Your changes were kept; try Apply again.";
      kLog.warn("kinetic-settings: failed to remove {}: {}", path.string(), ec.message());
      return false;
    }
    return true;
  }

  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    m_errorNotice = "Failed to create the fontconfig configuration directory: " + ec.message()
        + ". Your changes were kept; try Apply again.";
    kLog.warn("kinetic-settings: failed to create {}: {}", path.parent_path().string(), ec.message());
    return false;
  }

  const std::filesystem::path temp = path.parent_path() / (path.filename().string() + ".tmp");
  {
    std::ofstream out(temp, std::ios::trunc);
    if (!out.is_open()) {
      m_errorNotice = "Failed to write the fontconfig configuration. Your changes were kept; try Apply again.";
      kLog.warn("kinetic-settings: failed to open {} for writing", temp.string());
      return false;
    }

    out << "<?xml version=\"1.0\"?>\n";
    out << "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n";
    out << "<!-- Managed by Kinetic Settings. Changes to this file are overwritten. -->\n";
    out << "<fontconfig>\n";
    for (const AliasField& field : aliasFields()) {
      const auto alias = aliases.find(std::string(field.generic));
      if (alias == aliases.end() || alias->second.empty()) {
        continue;
      }
      out << "  <match target=\"pattern\"><test qual=\"any\" name=\"family\"><string>" << field.generic
          << "</string></test><edit name=\"family\" mode=\"assign\" binding=\"same\"><string>"
          << xmlEscape(alias->second) << "</string></edit></match>\n";
    }
    out << "</fontconfig>\n";
    if (!out) {
      m_errorNotice = "Failed to write the fontconfig configuration. Your changes were kept; try Apply again.";
      kLog.warn("kinetic-settings: failed to write {}", temp.string());
      out.close();
      std::filesystem::remove(temp, ec);
      return false;
    }
  }

  std::filesystem::rename(temp, path, ec);
  if (ec) {
    m_errorNotice = "Failed to write the fontconfig configuration: " + ec.message()
        + ". Your changes were kept; try Apply again.";
    kLog.warn("kinetic-settings: failed to rename {} to {}: {}", temp.string(), path.string(), ec.message());
    std::error_code removeError;
    std::filesystem::remove(temp, removeError);
    return false;
  }

  return true;
}

bool FontsPage::applyAll() {
  if (!m_pendingInterface.has_value() && !m_pendingFontSize.has_value() && m_pendingAliases.empty()) {
    m_errorNotice.clear();
    return true;
  }

  // The interface family and size go in one commit so a single reload applies the pair.
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
  if (m_pendingInterface.has_value()) {
    // Persist an explicit sans-serif override: the custom family may come
    // from a hand-authored config file rather than the override layer, so
    // merely clearing the override would not reliably restore the default.
    const std::string value = m_pendingInterface->empty() ? std::string("sans-serif") : *m_pendingInterface;
    overrides.emplace_back(std::vector<std::string>{"shell", "font_family"}, value);
  }
  if (m_pendingFontSize.has_value()) {
    overrides.emplace_back(
        std::vector<std::string>{"accessibility", "font_scale"},
        static_cast<double>(fontSizeScaleFromPx(*m_pendingFontSize))
    );
  }

  if (!overrides.empty()) {
    if (!m_config.setOverrides(overrides)) {
      m_errorNotice = m_config.lastMutationError();
      if (m_errorNotice.empty()) {
        m_errorNotice = "Failed to apply the interface font settings. Your changes were kept; try Apply again.";
      }
      kLog.warn("kinetic-settings: failed to apply interface font settings");
      return false;
    }
    m_pendingInterface.reset();
    m_pendingFontSize.reset();
  }

  if (!m_pendingAliases.empty()) {
    ensureAliasesLoaded();
    std::map<std::string, std::string> effective = m_aliases;
    for (const auto& [generic, value] : m_pendingAliases) {
      effective[generic] = value;
    }
    if (!writeAliases(effective)) {
      return false;
    }
    m_aliases = std::move(effective);
    m_pendingAliases.clear();
  }

  m_errorNotice.clear();
  m_pickerTarget.clear();
  return true;
}

void FontsPage::reloadFromDisk() noexcept {
  m_pendingInterface.reset();
  m_pendingFontSize.reset();
  m_pendingAliases.clear();
  m_aliases.clear();
  m_aliasesLoaded = false;
  m_pickerTarget.clear();
  m_errorNotice.clear();
}

// ---------------------------------------------------------------------------
// Controls + scene.
// ---------------------------------------------------------------------------

std::unique_ptr<Node> FontsPage::buildFontPicker(
    float scale, std::string_view target, std::string_view selectedValue, const Callbacks& callbacks
) {
  const bool interfaceTarget = target == kPickerInterface;
  const std::string current(selectedValue);

  std::vector<SearchPickerOption> options;
  options.push_back(SearchPickerOption{
      .value = {},
      .label = interfaceTarget ? "System default (sans-serif)" : "System default",
  });

  bool currentInstalled = current.empty();
  std::string pickerValue = current;
  for (const std::string& family : font_discovery::families()) {
    if (StringUtils::equalsInsensitive(family, current)) {
      currentInstalled = true;
      pickerValue = family;
    }
    options.push_back(SearchPickerOption{
        .value = family,
        .label = family,
        .icon = "typeface",
    });
  }
  if (!current.empty() && !currentInstalled) {
    options.push_back(SearchPickerOption{
        .value = current,
        .label = current + " (not installed)",
        .icon = "typeface",
    });
  }

  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  column->addChild(ui::searchPicker({
      .placeholder = "Search fonts...",
      .emptyText = "No fonts found.",
      .selectedValue = pickerValue,
      .options = std::move(options),
      .height = 320.0F * scale,
      .onActivated =
          [this, interfaceTarget, generic = std::string(target), callbacks](const SearchPickerOption& option) {
            if (interfaceTarget) {
              stageInterface(option.value);
            } else {
              stageAlias(generic, option.value);
            }
            m_pickerTarget.clear();
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
      .onCancel =
          [this, callbacks]() {
            m_pickerTarget.clear();
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
                m_pickerTarget.clear();
                if (callbacks.requestRebuild) {
                  callbacks.requestRebuild();
                }
              },
      })
  ));
  return column;
}

std::unique_ptr<Node>
FontsPage::buildInterfaceRow(float scale, std::string_view armedResetKey, const Callbacks& callbacks) {
  const bool overridden = interfaceOverridden();
  const std::string keyPath(kInterfaceKeyPath);
  const std::string family = interfaceFamily();
  // The renderer already applies the saved font scale globally; scale the
  // preview from the saved size so a staged change previews its real size.
  const int appliedFontSize = fontSizePxFromScale(m_config.config().accessibility.fontScale);
  const float previewFontScale = static_cast<float>(fontSizeValue()) / static_cast<float>(appliedFontSize);

  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(makeLabel(
      "Interface font", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  copy->addChild(ui::label({
      .text = std::string(kInterfaceSubtitle),
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = kSettingDescriptionMaxLines,
  }));

  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (overridden) {
    actions->addChild(makeOverrideBadge(scale));
    actions->addChild(makeResetButton(keyPath, armedResetKey == keyPath, scale, callbacks));
  }
  actions->addChild(makeValueButton(
      interfaceButtonLabel(), scale, callbacks.requestRebuild,
      [this, target = std::string(kPickerInterface)]() {
        m_pickerTarget = m_pickerTarget == target ? std::string{} : target;
      }
  ));

  auto row = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  row->addChild(std::move(copy));
  row->addChild(std::move(actions));

  auto preview = ui::column({
      .align = FlexAlign::Start,
      .gap = Style::spaceXs * scale,
      .padding = Style::spaceMd * scale,
      .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.45F),
      .radius = Style::scaledRadiusMd(scale),
      .fillWidth = true,
  });
  preview->addChild(ui::label({
      .text = std::string(kInterfaceSample),
      .fontSize = Style::fontSizeTitle * scale * previewFontScale,
      .fontFamily = family,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  preview->addChild(ui::label({
      .text = "0123456789",
      .fontSize = Style::fontSizeBody * scale * previewFontScale,
      .fontFamily = family,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));

  auto container = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  container->addChild(std::move(row));
  container->addChild(std::move(preview));
  if (m_pickerTarget == kPickerInterface) {
    container->addChild(buildFontPicker(scale, kPickerInterface, interfaceValue(), callbacks));
  }
  return container;
}

std::unique_ptr<Node>
FontsPage::buildFontSizeRow(float scale, std::string_view armedResetKey, const Callbacks& callbacks) {
  const bool overridden = fontSizeOverridden();
  const std::string keyPath(kFontSizeKeyPath);

  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(makeLabel(
      "Font size", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  copy->addChild(ui::label({
      .text = std::string(kFontSizeSubtitle),
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = kSettingDescriptionMaxLines,
  }));

  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (overridden) {
    actions->addChild(makeOverrideBadge(scale));
    actions->addChild(makeResetButton(keyPath, armedResetKey == keyPath, scale, callbacks));
  }
  actions->addChild(ui::stepper({
      .minValue = kMinFontSize,
      .maxValue = kMaxFontSize,
      .step = 1,
      .value = fontSizeValue(),
      .scale = scale,
      .valueSuffix = " px",
      .onValueCommitted =
          [this, callbacks](int committed) {
            stageFontSize(committed);
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
  }));

  auto row = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  row->addChild(std::move(copy));
  row->addChild(std::move(actions));

  auto container = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  container->addChild(std::move(row));
  return container;
}

std::unique_ptr<Node> FontsPage::buildAliasRow(
    std::string_view generic, float scale, std::string_view armedResetKey, const Callbacks& callbacks
) {
  const AliasField* field = findAliasField(generic);
  if (field == nullptr) {
    return {};
  }

  const bool overridden = aliasOverridden(generic);
  const std::string keyPath(field->keyPath);
  const std::string target(generic);
  const std::string family = aliasFamily(generic);

  auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
  copy->addChild(makeLabel(
      field->title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
  ));
  copy->addChild(ui::label({
      .text = std::string(field->subtitle),
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = kSettingDescriptionMaxLines,
  }));
  copy->addChild(ui::label({
      .text = std::string(kAliasSample),
      .fontSize = Style::fontSizeBody * scale,
      .fontFamily = family,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));

  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (overridden) {
    actions->addChild(makeOverrideBadge(scale));
    actions->addChild(makeResetButton(keyPath, armedResetKey == keyPath, scale, callbacks));
  }
  actions->addChild(makeValueButton(
      aliasButtonLabel(generic), scale, callbacks.requestRebuild,
      [this, target]() {
        m_pickerTarget = m_pickerTarget == target ? std::string{} : target;
      }
  ));

  auto row = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceXs * scale,
       .paddingV = 2.0F * scale,
       .fillWidth = true}
  );
  row->addChild(std::move(copy));
  row->addChild(std::move(actions));

  auto container = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
  container->addChild(std::move(row));
  if (m_pickerTarget == target) {
    container->addChild(buildFontPicker(scale, target, aliasValue(generic), callbacks));
  }
  return container;
}

std::unique_ptr<Node> FontsPage::build(
    float scale, const Filter& filter, std::string_view armedResetKey, const Callbacks& callbacks
) {
  ensureAliasesLoaded();

  const std::string interfaceLabel = interfaceButtonLabel();
  const bool showInterface = rowMatches(
      filter, interfaceOverridden(),
      {"Interface font", kInterfaceSubtitle, "Interface", interfaceLabel}
  );

  const std::string fontSizeLabel = std::to_string(fontSizeValue()) + " px";
  const bool showFontSize = rowMatches(
      filter, fontSizeOverridden(), {"Font size", kFontSizeSubtitle, "Interface", fontSizeLabel}
  );

  std::vector<const AliasField*> visibleAliases;
  for (const AliasField& field : aliasFields()) {
    const std::string aliasLabel = aliasButtonLabel(field.generic);
    if (rowMatches(
            filter, aliasOverridden(field.generic),
            {field.title, field.subtitle, "System font preferences", aliasLabel}
        )) {
      visibleAliases.push_back(&field);
    }
  }

  // A picker whose row was filtered out (or navigated away from) must not
  // reappear unexpectedly when the row becomes visible again.
  if (!m_pickerTarget.empty()) {
    if (m_pickerTarget == kPickerInterface) {
      if (!showInterface) {
        m_pickerTarget.clear();
      }
    } else {
      const bool targetVisible = std::ranges::any_of(
          visibleAliases, [this](const AliasField* field) { return field->generic == m_pickerTarget; }
      );
      if (!targetVisible) {
        m_pickerTarget.clear();
      }
    }
  }

  if (!showInterface && !showFontSize && visibleAliases.empty()) {
    // Global search merges hits from every page and drops pages with none.
    if (!filter.searchQuery.empty()) {
      return nullptr;
    }
    auto emptyRoot = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceLg * scale,
        .fill = clearColorSpec(),
        .fillWidth = true,
    });
    emptyRoot->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::glyph({
            .glyph = "typeface",
            .glyphSize = Style::fontSizeHeader * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        }),
        makeLabel("Fonts", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
    ));
    emptyRoot->addChild(makeLabel(
        kEmptyState, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
    if (!m_errorNotice.empty()) {
      emptyRoot->addChild(makeErrorNotice(m_errorNotice, scale));
    }
    return emptyRoot;
  }

  const auto& families = font_discovery::families();

  auto root = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceLg * scale,
      .fill = clearColorSpec(),
      .fillWidth = true,
  });

  auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
  header->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .glyph = "typeface",
          .glyphSize = Style::fontSizeHeader * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      makeLabel("Fonts", Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
  ));
  header->addChild(ui::spacer());
  header->addChild(ui::button({
      .text = "Open fonts folder",
      .glyph = "folder-open",
      .fontSize = Style::fontSizeCaption * scale,
      .glyphSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Ghost,
      .minHeight = Style::controlHeightSm * scale,
      .paddingV = Style::spaceXs * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick =
          [this, callbacks]() {
            if (!font_discovery::openUserFontsDirectory()) {
              m_errorNotice = "Failed to open the fonts folder.";
              kLog.warn("kinetic-settings: failed to open the user fonts folder");
            } else if (!hasPending()) {
              m_errorNotice.clear();
            }
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
  }));
  header->addChild(ui::button({
      .text = "Refresh font list",
      .glyph = "refresh",
      .fontSize = Style::fontSizeCaption * scale,
      .glyphSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Ghost,
      .minHeight = Style::controlHeightSm * scale,
      .paddingV = Style::spaceXs * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick =
          [this, callbacks]() {
            bool ok = true;
            if (process::commandExists("fc-cache")) {
              const auto result = process::runSyncWithTimeout({"fc-cache", "-f"}, std::chrono::seconds(15));
              if (!result) {
                ok = false;
                m_errorNotice = result.timedOut
                    ? "Refreshing the font cache timed out. The font list may be incomplete."
                    : "Failed to refresh the font cache. The font list may be incomplete.";
                kLog.warn(
                    "kinetic-settings: fc-cache failed (exit {}, timed out {})", result.exitCode, result.timedOut
                );
              }
            }
            font_discovery::refreshFamilies();
            if (ok && !hasPending()) {
              m_errorNotice.clear();
            }
            if (callbacks.requestRebuild) {
              callbacks.requestRebuild();
            }
          },
  }));
  root->addChild(std::move(header));

  root->addChild(makeLabel(
      kPageDescription, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
  ));

  if (!m_errorNotice.empty()) {
    root->addChild(makeErrorNotice(m_errorNotice, scale));
  }

  bool sectionStarted = false;
  if (showInterface || showFontSize) {
    root->addChild(makeSectionTitle("Interface", scale));
    if (showInterface) {
      if (families.empty()) {
        root->addChild(makeInfoNotice(kNoFamilies, scale));
      }
      root->addChild(buildInterfaceRow(scale, armedResetKey, callbacks));
    }
    if (showFontSize) {
      root->addChild(buildFontSizeRow(scale, armedResetKey, callbacks));
    }
    sectionStarted = true;
  }

  if (!visibleAliases.empty()) {
    if (sectionStarted) {
      root->addChild(ui::separator());
    }
    auto systemHeader = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    systemHeader->addChild(makeSectionTitle("System font preferences", scale));
    systemHeader->addChild(ui::label({
        .text = std::string(kSystemDescription),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = kSettingDescriptionMaxLines,
    }));
    root->addChild(std::move(systemHeader));

    for (const AliasField* field : visibleAliases) {
      root->addChild(buildAliasRow(field->generic, scale, armedResetKey, callbacks));
    }
  }

  return root;
}
