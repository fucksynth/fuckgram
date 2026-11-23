/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "settings/settings_type.h"

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Plugins::Flua {

// A row a plugin adds to a settings section (plugin.menu{ place =
// "settings.chats", ... }).
struct SettingsRow {
	QString text;
	const style::icon *icon = nullptr;
	Fn<void()> handler;
};
void AddSettingsRows(
	not_null<Ui::VerticalLayout*> container,
	std::vector<SettingsRow> rows);

// A page of a plugin as a settings section. `builder` is a page builder
// function of the run `generation`, or kNoRef for the plugin's settings
// page (plugin.settings.page), which survives restarts.
[[nodiscard]] Settings::Type PluginPageType(
	const QString &pluginId,
	int builder,
	int generation,
	const QString &title);
void OpenPluginPage(
	not_null<Window::SessionController*> controller,
	const QString &pluginId,
	int builder,
	int generation,
	const QString &title);

} // namespace Plugins::Flua
