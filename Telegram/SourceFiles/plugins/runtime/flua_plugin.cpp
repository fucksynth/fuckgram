/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_plugin.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "logs.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "plugins/plugins_hooks.h"
#include "plugins/runtime/flua_runtime.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "base/call_delayed.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

// Lua is built as C++ (see cmake/lib_lua.cmake): no extern "C" here.
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

namespace Plugins::Flua {
namespace {

constexpr auto kMemoryLimit = size_t(64 * 1024 * 1024);
constexpr auto kHookInstructions = 10000;
constexpr auto kMaxErrors = 5;
constexpr auto kStartBudget = crl::time(2000);
constexpr auto kFilterBudget = crl::time(50);
constexpr auto kEventBudget = crl::time(200);
constexpr auto kClickBudget = crl::time(1000);
constexpr auto kLoadedKey = "fuckgram.loaded";
constexpr auto kChannelShift = int64(1000000000000LL);
constexpr auto kMaxTimers = size_t(64);
constexpr auto kMinRepeat = lua_Integer(100);
constexpr auto kMaxDelay = lua_Integer(24 * 3600 * 1000);
constexpr auto kMaxRequests = 16;
constexpr auto kMaxRequestBody = 1024 * 1024;
constexpr auto kMaxResponseBody = qint64(4 * 1024 * 1024);
constexpr auto kRequestTimeout = 20000;
constexpr auto kMaxJsonDepth = 64;

void PushString(lua_State *L, const QString &value) {
	const auto utf8 = value.toUtf8();
	lua_pushlstring(L, utf8.constData(), size_t(utf8.size()));
}

[[nodiscard]] QString ToString(lua_State *L, int index) {
	auto size = size_t(0);
	const auto data = lua_tolstring(L, index, &size);
	return data ? QString::fromUtf8(data, int(size)) : QString();
}

[[nodiscard]] QString CheckString(lua_State *L, int index) {
	auto size = size_t(0);
	const auto data = luaL_checklstring(L, index, &size);
	return QString::fromUtf8(data, int(size));
}

void PushJson(lua_State *L, const QJsonValue &value, int depth) {
	if (depth > kMaxJsonDepth) {
		luaL_error(L, "JSON is nested too deep");
	}
	luaL_checkstack(L, 3, "JSON");
	switch (value.type()) {
	case QJsonValue::Bool:
		lua_pushboolean(L, value.toBool() ? 1 : 0);
		break;
	case QJsonValue::Double: {
		const auto number = value.toDouble();
		if (std::floor(number) == number
			&& std::abs(number) < 9007199254740992.) {
			lua_pushinteger(L, lua_Integer(number));
		} else {
			lua_pushnumber(L, number);
		}
	} break;
	case QJsonValue::String:
		PushString(L, value.toString());
		break;
	case QJsonValue::Array: {
		const auto array = value.toArray();
		lua_createtable(L, int(array.size()), 0);
		auto index = lua_Integer(0);
		for (const auto item : array) {
			PushJson(L, item, depth + 1);
			lua_rawseti(L, -2, ++index);
		}
	} break;
	case QJsonValue::Object: {
		const auto object = value.toObject();
		lua_createtable(L, 0, int(object.size()));
		for (auto i = object.begin(); i != object.end(); ++i) {
			PushJson(L, i.value(), depth + 1);
			lua_setfield(L, -2, i.key().toUtf8().constData());
		}
	} break;
	default:
		lua_pushnil(L);
		break;
	}
}

[[nodiscard]] QJsonValue ToJson(lua_State *L, int index, int depth) {
	index = lua_absindex(L, index);
	if (depth > kMaxJsonDepth) {
		luaL_error(L, "value is nested too deep for JSON");
	}
	luaL_checkstack(L, 3, "JSON");
	switch (lua_type(L, index)) {
	case LUA_TNIL:
		return QJsonValue(QJsonValue::Null);
	case LUA_TBOOLEAN:
		return QJsonValue(lua_toboolean(L, index) != 0);
	case LUA_TNUMBER:
		return lua_isinteger(L, index)
			? QJsonValue(qint64(lua_tointeger(L, index)))
			: QJsonValue(lua_tonumber(L, index));
	case LUA_TSTRING:
		return QJsonValue(ToString(L, index));
	case LUA_TTABLE: {
		const auto length = lua_Integer(lua_rawlen(L, index));
		auto count = lua_Integer(0);
		lua_pushnil(L);
		while (lua_next(L, index)) {
			++count;
			lua_pop(L, 1);
		}
		if (length > 0 && count == length) {
			auto array = QJsonArray();
			for (auto i = lua_Integer(1); i <= length; ++i) {
				lua_rawgeti(L, index, i);
				array.push_back(ToJson(L, -1, depth + 1));
				lua_pop(L, 1);
			}
			return array;
		}
		auto object = QJsonObject();
		lua_pushnil(L);
		while (lua_next(L, index)) {
			if (lua_type(L, -2) != LUA_TSTRING) {
				luaL_error(L, "JSON object keys must be strings");
			}
			const auto key = ToString(L, -2);
			object.insert(key, ToJson(L, -1, depth + 1));
			lua_pop(L, 1);
		}
		return object;
	}
	default:
		luaL_error(
			L,
			"a %s value can't be written as JSON",
			luaL_typename(L, index));
	}
	return QJsonValue();
}

[[nodiscard]] QByteArray ToBytes(lua_State *L, int index) {
	auto size = size_t(0);
	const auto data = lua_tolstring(L, index, &size);
	return data ? QByteArray(data, int(size)) : QByteArray();
}

[[nodiscard]] Main::Session *ActiveSession() {
	auto &account = Core::App().domain().active();
	return account.sessionExists() ? &account.session() : nullptr;
}

[[nodiscard]] const char *EventName(Event event) {
	switch (event) {
	case Event::MessageNew: return "message.new";
	case Event::MessageSend: return "message.send";
	case Event::AppState: return "app.state";
	case Event::UrlOpen: return "url.open";
	case Event::Action: return "action";
	case Event::Display: return "display";
	case Event::Screen: return "screen";
	case Event::ProfileRows: return "profile.rows";
	case Event::Count: break;
	}
	return "";
}

[[nodiscard]] const char *ActionName(ClientEvent event) {
	switch (event) {
	case ClientEvent::Reply: return "reply";
	case ClientEvent::Reaction: return "reaction";
	case ClientEvent::Call: return "call";
	case ClientEvent::Transcribe: return "transcribe";
	case ClientEvent::Notification: return "notification";
	case ClientEvent::PlayerNext: return "player.next";
	case ClientEvent::PlayerPrevious: return "player.previous";
	case ClientEvent::PlayerStop: return "player.stop";
	case ClientEvent::PlayMedia: return "play";
	case ClientEvent::OpenFile: return "file.open";
	case ClientEvent::SelectMessage: return "select";
	case ClientEvent::MessageMenu: return "message.menu";
	case ClientEvent::AvatarClick: return "header.click";
	}
	return "";
}

[[nodiscard]] const char *PlaceName(DisplayTextPlace place) {
	switch (place) {
	case DisplayTextPlace::Message: return "message";
	case DisplayTextPlace::ChatListName: return "chat.list";
	case DisplayTextPlace::ChatTitle: return "chat.title";
	case DisplayTextPlace::ChatSubtitle: return "chat.status";
	case DisplayTextPlace::CopiedText: return "copy";
	}
	return "";
}

[[nodiscard]] const char *ScreenName(Plugins::Screen screen) {
	switch (screen) {
	case Plugins::Screen::ChatList: return "chat.list";
	case Plugins::Screen::Chat: return "chat";
	case Plugins::Screen::Profile: return "profile";
	}
	return "";
}

} // namespace

int64 BotApiId(PeerId id) {
	if (peerIsUser(id)) {
		return int64(peerToUser(id).bare);
	} else if (peerIsChat(id)) {
		return -int64(peerToChat(id).bare);
	} else if (peerIsChannel(id)) {
		return -kChannelShift - int64(peerToChannel(id).bare);
	}
	return 0;
}

PeerId FromBotApiId(int64 id) {
	if (id > 0) {
		return peerFromUser(UserId(BareId(id)));
	} else if (id <= -kChannelShift) {
		return peerFromChannel(ChannelId(BareId(-kChannelShift - id)));
	} else if (id < 0) {
		return peerFromChat(ChatId(BareId(-id)));
	}
	return PeerId();
}

Plugin::Plugin(not_null<Host*> host, Package &&package, QString dataPath)
: _host(host)
, _package(std::move(package))
, _dataPath(std::move(dataPath)) {
}

Plugin::~Plugin() {
	stop();
}

const Manifest &Plugin::manifest() const {
	return _package.manifest;
}

const QString &Plugin::origin() const {
	return _package.origin;
}

bool Plugin::running() const {
	return _lua != nullptr;
}

const QString &Plugin::lastError() const {
	return _lastError;
}

Plugin *Plugin::From(lua_State *L) {
	return *static_cast<Plugin**>(lua_getextraspace(L));
}

void *Plugin::Alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	const auto plugin = static_cast<Plugin*>(ud);
	const auto old = ptr ? osize : size_t(0);
	if (nsize == 0) {
		plugin->_memory -= old;
		free(ptr);
		return nullptr;
	} else if (nsize > old
		&& plugin->_memory + (nsize - old) > kMemoryLimit) {
		return nullptr; // Lua raises "not enough memory".
	}
	const auto result = realloc(ptr, nsize);
	if (result) {
		plugin->_memory = plugin->_memory - old + nsize;
	}
	return result;
}

void Plugin::CountHook(lua_State *L, lua_Debug *ar) {
	if (crl::now() > From(L)->_deadline) {
		luaL_error(L, "time limit exceeded");
	}
}

int Plugin::Traceback(lua_State *L) {
	const auto message = lua_tostring(L, 1);
	luaL_traceback(L, L, message ? message : "(error object)", 1);
	return 1;
}

bool Plugin::start() {
	if (_lua) {
		return true;
	}
	++_generation;
	_errors = 0;
	_lastError = QString();
	_memory = 0;
	_lua = lua_newstate(&Plugin::Alloc, this);
	if (!_lua) {
		_lastError = u"could not create a Lua state"_q;
		return false;
	}
	*static_cast<Plugin**>(lua_getextraspace(_lua)) = this;
	loadSettings();

	lua_pushcfunction(_lua, &Plugin::Setup);
	if (!call(0, 0, kStartBudget)) {
		stop();
		return false;
	}
	if (!callGlobal("on_enable", kStartBudget)) {
		stop();
		return false;
	}
	LOG(("Plugins: started %1 %2"
		).arg(manifest().id
		).arg(manifest().version));
	return true;
}

void Plugin::stop() {
	if (!_lua) {
		return;
	}
	callGlobal("on_disable", kEventBudget);
	// Late timers and replies of this run must not reach the next one.
	++_generation;
	for (const auto &reply : base::take(_requests)) {
		if (reply) {
			reply->abort();
		}
	}
	_timers.clear();
	_threads.clear();
	lua_close(_lua);
	_lua = nullptr;
	for (auto &list : _handlers) {
		list.clear();
	}
	_menu.clear();
	_host->refreshHooks();
}

int Plugin::Setup(lua_State *L) {
	const auto plugin = From(L);
	plugin->openLibraries();
	plugin->registerApi();
	plugin->runFile(plugin->manifest().entry);
	return 0;
}

void Plugin::openLibraries() {
	const auto open = [&](const char *name, lua_CFunction function) {
		luaL_requiref(_lua, name, function, 1);
		lua_pop(_lua, 1);
	};
	open(LUA_GNAME, luaopen_base);
	open(LUA_STRLIBNAME, luaopen_string);
	open(LUA_TABLIBNAME, luaopen_table);
	open(LUA_MATHLIBNAME, luaopen_math);
	open(LUA_UTF8LIBNAME, luaopen_utf8);
	open(LUA_COLIBNAME, luaopen_coroutine);
	open(LUA_OSLIBNAME, luaopen_os);

	// No code loading from strings or files, no raw access around
	// metatables, no GC control.
	for (const auto name : {
			"dofile",
			"loadfile",
			"load",
			"collectgarbage",
			"rawget",
			"rawset" }) {
		lua_pushnil(_lua);
		lua_setglobal(_lua, name);
	}
	lua_getglobal(_lua, LUA_STRLIBNAME);
	lua_pushnil(_lua);
	lua_setfield(_lua, -2, "dump");
	lua_pop(_lua, 1);

	// os keeps only time functions.
	lua_getglobal(_lua, LUA_OSLIBNAME);
	for (const auto name : {
			"execute",
			"exit",
			"getenv",
			"remove",
			"rename",
			"setlocale",
			"tmpname" }) {
		lua_pushnil(_lua);
		lua_setfield(_lua, -2, name);
	}
	lua_pop(_lua, 1);

	lua_pushcfunction(_lua, &Plugin::ApiPrint);
	lua_setglobal(_lua, "print");
	lua_pushcfunction(_lua, &Plugin::ApiRequire);
	lua_setglobal(_lua, "require");

	lua_newtable(_lua);
	lua_setfield(_lua, LUA_REGISTRYINDEX, kLoadedKey);
}

void Plugin::registerApi() {
	const auto set = [&](const char *name, lua_CFunction function) {
		lua_pushcfunction(_lua, function);
		lua_setfield(_lua, -2, name);
	};

	// plugin = { id, name, version, on, menu, settings = { get, set } }
	lua_newtable(_lua);
	PushString(_lua, manifest().id);
	lua_setfield(_lua, -2, "id");
	PushString(_lua, manifest().name);
	lua_setfield(_lua, -2, "name");
	PushString(_lua, manifest().version);
	lua_setfield(_lua, -2, "version");
	set("on", &Plugin::ApiOn);
	set("menu", &Plugin::ApiMenu);
	lua_newtable(_lua);
	set("get", &Plugin::ApiSettingsGet);
	set("set", &Plugin::ApiSettingsSet);
	lua_setfield(_lua, -2, "settings");
	lua_setglobal(_lua, "plugin");

	// tg = { log, toast, messages = { send }, compose = { get, set },
	//       ui = { refresh } }
	lua_newtable(_lua);
	set("log", &Plugin::ApiPrint);
	set("toast", &Plugin::ApiToast);
	lua_newtable(_lua);
	set("send", &Plugin::ApiSendMessage);
	lua_setfield(_lua, -2, "messages");
	lua_newtable(_lua);
	set("get", &Plugin::ApiComposeGet);
	set("set", &Plugin::ApiComposeSet);
	lua_setfield(_lua, -2, "compose");
	lua_newtable(_lua);
	set("refresh", &Plugin::ApiRefreshDisplay);
	lua_setfield(_lua, -2, "ui");
	set("async", &Plugin::ApiAsync);
	lua_newtable(_lua);
	set("after", &Plugin::ApiTimerAfter);
	set("every", &Plugin::ApiTimerEvery);
	set("cancel", &Plugin::ApiTimerCancel);
	lua_setfield(_lua, -2, "timer");
	lua_newtable(_lua);
	set("request", &Plugin::ApiHttpRequest);
	lua_setfield(_lua, -2, "http");
	lua_newtable(_lua);
	set("encode", &Plugin::ApiJsonEncode);
	set("decode", &Plugin::ApiJsonDecode);
	lua_setfield(_lua, -2, "json");
	lua_setglobal(_lua, "tg");
}

void Plugin::runFile(const QString &path) {
	const auto i = _package.files.find(path);
	if (i == end(_package.files)) {
		luaL_error(_lua, "file not found in the package: %s", path.toUtf8().constData());
	}
	const auto chunk = QString(u"@"_q + path).toUtf8();
	// "t": text only, precompiled bytecode is refused.
	if (luaL_loadbufferx(
			_lua,
			i->second.constData(),
			size_t(i->second.size()),
			chunk.constData(),
			"t") != LUA_OK) {
		lua_error(_lua);
	}
	lua_call(_lua, 0, 0);
}

void Plugin::requirePermission(const char *permission) {
	if (!manifest().has(permission)) {
		luaL_error(
			_lua,
			"permission '%s' is not declared in manifest.json",
			permission);
	}
}

bool Plugin::call(int nargs, int nresults, crl::time budget) {
	const auto base = lua_gettop(_lua) - nargs;
	lua_pushcfunction(_lua, &Plugin::Traceback);
	lua_insert(_lua, base);
	_deadline = crl::now() + budget;
	lua_sethook(_lua, &Plugin::CountHook, LUA_MASKCOUNT, kHookInstructions);
	const auto status = lua_pcall(_lua, nargs, nresults, base);
	lua_sethook(_lua, nullptr, 0, 0);
	lua_remove(_lua, base);
	if (status != LUA_OK) {
		const auto error = ToString(_lua, -1);
		lua_pop(_lua, 1);
		fail(error);
		return false;
	}
	return true;
}

bool Plugin::callGlobal(const char *name, crl::time budget) {
	if (!_lua) {
		return false;
	}
	if (lua_getglobal(_lua, name) != LUA_TFUNCTION) {
		lua_pop(_lua, 1);
		return true;
	}
	return call(0, 0, budget);
}

void Plugin::fail(const QString &error) {
	_lastError = error;
	LOG(("Plugins Error: %1: %2").arg(manifest().id, error));
	if (++_errors >= kMaxErrors) {
		LOG(("Plugins: %1 stopped after %2 errors"
			).arg(manifest().id
			).arg(_errors));
		// Not right now: we may be inside a call into this state.
		crl::on_main(this, [=] {
			stop();
		});
	}
}

bool Plugin::handles(Event event) const {
	return _lua && !_handlers[size_t(event)].empty();
}

const std::vector<MenuEntry> &Plugin::menu() const {
	return _menu;
}

void Plugin::pushMessage(not_null<HistoryItem*> item) {
	lua_createtable(_lua, 0, 6);
	lua_pushinteger(_lua, lua_Integer(item->id.bare));
	lua_setfield(_lua, -2, "id");
	lua_pushinteger(_lua, BotApiId(item->history()->peer->id));
	lua_setfield(_lua, -2, "peer");
	lua_pushinteger(_lua, BotApiId(item->from()->id));
	lua_setfield(_lua, -2, "from");
	PushString(_lua, item->originalText().text);
	lua_setfield(_lua, -2, "text");
	lua_pushboolean(_lua, item->out() ? 1 : 0);
	lua_setfield(_lua, -2, "out");
	lua_pushinteger(_lua, lua_Integer(item->date()));
	lua_setfield(_lua, -2, "date");
}

void Plugin::messageNew(not_null<HistoryItem*> item) {
	const auto handlers = _handlers[size_t(Event::MessageNew)];
	for (const auto ref : handlers) {
		if (!_lua) {
			return;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		pushMessage(item);
		call(1, 0, kEventBudget);
	}
}

SendVerdict Plugin::messageSend(
		not_null<PeerData*> peer,
		const QString &text) {
	auto result = SendVerdict();
	auto current = text;
	const auto handlers = _handlers[size_t(Event::MessageSend)];
	for (const auto ref : handlers) {
		if (!_lua) {
			break;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		lua_createtable(_lua, 0, 2);
		lua_pushinteger(_lua, BotApiId(peer->id));
		lua_setfield(_lua, -2, "peer");
		PushString(_lua, current);
		lua_setfield(_lua, -2, "text");
		if (!call(1, 1, kFilterBudget)) {
			continue;
		}
		if (lua_isboolean(_lua, -1) && !lua_toboolean(_lua, -1)) {
			lua_pop(_lua, 1);
			result.cancel = true;
			return result;
		} else if (lua_type(_lua, -1) == LUA_TSTRING) {
			current = ToString(_lua, -1);
			result.text = current;
		}
		lua_pop(_lua, 1);
	}
	return result;
}

void Plugin::appState(const QString &state) {
	const auto handlers = _handlers[size_t(Event::AppState)];
	for (const auto ref : handlers) {
		if (!_lua) {
			return;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		PushString(_lua, state);
		call(1, 0, kEventBudget);
	}
}

void Plugin::menuClicked(int ref, PeerData *peer, HistoryItem *item) {
	if (!_lua) {
		return;
	}
	lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
	lua_createtable(_lua, 0, 2);
	if (peer) {
		lua_pushinteger(_lua, BotApiId(peer->id));
		lua_setfield(_lua, -2, "peer");
	}
	if (item) {
		pushMessage(item);
		lua_setfield(_lua, -2, "message");
	}
	call(1, 0, kClickBudget);
}

void Plugin::pushContext(PeerData *peer, HistoryItem *item) {
	lua_createtable(_lua, 0, 3);
	if (!peer && item) {
		peer = item->history()->peer;
	}
	if (peer) {
		lua_pushinteger(_lua, BotApiId(peer->id));
		lua_setfield(_lua, -2, "peer");
	}
	if (item) {
		pushMessage(item);
		lua_setfield(_lua, -2, "message");
	}
}

bool Plugin::urlOpen(const QString &url, bool local) {
	const auto handlers = _handlers[size_t(Event::UrlOpen)];
	for (const auto ref : handlers) {
		if (!_lua) {
			break;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		lua_createtable(_lua, 0, 2);
		PushString(_lua, url);
		lua_setfield(_lua, -2, "url");
		lua_pushboolean(_lua, local ? 1 : 0);
		lua_setfield(_lua, -2, "local");
		if (!call(1, 1, kFilterBudget)) {
			continue;
		}
		const auto handled = lua_toboolean(_lua, -1) != 0;
		lua_pop(_lua, 1);
		if (handled) {
			return true;
		}
	}
	return false;
}

bool Plugin::action(const ClientEventArgs &args) {
	const auto handlers = _handlers[size_t(Event::Action)];
	for (const auto ref : handlers) {
		if (!_lua) {
			break;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		pushContext(args.peer, args.item);
		lua_pushstring(_lua, ActionName(args.event));
		lua_setfield(_lua, -2, "type");
		switch (args.event) {
		case ClientEvent::Reaction:
			PushString(_lua, args.emoji);
			lua_setfield(_lua, -2, "emoji");
			lua_pushinteger(_lua, lua_Integer(args.customEmojiId));
			lua_setfield(_lua, -2, "custom_emoji_id");
			lua_pushboolean(_lua, args.remove ? 1 : 0);
			lua_setfield(_lua, -2, "remove");
			break;
		case ClientEvent::Call:
			lua_pushboolean(_lua, args.video ? 1 : 0);
			lua_setfield(_lua, -2, "video");
			break;
		case ClientEvent::Notification:
			lua_pushboolean(_lua, args.reaction ? 1 : 0);
			lua_setfield(_lua, -2, "reaction");
			break;
		case ClientEvent::OpenFile:
			PushString(_lua, args.path);
			lua_setfield(_lua, -2, "path");
			break;
		default:
			break;
		}
		if (!call(1, 1, kFilterBudget)) {
			continue;
		}
		const auto cancel = lua_isboolean(_lua, -1)
			&& !lua_toboolean(_lua, -1);
		lua_pop(_lua, 1);
		if (cancel) {
			return true;
		}
	}
	return false;
}

std::optional<QString> Plugin::display(
		DisplayTextPlace place,
		PeerData *peer,
		HistoryItem *item,
		const QString &text) {
	auto result = std::optional<QString>();
	auto current = text;
	const auto handlers = _handlers[size_t(Event::Display)];
	for (const auto ref : handlers) {
		if (!_lua) {
			break;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		pushContext(peer, item);
		lua_pushstring(_lua, PlaceName(place));
		lua_setfield(_lua, -2, "place");
		PushString(_lua, current);
		lua_setfield(_lua, -2, "text");
		if (!call(1, 1, kFilterBudget)) {
			continue;
		}
		if (lua_type(_lua, -1) == LUA_TSTRING) {
			current = ToString(_lua, -1);
			result = current;
		}
		lua_pop(_lua, 1);
	}
	return result;
}

void Plugin::screen(Plugins::Screen screen, PeerData *peer) {
	const auto handlers = _handlers[size_t(Event::Screen)];
	for (const auto ref : handlers) {
		if (!_lua) {
			break;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		pushContext(peer, nullptr);
		lua_pushstring(_lua, ScreenName(screen));
		lua_setfield(_lua, -2, "screen");
		call(1, 0, kEventBudget);
	}
}

void Plugin::profileRows(
		not_null<PeerData*> peer,
		std::vector<ProfileRow> &rows) {
	const auto handlers = _handlers[size_t(Event::ProfileRows)];
	for (const auto ref : handlers) {
		if (!_lua) {
			break;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		lua_pushinteger(_lua, BotApiId(peer->id));
		if (!call(1, 1, kFilterBudget)) {
			continue;
		}
		// { { label = "...", value = "..." }, ... } or { { "label", "value" } }
		if (lua_istable(_lua, -1)) {
			const auto count = lua_rawlen(_lua, -1);
			for (auto i = lua_Integer(1); i <= lua_Integer(count); ++i) {
				lua_rawgeti(_lua, -1, i);
				if (lua_istable(_lua, -1)) {
					lua_getfield(_lua, -1, "label");
					if (lua_isnil(_lua, -1)) {
						lua_pop(_lua, 1);
						lua_rawgeti(_lua, -1, 1);
					}
					lua_getfield(_lua, -2, "value");
					if (lua_isnil(_lua, -1)) {
						lua_pop(_lua, 1);
						lua_rawgeti(_lua, -2, 2);
					}
					rows.push_back({
						.label = ToString(_lua, -2),
						.value = ToString(_lua, -1),
					});
					lua_pop(_lua, 2);
				}
				lua_pop(_lua, 1);
			}
		}
		lua_pop(_lua, 1);
	}
}

void Plugin::loadSettings() {
	auto file = QFile(_dataPath + u"settings.json"_q);
	if (file.open(QIODevice::ReadOnly)) {
		_settings = QJsonDocument::fromJson(file.readAll()).object();
	}
}

void Plugin::saveSettings() {
	QDir().mkpath(_dataPath);
	auto file = QFile(_dataPath + u"settings.json"_q);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(_settings).toJson(QJsonDocument::Indented));
	}
}

// Lua API.

int Plugin::ApiPrint(lua_State *L) {
	const auto plugin = From(L);
	auto parts = QStringList();
	const auto count = lua_gettop(L);
	for (auto i = 1; i <= count; ++i) {
		auto size = size_t(0);
		const auto data = luaL_tolstring(L, i, &size); // pushes a string
		parts.push_back(QString::fromUtf8(data, int(size)));
		lua_pop(L, 1);
	}
	LOG(("Plugins [%1]: %2").arg(plugin->manifest().id, parts.join(u' ')));
	return 0;
}

int Plugin::ApiRequire(lua_State *L) {
	const auto plugin = From(L);
	const auto name = CheckString(L, 1);
	static const auto check = QRegularExpression(
		u"^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)*$"_q);
	if (!check.match(name).hasMatch()) {
		return luaL_error(L, "bad module name: %s", name.toUtf8().constData());
	}
	lua_getfield(L, LUA_REGISTRYINDEX, kLoadedKey);
	lua_getfield(L, -1, name.toUtf8().constData());
	if (!lua_isnil(L, -1)) {
		return 1;
	}
	lua_pop(L, 1);

	const auto path = QString(QString(name).replace(u'.', u'/') + u".lua"_q);
	auto found = QString();
	for (const auto &prefix : { u"source/"_q, u"lib/"_q, QString() }) {
		if (plugin->_package.files.contains(prefix + path)) {
			found = prefix + path;
			break;
		}
	}
	if (found.isEmpty()) {
		return luaL_error(L, "module not found: %s", name.toUtf8().constData());
	}
	const auto &bytes = plugin->_package.files.at(found);
	const auto chunk = QString(u"@"_q + found).toUtf8();
	if (luaL_loadbufferx(
			L,
			bytes.constData(),
			size_t(bytes.size()),
			chunk.constData(),
			"t") != LUA_OK) {
		return lua_error(L);
	}
	lua_call(L, 0, 1);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_pushboolean(L, 1);
	}
	lua_pushvalue(L, -1);
	lua_setfield(L, -3, name.toUtf8().constData());
	return 1;
}

int Plugin::ApiOn(lua_State *L) {
	const auto plugin = From(L);
	const auto name = CheckString(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	auto event = Event::Count;
	for (auto i = 0; i != int(Event::Count); ++i) {
		if (name == QString::fromLatin1(EventName(Event(i)))) {
			event = Event(i);
			break;
		}
	}
	switch (event) {
	case Event::MessageNew:
		plugin->requirePermission(kPermMessagesRead);
		break;
	case Event::MessageSend:
		plugin->requirePermission(kPermMessagesSend);
		break;
	case Event::AppState:
		break;
	case Event::UrlOpen:
		plugin->requirePermission(kPermLinks);
		break;
	case Event::Action:
		plugin->requirePermission(kPermActions);
		break;
	case Event::Display:
		plugin->requirePermission(kPermDisplay);
		break;
	case Event::Screen:
		plugin->requirePermission(kPermChatsRead);
		break;
	case Event::ProfileRows:
		plugin->requirePermission(kPermUi);
		break;
	case Event::Count:
		return luaL_error(L, "unknown event: %s", name.toUtf8().constData());
	}
	lua_pushvalue(L, 2);
	plugin->_handlers[size_t(event)].push_back(
		luaL_ref(L, LUA_REGISTRYINDEX));
	// Hooks are enabled when the call that registered this returns.
	crl::on_main(plugin, [host = plugin->_host] {
		host->refreshHooks();
	});
	return 0;
}

int Plugin::ApiMenu(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermUi);
	const auto place = CheckString(L, 1);
	const auto text = CheckString(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	auto entry = MenuEntry{ .text = text };
	if (place == u"message"_q) {
		entry.place = MenuPlace::Message;
	} else if (place == u"main"_q) {
		entry.place = MenuPlace::Main;
	} else if (place == u"chat"_q) {
		entry.place = MenuPlace::Chat;
	} else if (place == u"profile"_q) {
		entry.place = MenuPlace::Profile;
	} else {
		return luaL_error(L, "unknown menu: %s", place.toUtf8().constData());
	}
	if (text.isEmpty()) {
		return luaL_error(L, "menu item text is empty");
	}
	lua_pushvalue(L, 3);
	entry.ref = luaL_ref(L, LUA_REGISTRYINDEX);
	plugin->_menu.push_back(std::move(entry));
	crl::on_main(plugin, [host = plugin->_host] {
		host->refreshHooks();
	});
	return 0;
}

int Plugin::ApiSettingsGet(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermStorage);
	const auto key = CheckString(L, 1);
	const auto value = plugin->_settings.value(key);
	if (value.isBool()) {
		lua_pushboolean(L, value.toBool() ? 1 : 0);
	} else if (value.isDouble()) {
		// JSON keeps only doubles, give integers back as integers.
		const auto number = value.toDouble();
		if (std::floor(number) == number
			&& std::abs(number) < 9007199254740992.) {
			lua_pushinteger(L, lua_Integer(number));
		} else {
			lua_pushnumber(L, number);
		}
	} else if (value.isString()) {
		PushString(L, value.toString());
	} else {
		lua_pushvalue(L, 2); // the default, nil if not given
	}
	return 1;
}

int Plugin::ApiSettingsSet(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermStorage);
	const auto key = CheckString(L, 1);
	switch (lua_type(L, 2)) {
	case LUA_TNIL:
		plugin->_settings.remove(key);
		break;
	case LUA_TBOOLEAN:
		plugin->_settings.insert(key, lua_toboolean(L, 2) != 0);
		break;
	case LUA_TNUMBER:
		plugin->_settings.insert(key, lua_tonumber(L, 2));
		break;
	case LUA_TSTRING:
		plugin->_settings.insert(key, ToString(L, 2));
		break;
	default:
		return luaL_error(L, "settings store nil, boolean, number or string");
	}
	plugin->saveSettings();
	return 0;
}

int Plugin::ApiToast(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermUi);
	const auto text = CheckString(L, 1);
	crl::on_main(plugin, [=] {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto controller = window->sessionController()) {
				controller->showToast(text);
			}
		}
	});
	return 0;
}

int Plugin::ApiSendMessage(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermMessagesSend);
	const auto peerId = FromBotApiId(int64(luaL_checkinteger(L, 1)));
	const auto text = CheckString(L, 2);
	const auto session = ActiveSession();
	if (!peerId || !session || text.isEmpty()) {
		lua_pushboolean(L, 0);
		return 1;
	}
	// Sent on the next event loop turn: never call into the client from
	// the middle of a hook, and keep the plugin's own message out of its
	// send filter.
	crl::on_main(session, [=] {
		const auto history = session->data().history(peerId);
		auto message = Api::MessageToSend(Api::SendAction(history));
		message.textWithTags = TextWithTags{ text };
		const auto bypass = Plugins::BypassScope();
		session->api().sendMessage(std::move(message));
	});
	lua_pushboolean(L, 1);
	return 1;
}

int Plugin::ApiComposeGet(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermCompose);
	if (const auto text = Plugins::ComposeText()) {
		PushString(L, text->text);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int Plugin::ApiComposeSet(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermCompose);
	const auto text = CheckString(L, 1);
	if (!Plugins::ComposeText()) {
		lua_pushboolean(L, 0);
		return 1;
	}
	// Not in the middle of a hook: the field may be laying out right now.
	crl::on_main(plugin, [=] {
		Plugins::SetComposeText(TextWithTags{ text });
	});
	lua_pushboolean(L, 1);
	return 1;
}

int Plugin::ApiRefreshDisplay(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(kPermDisplay);
	crl::on_main(plugin, [] {
		Plugins::RefreshDisplayText();
	});
	return 0;
}

// Async: coroutines, timers, HTTP.

void Plugin::resume(lua_State *thread, lua_State *from, int nargs) {
	auto results = 0;
	const auto status = lua_resume(thread, from, nargs, &results);
	if (status == LUA_YIELD) {
		lua_pop(thread, results);
		return; // Waits for a reply, resumed by finishRequest().
	} else if (status != LUA_OK) {
		const auto message = lua_tostring(thread, -1);
		luaL_traceback(
			_lua,
			thread,
			message ? message : "(error object)",
			0);
		const auto error = ToString(_lua, -1);
		lua_pop(_lua, 1);
		fail(error);
	}
	if (const auto i = _threads.find(thread); i != end(_threads)) {
		luaL_unref(_lua, LUA_REGISTRYINDEX, i->second);
		_threads.erase(i);
	}
}

int Plugin::ApiAsync(lua_State *L) {
	const auto plugin = From(L);
	luaL_checktype(L, 1, LUA_TFUNCTION);
	const auto nargs = lua_gettop(L) - 1;
	const auto thread = lua_newthread(L); // L: fn, args..., thread
	lua_insert(L, 1);                     // L: thread, fn, args...
	lua_xmove(L, thread, nargs + 1);      // thread: fn, args...
	plugin->_threads.emplace(thread, luaL_ref(L, LUA_REGISTRYINDEX));
	plugin->resume(thread, L, nargs);
	return 0;
}

int Plugin::AddTimer(lua_State *L, bool repeat) {
	const auto plugin = From(L);
	const auto delay = luaL_checkinteger(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (delay < 0 || delay > kMaxDelay) {
		return luaL_error(L, "timer delay must be 0..86400000 ms");
	} else if (repeat && delay < kMinRepeat) {
		return luaL_error(L, "repeating timers need at least 100 ms");
	} else if (plugin->_timers.size() >= kMaxTimers) {
		return luaL_error(L, "too many timers");
	}
	lua_pushvalue(L, 2);
	const auto id = ++plugin->_nextTimerId;
	plugin->_timers.emplace(id, Timer{
		.ref = luaL_ref(L, LUA_REGISTRYINDEX),
		.interval = crl::time(delay),
		.repeat = repeat,
	});
	plugin->scheduleTimer(id);
	lua_pushinteger(L, id);
	return 1;
}

int Plugin::ApiTimerAfter(lua_State *L) {
	return AddTimer(L, false);
}

int Plugin::ApiTimerEvery(lua_State *L) {
	return AddTimer(L, true);
}

int Plugin::ApiTimerCancel(lua_State *L) {
	const auto plugin = From(L);
	const auto id = int(luaL_checkinteger(L, 1));
	if (const auto i = plugin->_timers.find(id); i != end(plugin->_timers)) {
		luaL_unref(L, LUA_REGISTRYINDEX, i->second.ref);
		plugin->_timers.erase(i);
	}
	return 0;
}

void Plugin::scheduleTimer(int id) {
	const auto i = _timers.find(id);
	if (i == end(_timers)) {
		return;
	}
	const auto generation = _generation;
	base::call_delayed(i->second.interval, this, [=] {
		fireTimer(id, generation);
	});
}

void Plugin::fireTimer(int id, int generation) {
	if (generation != _generation || !_lua) {
		return;
	}
	const auto i = _timers.find(id);
	if (i == end(_timers)) {
		return;
	}
	const auto timer = i->second;
	if (!timer.repeat) {
		_timers.erase(i);
	}
	lua_rawgeti(_lua, LUA_REGISTRYINDEX, timer.ref);
	call(0, 0, kEventBudget);
	if (!_lua || generation != _generation) {
		return;
	} else if (!timer.repeat) {
		luaL_unref(_lua, LUA_REGISTRYINDEX, timer.ref);
	} else if (_timers.contains(id)) {
		scheduleTimer(id);
	}
}

int Plugin::ApiHttpRequest(lua_State *L) {
	const auto plugin = From(L);
	luaL_checktype(L, 1, LUA_TTABLE);
	const auto callback = lua_isfunction(L, 2);
	if (!callback && !plugin->_threads.contains(L)) {
		return luaL_error(
			L,
			"tg.http.request needs a callback or a tg.async(...) coroutine");
	}
	std::erase_if(plugin->_requests, [](const QPointer<QNetworkReply> &r) {
		return !r;
	});
	if (int(plugin->_requests.size()) >= kMaxRequests) {
		return luaL_error(L, "too many requests at once");
	}

	const auto field = [&](const char *name) {
		lua_getfield(L, 1, name);
		auto result = lua_isstring(L, -1) ? ToBytes(L, -1) : QByteArray();
		lua_pop(L, 1);
		return result;
	};
	const auto url = QUrl(QString::fromUtf8(field("url")));
	auto method = field("method").toUpper();
	if (method.isEmpty()) {
		method = "GET";
	}
	const auto body = field("body");
	if (!url.isValid() || url.scheme() != u"https"_q) {
		return luaL_error(L, "only https:// URLs are allowed");
	} else if (!plugin->manifest().hostAllowed(url.host())) {
		return luaL_error(
			L,
			"host %s is not declared (add \"net:%s\" to permissions)",
			url.host().toUtf8().constData(),
			url.host().toUtf8().constData());
	} else if (body.size() > kMaxRequestBody) {
		return luaL_error(L, "request body is too large");
	}
	static const auto methodCheck = QRegularExpression(u"^[A-Z]{1,16}$"_q);
	if (!methodCheck.match(QString::fromLatin1(method)).hasMatch()) {
		return luaL_error(L, "bad HTTP method");
	}

	auto request = QNetworkRequest(url);
	// No automatic redirects: they could lead to an undeclared host.
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(kRequestTimeout);
	lua_getfield(L, 1, "headers");
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
				request.setRawHeader(ToBytes(L, -2), ToBytes(L, -1));
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);

	if (!plugin->_network) {
		plugin->_network = std::make_unique<QNetworkAccessManager>();
	}
	const auto reply = plugin->_network->sendCustomRequest(
		request,
		method,
		body);
	plugin->_requests.push_back(reply);

	auto callbackRef = LUA_NOREF;
	if (callback) {
		lua_pushvalue(L, 2);
		callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	const auto thread = callback ? nullptr : L;
	const auto generation = plugin->_generation;
	const auto weak = base::make_weak(plugin);
	QObject::connect(
		reply,
		&QNetworkReply::downloadProgress,
		reply,
		[=](qint64 received, qint64) {
			if (received > kMaxResponseBody) {
				reply->abort();
			}
		});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		if (const auto strong = weak.get()) {
			strong->finishRequest(reply, generation, callbackRef, thread);
		}
	});
	if (callback) {
		return 0;
	}
	return lua_yield(L, 0); // resumed with the response by finishRequest()
}

void Plugin::finishRequest(
		QPointer<QNetworkReply> reply,
		int generation,
		int callbackRef,
		lua_State *thread) {
	if (!reply || generation != _generation || !_lua) {
		return;
	}
	std::erase_if(_requests, [&](const QPointer<QNetworkReply> &r) {
		return !r || (r == reply);
	});
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto body = reply->readAll().left(int(kMaxResponseBody));
	const auto error = (reply->error() != QNetworkReply::NoError)
		? reply->errorString()
		: QString();
	const auto headers = reply->rawHeaderPairs();

	// { status, body, headers = { name = value }, error }
	const auto push = [&](lua_State *to) {
		lua_createtable(to, 0, 4);
		lua_pushinteger(to, status);
		lua_setfield(to, -2, "status");
		lua_pushlstring(to, body.constData(), size_t(body.size()));
		lua_setfield(to, -2, "body");
		lua_createtable(to, 0, int(headers.size()));
		for (const auto &header : headers) {
			lua_pushlstring(
				to,
				header.second.constData(),
				size_t(header.second.size()));
			lua_setfield(to, -2, header.first.toLower().constData());
		}
		lua_setfield(to, -2, "headers");
		if (!error.isEmpty()) {
			PushString(to, error);
			lua_setfield(to, -2, "error");
		}
	};

	if (callbackRef != LUA_NOREF) {
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, callbackRef);
		luaL_unref(_lua, LUA_REGISTRYINDEX, callbackRef);
		push(_lua);
		call(1, 0, kEventBudget);
	} else if (thread && _threads.contains(thread)) {
		_deadline = crl::now() + kEventBudget;
		lua_sethook(thread, &Plugin::CountHook, LUA_MASKCOUNT, kHookInstructions);
		push(thread);
		resume(thread, nullptr, 1);
	}
}

int Plugin::ApiJsonEncode(lua_State *L) {
	luaL_checkany(L, 1);
	// QJsonDocument holds only arrays and objects: wrap and unwrap.
	auto wrapper = QJsonArray();
	wrapper.push_back(ToJson(L, 1, 0));
	const auto json = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
	const auto inner = json.mid(1, json.size() - 2);
	lua_pushlstring(L, inner.constData(), size_t(inner.size()));
	return 1;
}

int Plugin::ApiJsonDecode(lua_State *L) {
	auto size = size_t(0);
	const auto data = luaL_checklstring(L, 1, &size);
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(
		QByteArray(data, int(size)),
		&error);
	if (error.error != QJsonParseError::NoError) {
		lua_pushnil(L);
		PushString(L, error.errorString());
		return 2;
	}
	PushJson(
		L,
		document.isArray()
			? QJsonValue(document.array())
			: QJsonValue(document.object()),
		0);
	return 1;
}

} // namespace Plugins::Flua
