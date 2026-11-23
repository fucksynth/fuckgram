/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/data_peer_id.h"
#include "base/unique_qptr.h"
#include "plugins/plugins_hooks.h"
#include "plugins/runtime/flua_canvas.h"
#include "plugins/runtime/flua_package.h"
#include "plugins/runtime/flua_tl.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QPointer>

struct lua_State;
struct lua_Debug;

class QNetworkAccessManager;
class QNetworkReply;

class HistoryItem;
class PeerData;
class QPainter;

namespace Ui {
class RpWidget;
} // namespace Ui

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

inline constexpr auto kNoRef = -2; // LUA_NOREF

enum class MenuPlace {
	Message,
	Main,
	Chat,
	Profile,
	Send,     // the menu of the send button
	Settings, // a row in a settings section
};

struct MenuEntry {
	int id = 0;
	MenuPlace place = MenuPlace::Message;
	QString section; // Settings: "main", "chats", "fuckgram"...
	QString text;
	QString icon; // a name for IconByName()
	int ref = kNoRef; // on_click: Lua registry reference
	int page = kNoRef; // page builder, opens a page on click
};

// A row of a plugin page, as its builder function returned it.
struct PageItem {
	enum class Type {
		Header,
		Divider,
		Switch,
		Selector,
		Input,
		Text,
		Slider,
		Custom,
	};
	Type type = Type::Text;
	QString key; // the setting it changes
	QString text;
	QString subtext;
	QString icon;
	QString placeholder;
	std::vector<QString> items; // Selector
	QJsonValue fallback; // the value while the setting is not set
	double min = 0.;
	double max = 100.;
	double step = 1.;
	int height = 0; // Custom
	bool accent = false;
	bool red = false;
	int onChange = kNoRef;
	int onClick = kNoRef;
	int page = kNoRef; // a sub page builder
	int paint = kNoRef; // Custom
};

struct Page {
	QString title;
	std::vector<PageItem> items;
	std::vector<int> refs; // the handlers, released with the page
	int generation = 0;
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
	[[nodiscard]] const std::map<QString, QByteArray> &files() const;
	[[nodiscard]] bool running() const;
	[[nodiscard]] const QString &lastError() const;
	// The manifest picture, null if there is none or it is not an image.
	[[nodiscard]] QImage icon() const;
	// What the plugin logged and its errors, oldest first.
	[[nodiscard]] const std::deque<QString> &journal() const;

	bool start();
	void stop();

	[[nodiscard]] bool handles(Event event) const;
	[[nodiscard]] const std::vector<MenuEntry> &menu() const;
	// Bumped on every start / stop.
	[[nodiscard]] int generation() const;

	// Pages. A page builder is a Lua function that returns the rows.
	[[nodiscard]] bool hasSettingsPage() const;
	// Calls a builder: the settings page of the plugin for kNoRef. The
	// handlers the page keeps must be released with releasePage().
	[[nodiscard]] std::optional<Page> buildPage(int ref, QString *error);
	void releasePage(const Page &page);
	// plugin.settings.refresh(): open pages of the plugin build again.
	[[nodiscard]] rpl::producer<> pageRefreshes() const;
	[[nodiscard]] QJsonValue setting(const QString &key) const;
	void setSetting(const QString &key, const QJsonValue &value);
	// on_change(value), or on_click() for an undefined value; handlers
	// of a page built in another run are not called.
	void pageCallback(int generation, int ref, const QJsonValue &value);
	// Paints a custom row, `size` in unscaled pixels.
	void paintCanvas(int generation, int ref, QPainter &p, QSize size);

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
	static int ApiEditMessage(lua_State *L);
	static int ApiDeleteMessages(lua_State *L);
	static int ApiGetMessage(lua_State *L);
	static int ApiPeer(lua_State *L);
	static int ApiMe(lua_State *L);
	static int ApiCurrentChat(lua_State *L);
	static int ApiAlert(lua_State *L);
	static int ApiConfirm(lua_State *L);
	static int ApiInput(lua_State *L);
	static int ApiCopy(lua_State *L);
	static int ApiResource(lua_State *L);
	static int ApiFileRead(lua_State *L);
	static int ApiFileWrite(lua_State *L);
	static int ApiFileRemove(lua_State *L);
	static int ApiFileList(lua_State *L);
	static int ApiMenuRemove(lua_State *L);
	static int ApiSettingsPage(lua_State *L);
	static int ApiSettingsRefresh(lua_State *L);
	static int ApiSettingsAll(lua_State *L);
	static int ApiSettingsReset(lua_State *L);
	static int ApiDialog(lua_State *L);
	static int ApiCurrent(lua_State *L);
	static int ApiOpen(lua_State *L);
	static int ApiColor(lua_State *L);
	static int ApiImage(lua_State *L);
	static int ApiImageFree(lua_State *L);
	static int ApiOverlay(lua_State *L);
	static int OverlayUpdate(lua_State *L);
	static int OverlayMove(lua_State *L);
	static int OverlayClose(lua_State *L);
	static int ApiSendFile(lua_State *L);
	static int ApiDump(lua_State *L);
	static int ApiWindowSize(lua_State *L);
	static int ApiNow(lua_State *L);
	static int ApiPlayerState(lua_State *L);
	static int ApiPlayerToggle(lua_State *L);
	static int ApiPlayerPlay(lua_State *L);
	static int ApiPlayerPause(lua_State *L);
	static int ApiPlayerNext(lua_State *L);
	static int ApiPlayerPrevious(lua_State *L);
	static int ApiPlayerStop(lua_State *L);
	static int ApiPlayerSeek(lua_State *L);
	// Reads the menu entry table at `index` (plugin.menu{...}).
	[[nodiscard]] MenuEntry readMenuEntry(lua_State *L, int index);
	// Reads the rows the builder returned, at `index`.
	void readPage(lua_State *L, int index, Page &page);
	void closeOverlay(int id);
	void paintOverlay(int id, QPainter &p, QSize size);
	void clickOverlay(int id, QPoint point);
	// plugins_data/<id>/files/<name> for a checked name, raises otherwise.
	[[nodiscard]] QString filePath(lua_State *L, int index) const;

	// Where the answer of an async call goes: a callback or the coroutine
	// that called it. Of an older run it is dropped.
	struct Waiter {
		int callbackRef = -2; // LUA_NOREF: no callback
		lua_State *thread = nullptr;
		int generation = 0;
	};
	// The callback at `index` or the running tg.async coroutine, raises a
	// Lua error if there is neither.
	[[nodiscard]] Waiter waiter(lua_State *L, int index, const char *what);
	int wait(lua_State *L, const Waiter &waiter); // return from the API
	void answer(const Waiter &waiter, Fn<void(lua_State*)> push);
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
	void requirePermission(lua_State *L, const char *permission);

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

	void pushMessage(lua_State *L, not_null<HistoryItem*> item);
	void pushPeer(lua_State *L, not_null<PeerData*> peer);
	// Pushes { peer = ..., message = {...} } with what is known.
	void pushContext(PeerData *peer, HistoryItem *item);
	void loadSettings();
	void saveSettings();

	const not_null<Host*> _host;
	Package _package;
	const QString _dataPath;

	lua_State *_lua = nullptr;
	size_t _memory = 0;
	int _running = 0; // plugin code runs: the memory limit applies
	crl::time _deadline = 0;
	int _errors = 0;
	QString _lastError;

	std::array<std::vector<int>, size_t(Event::Count)> _handlers;
	std::vector<MenuEntry> _menu;
	int _nextMenuId = 0;
	QJsonObject _settings;
	int _settingsPage = kNoRef;
	// Builders of pages that may still be opened: kept until stop().
	base::flat_set<int> _pageBuilders;
	rpl::event_stream<> _pageRefreshes;

	struct OverlayState {
		base::unique_qptr<Ui::RpWidget> widget;
		QRect geometry; // unscaled, relative to the corner
		Qt::Corner corner = Qt::TopLeftCorner;
		int paint = kNoRef;
		int click = kNoRef;
	};
	std::map<int, OverlayState> _overlays;
	int _nextOverlayId = 0;
	std::map<int, QImage> _images;
	int _nextImageId = 0;
	ImageLookup _imageLookup;

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

	std::deque<QString> _journal;
	mutable std::optional<QImage> _icon;

	void addToJournal(const QString &text);

};

// Ids as the Bot API writes them: user > 0, basic group < 0,
// channel / supergroup -100xxxxxxxxxx.
[[nodiscard]] int64 BotApiId(PeerId id);
[[nodiscard]] PeerId FromBotApiId(int64 id);

} // namespace Plugins::Flua
