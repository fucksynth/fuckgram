/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

// Pieces the plugin manager and the plugin catalog share.

namespace Plugins::Flua {

struct PluginView;

struct Badge {
	QString text;
	QColor color;
};

// The picture of the plugin (view.icon) or the first letter of its name
// on a color picked by its id, in a rounded square.
[[nodiscard]] QImage PrepareIcon(
	const PluginView &view,
	int size,
	int radius);

// Lays out badges in lines of `width`, paints them if `p` is given.
// Returns the height they take.
int LayoutBadges(
	const std::vector<Badge> &badges,
	int width,
	QPainter *p = nullptr,
	QPoint origin = QPoint());

} // namespace Plugins::Flua
