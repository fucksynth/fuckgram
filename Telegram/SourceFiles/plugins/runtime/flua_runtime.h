/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "plugins/plugins_hooks.h"

namespace Plugins::Flua {

class Plugin;

// Loads plugins from <working dir>/plugins (.flua files and unpacked
// development folders) and connects them to the hook layer.
void Start();
void Stop();

[[nodiscard]] QString PluginsPath();
[[nodiscard]] QString PluginsDataPath();

class Host final : public Sink {
public:
	Host();
	~Host();

	void start();
	void stop();

	// A plugin started or stopped: enable only the hooks in use.
	void refreshHooks();

	SendTextResult sendText(const SendTextRequest &request) override;
	void newMessage(not_null<HistoryItem*> item) override;
	void appState(AppState state) override;
	void messageMenu(
		not_null<HistoryItem*> item,
		std::vector<MenuItem> &items) override;
	void peerMenu(
		PeerMenuPlace place,
		not_null<PeerData*> peer,
		std::vector<MenuItem> &items) override;
	void mainMenu(std::vector<MenuItem> &items) override;
	bool hasPluginManager() override;
	void openPluginManager(Window::SessionController *controller) override;

private:
	struct Invalid {
		QString origin;
		QString error;
	};

	void load(const QString &path);

	std::vector<std::unique_ptr<Plugin>> _plugins;
	std::vector<Invalid> _invalid;

};

} // namespace Plugins::Flua
