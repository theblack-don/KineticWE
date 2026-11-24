#pragma once

#include "wayland/wayland_extension.h"

#include <functional>

class WaylandConnection;
struct ext_idle_notification_v1;
struct ext_idle_notifier_v1;
struct ext_session_lock_manager_v1;
struct wl_surface;
struct zwlr_gamma_control_manager_v1;
struct zwp_idle_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibitor_v1;

// Managers for shell services: session lock, idle notify and inhibit, gamma control, and
// keyboard shortcuts inhibit.
class ShellGlobals final : public WaylandExtension {
public:
  using ChangeCallback = std::function<void()>;

  explicit ShellGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  ext_session_lock_manager_v1* sessionLockManager() const noexcept { return m_sessionLockManager; }
  zwlr_gamma_control_manager_v1* gammaControlManager() const noexcept { return m_gammaControlManager; }
  zwp_idle_inhibit_manager_v1* idleInhibitManager() const noexcept { return m_idleInhibitManager; }
  bool hasIdleNotifier() const noexcept { return m_idleNotifier != nullptr; }

  /// Fired when both `ext_idle_notifier_v1` and `wl_seat` are bound (including late registry globals).
  void setIdleCapabilitiesReadyCallback(ChangeCallback callback);
  /// Inhibitor-aware idle notification (`get_idle_notification`); honors `zwp_idle_inhibitor_v1`.
  ext_idle_notification_v1* createIdleNotification(std::uint32_t timeoutMs) const;
  /// Input-only idle notification (`get_input_idle_notification`, notifier v2); ignores idle inhibitors.
  /// Returns null when the compositor's notifier is older than v2.
  ext_idle_notification_v1* createInputIdleNotification(std::uint32_t timeoutMs) const;
  /// Inhibits compositor keybindings while the surface captures keys (e.g. keybind recording).
  /// Returns null when the compositor does not advertise the protocol.
  zwp_keyboard_shortcuts_inhibitor_v1* createKeyboardShortcutsInhibitor(wl_surface* surface) const;

private:
  void notifyIdleCapabilitiesReady();

  WaylandConnection& m_connection;
  ext_session_lock_manager_v1* m_sessionLockManager = nullptr;
  ext_idle_notifier_v1* m_idleNotifier = nullptr;
  zwp_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
  zwp_keyboard_shortcuts_inhibit_manager_v1* m_keyboardShortcutsInhibitManager = nullptr;
  zwlr_gamma_control_manager_v1* m_gammaControlManager = nullptr;
  ChangeCallback m_idleCapabilitiesReadyCallback;
};
