/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/data_peer_id.h"
#include "plugins/plugins_hooks.h"
#include "plugins/runtime/flua_package.h"
#include "plugins/runtime/flua_tl.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointer>

struct lua_State;
struct lua_Debug;

class QNetworkAccessManager;
class QNetworkReply;

class HistoryItem;
class PeerData;

namespace MTP {
class Error;
} // namespace MTP

namespace Plugins::Flua {

class Host;

enum class Event {
	MessageNew,  // plugin.on("message.new", fn(message))
	MessageSend, // plugin.on("message.send", fn(draft)) -> nil | false | text
	AppState,    // plugin.on("app.state", fn(state))
	UrlOpen,     // plugin.on("url.open", fn(link)) -> true handles it
	Action,      // plugin.on("action", fn(action)) -> false cancels it
	Display,     // plugin.on("display", fn(shown)) -> text replaces it
	Screen,      // plugin.on("screen", fn(screen))
	ProfileRows, // plugin.on("profile.rows", fn(peer)) -> { {label, value} }

	// Raw MTProto, decoded with the TL schema. An optional third argument
	// of plugin.on() lists the methods a handler wants, e.g.
	// plugin.on("mtproto.request", fn, { "messages.sendMessage" }).
	MtprotoRequest,  // fn(call) -> nil | false (cancel) | call (replace)
	MtprotoResponse, // fn(result, method) -> nil | value (replace)
	MtprotoUpdates,  // fn(updates) -> nil | false (drop) | updates

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
	[[nodiscard]] bool urlOpen(const QString &url, bool local);
	[[nodiscard]] bool action(const ClientEventArgs &args);
	[[nodiscard]] std::optional<QString> display(
		DisplayTextPlace place,
		PeerData *peer,
		HistoryItem *item,
		const QString &text);
	void screen(Plugins::Screen screen, PeerData *peer);
	void profileRows(not_null<PeerData*> peer, std::vector<ProfileRow> &rows);

	// Some handler of the event takes this method (0: unknown method).
	[[nodiscard]] bool wants(Event event, mtpTypeId method) const;
	[[nodiscard]] RawResult mtprotoRequest(
		const mtpBuffer &body,
		mtpTypeId method);
	[[nodiscard]] std::optional<mtpBuffer> mtprotoResponse(
		const mtpBuffer &reply,
		mtpTypeId method);
	[[nodiscard]] RawResult mtprotoUpdates(const mtpBuffer &updates);

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
	static int ApiComposeGet(lua_State *L);
	static int ApiComposeSet(lua_State *L);
	static int ApiRefreshDisplay(lua_State *L);
	static int ApiAsync(lua_State *L);
	static int ApiTimerAfter(lua_State *L);
	static int ApiTimerEvery(lua_State *L);
	static int ApiTimerCancel(lua_State *L);
	static int ApiHttpRequest(lua_State *L);
	static int ApiJsonEncode(lua_State *L);
	static int ApiJsonDecode(lua_State *L);
	static int ApiInvoke(lua_State *L);
	static int RunProtected(lua_State *L);

	struct Timer {
		int ref = 0;
		crl::time interval = 0;
		bool repeat = false;
	};
	static int AddTimer(lua_State *L, bool repeat);
	void scheduleTimer(int id);
	void fireTimer(int id, int generation);

	// Coroutines started by tg.async: thread -> registry reference.
	// `nargs` values are on the thread's stack. `from` is the thread that
	// resumes it (nullptr from the event loop), so Lua keeps counting the
	// C stack depth and nested tg.async can't overflow it.
	void resume(lua_State *thread, lua_State *from, int nargs);
	void finishRequest(
		QPointer<QNetworkReply> reply,
		int generation,
		int callbackRef,
		lua_State *thread);
	// Exactly one of reply / error.
	void finishInvoke(
		const mtpBuffer *reply,
		const MTP::Error *error,
		Tl::Type type,
		int generation,
		int callbackRef,
		lua_State *thread);
	// Gives the `nresults` values `push` makes to the callback of a
	// finished async call, or resumes the coroutine waiting for them.
	void deliver(
		int callbackRef,
		lua_State *thread,
		int nresults,
		Fn<void(lua_State*)> push);
	void forgetThread(lua_State *thread);

	void openLibraries();
	void registerApi();
	void runFile(const QString &path); // inside a protected call
	void requirePermission(const char *permission);

	// Stack: function, nargs arguments. Pops them, pushes nresults on
	// success. On error logs it and returns false.
	bool call(int nargs, int nresults, crl::time budget);
	bool callGlobal(const char *name, crl::time budget);
	// Runs `body` in a protected call with `nargs` values from the stack
	// as its arguments, leaves `nresults` of what it pushed. On error pops
	// it and logs it with fail(), or only stores it into `error`.
	bool protect(
		int nargs,
		int nresults,
		Fn<void(lua_State*)> body,
		QString *error = nullptr);
	[[nodiscard]] bool accepts(int ref, mtpTypeId method) const;
	void fail(const QString &error);

	void pushMessage(not_null<HistoryItem*> item);
	// Pushes { peer = ..., message = {...} } with what is known.
	void pushContext(PeerData *peer, HistoryItem *item);
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

	// Bumped on every start / stop: async results of an older run are
	// dropped instead of touching a closed or restarted state.
	int _generation = 0;
	int _nextTimerId = 0;
	std::map<int, Timer> _timers;
	std::map<lua_State*, int> _threads;
	std::unique_ptr<QNetworkAccessManager> _network;
	std::vector<QPointer<QNetworkReply>> _requests;
	int _invokes = 0; // tg.invoke calls waiting for an answer

	// Handler reference -> the only methods it wants, all if absent.
	std::map<int, base::flat_set<mtpTypeId>> _methodFilters;

};

// Ids as the Bot API writes them: user > 0, basic group < 0,
// channel / supergroup -100xxxxxxxxxx.
[[nodiscard]] int64 BotApiId(PeerId id);
[[nodiscard]] PeerId FromBotApiId(int64 id);

} // namespace Plugins::Flua
