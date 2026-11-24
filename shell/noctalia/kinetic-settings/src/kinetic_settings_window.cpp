#include "kinetic_settings_window.h"

#include "compositors/compositor_platform.h"
#include "core/input/key_symbols.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/wayland/shell_globals.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/controls/separator.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace {

  constexpr Logger kLog("kinetic-settings");

  // Coalesces rapid typing into one cross-page rebuild. Global search builds
  // every page, so rebuilding on each keystroke is noticeably wasteful.
  constexpr auto kSearchDebounceInterval = std::chrono::milliseconds(120);

  constexpr float kWindowOutputFraction = 0.66F;
  constexpr float kGoldenRatio = std::numbers::phi_v<float>;
  constexpr float kWindowWidth = 1100.0F;
  constexpr float kWindowHeight = 620.0F;
  constexpr float kWindowMinWidth = 860.0F;
  constexpr float kWindowMinHeight = 480.0F;

  constexpr float kBackgroundOpacity = 1.0F;
  // Sidebar geometry mirrors Noctalia's settings_sidebar.cpp, widened slightly
  // so the larger category rows keep "Window Management" fully visible.
  constexpr float kSidebarWidth = 220.0F;
  constexpr float kSidebarPadding = 6.0F;
  constexpr float kSidebarGap = 2.0F;
  constexpr float kPrimaryNavGlyphSize = 18.0F;
  constexpr float kPrimaryNavGap = 6.0F;
  constexpr float kPrimaryNavPaddingH = 10.0F;

  constexpr std::string_view kAppId = "dev.kineticwe.Settings";

  void focusWindow(CompositorPlatform& platform, WaylandConnection& wayland, wl_surface* surface) {
    wayland.activateSurface(surface);
    platform.activateToplevelForAppId(kAppId);
  }

  void removeAllChildren(Flex& flex) {
    while (!flex.children().empty()) {
      (void)flex.removeChild(flex.children().back().get());
    }
  }

} // namespace

KineticSettingsWindow::KineticSettingsWindow() = default;

KineticSettingsWindow::~KineticSettingsWindow() = default;

void KineticSettingsWindow::initialize(
    WaylandConnection& wayland, CompositorPlatform& platform, RenderContext& renderContext, SessionBus& bus,
    ConfigService& config, CustomKeybinds& customKeybinds, float uiScale
) {
  m_wayland = &wayland;
  m_platform = &platform;
  m_renderContext = &renderContext;
  m_bus = &bus;
  m_config = &config;
  m_customKeybinds = &customKeybinds;
  m_uiScale = std::max(0.1F, uiScale);
  m_fontsPage = std::make_unique<FontsPage>(*m_config);
}

void KineticSettingsWindow::open() {
  if (m_wayland == nullptr || m_renderContext == nullptr || !m_wayland->hasXdgShell()) {
    return;
  }
  if (isOpen()) {
    focusWindow(*m_platform, *m_wayland, m_surface->wlSurface());
    return;
  }

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    const auto& outputs = m_wayland->outputs();
    if (!outputs.empty() && outputs.front().output != nullptr) {
      output = outputs.front().output;
    }
  }
  if (output == nullptr) {
    return;
  }
  m_output = output;

  const float scale = uiScale();
  float desiredWidth = kWindowWidth * scale;
  float desiredHeight = kWindowHeight * scale;
  if (const WaylandOutput* info = m_wayland->findOutputByWl(output); info != nullptr && info->hasUsableGeometry()) {
    const float outputW = static_cast<float>(info->effectiveLogicalWidth());
    const float outputH = static_cast<float>(info->effectiveLogicalHeight());
    if (outputW >= outputH) {
      desiredHeight = std::clamp(outputH * kWindowOutputFraction, kWindowMinHeight * scale, outputH);
      desiredWidth = std::clamp(desiredHeight * kGoldenRatio, kWindowMinWidth * scale, outputW);
    } else {
      desiredWidth = std::clamp(outputW * kWindowOutputFraction, kWindowMinWidth * scale, outputW);
      desiredHeight = std::clamp(desiredWidth * kGoldenRatio, kWindowMinHeight * scale, outputH);
    }
  }

  m_surface = std::make_unique<ToplevelSurface>(*m_wayland);
  m_surface->setRenderContext(m_renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setClosedCallback([this]() { destroyWindow(); });
  m_surface->setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  });
  m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  m_surface->setUpdateCallback([]() {});

  ToplevelSurfaceConfig cfg{
      .width = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::round(desiredWidth))),
      .height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::round(desiredHeight))),
      .minWidth = static_cast<std::uint32_t>(std::round(kWindowMinWidth * scale)),
      .minHeight = static_cast<std::uint32_t>(std::round(kWindowMinHeight * scale)),
      .title = "Kinetic Settings",
      .appId = "dev.kineticwe.Settings",
      .decorated = true,
  };
  if (!m_surface->initialize(output, cfg)) {
    kLog.warn("kinetic-settings: failed to create toplevel surface");
    m_surface.reset();
    return;
  }

  m_rebuildRequested = true;
  m_contentRebuildRequested = false;
  m_inputDispatcher.setSceneRoot(nullptr);
  if (m_tilingPage == nullptr) {
    m_tilingPage = std::make_unique<TilingPage>(*m_bus);
  }
  if (m_workspacesPage == nullptr) {
    m_workspacesPage = std::make_unique<WorkspacesPage>(*m_bus);
  }
  if (m_keybindsPage == nullptr) {
    m_keybindsPage = std::make_unique<KeybindsPage>(*m_bus, *m_customKeybinds);
  }
  if (m_keyboardPage == nullptr) {
    m_keyboardPage = std::make_unique<KeyboardPage>(*m_bus);
  }
  if (m_mousePage == nullptr) {
    m_mousePage = std::make_unique<PointingDevicesPage>(*m_bus, PointingDevicesPage::Kind::Mouse);
  }
  if (m_touchpadPage == nullptr) {
    m_touchpadPage = std::make_unique<PointingDevicesPage>(*m_bus, PointingDevicesPage::Kind::Touchpad);
  }
  if (m_windowRulesPage == nullptr) {
    m_windowRulesPage = std::make_unique<WindowRulesPage>(*m_bus, *m_wayland);
  }
  if (m_windowBehaviorPage == nullptr) {
    m_windowBehaviorPage = std::make_unique<WindowBehaviorPage>(*m_bus);
  }
  if (m_displayPage == nullptr) {
    m_displayPage = std::make_unique<DisplayPage>(*m_wayland);
    m_displayPage->setOutputChangeCallback([this]() {
      if (m_activePage == ActivePage::Displays) {
        requestContentRebuild();
      }
    });
  }
  if (m_hotCornersPage == nullptr) {
    m_hotCornersPage = std::make_unique<HotCornersPage>(*m_bus);
  }
  if (m_appearancePage == nullptr) {
    m_appearancePage = std::make_unique<AppearancePage>(*m_bus);
  }
  if (m_iconsPage == nullptr) {
    m_iconsPage = std::make_unique<IconsPage>();
  }
  if (m_cursorsPage == nullptr) {
    m_cursorsPage = std::make_unique<CursorsPage>();
  }
  if (m_desktopEffectsPage == nullptr) {
    m_desktopEffectsPage = std::make_unique<DesktopEffectsPage>(*m_bus);
  }
  if (m_blurPage == nullptr) {
    m_blurPage = std::make_unique<BlurPage>(*m_bus);
  }
  if (m_shadowsPage == nullptr) {
    m_shadowsPage = std::make_unique<ShadowsPage>(*m_bus);
  }
  if (m_animationsPage == nullptr) {
    m_animationsPage = std::make_unique<AnimationsPage>(*m_bus);
  }
  if (m_defaultAppsPage == nullptr) {
    m_defaultAppsPage = std::make_unique<DefaultAppsPage>();
  }
  if (m_accessibilityPage == nullptr) {
    m_accessibilityPage = std::make_unique<AccessibilityPage>(*m_bus);
  }
  if (m_autostartPage == nullptr) {
    m_autostartPage = std::make_unique<AutostartPage>();
  }
  m_tilingPage->reloadFromDisk();
  m_workspacesPage->reloadFromDisk();
  m_keybindsPage->reloadFromDisk();
  m_keyboardPage->reloadFromDisk();
  m_mousePage->reloadFromDisk();
  m_touchpadPage->reloadFromDisk();
  m_windowRulesPage->reloadFromDisk();
  m_windowBehaviorPage->reloadFromDisk();
  m_appearancePage->reloadFromDisk();
  m_iconsPage->reloadFromDisk();
  m_cursorsPage->reloadFromDisk();
  m_desktopEffectsPage->reloadFromDisk();
  m_blurPage->reloadFromDisk();
  m_shadowsPage->reloadFromDisk();
  m_animationsPage->reloadFromDisk();
  m_defaultAppsPage->reloadFromDisk();
  m_accessibilityPage->reloadFromDisk();
  m_autostartPage->reloadFromDisk();
  m_fontsPage->reloadFromDisk();
  m_displayPage->reloadFromDisk();
  m_hotCornersPage->reloadFromDisk();
  m_selectPopup = std::make_unique<SelectDropdownPopup>(*m_wayland, *m_renderContext);
  m_selectPopup->setParent(m_surface->xdgSurface(), m_surface->wlSurface(), m_output);
  m_surface->requestLayout();
  focusWindow(*m_platform, *m_wayland, m_surface->wlSurface());
}

void KineticSettingsWindow::close() {
  destroyWindow();
}

void KineticSettingsWindow::toggle() {
  if (isOpen()) {
    close();
  } else {
    open();
  }
}

void KineticSettingsWindow::destroyWindow() {
  if (m_shortcutInhibitor != nullptr) {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_shortcutInhibitor);
    m_shortcutInhibitor = nullptr;
  }
  m_inputDispatcher.pointerLeave();
  m_inputDispatcher.setSceneRoot(nullptr);
  if (m_selectPopup != nullptr) {
    m_selectPopup->closeSelectDropdown();
    m_selectPopup.reset();
  }
  if (m_surface != nullptr) {
    m_surface->setSceneRoot(nullptr);
  }
  m_sceneRoot.reset();
  m_surface.reset();
  m_output = nullptr;
  m_background = nullptr;
  m_pageContainer = nullptr;
  m_mainContainer = nullptr;
  m_searchInput = nullptr;
  m_focusSearchOnRebuild = false;
  m_searchDebounceTimer.stop();

  // Fresh filters on the next open (Noctalia resets search + Overridden the
  // same way); the Advanced toggle keeps its session value.
  m_searchQuery.clear();
  m_showOverriddenOnly = false;
  clearArmedResets();
  m_sidebarScrollState = {};
  m_contentScrollState = {};
}

bool KineticSettingsWindow::ownsKeyboardSurface(wl_surface* surface) const noexcept {
  if (m_surface != nullptr && surface == m_surface->wlSurface()) {
    return true;
  }
  return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()
         && surface == m_selectPopup->wlSurface();
}

void KineticSettingsWindow::prepareFrame(bool /*needsUpdate*/, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  const auto width = m_surface->width();
  const auto height = m_surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());
  auto& renderer = m_surface->renderTarget().renderer();

  const float floatWidth = static_cast<float>(width);
  const float floatHeight = static_cast<float>(height);
  const bool firstBuild = m_sceneRoot == nullptr;
  const bool sizeChanged = !firstBuild && (m_lastSceneWidth != floatWidth || m_lastSceneHeight != floatHeight);
  if (firstBuild || m_rebuildRequested) {
    buildScene(floatWidth, floatHeight);
    m_lastSceneWidth = floatWidth;
    m_lastSceneHeight = floatHeight;
    m_rebuildRequested = false;
    m_contentRebuildRequested = false;
  } else if (m_contentRebuildRequested && m_sceneRoot != nullptr) {
    // Content-only rebuild (page inside the scroll view). The filter row —
    // including the search input and its focus — survives.
    rebuildContent(renderer);
    m_contentRebuildRequested = false;
    m_sceneRoot->setSize(floatWidth, floatHeight);
    resizeSceneChrome(floatWidth, floatHeight);
    m_sceneRoot->layout(renderer);
    m_lastSceneWidth = floatWidth;
    m_lastSceneHeight = floatHeight;
  } else if (m_sceneRoot != nullptr && (sizeChanged || needsLayout)) {
    m_sceneRoot->setSize(floatWidth, floatHeight);
    resizeSceneChrome(floatWidth, floatHeight);
    m_sceneRoot->layout(renderer);
    m_lastSceneWidth = floatWidth;
    m_lastSceneHeight = floatHeight;
  }
}

void KineticSettingsWindow::resizeSceneChrome(float width, float height) {
  // The scene root is a plain Node: it never resizes its children, so the
  // chrome layers with explicit sizes must be updated before each relayout.
  if (m_background != nullptr) {
    m_background->setSize(width, height);
  }
  if (m_mainContainer != nullptr) {
    m_mainContainer->setSize(width, height);
  }
}

void KineticSettingsWindow::requestSceneRebuild() {
  if (m_surface == nullptr) {
    return;
  }
  m_rebuildRequested = true;
  m_surface->requestLayout();
}

void KineticSettingsWindow::requestContentRebuild() {
  if (m_surface == nullptr) {
    return;
  }
  if (m_pageContainer == nullptr) {
    requestSceneRebuild();
    return;
  }
  m_contentRebuildRequested = true;
  m_surface->requestLayout();
}

void KineticSettingsWindow::clearArmedResets() noexcept {
  m_armedResetKeyPath.clear();
  m_resetPageArmed = false;
}

TilingPage::Filter KineticSettingsWindow::pageFilter() const {
  return TilingPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

TilingPage::Callbacks KineticSettingsWindow::tilingPageCallbacks() {
  return TilingPage::Callbacks{
      .requestRebuild =
          [this]() {
            // Staging changes the Apply button's enabled state (filter row),
            // so content-only rebuilds are not enough here.
            clearArmedResets();
            requestSceneRebuild();
          },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_tilingPage != nullptr) {
              m_tilingPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
      .jumpRow = m_groupJumpRow,
  };
}

WorkspacesPage::Filter KineticSettingsWindow::workspacesFilter() const {
  return WorkspacesPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

WorkspacesPage::Callbacks KineticSettingsWindow::workspacesPageCallbacks() {
  return WorkspacesPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_workspacesPage != nullptr) {
              m_workspacesPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

KeybindsPage::Filter KineticSettingsWindow::keybindsFilter() const {
  return KeybindsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

KeybindsPage::Callbacks KineticSettingsWindow::keybindsPageCallbacks() {
  return KeybindsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .recordingChanged = [this](bool) { updateKeyboardShortcutInhibit(); },
      .jumpRow = m_groupJumpRow,
  };
}

KeyboardPage::Filter KineticSettingsWindow::keyboardFilter() const {
  return KeyboardPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

KeyboardPage::Callbacks KineticSettingsWindow::keyboardPageCallbacks() {
  return KeyboardPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_keyboardPage != nullptr) {
              m_keyboardPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

PointingDevicesPage::Filter KineticSettingsWindow::mouseFilter() const {
  return PointingDevicesPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

PointingDevicesPage::Callbacks KineticSettingsWindow::mousePageCallbacks() {
  return PointingDevicesPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_mousePage != nullptr) {
              m_mousePage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

PointingDevicesPage::Filter KineticSettingsWindow::touchpadFilter() const {
  return PointingDevicesPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

PointingDevicesPage::Callbacks KineticSettingsWindow::touchpadPageCallbacks() {
  return PointingDevicesPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_touchpadPage != nullptr) {
              m_touchpadPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

WindowRulesPage::Filter KineticSettingsWindow::windowRulesFilter() const {
  return WindowRulesPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

WindowRulesPage::Callbacks KineticSettingsWindow::windowRulesPageCallbacks() {
  return WindowRulesPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
  };
}

WindowBehaviorPage::Filter KineticSettingsWindow::windowBehaviorFilter() const {
  return WindowBehaviorPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

WindowBehaviorPage::Callbacks KineticSettingsWindow::windowBehaviorPageCallbacks() {
  return WindowBehaviorPage::Callbacks{
      .requestRebuild =
          [this]() {
            clearArmedResets();
            requestSceneRebuild();
          },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_windowBehaviorPage != nullptr) {
              m_windowBehaviorPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
      .jumpRow = m_groupJumpRow,
  };
}

AppearancePage::Filter KineticSettingsWindow::appearanceFilter() const {
  return AppearancePage::Filter{
      .searchQuery = m_searchQuery,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

AppearancePage::Callbacks KineticSettingsWindow::appearancePageCallbacks() {
  return AppearancePage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_appearancePage != nullptr) {
              m_appearancePage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

IconsPage::Filter KineticSettingsWindow::iconsFilter() const {
  return IconsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

IconsPage::Callbacks KineticSettingsWindow::iconsPageCallbacks() {
  return IconsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_iconsPage != nullptr) {
              m_iconsPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

CursorsPage::Filter KineticSettingsWindow::cursorsFilter() const {
  return CursorsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

CursorsPage::Callbacks KineticSettingsWindow::cursorsPageCallbacks() {
  return CursorsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_cursorsPage != nullptr) {
              m_cursorsPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

FontsPage::Filter KineticSettingsWindow::fontsFilter() const {
  return FontsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

FontsPage::Callbacks KineticSettingsWindow::fontsPageCallbacks() {
  return FontsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_fontsPage != nullptr) {
              m_fontsPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

DesktopEffectsPage::Filter KineticSettingsWindow::desktopEffectsFilter() const {
  return DesktopEffectsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

DesktopEffectsPage::Callbacks KineticSettingsWindow::desktopEffectsPageCallbacks() {
  return DesktopEffectsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_desktopEffectsPage != nullptr) {
              m_desktopEffectsPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

BlurPage::Filter KineticSettingsWindow::blurFilter() const {
  return BlurPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

BlurPage::Callbacks KineticSettingsWindow::blurPageCallbacks() {
  return BlurPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_blurPage != nullptr) {
              m_blurPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

ShadowsPage::Filter KineticSettingsWindow::shadowsFilter() const {
  return ShadowsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

ShadowsPage::Callbacks KineticSettingsWindow::shadowsPageCallbacks() {
  return ShadowsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_shadowsPage != nullptr) {
              m_shadowsPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

AnimationsPage::Filter KineticSettingsWindow::animationsFilter() const {
  return AnimationsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

AnimationsPage::Callbacks KineticSettingsWindow::animationsPageCallbacks() {
  return AnimationsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_animationsPage != nullptr) {
              m_animationsPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

DefaultAppsPage::Filter KineticSettingsWindow::defaultAppsFilter() const {
  return DefaultAppsPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

DefaultAppsPage::Callbacks KineticSettingsWindow::defaultAppsPageCallbacks() {
  return DefaultAppsPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_defaultAppsPage != nullptr) {
              m_defaultAppsPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

DisplayPage::Filter KineticSettingsWindow::displayFilter() const {
  return DisplayPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

DisplayPage::Callbacks KineticSettingsWindow::displayPageCallbacks() {
  return DisplayPage::Callbacks{
      .requestRebuild =
          [this]() {
            clearArmedResets();
            requestSceneRebuild();
          },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_displayPage != nullptr) {
              m_displayPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

HotCornersPage::Filter KineticSettingsWindow::hotCornersFilter() const {
  return HotCornersPage::Filter{
      .searchQuery = m_searchQuery,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

HotCornersPage::Callbacks KineticSettingsWindow::hotCornersPageCallbacks() {
  return HotCornersPage::Callbacks{
      .requestRebuild =
          [this]() {
            clearArmedResets();
            requestSceneRebuild();
          },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_hotCornersPage != nullptr) {
              m_hotCornersPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
      .jumpRow = m_groupJumpRow,
  };
}

AccessibilityPage::Filter KineticSettingsWindow::accessibilityFilter() const {
  return AccessibilityPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

AccessibilityPage::Callbacks KineticSettingsWindow::accessibilityPageCallbacks() {
  return AccessibilityPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
      .armReset =
          [this](std::string keyPath) {
            m_armedResetKeyPath = std::move(keyPath);
            m_resetPageArmed = false;
            requestContentRebuild();
          },
      .confirmReset =
          [this](std::string keyPath) {
            clearArmedResets();
            if (m_accessibilityPage != nullptr) {
              m_accessibilityPage->resetKeyPath(keyPath);
            }
            requestSceneRebuild();
          },
  };
}

AutostartPage::Filter KineticSettingsWindow::autostartFilter() const {
  return AutostartPage::Filter{
      .searchQuery = m_searchQuery,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
  };
}

AutostartPage::Callbacks KineticSettingsWindow::autostartPageCallbacks() {
  return AutostartPage::Callbacks{
      .requestRebuild =
          [this]() { clearArmedResets(); requestSceneRebuild(); },
  };
}

void KineticSettingsWindow::confirmResetPage() {
  clearArmedResets();
  resetActivePage();
  // The Reset Page button lives in the filter row, so rebuild the full scene.
  requestSceneRebuild();
}

void KineticSettingsWindow::applyActivePage() {
  bool applied = false;
  switch (m_activePage) {
  case ActivePage::AppearanceTitleBars:
    applied = m_appearancePage != nullptr && m_appearancePage->applyAll();
    break;
  case ActivePage::AppearanceIcons:
    applied = m_iconsPage != nullptr && m_iconsPage->applyAll();
    break;
  case ActivePage::AppearanceCursors:
    applied = m_cursorsPage != nullptr && m_cursorsPage->applyAll();
    break;
  case ActivePage::AppearanceFonts:
    applied = m_fontsPage != nullptr && m_fontsPage->applyAll();
    break;
  case ActivePage::Animations:
    applied = m_animationsPage != nullptr && m_animationsPage->applyAll();
    break;
  case ActivePage::DesktopEffects:
    applied = m_desktopEffectsPage != nullptr && m_desktopEffectsPage->applyAll();
    break;
  case ActivePage::Blur:
    applied = m_blurPage != nullptr && m_blurPage->applyAll();
    break;
  case ActivePage::AppearanceShadows:
    applied = m_shadowsPage != nullptr && m_shadowsPage->applyAll();
    break;
  case ActivePage::Tiling:
    applied = m_tilingPage != nullptr && m_tilingPage->applyAll();
    break;
  case ActivePage::WindowRules:
    applied = m_windowRulesPage != nullptr && m_windowRulesPage->applyAll();
    break;
  case ActivePage::WindowBehavior:
    applied = m_windowBehaviorPage != nullptr && m_windowBehaviorPage->applyAll();
    break;
  case ActivePage::Workspaces:
    applied = m_workspacesPage != nullptr && m_workspacesPage->applyAll();
    break;
  case ActivePage::Keyboard:
    applied = m_keyboardPage != nullptr && m_keyboardPage->applyAll();
    break;
  case ActivePage::Touchpad:
    applied = m_touchpadPage != nullptr && m_touchpadPage->applyAll();
    break;
  case ActivePage::Mouse:
    applied = m_mousePage != nullptr && m_mousePage->applyAll();
    break;
  case ActivePage::Keybinds:
    applied = m_keybindsPage != nullptr && m_keybindsPage->applyAll();
    break;
  case ActivePage::Displays:
    applied = m_displayPage != nullptr && m_displayPage->applyAll();
    break;
  case ActivePage::HotCorners:
    applied = m_hotCornersPage != nullptr && m_hotCornersPage->applyAll();
    break;
  case ActivePage::DefaultApps:
    applied = m_defaultAppsPage != nullptr && m_defaultAppsPage->applyAll();
    break;
  case ActivePage::Accessibility:
    applied = m_accessibilityPage != nullptr && m_accessibilityPage->applyAll();
    break;
  case ActivePage::Autostart:
    // The page writes on change; there is nothing to flush.
    break;
  }
  if (applied) {
    clearArmedResets();
    requestSceneRebuild();
  } else {
    // Pages surface apply failures through an on-page error notice (e.g. the
    // keybinds page when the daemon rejects a chord). Rebuild so that notice
    // is visible instead of leaving the user staring at an unresponsive Apply
    // button.
    requestSceneRebuild();
  }
}

void KineticSettingsWindow::resetActivePage() {
  switch (m_activePage) {
  case ActivePage::AppearanceTitleBars:
    if (m_appearancePage != nullptr) {
      m_appearancePage->resetAll();
    }
    break;
  case ActivePage::AppearanceIcons:
    if (m_iconsPage != nullptr) {
      m_iconsPage->resetAll();
    }
    break;
  case ActivePage::AppearanceCursors:
    if (m_cursorsPage != nullptr) {
      m_cursorsPage->resetAll();
    }
    break;
  case ActivePage::AppearanceFonts:
    if (m_fontsPage != nullptr) {
      m_fontsPage->resetAll();
    }
    break;
  case ActivePage::Animations:
    if (m_animationsPage != nullptr) {
      m_animationsPage->resetAll();
    }
    break;
  case ActivePage::DesktopEffects:
    if (m_desktopEffectsPage != nullptr) {
      m_desktopEffectsPage->resetAll();
    }
    break;
  case ActivePage::Blur:
    if (m_blurPage != nullptr) {
      m_blurPage->resetAll();
    }
    break;
  case ActivePage::AppearanceShadows:
    if (m_shadowsPage != nullptr) {
      m_shadowsPage->resetAll();
    }
    break;
  case ActivePage::Tiling:
    if (m_tilingPage != nullptr) {
      m_tilingPage->resetAll();
    }
    break;
  case ActivePage::WindowRules:
    if (m_windowRulesPage != nullptr) {
      m_windowRulesPage->resetAll();
    }
    break;
  case ActivePage::WindowBehavior:
    if (m_windowBehaviorPage != nullptr) {
      m_windowBehaviorPage->resetAll();
    }
    break;
  case ActivePage::Workspaces:
    if (m_workspacesPage != nullptr) {
      m_workspacesPage->resetAll();
    }
    break;
  case ActivePage::Keyboard:
    if (m_keyboardPage != nullptr) {
      m_keyboardPage->resetAll();
    }
    break;
  case ActivePage::Touchpad:
    if (m_touchpadPage != nullptr) {
      m_touchpadPage->resetAll();
    }
    break;
  case ActivePage::Mouse:
    if (m_mousePage != nullptr) {
      m_mousePage->resetAll();
    }
    break;
  case ActivePage::Keybinds:
    if (m_keybindsPage != nullptr) {
      m_keybindsPage->resetAll();
    }
    break;
  case ActivePage::Displays:
    if (m_displayPage != nullptr) {
      m_displayPage->resetAll();
    }
    break;
  case ActivePage::HotCorners:
    if (m_hotCornersPage != nullptr) {
      m_hotCornersPage->resetAll();
    }
    break;
  case ActivePage::DefaultApps:
    if (m_defaultAppsPage != nullptr) {
      m_defaultAppsPage->resetAll();
    }
    break;
  case ActivePage::Accessibility:
    if (m_accessibilityPage != nullptr) {
      m_accessibilityPage->resetAll();
    }
    break;
  case ActivePage::Autostart:
    break;
  }
}

bool KineticSettingsWindow::activeHasPending() const {
  switch (m_activePage) {
  case ActivePage::AppearanceTitleBars:
    return m_appearancePage != nullptr && m_appearancePage->hasPending();
  case ActivePage::AppearanceIcons:
    return m_iconsPage != nullptr && m_iconsPage->hasPending();
  case ActivePage::AppearanceCursors:
    return m_cursorsPage != nullptr && m_cursorsPage->hasPending();
  case ActivePage::AppearanceFonts:
    return m_fontsPage != nullptr && m_fontsPage->hasPending();
  case ActivePage::Animations:
    return m_animationsPage != nullptr && m_animationsPage->hasPending();
  case ActivePage::DesktopEffects:
    return m_desktopEffectsPage != nullptr && m_desktopEffectsPage->hasPending();
  case ActivePage::Blur:
    return m_blurPage != nullptr && m_blurPage->hasPending();
  case ActivePage::AppearanceShadows:
    return m_shadowsPage != nullptr && m_shadowsPage->hasPending();
  case ActivePage::Tiling:
    return m_tilingPage != nullptr && m_tilingPage->hasPending();
  case ActivePage::WindowRules:
    return m_windowRulesPage != nullptr && m_windowRulesPage->hasPending();
  case ActivePage::WindowBehavior:
    return m_windowBehaviorPage != nullptr && m_windowBehaviorPage->hasPending();
  case ActivePage::Workspaces:
    return m_workspacesPage != nullptr && m_workspacesPage->hasPending();
  case ActivePage::Keyboard:
    return m_keyboardPage != nullptr && m_keyboardPage->hasPending();
  case ActivePage::Touchpad:
    return m_touchpadPage != nullptr && m_touchpadPage->hasPending();
  case ActivePage::Mouse:
    return m_mousePage != nullptr && m_mousePage->hasPending();
  case ActivePage::Keybinds:
    return m_keybindsPage != nullptr && m_keybindsPage->hasPending();
  case ActivePage::Displays:
    return m_displayPage != nullptr && m_displayPage->hasPending();
  case ActivePage::HotCorners:
    return m_hotCornersPage != nullptr && m_hotCornersPage->hasPending();
  case ActivePage::DefaultApps:
    return m_defaultAppsPage != nullptr && m_defaultAppsPage->hasPending();
  case ActivePage::Accessibility:
    return m_accessibilityPage != nullptr && m_accessibilityPage->hasPending();
  case ActivePage::Autostart:
    return false;
  }
  return false;
}

bool KineticSettingsWindow::activeHasResettableSettings() const {
  switch (m_activePage) {
  case ActivePage::AppearanceTitleBars:
    return m_appearancePage != nullptr && !m_appearancePage->resettableKeyPaths().empty();
  case ActivePage::AppearanceIcons:
    return m_iconsPage != nullptr && !m_iconsPage->resettableKeyPaths().empty();
  case ActivePage::AppearanceCursors:
    return m_cursorsPage != nullptr && !m_cursorsPage->resettableKeyPaths().empty();
  case ActivePage::AppearanceFonts:
    return m_fontsPage != nullptr && !m_fontsPage->resettableKeyPaths().empty();
  case ActivePage::Animations:
    return m_animationsPage != nullptr && !m_animationsPage->resettableKeyPaths().empty();
  case ActivePage::DesktopEffects:
    return m_desktopEffectsPage != nullptr && !m_desktopEffectsPage->resettableKeyPaths().empty();
  case ActivePage::Blur:
    return m_blurPage != nullptr && !m_blurPage->resettableKeyPaths().empty();
  case ActivePage::AppearanceShadows:
    return m_shadowsPage != nullptr && !m_shadowsPage->resettableKeyPaths().empty();
  case ActivePage::Tiling:
    return m_tilingPage != nullptr && !m_tilingPage->resettableKeyPaths().empty();
  case ActivePage::WindowRules:
    return m_windowRulesPage != nullptr && !m_windowRulesPage->resettableKeyPaths().empty();
  case ActivePage::WindowBehavior:
    return m_windowBehaviorPage != nullptr && !m_windowBehaviorPage->resettableKeyPaths().empty();
  case ActivePage::Workspaces:
    return m_workspacesPage != nullptr && !m_workspacesPage->resettableKeyPaths().empty();
  case ActivePage::Keyboard:
    return m_keyboardPage != nullptr && !m_keyboardPage->resettableKeyPaths().empty();
  case ActivePage::Touchpad:
    return m_touchpadPage != nullptr && !m_touchpadPage->resettableKeyPaths().empty();
  case ActivePage::Mouse:
    return m_mousePage != nullptr && !m_mousePage->resettableKeyPaths().empty();
  case ActivePage::Keybinds:
    return m_keybindsPage != nullptr && !m_keybindsPage->resettableKeyPaths().empty();
  case ActivePage::Displays:
    return m_displayPage != nullptr && !m_displayPage->resettableKeyPaths().empty();
  case ActivePage::HotCorners:
    return m_hotCornersPage != nullptr && !m_hotCornersPage->resettableKeyPaths().empty();
  case ActivePage::DefaultApps:
    return m_defaultAppsPage != nullptr && !m_defaultAppsPage->resettableKeyPaths().empty();
  case ActivePage::Accessibility:
    return m_accessibilityPage != nullptr && !m_accessibilityPage->resettableKeyPaths().empty();
  case ActivePage::Autostart:
    return false;
  }
  return false;
}

bool KineticSettingsWindow::anyPageHasPending() const {
  const auto pending = [](const auto& page) { return page != nullptr && page->hasPending(); };
  return pending(m_tilingPage) || pending(m_workspacesPage) || pending(m_keybindsPage) || pending(m_keyboardPage)
      || pending(m_mousePage) || pending(m_touchpadPage) || pending(m_windowRulesPage)
      || pending(m_windowBehaviorPage) || pending(m_displayPage) || pending(m_hotCornersPage)
      || pending(m_appearancePage) || pending(m_iconsPage) || pending(m_cursorsPage) || pending(m_fontsPage)
      || pending(m_desktopEffectsPage) || pending(m_blurPage) || pending(m_animationsPage)
      || pending(m_defaultAppsPage) || pending(m_accessibilityPage) || pending(m_autostartPage)
      || pending(m_shadowsPage);
}

void KineticSettingsWindow::applyAllPages() {
  const auto applyPending = [](auto& page) {
    if (page != nullptr && page->hasPending()) {
      (void)page->applyAll();
    }
  };
  applyPending(m_tilingPage);
  applyPending(m_workspacesPage);
  applyPending(m_keybindsPage);
  applyPending(m_keyboardPage);
  applyPending(m_mousePage);
  applyPending(m_touchpadPage);
  applyPending(m_windowRulesPage);
  applyPending(m_windowBehaviorPage);
  applyPending(m_displayPage);
  applyPending(m_hotCornersPage);
  applyPending(m_appearancePage);
  applyPending(m_iconsPage);
  applyPending(m_cursorsPage);
  applyPending(m_fontsPage);
  applyPending(m_desktopEffectsPage);
  applyPending(m_blurPage);
  applyPending(m_animationsPage);
  applyPending(m_defaultAppsPage);
  applyPending(m_accessibilityPage);
  applyPending(m_autostartPage);
  // Last: ShadowsPage reloads the file before writing, so edits flushed by the
  // pages above (global search can stage several pages at once) are preserved.
  applyPending(m_shadowsPage);
  clearArmedResets();
  // A page whose write failed keeps its staged edits and shows an on-page error.
  requestSceneRebuild();
}

std::unique_ptr<Flex> KineticSettingsWindow::buildFilterRow(float scale) {
  auto filters = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
  });

  filters->addChild(ui::input({
      .out = &m_searchInput,
      .value = m_searchQuery,
      .placeholder = i18n::tr("settings.window.search-placeholder"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceSm * scale,
      .clearButtonEnabled = true,
      .width = 320.0F * scale,
      .height = Style::controlHeight * scale,
      .onChange = [this](const std::string& value) {
        const bool wasSearchActive = !m_searchQuery.empty();
        m_searchQuery = value;
        clearArmedResets();
        if (wasSearchActive != !m_searchQuery.empty()) {
          // Entering/leaving global search swaps the filter row (Reset button,
          // Apply scope) and the sidebar highlight, so rebuild the whole scene
          // and restore focus to the rebuilt search field.
          m_searchDebounceTimer.stop();
          m_focusSearchOnRebuild = true;
          requestSceneRebuild();
        } else {
          // Typing within a query only re-filters the merged results; coalesce
          // bursts so every key repeat doesn't rebuild all pages.
          m_searchDebounceTimer.start(kSearchDebounceInterval, [this]() { requestContentRebuild(); });
        }
      },
  }));
  filters->addChild(ui::spacer());

  filters->addChild(ui::label({
      .text = i18n::tr("settings.badges.advanced"),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 1.0F),
  }));
  filters->addChild(ui::toggle({
      .checked = m_showAdvanced,
      .scale = scale,
      .onChange = [this](bool value) {
        m_showAdvanced = value;
        clearArmedResets();
        requestContentRebuild();
      },
  }));

  filters->addChild(ui::label({
      .text = i18n::tr("settings.window.filter-overridden"),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 1.0F),
  }));
  filters->addChild(ui::toggle({
      .checked = m_showOverriddenOnly,
      .scale = scale,
      .onChange = [this](bool value) {
        m_showOverriddenOnly = value;
        clearArmedResets();
        requestContentRebuild();
      },
  }));

  if (m_searchQuery.empty() && activeHasResettableSettings()) {
    filters->addChild(ui::button({
        .text = i18n::tr(m_resetPageArmed ? "settings.window.reset-page-confirm" : "settings.window.reset-page"),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = m_resetPageArmed ? ButtonVariant::Destructive : ButtonVariant::Ghost,
        .minHeight = Style::controlHeightSm * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this]() {
          if (!m_resetPageArmed) {
            m_resetPageArmed = true;
            m_armedResetKeyPath.clear();
            requestSceneRebuild();
            return;
          }
          confirmResetPage();
        },
    }));
  }

  // Apply: flushes the staged edits with one file write (workspaces also D-Bus
  // syncs the workspace list) + one compositor reload. While a global search is
  // active the results span pages, so Apply flushes every page with pending
  // edits; otherwise it flushes only the active page. Always visible; enabled
  // only while there are unapplied changes.
  const bool hasPending = m_searchQuery.empty() ? activeHasPending() : anyPageHasPending();
  filters->addChild(ui::button({
      .text = "Apply",
      .fontSize = Style::fontSizeCaption * scale,
      .enabled = hasPending,
      .variant = ButtonVariant::Primary,
      .minHeight = Style::controlHeightSm * scale,
      .paddingV = Style::spaceXs * scale,
      .paddingH = Style::spaceMd * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = [this]() {
        if (m_searchQuery.empty()) {
          applyActivePage();
        } else {
          applyAllPages();
        }
      },
  }));

  return filters;
}



std::unique_ptr<Node> KineticSettingsWindow::buildSidebar(float scale) {
  auto sidebarScroll = ui::scrollView({
      .state = &m_sidebarScrollState,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .fill = clearColorSpec(),
      .radius = Style::scaledRadiusXl(scale),
      .minWidth = kSidebarWidth * scale,
      .fillHeight = true,
      .width = kSidebarWidth * scale,
      .height = 0.0F,
      .configure = [](ScrollView& scrollView) { scrollView.clearBorder(); },
  });

  auto* nav = sidebarScroll->content();
  nav->setDirection(FlexDirection::Vertical);
  nav->setAlign(FlexAlign::Stretch);
  nav->setGap(kSidebarGap * scale);
  nav->setPadding(kSidebarPadding * scale);

  // Selecting a sub-page updates the sidebar highlight AND the content, so the
  // whole scene (not just the content) is rebuilt.
  const auto selectPage = [this](ActivePage page) {
    if (page != m_activePage && m_fontsPage != nullptr) {
      m_fontsPage->cancelPicker();
    }
    m_activePage = page;
    // Picking a page leaves global search (Noctalia clears the query the same
    // way), so the page shows its full content rather than the merged results.
    m_searchQuery.clear();
    clearArmedResets();
    requestSceneRebuild();
  };

  // Primary nav row: collapsible category. The chevron doubles as the visual
  // affordance for the expand/collapse toggle.
  const auto addCategory = [&](std::string_view title, bool expanded, std::function<void()> onToggle) {
    nav->addChild(ui::button({
        .text = std::string(title),
        .glyph = expanded ? "chevron-down" : "chevron-right",
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = kPrimaryNavGlyphSize * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Tab,
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceXs * scale,
        .paddingH = kPrimaryNavPaddingH * scale,
        .gap = kPrimaryNavGap * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [onToggle]() { onToggle(); },
        .configure = [](Button& button) {
          if (button.label() != nullptr) {
            button.label()->setFontWeight(FontWeight::Bold);
          }
          button.setTabStop(false);
        },
    }));
  };

  // Secondary nav rows: indented, compact, one per sub-page. Mirrors Noctalia's
  // settings_sidebar secondary nav ("paddingLeft = spaceLg").
  const auto addSubNav = [&](ActivePage page, std::string_view glyph, std::string_view text) {
    // No page is highlighted while global search results are shown.
    const bool selected = m_searchQuery.empty() && m_activePage == page;
    nav->addChild(ui::button({
        .text = std::string(text),
        .glyph = std::string(glyph),
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeCaption * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = selected ? ButtonVariant::TabActive : ButtonVariant::Tab,
        .minHeight = Style::controlHeightSm * scale,
        .paddingTop = Style::spaceXs * scale,
        .paddingRight = Style::spaceMd * scale,
        .paddingBottom = Style::spaceXs * scale,
        .paddingLeft = Style::spaceLg * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [page, selectPage]() { selectPage(page); },
        .configure = [selected](Button& button) {
          if (selected && button.label() != nullptr) {
            button.label()->setFontWeight(FontWeight::Bold);
          }
          button.setTabStop(false);
        },
    }));
  };

  // Categories are listed alphabetically: Appearance, Input & Output, System,
  // then Window Management.
  addCategory("Appearance", m_appearanceExpanded, [this]() {
    m_appearanceExpanded = !m_appearanceExpanded;
    clearArmedResets();
    requestSceneRebuild();
  });
  if (m_appearanceExpanded) {
    addSubNav(ActivePage::AppearanceTitleBars, "palette", "Title Bars");
    addSubNav(ActivePage::AppearanceIcons, "apps", "Icons");
    addSubNav(ActivePage::AppearanceCursors, "mouse-2", "Cursors");
    addSubNav(ActivePage::AppearanceFonts, "typeface", "Fonts");
    addSubNav(ActivePage::Animations, "movie", "Animations");
    addSubNav(ActivePage::DesktopEffects, "sparkles", "Desktop Effects");
    addSubNav(ActivePage::AppearanceShadows, "shadow", "Shadows");
    addSubNav(ActivePage::Blur, "sparkles", "Blur");
  }

  addCategory("Input & Output", m_inputOutputExpanded, [this]() {
    m_inputOutputExpanded = !m_inputOutputExpanded;
    clearArmedResets();
    requestSceneRebuild();
  });
  if (m_inputOutputExpanded) {
    addSubNav(ActivePage::Keyboard, "keyboard", "Kweboard");
    addSubNav(ActivePage::Mouse, "mouse", "Mouse");
    addSubNav(ActivePage::Touchpad, "mouse-2", "Touchpad");
    addSubNav(ActivePage::Keybinds, "keyboard", "Kwebinds");
    addSubNav(ActivePage::Displays, "device-desktop", "Displays");
    addSubNav(ActivePage::HotCorners, "border-corners", "Hot Corners");
  }

  addCategory("Window Management", m_windowManagementExpanded, [this]() {
    m_windowManagementExpanded = !m_windowManagementExpanded;
    clearArmedResets();
    requestSceneRebuild();
  });
  if (m_windowManagementExpanded) {
    addSubNav(ActivePage::WindowBehavior, "window", "Window Behavior");
    addSubNav(ActivePage::Workspaces, "device-desktop", "Workspaces");
    addSubNav(ActivePage::Tiling, "layout-grid", "Tiling");
    addSubNav(ActivePage::WindowRules, "window", "Window Rules");
  }

  addCategory("System", m_systemExpanded, [this]() {
    m_systemExpanded = !m_systemExpanded;
    clearArmedResets();
    requestSceneRebuild();
  });
  if (m_systemExpanded) {
    addSubNav(ActivePage::DefaultApps, "apps", "Default Apps");
    addSubNav(ActivePage::Autostart, "rocket", "Autostart");
    addSubNav(ActivePage::Accessibility, "accessible", "Accessibility");
  }

  return sidebarScroll;
}

std::string_view KineticSettingsWindow::pageKeywords(ActivePage page) {
  switch (page) {
  case ActivePage::AppearanceTitleBars:
    return "appearance title bars titlebar dynamic neutral look and feel window decorations decoration "
           "title bar buttons minimize maximize close window menu";
  case ActivePage::AppearanceIcons:
    return "appearance icons icon theme";
  case ActivePage::AppearanceCursors:
    return "appearance cursors cursor theme pointer size";
  case ActivePage::AppearanceFonts:
    return "appearance fonts font typeface family size text scale sans sans-serif serif monospace";
  case ActivePage::Animations:
    return "appearance animations animation speed effects";
  case ActivePage::DesktopEffects:
    return "appearance desktop effects wobbly";
  case ActivePage::Blur:
    return "appearance blur blur all windows translucency transparency background opacity";
  case ActivePage::AppearanceShadows:
    return "appearance shadows shadow drop shadow window tiled windows blur radius offset opacity color";
  case ActivePage::Keyboard:
    return "input output keyboard input devices key repeat num lock";
  case ActivePage::Mouse:
    return "input output mouse pointer devices screen edges edge barrier";
  case ActivePage::Touchpad:
    return "input output touchpad trackpad pointer devices";
  case ActivePage::Keybinds:
    return "input output keybinds keyboard shortcuts keys bindings";
  case ActivePage::Displays:
    return "input output displays display monitor screen";
  case ActivePage::HotCorners:
    return "input output hot corners corner screen edge edges electric border overview present windows";
  case ActivePage::WindowBehavior:
    return "window management window behavior focus raise";
  case ActivePage::Workspaces:
    return "window management workspaces virtual desktops";
  case ActivePage::Tiling:
    return "window management tiling tiles layout gaps";
  case ActivePage::WindowRules:
    return "window management window rules title bar opacity";
  case ActivePage::DefaultApps:
    return "system default apps applications browser terminal portals portal file picker chooser gtk "
           "xdg-desktop-portal screensharing";
  case ActivePage::Accessibility:
    return "system accessibility zoom magnifier shake pointer cursor a11y";
  case ActivePage::Autostart:
    return "system autostart startup login session applications commands";
  }
  return {};
}

std::unique_ptr<Node> KineticSettingsWindow::buildPageNode(ActivePage page, Renderer& renderer) {
  // Noctalia folds a section's name into every entry's search text, so a query
  // like "keyboard" or "keybinds" surfaces the whole page. Mirror that: when
  // the query names the page, build it with the query cleared (all rows shown).
  const bool pageNamed =
      !m_searchQuery.empty() && StringUtils::containsInsensitive(pageKeywords(page), m_searchQuery);
  if (!pageNamed) {
    return buildPageNodeWithQuery(page, renderer);
  }
  const std::string activeQuery = m_searchQuery;
  m_searchQuery.clear();
  auto node = buildPageNodeWithQuery(page, renderer);
  m_searchQuery = activeQuery;
  return node;
}

std::unique_ptr<Node> KineticSettingsWindow::buildPageNodeWithQuery(ActivePage page, Renderer& renderer) {
  switch (page) {
  case ActivePage::AppearanceTitleBars:
    if (m_appearancePage != nullptr) {
      return m_appearancePage->build(
          uiScale(), renderer, appearanceFilter(), m_armedResetKeyPath, appearancePageCallbacks()
      );
    }
    break;
  case ActivePage::AppearanceIcons:
    if (m_iconsPage != nullptr) {
      return m_iconsPage->build(uiScale(), renderer, iconsFilter(), m_armedResetKeyPath, iconsPageCallbacks());
    }
    break;
  case ActivePage::AppearanceCursors:
    if (m_cursorsPage != nullptr) {
      return m_cursorsPage->build(uiScale(), renderer, cursorsFilter(), m_armedResetKeyPath, cursorsPageCallbacks());
    }
    break;
  case ActivePage::AppearanceFonts:
    if (m_fontsPage != nullptr) {
      return m_fontsPage->build(uiScale(), fontsFilter(), m_armedResetKeyPath, fontsPageCallbacks());
    }
    break;
  case ActivePage::Animations:
    if (m_animationsPage != nullptr) {
      return m_animationsPage->build(uiScale(), animationsFilter(), m_armedResetKeyPath, animationsPageCallbacks());
    }
    break;
  case ActivePage::DesktopEffects:
    if (m_desktopEffectsPage != nullptr) {
      return m_desktopEffectsPage->build(
          uiScale(), desktopEffectsFilter(), m_armedResetKeyPath, desktopEffectsPageCallbacks()
      );
    }
    break;
  case ActivePage::Blur:
    if (m_blurPage != nullptr) {
      return m_blurPage->build(uiScale(), blurFilter(), m_armedResetKeyPath, blurPageCallbacks());
    }
    break;
  case ActivePage::AppearanceShadows:
    if (m_shadowsPage != nullptr) {
      return m_shadowsPage->build(uiScale(), shadowsFilter(), m_armedResetKeyPath, shadowsPageCallbacks());
    }
    break;
  case ActivePage::Tiling:
    if (m_tilingPage != nullptr) {
      // The compositor's D-Bus only exposes the active output, so the full
      // monitor list comes from the shell's Wayland connection (connector
      // names match the compositor's [Tiling][DesktopOutput <n>:<out>] keys).
      std::vector<std::string> outputs;
      if (m_wayland != nullptr) {
        for (const auto& info : m_wayland->outputs()) {
          if (!info.connectorName.empty()) {
            outputs.push_back(info.connectorName);
          }
        }
      }
      m_tilingPage->setConnectedOutputs(std::move(outputs));
      return m_tilingPage->build(uiScale(), pageFilter(), m_armedResetKeyPath, tilingPageCallbacks());
    }
    break;
  case ActivePage::Workspaces:
    if (m_workspacesPage != nullptr) {
      return m_workspacesPage->build(uiScale(), workspacesFilter(), m_armedResetKeyPath, workspacesPageCallbacks());
    }
    break;
  case ActivePage::WindowRules:
    if (m_windowRulesPage != nullptr) {
      return m_windowRulesPage->build(uiScale(), windowRulesFilter(), windowRulesPageCallbacks());
    }
    break;
  case ActivePage::WindowBehavior:
    if (m_windowBehaviorPage != nullptr) {
      return m_windowBehaviorPage->build(
          uiScale(), windowBehaviorFilter(), m_armedResetKeyPath, windowBehaviorPageCallbacks()
      );
    }
    break;
  case ActivePage::Keyboard:
    if (m_keyboardPage != nullptr) {
      return m_keyboardPage->build(uiScale(), keyboardFilter(), m_armedResetKeyPath, keyboardPageCallbacks());
    }
    break;
  case ActivePage::Touchpad:
    if (m_touchpadPage != nullptr) {
      return m_touchpadPage->build(uiScale(), touchpadFilter(), m_armedResetKeyPath, touchpadPageCallbacks());
    }
    break;
  case ActivePage::Mouse:
    if (m_mousePage != nullptr) {
      return m_mousePage->build(uiScale(), mouseFilter(), m_armedResetKeyPath, mousePageCallbacks());
    }
    break;
  case ActivePage::Keybinds:
    if (m_keybindsPage != nullptr) {
      return m_keybindsPage->build(uiScale(), keybindsFilter(), keybindsPageCallbacks());
    }
    break;
  case ActivePage::Displays:
    if (m_displayPage != nullptr) {
      return m_displayPage->build(uiScale(), displayFilter(), m_armedResetKeyPath, displayPageCallbacks());
    }
    break;
  case ActivePage::HotCorners:
    if (m_hotCornersPage != nullptr) {
      return m_hotCornersPage->build(uiScale(), hotCornersFilter(), m_armedResetKeyPath, hotCornersPageCallbacks());
    }
    break;
  case ActivePage::DefaultApps:
    if (m_defaultAppsPage != nullptr) {
      return m_defaultAppsPage->build(
          uiScale(), defaultAppsFilter(), m_armedResetKeyPath, defaultAppsPageCallbacks()
      );
    }
    break;
  case ActivePage::Accessibility:
    if (m_accessibilityPage != nullptr) {
      return m_accessibilityPage->build(
          uiScale(), accessibilityFilter(), m_armedResetKeyPath, accessibilityPageCallbacks()
      );
    }
    break;
  case ActivePage::Autostart:
    if (m_autostartPage != nullptr) {
      return m_autostartPage->build(uiScale(), autostartFilter(), autostartPageCallbacks());
    }
    break;
  }
  return nullptr;
}

void KineticSettingsWindow::rebuildContent(Renderer& renderer) {
  if (m_pageContainer == nullptr) {
    return;
  }
  clearGroupJumpRow();
  removeAllChildren(*m_pageContainer);
  if (!m_searchQuery.empty()) {
    // Global search: merge the matching settings of every page into one list.
    rebuildSearchResults(renderer);
    updateGroupJumpRowVisibility();
    return;
  }
  if (auto node = buildPageNode(m_activePage, renderer); node != nullptr) {
    m_pageContainer->addChild(std::move(node));
  }
  updateGroupJumpRowVisibility();
}

void KineticSettingsWindow::clearGroupJumpRow() {
  if (m_groupJumpRow == nullptr) {
    return;
  }
  while (!m_groupJumpRow->children().empty()) {
    m_groupJumpRow->removeChild(m_groupJumpRow->children().back().get());
  }
}

void KineticSettingsWindow::updateGroupJumpRowVisibility() {
  if (m_groupJumpRow == nullptr) {
    return;
  }
  // Hidden during a search (pages are flattened then) and when a page has fewer
  // than two sections, where a lone pill would be pointless.
  const bool show = m_searchQuery.empty() && m_groupJumpRow->children().size() >= 2;
  m_groupJumpRow->setVisible(show);
  m_groupJumpRow->setParticipatesInLayout(show);
}

void KineticSettingsWindow::rebuildSearchResults(Renderer& renderer) {
  // Mirrors the sidebar order so results read top-to-bottom by category. Pages
  // return nullptr when nothing in them matches the query.
  static constexpr ActivePage kSearchPageOrder[] = {
      ActivePage::AppearanceTitleBars, ActivePage::AppearanceIcons,   ActivePage::AppearanceCursors,
      ActivePage::AppearanceFonts,     ActivePage::Animations,        ActivePage::DesktopEffects,
      ActivePage::AppearanceShadows,   ActivePage::Blur,              ActivePage::Keyboard,
      ActivePage::Mouse,               ActivePage::Touchpad,          ActivePage::Keybinds,
      ActivePage::Displays,            ActivePage::HotCorners,        ActivePage::WindowBehavior,
      ActivePage::Workspaces,          ActivePage::Tiling,            ActivePage::WindowRules,
      ActivePage::DefaultApps,         ActivePage::Accessibility,     ActivePage::Autostart,
  };

  // Title Bars (window decoration previews), Icons and Cursors rescan the
  // theme directories and decode a preview image per matching entry on every
  // rebuild. A one-character query matches whole swathes of theme names, so
  // skip those pages until the query narrows.
  const bool skipThemePages = m_searchQuery.size() < 2;

  bool any = false;
  for (const ActivePage page : kSearchPageOrder) {
    if (skipThemePages
        && (page == ActivePage::AppearanceTitleBars || page == ActivePage::AppearanceIcons
            || page == ActivePage::AppearanceCursors)) {
      continue;
    }
    if (auto node = buildPageNode(page, renderer); node != nullptr) {
      m_pageContainer->addChild(std::move(node));
      any = true;
    }
  }

  if (!any) {
    const float scale = uiScale();
    m_pageContainer->addChild(ui::column(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .gap = Style::spaceSm * scale,
         .padding = Style::spaceLg * scale,
         .fillWidth = true},
        ui::label({
            .text = i18n::tr("settings.window.no-results"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }),
        ui::label({
            .text = i18n::tr("settings.window.no-results-hint"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    ));
  }
}

void KineticSettingsWindow::buildScene(float width, float height) {
  const float scale = uiScale();
  m_inputDispatcher.setSceneRoot(nullptr);
  m_pageContainer = nullptr;
  m_mainContainer = nullptr;
  m_groupJumpRow = nullptr;

  m_sceneRoot = ui::node({.width = width, .height = height});
  m_sceneRoot->setAnimationManager(&m_animations);
  m_sceneRoot->setSize(width, height);
  if (m_selectPopup != nullptr) {
    m_sceneRoot->setPopupContext(m_selectPopup.get());
  }

  m_sceneRoot->addChild(ui::box({
      .width = width,
      .height = height,
      .configure = [this](Box& box) {
        m_background = &box;
        box.setPanelStyle();
        box.setRadius(0.0F);
        box.setBorder(clearColor(), 0);
        box.setPosition(0.0F, 0.0F);
        box.setFill(colorSpecFromRole(ColorRole::Surface, kBackgroundOpacity));
      },
  }));

  // Explicit size, not fill flags: the scene root is a plain Node, so fill
  // flags never resolve and the column would shrink to its content width.
  // prepareFrame re-applies the window size via m_mainContainer on
  // resize/maximize before relayout.
  auto main = ui::column({
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
      .width = width,
      .height = height,
  });

  main->addChild(buildFilterRow(scale));

  auto body = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0F,
  });
  body->addChild(buildSidebar(scale));
  body->addChild(ui::separator());

  auto scroll = ui::scrollView({
      .state = &m_contentScrollState,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .onScrollChanged = [this](float /*offset*/) {
        if (m_selectPopup != nullptr) {
          m_selectPopup->closeSelectDropdown();
        }
      },
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });
  auto* content = scroll->content();
  m_pageContainer = content;
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);

  // Fixed section-jump (pill) row: above the scrolling page content and to the
  // right of the sidebar, so the pills line up with the section cards. It is
  // populated by the active page during rebuildContent().
  auto contentColumn = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0F,
  });
  contentColumn->addChild(ui::row({
      .out = &m_groupJumpRow,
      .align = FlexAlign::Center,
      .wrap = true,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .configure = [scale](Flex& flex) { flex.setPadding(0.0F, Style::spaceLg * scale); },
  }));

  auto& renderer = m_surface->renderTarget().renderer();
  rebuildContent(renderer);

  contentColumn->addChild(std::move(scroll));
  body->addChild(std::move(contentColumn));
  main->addChild(std::move(body));

  // Lay the new tree out before attaching it: the render path only draws
  // current geometry, so an unlaid-out scene renders blank until some later
  // relayout happens to run (e.g. on refocus).
  main->layout(renderer);
  m_mainContainer = static_cast<Flex*>(m_sceneRoot->addChild(std::move(main)));
  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  m_surface->setSceneRoot(m_sceneRoot.get());

  // Entering/leaving global search rebuilds the whole scene (the filter row's
  // Apply/Reset and the sidebar highlight change); put the keyboard focus back
  // in the search field so typing continues uninterrupted.
  if (m_focusSearchOnRebuild) {
    m_focusSearchOnRebuild = false;
    if (m_searchInput != nullptr && m_searchInput->inputArea() != nullptr) {
      m_inputDispatcher.setFocus(m_searchInput->inputArea());
    }
  } else if (m_keybindsPage != nullptr) {
    // Always consume the keybinds page's one-shot focus request so it cannot go
    // stale, but only honor it on the page itself: the add forms can also render
    // under global search, where the field must not steal the caret. Honoring it
    // puts the caret in the field so the user can type without clicking first.
    if (auto* focusArea = m_keybindsPage->takePendingFocusArea();
        focusArea != nullptr && m_activePage == ActivePage::Keybinds && m_searchQuery.empty()) {
      m_inputDispatcher.setFocus(focusArea);
    }
  }
}


bool KineticSettingsWindow::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return false;
  }
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }
  if (event.surface != m_surface->wlSurface()) {
    return false;
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    return true;
  case PointerEvent::Type::Leave:
    m_inputDispatcher.pointerLeave();
    return true;
  case PointerEvent::Type::Motion:
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    return true;
  case PointerEvent::Type::Button:
    return m_inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed, event.serial, event.time,
        event.touch
    );
  case PointerEvent::Type::Axis:
    return m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines, event.axisGestureSerial
    );
  }
  return false;
}

void KineticSettingsWindow::updateKeyboardShortcutInhibit() {
  if (!isOpen() || m_surface == nullptr || m_wayland == nullptr) {
    return;
  }
  const bool capturing = m_keybindsPage != nullptr && m_keybindsPage->isCapturing();
  if (capturing && m_shortcutInhibitor == nullptr) {
    m_shortcutInhibitor =
        m_wayland->extension<ShellGlobals>().createKeyboardShortcutsInhibitor(m_surface->wlSurface());
  } else if (!capturing && m_shortcutInhibitor != nullptr) {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_shortcutInhibitor);
    m_shortcutInhibitor = nullptr;
  }
}

void KineticSettingsWindow::onFontChanged() {
  if (!isOpen()) {
    return;
  }
  // The Fonts page reads the interface family live from ConfigService, so a
  // scene rebuild is enough here. Do not reload the page: that would discard
  // staged edits while the user is changing fonts from another surface.
  requestSceneRebuild();
}

void KineticSettingsWindow::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen()) {
    return;
  }
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }
  if (event.pressed && KeySymbol::isEscape(event.sym)) {
    // An inline Fonts picker consumes Escape to cancel itself instead of
    // closing the whole window.
    if (m_activePage == ActivePage::AppearanceFonts && m_fontsPage != nullptr && m_fontsPage->hasOpenPicker()) {
      m_fontsPage->cancelPicker();
      requestContentRebuild();
      return;
    }
    // A recorder capture consumes Escape itself (escapeCancels); the window
    // must not interpret it as a close gesture while one is recording.
    if (m_keybindsPage == nullptr || !m_keybindsPage->isCapturing()) {
      close();
      return;
    }
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}
