/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/data_peer_id.h"
#include "plugins/runtime/flua_package.h"

#include <QtCore/QJsonObject>

struct lua_State;
struct lua_Debug;

class HistoryItem;
class PeerData;

namespace Plugins::Flua {

class Host;

enum class Event {
	MessageNew,  // plugin.on("message.new", fn(message))
	MessageSend, // plugin.on("message.send", fn(draft)) -> nil | false | text
	AppState,    // plugin.on("app.state", fn(state))

	Count,
};

enum class MenuPlace {
	Message,
	Main,
	Chat,
	Profile,
};

struct MenuEntry {
	MenuPlace place = MenuPlace::Message;
	QString text;
	int ref = 0; // Lua registry reference of the handler
};

struct SendVerdict {
	bool cancel = false;
	std::optional<QString> text; // replacement
};

// One plugin: its package, its own lua_State and what it registered.
// Everything runs on the main thread.
class Plugin final : public base::has_weak_ptr {
public:
	Plugin(not_null<Host*> host, Package &&package, QString dataPath);
	~Plugin();

	[[nodiscard]] const Manifest &manifest() const;
	[[nodiscard]] const QString &origin() const;
	[[nodiscard]] bool running() const;
	[[nodiscard]] const QString &lastError() const;

	bool start();
	void stop();

	[[nodiscard]] bool handles(Event event) const;
	[[nodiscard]] const std::vector<MenuEntry> &menu() const;

	void messageNew(not_null<HistoryItem*> item);
	[[nodiscard]] SendVerdict messageSend(
		not_null<PeerData*> peer,
		const QString &text);
	void appState(const QString &state);
	void menuClicked(int ref, PeerData *peer, HistoryItem *item);

private:
	[[nodiscard]] static Plugin *From(lua_State *L);
	static void *Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
	static void CountHook(lua_State *L, lua_Debug *ar);
	static int Traceback(lua_State *L);
	static int Setup(lua_State *L);

	static int ApiPrint(lua_State *L);
	static int ApiRequire(lua_State *L);
	static int ApiOn(lua_State *L);
	static int ApiMenu(lua_State *L);
	static int ApiSettingsGet(lua_State *L);
	static int ApiSettingsSet(lua_State *L);
	static int ApiToast(lua_State *L);
	static int ApiSendMessage(lua_State *L);

	void openLibraries();
	void registerApi();
	void runFile(const QString &path); // inside a protected call
	void requirePermission(const char *permission);

	// Stack: function, nargs arguments. Pops them, pushes nresults on
	// success. On error logs it and returns false.
	bool call(int nargs, int nresults, crl::time budget);
	bool callGlobal(const char *name, crl::time budget);
	void fail(const QString &error);

	void pushMessage(not_null<HistoryItem*> item);
	void loadSettings();
	void saveSettings();

	const not_null<Host*> _host;
	Package _package;
	const QString _dataPath;

	lua_State *_lua = nullptr;
	size_t _memory = 0;
	crl::time _deadline = 0;
	int _errors = 0;
	QString _lastError;

	std::array<std::vector<int>, size_t(Event::Count)> _handlers;
	std::vector<MenuEntry> _menu;
	QJsonObject _settings;

};

// Ids as the Bot API writes them: user > 0, basic group < 0,
// channel / supergroup -100xxxxxxxxxx.
[[nodiscard]] int64 BotApiId(PeerId id);
[[nodiscard]] PeerId FromBotApiId(int64 id);

} // namespace Plugins::Flua
