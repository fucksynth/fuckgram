/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/flat_set.h"
#include "base/timer.h"
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

// The English or the Russian text, by the language of the client.
[[nodiscard]] QString Translated(const char *english, const char *russian);
// What a permission lets a plugin do, for the install box.
[[nodiscard]] QString PermissionText(const QString &permission);
// Stars, gifts, payments, sign in, sessions, the password, the account:
// a plugin calls these only after the user allows it.
[[nodiscard]] bool IsSensitiveMethod(const QString &name);

struct EngineSettings {
	bool enabled = true;          // plugins run at all
	bool devMode = true;          // development folders reload themselves
	bool compact = false;         // short cards in the manager
	bool unknownSources = false;  // install .flua files sent in chats
	bool confirmSensitive = true; // see IsSensitiveMethod()
	bool safeMode = false;        // nothing starts; set after a crash

	friend inline bool operator==(
		const EngineSettings &,
		const EngineSettings &) = default;
};

// A plugin as the manager shows it.
struct PluginView {
	QString id;
	QString name;
	QString version;
	QString author;
	QString description;
	std::vector<QString> permissions;
	QString origin;
	QImage icon;
	QString error;
	bool dev = false; // an unpacked development folder
	bool enabled = false;
	bool running = false;
	bool settings = false; // has a settings page (plugin.settings.page)
};

struct InvalidView {
	QString origin;
	QString error;
};

class Host;
[[nodiscard]] Host *CurrentHost();

class Host final : public Sink, public base::has_weak_ptr {
public:
	Host();
	~Host();

	void start();
	void stop();

	// A plugin started, stopped or registered something: enable only
	// the hooks in use.
	void refreshHooks();

	// For the manager: what is loaded and when that changes.
	[[nodiscard]] std::vector<PluginView> pluginViews() const;
	// The loaded plugin of the id, nullptr if there is none.
	[[nodiscard]] Plugin *plugin(const QString &id) const;
	[[nodiscard]] std::vector<InvalidView> invalidViews() const;
	[[nodiscard]] std::deque<QString> journal(const QString &id) const;
	[[nodiscard]] rpl::producer<> changes() const;
	void notifyChanged();

	[[nodiscard]] const EngineSettings &engine() const;
	void setEngine(const EngineSettings &settings);
	void setPluginEnabled(const QString &id, bool enabled);
	// Shows what the package is and can do, installs it on "Install".
	// From a file or an own catalog channel only with "Unknown sources".
	enum class InstallSource {
		File,
		VerifiedChannel,
		Channel,
	};
	void offerInstall(const QString &path, InstallSource source);

	// Removes an installed .flua and the data of the plugin.
	void removePlugin(const QString &id);
	// A .flua of the plugin to send, packed for a development folder.
	[[nodiscard]] QString sharePath(const QString &id);

	// Asks the user about a sensitive request of a plugin.
	void confirmSensitive(
		const QString &pluginName,
		const QString &method,
		Fn<void(bool allowed)> done);

	// tg.invoke calls: the filters of plugins skip them.
	void ownRequest(mtpRequestId requestId, mtpTypeId method);
	void forgetOwnRequest(mtpRequestId requestId);

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
	void sendMenu(PeerData *peer, std::vector<MenuItem> &items) override;
	void settingsSection(
		const QString &section,
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) override;
	void profileRows(
		not_null<PeerData*> peer,
		std::vector<ProfileRow> &rows) override;
	RawResult request(const RawRequest &request) override;
	std::optional<mtpBuffer> response(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const mtpBuffer &reply) override;
	RawResult updates(
		not_null<MTP::Instance*> instance,
		const mtpBuffer &updates) override;
	bool hasPluginManager() override;
	void openPluginManager(Window::SessionController *controller) override;

private:
	struct Invalid {
		QString origin;
		QString error;
	};

	void load(const QString &path);
	void install(const QString &path, const QString &id, bool enable);
	void loadDisabled();
	void saveDisabled();
	void loadEngine();
	void saveEngine();
	[[nodiscard]] bool mayRun() const;
	void startAll();
	void stopAll();

	// Development folders reload themselves when their files change.
	void watchDevFolders();
	void checkDevFolders();
	void reload(const QString &origin);
	void unload(const QString &origin);

	static constexpr auto kMaxWatched = size_t(4096);

	std::vector<std::unique_ptr<Plugin>> _plugins;
	// Requests in flight -> their method, to decode the responses.
	std::map<mtpRequestId, mtpTypeId> _methods;
	base::flat_map<mtpRequestId, mtpTypeId> _own;
	std::vector<Invalid> _invalid;
	base::flat_set<QString> _disabled;
	base::Timer _devTimer;
	std::map<QString, QString> _devStamps; // folder -> files and times
	EngineSettings _engine;
	rpl::event_stream<> _changes;
	bool _changeQueued = false;

};

} // namespace Plugins::Flua
