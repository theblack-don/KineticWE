#pragma once

#include "capture/toplevel_thumbnail_source.h"
#include "core/timer_manager.h"

#include <memory>
#include <optional>
#include <span>
#include <string>

class ImageCaptureGlobals;
class WaylandConnection;
struct ToplevelThumbnailCapturePending;

namespace capture {
  std::optional<ScreencopyImage> makeToplevelThumbnail(
      std::span<const std::uint8_t> pixels, int width, int height, std::uint32_t format, int maxWidth, int maxHeight,
      std::int32_t transform
  );
}

// One-shot, bounded-size snapshots sourced directly from an ext-foreign-toplevel handle.
class ToplevelThumbnailCapture final : public ToplevelThumbnailSource {
public:
  explicit ToplevelThumbnailCapture(WaylandConnection& wayland);
  ~ToplevelThumbnailCapture() override;

  bool available() const noexcept override;
  bool busy() const noexcept override { return m_pending != nullptr; }
  void capture(const ToplevelCaptureSource& source, int maxWidth, int maxHeight, CompletionCallback onComplete)
      override;
  void cancelInFlight() override;

private:
  friend struct ToplevelThumbnailCapturePending;
  void fail(std::string message);
  void finish(ScreencopyImage image);

  WaylandConnection& m_wayland;
  ImageCaptureGlobals& m_imageCapture;
  std::unique_ptr<ToplevelThumbnailCapturePending> m_pending;
  CompletionCallback m_onComplete;
  Timer m_timeout;
};
