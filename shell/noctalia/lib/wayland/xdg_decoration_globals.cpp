#include "wayland/xdg_decoration_globals.h"

#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <algorithm>

namespace {
  constexpr std::uint32_t kXdgDecorationManagerVersion = 1;
} // namespace

XdgDecorationGlobals::XdgDecorationGlobals(WaylandConnection& /*connection*/) {}

void XdgDecorationGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface != zxdg_decoration_manager_v1_interface.name) {
    return;
  }
  const auto bindVersion = std::min(version, kXdgDecorationManagerVersion);
  m_manager = static_cast<zxdg_decoration_manager_v1*>(
      wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, bindVersion)
  );
}

void XdgDecorationGlobals::releaseGlobals() {
  if (m_manager != nullptr) {
    zxdg_decoration_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}
