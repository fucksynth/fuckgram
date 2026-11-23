/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "settings/settings_type.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

// Settings > Plugins: the list of plugins.
[[nodiscard]] Type PluginsId();
// Settings > Plugins > Plugin engine.
[[nodiscard]] Type PluginsEngineId();

} // namespace Settings

namespace Plugins::Flua {

struct PluginView;

// What a plugin from a file is and can do; `install` gets whether to turn
// it on right away. `trusted`: from a verified catalog channel.
void ShowInstallBox(
	not_null<Window::SessionController*> controller,
	PluginView view,
	bool trusted,
	Fn<void(bool enable)> install);

} // namespace Plugins::Flua
