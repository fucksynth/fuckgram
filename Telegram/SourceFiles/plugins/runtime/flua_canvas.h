/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/unique_qptr.h"

struct lua_State;
class QPainter;

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Plugins::Flua {

// "#rgb", "#rrggbb", "#aarrggbb", a theme color name ("windowBg",
// "windowActiveTextFg"...) or { r, g, b, a } at `index`; nullopt for
// anything else.
[[nodiscard]] std::optional<QColor> ReadColor(lua_State *L, int index);
[[nodiscard]] std::optional<QColor> ThemeColor(const QString &name);
[[nodiscard]] const std::vector<QString> &ThemeColorNames();

// A client icon by a short name ("palette", "link", "lock"...), nullptr
// for an unknown name.
[[nodiscard]] const style::icon *IconByName(const QString &name);
[[nodiscard]] std::vector<QString> IconNames();

// Pictures of a plugin by the ids tg.ui.image() gave.
using ImageLookup = Fn<const QImage*(int id)>;

// Pushes the canvas of one paint call. Its methods draw with `p` in
// unscaled pixels, the interface scale is applied; `images` must live
// until InvalidateCanvas(), after which the methods raise an error.
void PushCanvas(
	lua_State *L,
	not_null<QPainter*> p,
	QSize size,
	not_null<const ImageLookup*> images);
void InvalidateCanvas(lua_State *L, int index);

enum class OverlayDrag {
	Start,
	Move,
	Drop,
};

// A widget over the window: painted, clicked and dragged by a plugin.
// Sizes are unscaled pixels, `paint` gets them too. A draggable overlay
// follows the cursor by itself once a press moves past the drag
// distance; `drag` gets its top-left in the window then.
struct OverlayDescriptor {
	QRect geometry; // relative to `corner`
	Qt::Corner corner = Qt::TopLeftCorner;
	bool clickThrough = true;
	bool draggable = false;
	Fn<void(QPainter&, QSize)> paint;
	Fn<void(QPoint)> click;
	Fn<void(OverlayDrag, QPoint)> drag;
};
[[nodiscard]] base::unique_qptr<Ui::RpWidget> CreateOverlay(
	not_null<Ui::RpWidget*> parent,
	OverlayDescriptor &&descriptor);
void MoveOverlay(
	not_null<Ui::RpWidget*> overlay,
	QRect geometry,
	Qt::Corner corner);

// The overlay is being dragged: it does not take moves from the plugin.
[[nodiscard]] bool OverlayDragging(not_null<Ui::RpWidget*> overlay);

} // namespace Plugins::Flua
