/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "plugins/plugins_hooks.h"

namespace Plugins::Flua {

class Plugin;

// Loads plugins from <working dir>/plugins (.flua files and unpacked
// development folders) and connects them to the hook layer.
void Start();
void Stop();

[[nodiscard]] QString PluginsPath();
[[nodiscard]] QString PluginsDataPath();

class Host final : public Sink, public base::has_weak_ptr {
public:
	Host();
	~Host();

	void start();
	void stop();

	// A plugin started, stopped or registered something: enable only
	// the hooks in use.
	void refreshHooks();

	SendTextResult sendText(const SendTextRequest &request) override;
	void newMessage(not_null<HistoryItem*> item) override;
	bool openUrl(const QString &url, bool local) override;
	void appState(AppState state) override;
	void screen(Plugins::Screen screen, PeerData *peer) override;
	void peerMenu(
		PeerMenuPlace place,
		not_null<PeerData*> peer,
		std::vector<MenuItem> &items) override;
	bool clientEvent(const ClientEventArgs &args) override;
	void messageMenu(
		not_null<HistoryItem*> item,
		std::vector<MenuItem> &items) override;
	std::optional<QString> displayText(
		DisplayTextPlace place,
		PeerData *peer,
		HistoryItem *item,
		const QString &text) override;
	void mainMenu(std::vector<MenuItem> &items) override;
	void profileRows(
		not_null<PeerData*> peer,
		std::vector<ProfileRow> &rows) override;
	bool hasPluginManager() override;
	void openPluginManager(Window::SessionController *controller) override;

private:
	struct Invalid {
		QString origin;
		QString error;
	};

	void load(const QString &path);
	void setPluginEnabled(const QString &id, bool enabled);
	void offerInstall(const QString &path);
	void install(const QString &path, const QString &id);
	void loadDisabled();
	void saveDisabled();

	std::vector<std::unique_ptr<Plugin>> _plugins;
	std::vector<Invalid> _invalid;
	base::flat_set<QString> _disabled;

};

} // namespace Plugins::Flua
