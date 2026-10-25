#pragma once

#include "FreeInkUICore.h"

namespace freeink {
namespace ui {

// Compact overrides retain inheritance without storing bitmap-capable Paints per widget.
struct BoxStyleOverride {
  enum Field : uint8_t { Background = 1, Foreground = 2, Border = 4, BorderWidth = 8, Radius = 16 };
  uint8_t fields = 0;
  Color background = Color::Transparent;
  Color foreground = Color::Black;
  Color border = Color::Transparent;
  uint8_t borderWidth = 0;
  uint8_t radius = 0;

  static Paint paint(Color color) {
    if (color == Color::Transparent) return Paint::none();
    return color == Color::Black || color == Color::White ? Paint::solid(color) : Paint::dither(color);
  }
  void apply(BoxStyle& box) const {
    if (fields & Background) box.background = paint(background);
    if (fields & Foreground) box.foreground = paint(foreground);
    if (fields & Border) box.border = paint(border);
    if (fields & BorderWidth) box.borderWidth = borderWidth;
    if (fields & Radius) box.radius = radius;
  }
};

struct TextStyleOverride {
  enum Field : uint8_t { Font = 1, Alignment = 2, Ink = 4, Lines = 8, Bold = 16, Inverted = 32 };
  uint8_t fields = 0;
  TextStyle value{};
  void apply(TextStyle& text) const {
    if (fields & Font) text.font = value.font;
    if (fields & Alignment) text.align = value.align;
    if (fields & Ink) text.color = value.color;
    if (fields & Lines) text.maxLines = value.maxLines;
    if (fields & Bold) text.bold = value.bold;
    if (fields & Inverted) text.inverted = value.inverted;
  }
};

enum class MarkerWidth : uint8_t { Row, Text };

struct ComponentStyle {
  TextStyleOverride text{};
  BoxStyleOverride normal{}, selected{}, focused{}, active{}, disabled{};
  Insets padding{};
  Align contentAlign = Align::Start;
  MarkerWidth markerWidth = MarkerWidth::Row;

  void apply(StyleSet& styles) const {
    if (text.fields & TextStyleOverride::Ink) {
      const Paint ink = BoxStyleOverride::paint(text.value.color);
      styles.normal.foreground = styles.selected.foreground = styles.focused.foreground = ink;
      styles.active.foreground = styles.disabled.foreground = ink;
    }
    normal.apply(styles.normal);
    selected.apply(styles.selected);
    focused.apply(styles.focused);
    active.apply(styles.active);
    disabled.apply(styles.disabled);
    if (normal.fields || selected.fields || focused.fields || active.fields || disabled.fields)
      styles.explicitlySet = true;
  }

  Rect alignContent(Rect rect, int height) const {
    if (height < 0 || height >= rect.height || contentAlign == Align::Stretch) return rect;
    const int extra = rect.height - height;
    if (contentAlign == Align::Center) rect.y += extra / 2;
    else if (contentAlign == Align::End) rect.y += extra;
    rect.height = static_cast<int16_t>(height);
    return rect;
  }
};

static_assert(sizeof(ComponentStyle) <= 64, "Component overrides must remain compact");

}  // namespace ui
}  // namespace freeink
