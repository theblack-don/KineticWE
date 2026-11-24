#pragma once

#include "wayland/wayland_extension.h"

class WaylandConnection;
struct zxdg_decoration_manager_v1;

// Binds xdg-decoration-unstable-v1 so toplevels can request server-side decorations.
class XdgDecorationGlobals final : public WaylandExtension {
public:
  explicit XdgDecorationGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  [[nodiscard]] bool available() const noexcept { return m_manager != nullptr; }
  zxdg_decoration_manager_v1* manager() const noexcept { return m_manager; }

private:
  zxdg_decoration_manager_v1* m_manager = nullptr;
};
