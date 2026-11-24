#include "ui/controls/keybind_recorder.h"

#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

namespace {

  constexpr float kRecorderMinWidth = 220.0F;

  std::function<void(KeybindRejection)> gDefaultOnReject;

  // Maps a modifier keysym to its KeyMod bit so we can track held modifiers
  // from the key symbols themselves. The compositor's key-event `modifiers`
  // bitmask drops the Meta/Super bit on this fork (xkb LOGO is not reflected),
  // so keying off it alone would silently drop Meta from recorded chords.
  [[nodiscard]] std::uint32_t modifierBitForSym(std::uint32_t sym) noexcept {
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Shift_Lock:
      return KeyMod::Shift;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
      return KeyMod::Ctrl;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
      return KeyMod::Alt;
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
      return KeyMod::Super;
    default:
      return 0;
    }
  }

  // Mirrors KDE's KKeySequenceRecorder::isShiftAsModifierAllowed(): Shift stays
  // a modifier for letters, F-keys and the non-text keys (arrows, media, …),
  // but not for keys whose shifted form is a different character (1 -> !,
  // = -> +). For those the shifted keysym is recorded and Shift is dropped,
  // which is how the compositor matches e.g. Meta+Shift+1 (as Meta+!).
  [[nodiscard]] bool shiftAllowedAsModifier(std::uint32_t sym) noexcept {
    if ((sym >= XKB_KEY_a && sym <= XKB_KEY_z) || (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)) {
      return true;
    }
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F35) {
      return true;
    }
    switch (sym) {
    // Printable keys for which Shift is still a usable modifier.
    case XKB_KEY_space:
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
    case XKB_KEY_BackSpace:
    case XKB_KEY_Escape:
      return true;
    default:
      break;
    }
    // Non-text keys (arrows, navigation, media/launch, keypad, …) never change
    // their keysym when Shift is held, so Shift is a real modifier there.
    return !isPrintableKey(sym);
  }

} // namespace

KeybindRecorder::KeybindRecorder() {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Center);
  setGap(Style::spaceXs);
  setPadding(Style::spaceXs, Style::spaceSm);
  setStyleRadius(Style::radiusMd);
  setMinHeight(Style::controlHeightSm);
  setMinWidth(kRecorderMinWidth);

  auto label = std::make_unique<Label>();
  label->setFontSize(Style::fontSizeCaption);
  label->setColor(colorSpecFromRole(ColorRole::OnSurface));
  label->setMaxLines(1);
  label->setEllipsize(TextEllipsize::End);
  label->setFlexGrow(1.0F);
  m_label = static_cast<Label*>(addChild(std::move(label)));

  auto glyph = std::make_unique<Glyph>();
  glyph->setGlyph("keyboard");
  glyph->setGlyphSize(Style::fontSizeCaption);
  glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_glyph = static_cast<Glyph*>(addChild(std::move(glyph)));

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setOnFocusGain([this] {
    if (!m_recording) {
      applyVisualState(VisualState::Focused);
      markPaintDirty();
    }
  });
  area->setOnFocusLoss([this] {
    if (m_recording) {
      exitRecording(false);
    } else {
      applyVisualState(VisualState::Idle);
      markPaintDirty();
    }
  });
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (data.pressed && m_enabled && !m_recording) {
      enterRecording();
    }
  });
  area->setOnKeyDown([this](const InputArea::KeyData& data) {
    if (data.preedit) {
      return;
    }
    if (!m_recording && data.pressed && KeybindMatcher::matches(KeybindAction::Validate, data.sym, data.modifiers)) {
      enterRecording();
      return;
    }
    handleKeyDown(data.sym, data.modifiers);
  });
  area->setOnKeyUp([this](const InputArea::KeyData& data) {
    if (data.preedit) {
      return;
    }
    handleKeyUp(data.sym, data.modifiers);
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  m_inputArea->setRetainsFocusOnPointerRelease(true);
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);
  m_inputArea->setPosition(0.0F, 0.0F);
  m_inputArea->setFrameSize(width(), height());

  applyVisualState(VisualState::Idle);
  refreshLabel();
}

KeybindRecorder::~KeybindRecorder() = default;

void KeybindRecorder::setChord(std::optional<KeyChord> chord) {
  m_chord = chord;
  refreshLabel();
  applyVisualState(m_visualState);
}

void KeybindRecorder::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  setGap(Style::spaceXs * m_scale);
  setPadding(Style::spaceXs * m_scale, Style::spaceSm * m_scale);
  setStyleRadius(Style::radiusMd, m_scale);
  setMinHeight(Style::controlHeightSm * m_scale);
  setMinWidth(kRecorderMinWidth * m_scale);
  if (m_label != nullptr) {
    m_label->setFontSize(Style::fontSizeCaption * m_scale);
  }
  if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(Style::fontSizeCaption * m_scale);
  }
}

void KeybindRecorder::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  if (!enabled && m_recording) {
    exitRecording(false);
  }
  applyVisualState(m_visualState);
}

void KeybindRecorder::setUnsetPlaceholder(std::string_view text) {
  m_unsetPlaceholder = std::string(text);
  refreshLabel();
}

void KeybindRecorder::setRecordingPlaceholder(std::string_view text) {
  m_recordingPlaceholder = std::string(text);
  refreshLabel();
}

void KeybindRecorder::setOnCommit(std::function<void(KeyChord)> callback) { m_onCommit = std::move(callback); }

void KeybindRecorder::setOnReject(std::function<void(KeybindRejection)> callback) { m_onReject = std::move(callback); }

void KeybindRecorder::reject(KeybindRejection reason) {
  exitRecording(false);
  if (m_onReject) {
    m_onReject(reason);
    return;
  }
  if (gDefaultOnReject) {
    gDefaultOnReject(reason);
  }
}

void KeybindRecorder::setModifierPolicy(ModifierPolicy policy) { m_modifierPolicy = policy; }

void KeybindRecorder::setSuperAllowed(bool allowed) {
  if (m_superAllowed == allowed) {
    return;
  }
  m_superAllowed = allowed;
  if (m_recording) {
    refreshLabel();
  }
}

void KeybindRecorder::setEscapeCancels(bool enabled) { m_escapeCancels = enabled; }

void KeybindRecorder::setOnRecordingChanged(std::function<void(bool)> callback) {
  m_onRecordingChanged = std::move(callback);
}

void KeybindRecorder::setTabFocusKey(std::string key) {
  if (m_inputArea != nullptr) {
    m_inputArea->setTabFocusKey(std::move(key));
  }
}

void KeybindRecorder::doLayout(Renderer& renderer) {
  if (m_label != nullptr) {
    m_label->measure(renderer);
  }
  if (m_glyph != nullptr) {
    m_glyph->measure(renderer);
  }
  Flex::doLayout(renderer);
  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0F, 0.0F);
    m_inputArea->setFrameSize(width(), height());
  }
}

LayoutSize KeybindRecorder::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void KeybindRecorder::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void KeybindRecorder::handleKeyDown(std::uint32_t sym, std::uint32_t modifiers) {
  if (!m_recording) {
    return;
  }

  // Escape aborts the capture instead of becoming part of the chord; with
  // m_escapeCancels the key event is fully consumed here.
  if (m_escapeCancels && KeySymbol::isEscape(sym)) {
    exitRecording(false);
    return;
  }

  // Must run before the modifier-preview branch so a bare Super press is also caught.
  if (!m_superAllowed && (((m_pendingModifiers | modifiers) & KeyMod::Super) != 0 || KeySymbol::isSuperModifier(sym))) {
    reject(KeybindRejection::Super);
    return;
  }

  if (m_modifierPolicy == ModifierPolicy::Forbidden && KeySymbol::isModifier(sym)) {
    reject(KeybindRejection::Modifier);
    return;
  }

  if (KeySymbol::isModifier(sym)) {
    // Accumulate held modifiers from the modifier key's own symbol, so a held
    // Meta/Super survives even when the compositor's `modifiers` mask omits it.
    // Overwriting with `modifiers` alone would drop Meta mid-chord (only "Ctrl"
    // would end up recorded/displayed).
    m_pendingModifiers |= (modifiers | modifierBitForSym(sym));
    refreshLabel();
    return;
  }

  if (m_modifierPolicy == ModifierPolicy::Required && (m_pendingModifiers | modifiers) == 0 && isPrintableKey(sym)) {
    reject(KeybindRejection::Printable);
    return;
  }

  if (m_modifierPolicy == ModifierPolicy::Forbidden && (m_pendingModifiers | modifiers) != 0) {
    reject(KeybindRejection::Modifier);
    return;
  }

  // Shift-only + printable is just a character variant (e.g. Shift+l = L),
  // not a meaningful shortcut. Reject it in both Required and Optional modes.
  if ((m_pendingModifiers | modifiers) == KeyMod::Shift && isPrintableKey(sym)) {
    reject(KeybindRejection::Printable);
    return;
  }

  std::uint32_t effectiveModifiers = m_pendingModifiers | modifiers;
  // Shift is not a modifier for keys whose shifted form is a different
  // character (1 -> !): record the symbol and drop Shift, matching
  // KKeySequenceRecorder and the compositor's shortcut modifiers.
  if ((effectiveModifiers & KeyMod::Shift) != 0 && !shiftAllowedAsModifier(sym)) {
    effectiveModifiers &= ~KeyMod::Shift;
  }
  const std::uint32_t cleanModifiers = m_superAllowed ? effectiveModifiers : (effectiveModifiers & ~KeyMod::Super);
  KeyChord chord{.sym = sym, .modifiers = cleanModifiers};
  m_chord = chord;
  if (m_onCommit) {
    m_onCommit(chord);
  }
  exitRecording(true);
}

void KeybindRecorder::handleKeyUp(std::uint32_t sym, std::uint32_t modifiers) {
  if (!m_recording) {
    return;
  }
  if (KeySymbol::isModifier(sym)) {
    m_pendingModifiers = (m_pendingModifiers | modifiers) & ~modifierBitForSym(sym);
    refreshLabel();
  }
}

void KeybindRecorder::enterRecording() {
  if (!m_enabled || m_recording) {
    return;
  }
  m_recording = true;
  m_inputArea->setCapturesAllKeys(true);
  m_pendingModifiers = 0;
  if (m_onRecordingChanged) {
    m_onRecordingChanged(true);
  }
  applyVisualState(VisualState::Recording);
  refreshLabel();
}

void KeybindRecorder::exitRecording(bool /*commit*/) {
  if (!m_recording) {
    return;
  }
  m_recording = false;
  m_inputArea->setCapturesAllKeys(false);
  m_pendingModifiers = 0;
  if (m_onRecordingChanged) {
    m_onRecordingChanged(false);
  }
  applyVisualState(VisualState::Idle);
  refreshLabel();
}

void KeybindRecorder::refreshLabel() {
  if (m_label == nullptr) {
    return;
  }

  if (m_recording) {
    if (m_pendingModifiers != 0) {
      const KeyChord preview{.sym = 0, .modifiers = m_pendingModifiers};
      std::string text;
      if (m_superAllowed && (preview.modifiers & KeyMod::Super) != 0) {
        text += "Meta + ";
      }
      if ((preview.modifiers & KeyMod::Ctrl) != 0) {
        text += "Ctrl + ";
      }
      if ((preview.modifiers & KeyMod::Alt) != 0) {
        text += "Alt + ";
      }
      if ((preview.modifiers & KeyMod::Shift) != 0) {
        text += "Shift + ";
      }
      text += "…";
      m_label->setText(text);
    } else {
      m_label->setText(m_recordingPlaceholder);
    }
  } else if (m_chord.has_value() && m_chord->sym != 0) {
    m_label->setText(keyChordDisplayLabel(*m_chord));
  } else {
    m_label->setText(m_unsetPlaceholder);
  }
}

void KeybindRecorder::applyVisualState(VisualState state) {
  m_visualState = state;
  if (!m_enabled) {
    setFill(colorSpecFromRole(ColorRole::SurfaceContainer, 0.4F));
    setBorder(colorSpecFromRole(ColorRole::OutlineVariant, Style::disabledOutlineAlpha), Style::borderWidth);
    if (m_label != nullptr) {
      m_label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55F));
    }
    if (m_glyph != nullptr) {
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55F));
    }
    return;
  }

  switch (state) {
  case VisualState::Idle:
    setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
    setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    if (m_label != nullptr) {
      const bool placeholder = !m_chord.has_value() || m_chord->sym == 0;
      m_label->setColor(
          placeholder ? colorSpecFromRole(ColorRole::OnSurfaceVariant) : colorSpecFromRole(ColorRole::OnSurface)
      );
    }
    if (m_glyph != nullptr) {
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    }
    break;
  case VisualState::Focused:
    setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
    setBorder(focusRingColorSpec(), Style::focusRingWidth);
    if (m_label != nullptr) {
      const bool placeholder = !m_chord.has_value() || m_chord->sym == 0;
      m_label->setColor(
          placeholder ? colorSpecFromRole(ColorRole::OnSurfaceVariant) : colorSpecFromRole(ColorRole::OnSurface)
      );
    }
    if (m_glyph != nullptr) {
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    }
    break;
  case VisualState::Recording:
    setFill(colorSpecFromRole(ColorRole::PrimaryContainer));
    setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
    if (m_label != nullptr) {
      m_label->setColor(colorSpecFromRole(ColorRole::OnPrimaryContainer));
    }
    if (m_glyph != nullptr) {
      m_glyph->setColor(colorSpecFromRole(ColorRole::OnPrimaryContainer));
    }
    break;
  }
}

namespace ui {
  void setDefaultKeybindRejectionHandler(std::function<void(KeybindRejection)> handler) {
    gDefaultOnReject = std::move(handler);
  }
} // namespace ui
