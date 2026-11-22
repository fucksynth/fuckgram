/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_plugin.h"

#include "api/api_common.h"
#include "api/api_editing.h"
#include "apiwrap.h"
#include "core/application.h"
#include "base/unixtime.h"
#include "data/data_drafts.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "plugins/plugins_hooks.h"
#include "plugins/runtime/flua_runtime.h"
#include "plugins/runtime/flua_tl.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include "base/call_delayed.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
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
constexpr auto kMaxInvokes = 32;
constexpr auto kMaxInput = 4096;
constexpr auto kMaxDelete = 100;
constexpr auto kMaxFile = qint64(16 * 1024 * 1024);
constexpr auto kMaxFiles = qint64(64 * 1024 * 1024); // all files of a plugin

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

// A string field of the table at `index`, empty if absent.
[[nodiscard]] QString StringField(lua_State *L, int index, const char *name) {
	lua_getfield(L, index, name);
	auto result = (lua_type(L, -1) == LUA_TSTRING)
		? ToString(L, -1)
		: QString();
	lua_pop(L, 1);
	return result;
}

[[nodiscard]] Window::SessionController *ActiveController() {
	const auto window = Core::App().activeWindow();
	return window ? window->sessionController() : nullptr;
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
	case Event::MtprotoRequest: return "mtproto.request";
	case Event::MtprotoResponse: return "mtproto.response";
	case Event::MtprotoUpdates: return "mtproto.updates";
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

// { code, type, description } of a failed tg.invoke.
void PushError(
		lua_State *L,
		int code,
		const QString &type,
		const QString &description) {
	lua_createtable(L, 0, 3);
	lua_pushinteger(L, code);
	lua_setfield(L, -2, "code");
	PushString(L, type);
	lua_setfield(L, -2, "type");
	PushString(L, description);
	lua_setfield(L, -2, "description");
}

struct DecodeJob {
	Tl::Type type;
	const mtpBuffer *data = nullptr;
};

// setmetatable without __gc: finalizers run with hooks off, so a loop in
// one could never be stopped. Lua marks an object for finalization only
// in setmetatable, checking here is enough.
int SafeSetMetatable(lua_State *L) {
	if (lua_istable(L, 2)) {
		lua_pushliteral(L, "__gc");
		if (lua_rawget(L, 2) != LUA_TNIL) {
			return luaL_error(L, "__gc metamethods are not allowed");
		}
		lua_pop(L, 1);
	}
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	return 1;
}

int RunDecode(lua_State *L) {
	const auto job = static_cast<const DecodeJob*>(lua_touserdata(L, 1));
	Tl::Push(L, job->type, *job->data);
	return 1;
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
	} else if (plugin->_running > 0
		&& nsize > old
		&& plugin->_memory + (nsize - old) > kMemoryLimit) {
		// Lua raises "not enough memory" inside the protected call. Outside
		// of one (the client building event arguments) an error would end
		// in abort(), so there the limit waits for the next call.
		return nullptr;
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
	_invokes = 0;
	lua_close(_lua);
	_lua = nullptr;
	for (auto &list : _handlers) {
		list.clear();
	}
	_methodFilters.clear();
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
	lua_getglobal(_lua, "setmetatable");
	lua_pushcclosure(_lua, &SafeSetMetatable, 1);
	lua_setglobal(_lua, "setmetatable");

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
	set("resource", &Plugin::ApiResource);
	lua_newtable(_lua);
	set("read", &Plugin::ApiFileRead);
	set("write", &Plugin::ApiFileWrite);
	set("remove", &Plugin::ApiFileRemove);
	set("list", &Plugin::ApiFileList);
	lua_setfield(_lua, -2, "files");
	lua_setglobal(_lua, "plugin");

	// tg = { log, toast, messages = { send }, compose = { get, set },
	//       ui = { refresh } }
	lua_newtable(_lua);
	set("log", &Plugin::ApiPrint);
	set("toast", &Plugin::ApiToast);
	lua_newtable(_lua);
	set("send", &Plugin::ApiSendMessage);
	set("edit", &Plugin::ApiEditMessage);
	set("delete", &Plugin::ApiDeleteMessages);
	set("get", &Plugin::ApiGetMessage);
	lua_setfield(_lua, -2, "messages");
	lua_newtable(_lua);
	set("get", &Plugin::ApiPeer);
	lua_setfield(_lua, -2, "peers");
	lua_newtable(_lua);
	set("current", &Plugin::ApiCurrentChat);
	lua_setfield(_lua, -2, "chats");
	set("me", &Plugin::ApiMe);
	lua_newtable(_lua);
	set("get", &Plugin::ApiComposeGet);
	set("set", &Plugin::ApiComposeSet);
	lua_setfield(_lua, -2, "compose");
	lua_newtable(_lua);
	set("refresh", &Plugin::ApiRefreshDisplay);
	set("alert", &Plugin::ApiAlert);
	set("confirm", &Plugin::ApiConfirm);
	set("input", &Plugin::ApiInput);
	set("copy", &Plugin::ApiCopy);
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
	set("invoke", &Plugin::ApiInvoke);
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

void Plugin::requirePermission(lua_State *L, const char *permission) {
	// Raised on the calling state: it may be a tg.async coroutine.
	if (!manifest().has(permission)) {
		luaL_error(
			L,
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
	++_running;
	const auto status = lua_pcall(_lua, nargs, nresults, base);
	--_running;
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
	// Raw: a metatable on _G must not run outside a protected call.
	lua_pushglobaltable(_lua);
	lua_pushstring(_lua, name);
	const auto type = lua_rawget(_lua, -2);
	lua_remove(_lua, -2);
	if (type != LUA_TFUNCTION) {
		lua_pop(_lua, 1);
		return true;
	}
	return call(0, 0, budget);
}

int Plugin::RunProtected(lua_State *L) {
	const auto body = static_cast<Fn<void(lua_State*)>*>(
		lua_touserdata(L, 1));
	lua_remove(L, 1);
	(*body)(L);
	return lua_gettop(L);
}

bool Plugin::protect(
		int nargs,
		int nresults,
		Fn<void(lua_State*)> body,
		QString *error) {
	const auto base = lua_gettop(_lua) - nargs + 1;
	lua_pushcfunction(_lua, &Plugin::RunProtected);
	lua_insert(_lua, base);
	lua_pushlightuserdata(_lua, &body);
	lua_insert(_lua, base + 1);
	++_running;
	const auto status = lua_pcall(_lua, nargs + 1, nresults, 0);
	--_running;
	if (status == LUA_OK) {
		return true;
	}
	const auto message = ToString(_lua, -1);
	lua_pop(_lua, 1);
	if (error) {
		*error = message;
	} else {
		fail(message);
	}
	return false;
}

void Plugin::fail(const QString &error) {
	_lastError = error;
	LOG(("Plugins Error: %1: %2").arg(manifest().id, error));
	if (++_errors >= kMaxErrors) {
		LOG(("Plugins: %1 stopped after %2 errors"
			).arg(manifest().id
			).arg(_errors));
		// Not right now: we may be inside a call into this state.
		crl::on_main(this, [=, generation = _generation] {
			if (_generation == generation) {
				stop();
			}
		});
	}
}

bool Plugin::handles(Event event) const {
	return _lua && !_handlers[size_t(event)].empty();
}

const std::vector<MenuEntry> &Plugin::menu() const {
	return _menu;
}

void Plugin::pushMessage(lua_State *L, not_null<HistoryItem*> item) {
	lua_createtable(L, 0, 6);
	lua_pushinteger(L, lua_Integer(item->id.bare));
	lua_setfield(L, -2, "id");
	lua_pushinteger(L, BotApiId(item->history()->peer->id));
	lua_setfield(L, -2, "peer");
	lua_pushinteger(L, BotApiId(item->from()->id));
	lua_setfield(L, -2, "from");
	PushString(L, item->originalText().text);
	lua_setfield(L, -2, "text");
	lua_pushboolean(L, item->out() ? 1 : 0);
	lua_setfield(L, -2, "out");
	lua_pushinteger(L, lua_Integer(item->date()));
	lua_setfield(L, -2, "date");
}

void Plugin::pushPeer(lua_State *L, not_null<PeerData*> peer) {
	lua_createtable(L, 0, 8);
	lua_pushinteger(L, BotApiId(peer->id));
	lua_setfield(L, -2, "id");
	const auto user = peer->asUser();
	lua_pushstring(L, user
		? (user->isBot() ? "bot" : "user")
		: peer->isChat()
		? "group"
		: peer->isMegagroup()
		? "supergroup"
		: "channel");
	lua_setfield(L, -2, "type");
	PushString(L, peer->name());
	lua_setfield(L, -2, "name");
	if (const auto username = peer->username(); !username.isEmpty()) {
		PushString(L, username);
		lua_setfield(L, -2, "username");
	}
	lua_pushboolean(L, peer->isSelf() ? 1 : 0);
	lua_setfield(L, -2, "is_self");
	lua_pushboolean(L, peer->isPremium() ? 1 : 0);
	lua_setfield(L, -2, "premium");
	lua_pushboolean(L, peer->isVerified() ? 1 : 0);
	lua_setfield(L, -2, "verified");
}

void Plugin::messageNew(not_null<HistoryItem*> item) {
	const auto handlers = _handlers[size_t(Event::MessageNew)];
	for (const auto ref : handlers) {
		if (!_lua) {
			return;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		pushMessage(_lua, item);
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
		pushMessage(_lua, item);
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
		pushMessage(_lua, item);
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
					// Raw: no metamethods outside a protected call.
					lua_pushliteral(_lua, "label");
					if (lua_rawget(_lua, -2) == LUA_TNIL) {
						lua_pop(_lua, 1);
						lua_rawgeti(_lua, -1, 1);
					}
					lua_pushliteral(_lua, "value");
					if (lua_rawget(_lua, -3) == LUA_TNIL) {
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

bool Plugin::accepts(int ref, mtpTypeId method) const {
	const auto i = _methodFilters.find(ref);
	return (i == end(_methodFilters)) || i->second.contains(method);
}

bool Plugin::wants(Event event, mtpTypeId method) const {
	if (!_lua) {
		return false;
	}
	for (const auto ref : _handlers[size_t(event)]) {
		if (accepts(ref, method)) {
			return true;
		}
	}
	return false;
}

RawResult Plugin::mtprotoRequest(const mtpBuffer &body, mtpTypeId method) {
	const auto handlers = _handlers[size_t(Event::MtprotoRequest)];
	auto decoded = false;
	auto changed = false;
	for (const auto ref : handlers) {
		if (!_lua) {
			return {};
		} else if (!accepts(ref, method)) {
			continue;
		}
		if (!decoded) {
			// Decoded once, the table stays on top between the handlers.
			auto error = QString();
			const auto push = [&](lua_State *L) {
				Tl::Push(L, Tl::Query(), body);
			};
			if (!protect(0, 1, push, &error)) {
				DEBUG_LOG(("Plugins: could not decode a request: %1"
					).arg(error));
				return {};
			}
			decoded = true;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		lua_pushvalue(_lua, -2);
		if (!call(1, 1, kFilterBudget)) {
			continue;
		}
		if (lua_isboolean(_lua, -1) && !lua_toboolean(_lua, -1)) {
			lua_pop(_lua, 2); // false, the call
			return { .verdict = Verdict::Cancel };
		} else if (lua_istable(_lua, -1)) {
			lua_replace(_lua, -2); // the returned table is the call now
			changed = true;
		} else {
			lua_pop(_lua, 1);
		}
	}
	if (!decoded) {
		return {};
	} else if (!changed) {
		lua_pop(_lua, 1);
		return {};
	}
	auto result = RawResult{ .verdict = Verdict::Modify };
	const auto read = [&](lua_State *L) {
		result.body = Tl::Read(L, 1, Tl::Query());
	};
	if (!protect(1, 0, read)) {
		return {}; // a bad table is the plugin's error, the call goes as is
	}
	return result;
}

std::optional<mtpBuffer> Plugin::mtprotoResponse(
		const mtpBuffer &reply,
		mtpTypeId method) {
	const auto error = !reply.isEmpty()
		&& (mtpTypeId(reply[0]) == Tl::kRpcError);
	const auto type = error ? Tl::Object() : Tl::ResultOf(method);
	const auto name = Tl::Name(method);
	const auto handlers = _handlers[size_t(Event::MtprotoResponse)];
	auto decoded = false;
	for (const auto ref : handlers) {
		if (!_lua) {
			return std::nullopt;
		} else if (!accepts(ref, method)) {
			continue;
		}
		if (!decoded) {
			auto message = QString();
			const auto push = [&](lua_State *L) {
				Tl::Push(L, type, reply);
			};
			if (!protect(0, 1, push, &message)) {
				DEBUG_LOG(("Plugins: could not decode a response: %1"
					).arg(message));
				return std::nullopt;
			}
			decoded = true;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		lua_pushvalue(_lua, -2);
		if (name.isEmpty()) {
			lua_pushnil(_lua);
		} else {
			lua_pushlstring(_lua, name.constData(), size_t(name.size()));
		}
		if (!call(2, 1, kFilterBudget)) {
			continue;
		} else if (lua_isnil(_lua, -1)) {
			lua_pop(_lua, 1);
			continue;
		}
		lua_remove(_lua, -2); // the decoded value, keep the replacement
		auto result = mtpBuffer();
		const auto read = [&](lua_State *L) {
			// An error can be answered with a result and back.
			auto as = type;
			if (lua_istable(L, 1)) {
				lua_pushliteral(L, "_");
				lua_rawget(L, 1);
				if (lua_type(L, -1) == LUA_TSTRING
					&& ToBytes(L, -1) == "rpc_error") {
					as = Tl::Object();
				} else if (error) {
					as = Tl::ResultOf(method);
				}
				lua_pop(L, 1);
			}
			result = Tl::Read(L, 1, as);
		};
		if (!protect(1, 0, read)) {
			return std::nullopt;
		}
		return result;
	}
	if (decoded) {
		lua_pop(_lua, 1);
	}
	return std::nullopt;
}

RawResult Plugin::mtprotoUpdates(const mtpBuffer &updates) {
	// The session passes new_session_created here as well: it is a service
	// message of the transport, not an Updates of api.tl.
	if (updates.empty()
		|| mtpTypeId(updates[0]) == mtpc_new_session_created) {
		return {};
	}
	const auto type = Tl::Boxed("Updates");
	if (!type.valid()) {
		return {};
	}
	const auto handlers = _handlers[size_t(Event::MtprotoUpdates)];
	auto decoded = false;
	auto changed = false;
	for (const auto ref : handlers) {
		if (!_lua) {
			return {};
		}
		if (!decoded) {
			auto error = QString();
			const auto push = [&](lua_State *L) {
				Tl::Push(L, type, updates);
			};
			if (!protect(0, 1, push, &error)) {
				DEBUG_LOG(("Plugins: could not decode updates: %1"
					).arg(error));
				return {};
			}
			decoded = true;
		}
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
		lua_pushvalue(_lua, -2);
		if (!call(1, 1, kFilterBudget)) {
			continue;
		}
		if (lua_isboolean(_lua, -1) && !lua_toboolean(_lua, -1)) {
			lua_pop(_lua, 2);
			return { .verdict = Verdict::Cancel };
		} else if (lua_istable(_lua, -1)) {
			lua_replace(_lua, -2);
			changed = true;
		} else {
			lua_pop(_lua, 1);
		}
	}
	if (!decoded) {
		return {};
	} else if (!changed) {
		lua_pop(_lua, 1);
		return {};
	}
	auto result = RawResult{ .verdict = Verdict::Modify };
	const auto read = [&](lua_State *L) {
		result.body = Tl::Read(L, 1, type);
	};
	if (!protect(1, 0, read)) {
		return {};
	}
	return result;
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
		u"^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)*\\z"_q); // \z: no final \n
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
		plugin->requirePermission(L, kPermMessagesRead);
		break;
	case Event::MessageSend:
		plugin->requirePermission(L, kPermMessagesSend);
		break;
	case Event::AppState:
		break;
	case Event::UrlOpen:
		plugin->requirePermission(L, kPermLinks);
		break;
	case Event::Action:
		plugin->requirePermission(L, kPermActions);
		break;
	case Event::Display:
		plugin->requirePermission(L, kPermDisplay);
		break;
	case Event::Screen:
		plugin->requirePermission(L, kPermChatsRead);
		break;
	case Event::ProfileRows:
		plugin->requirePermission(L, kPermUi);
		break;
	case Event::MtprotoRequest:
	case Event::MtprotoResponse:
	case Event::MtprotoUpdates:
		plugin->requirePermission(L, kPermMtproto);
		if (!Tl::Available()) {
			return luaL_error(L, "the TL schema is not available");
		}
		break;
	case Event::Count:
		return luaL_error(L, "unknown event: %s", name.toUtf8().constData());
	}

	// Requests and responses: an optional method name or list of them.
	auto filter = std::optional<base::flat_set<mtpTypeId>>();
	if (!lua_isnoneornil(L, 3)) {
		if (event != Event::MtprotoRequest
			&& event != Event::MtprotoResponse) {
			return luaL_error(L, "only mtproto.request and mtproto.response "
				"take a list of methods");
		}
		auto methods = base::flat_set<mtpTypeId>();
		const auto add = [&](int index) {
			if (lua_type(L, index) != LUA_TSTRING) {
				luaL_error(L, "method names must be strings");
			}
			const auto method = ToBytes(L, index);
			const auto id = Tl::Id(method);
			if (!id || !Tl::IsMethod(id)) {
				luaL_error(L, "unknown method: %s", method.constData());
			}
			methods.emplace(id);
		};
		if (lua_type(L, 3) == LUA_TSTRING) {
			add(3);
		} else {
			luaL_checktype(L, 3, LUA_TTABLE);
			const auto count = lua_Integer(lua_rawlen(L, 3));
			for (auto i = lua_Integer(1); i <= count; ++i) {
				lua_rawgeti(L, 3, i);
				add(-1);
				lua_pop(L, 1);
			}
		}
		filter = std::move(methods);
	}

	lua_pushvalue(L, 2);
	const auto ref = luaL_ref(L, LUA_REGISTRYINDEX);
	plugin->_handlers[size_t(event)].push_back(ref);
	if (filter) {
		plugin->_methodFilters.emplace(ref, std::move(*filter));
	}
	// Hooks are enabled when the call that registered this returns.
	crl::on_main(plugin, [host = plugin->_host] {
		host->refreshHooks();
	});
	return 0;
}

int Plugin::ApiMenu(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
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
	plugin->requirePermission(L, kPermStorage);
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
	plugin->requirePermission(L, kPermStorage);
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
	plugin->requirePermission(L, kPermUi);
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
	plugin->requirePermission(L, kPermMessagesSend);
	const auto peerId = FromBotApiId(int64(luaL_checkinteger(L, 1)));
	const auto text = CheckString(L, 2);
	// { reply_to = message id, silent = true, schedule = unixtime }
	auto replyTo = MsgId();
	auto silent = false;
	auto scheduled = TimeId();
	if (!lua_isnoneornil(L, 3)) {
		luaL_checktype(L, 3, LUA_TTABLE);
		lua_getfield(L, 3, "reply_to");
		if (lua_isinteger(L, -1)) {
			replyTo = MsgId(lua_tointeger(L, -1));
		}
		lua_getfield(L, 3, "silent");
		silent = lua_toboolean(L, -1) != 0;
		lua_getfield(L, 3, "schedule");
		if (lua_isinteger(L, -1)) {
			scheduled = TimeId(lua_tointeger(L, -1));
		}
		lua_pop(L, 3);
	}
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
		auto action = Api::SendAction(history);
		action.options.silent = silent;
		action.options.scheduled = scheduled;
		if (replyTo) {
			action.replyTo = FullReplyTo{
				.messageId = FullMsgId(peerId, replyTo),
			};
		}
		auto message = Api::MessageToSend(std::move(action));
		message.textWithTags = TextWithTags{ text };
		const auto bypass = Plugins::BypassScope();
		session->api().sendMessage(std::move(message));
	});
	lua_pushboolean(L, 1);
	return 1;
}

int Plugin::ApiComposeGet(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermCompose);
	if (const auto text = Plugins::ComposeText()) {
		PushString(L, text->text);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int Plugin::ApiComposeSet(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermCompose);
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
	plugin->requirePermission(L, kPermDisplay);
	crl::on_main(plugin, [] {
		Plugins::RefreshDisplayText();
	});
	return 0;
}

// Async: coroutines, timers, HTTP.

void Plugin::resume(lua_State *thread, lua_State *from, int nargs) {
	auto results = 0;
	++_running;
	const auto status = lua_resume(thread, from, nargs, &results);
	--_running;
	if (status == LUA_YIELD) {
		lua_pop(thread, results);
		return; // Waits for a reply, resumed by finishRequest().
	} else if (status != LUA_OK) {
		// The traceback goes to the thread that resumed this one.
		const auto out = from ? from : _lua;
		const auto message = lua_tostring(thread, -1);
		luaL_traceback(
			out,
			thread,
			message ? message : "(error object)",
			0);
		const auto error = ToString(out, -1);
		lua_pop(out, 1);
		fail(error);
	}
	forgetThread(thread);
}

void Plugin::forgetThread(lua_State *thread) {
	if (const auto i = _threads.find(thread); i != end(_threads)) {
		luaL_unref(_lua, LUA_REGISTRYINDEX, i->second);
		_threads.erase(i);
	}
}

void Plugin::deliver(
		int callbackRef,
		lua_State *thread,
		int nresults,
		Fn<void(lua_State*)> push) {
	// Values are made in the main state under protection and only then
	// moved: a suspended coroutine can't run a protected call itself.
	if (callbackRef != LUA_NOREF) {
		lua_rawgeti(_lua, LUA_REGISTRYINDEX, callbackRef);
		luaL_unref(_lua, LUA_REGISTRYINDEX, callbackRef);
		if (!protect(0, nresults, std::move(push))) {
			lua_pop(_lua, 1); // the callback
			return;
		}
		call(nresults, 0, kEventBudget);
	} else if (thread && _threads.contains(thread)) {
		if (!protect(0, nresults, std::move(push))) {
			forgetThread(thread); // it can't go on without its values
			return;
		}
		lua_xmove(_lua, thread, nresults);
		_deadline = crl::now() + kEventBudget;
		lua_sethook(
			thread,
			&Plugin::CountHook,
			LUA_MASKCOUNT,
			kHookInstructions);
		resume(thread, nullptr, nresults);
	}
}

int Plugin::ApiAsync(lua_State *L) {
	const auto plugin = From(L);
	luaL_checktype(L, 1, LUA_TFUNCTION);
	const auto nargs = lua_gettop(L) - 1;
	const auto thread = lua_newthread(L); // L: fn, args..., thread
	if (!lua_checkstack(thread, nargs + 1)) {
		return luaL_error(L, "too many arguments for tg.async");
	}
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
	if (!callback
		&& !(plugin->_threads.contains(L) && lua_isyieldable(L))) {
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
	static const auto methodCheck = QRegularExpression(u"^[A-Z]{1,16}\\z"_q);
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
				const auto name = ToBytes(L, -2);
				const auto value = ToBytes(L, -1);
				if (name.contains('\r') || name.contains('\n')
					|| value.contains('\r') || value.contains('\n')) {
					return luaL_error(L, "line breaks in HTTP headers");
				}
				request.setRawHeader(name, value);
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

	deliver(callbackRef, thread, 1, push);
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
	// Qt 5 parses only an array or an object at the top: wrap, so plain
	// values like "5" or "true" (what tg.json.encode gives) work too.
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(
		'[' + QByteArray(data, int(size)) + ']',
		&error);
	if (error.error != QJsonParseError::NoError
		|| document.array().size() != 1) {
		lua_pushnil(L);
		PushString(L, (error.error != QJsonParseError::NoError)
			? error.errorString()
			: u"expected one JSON value"_q);
		return 2;
	}
	PushJson(L, document.array().at(0), 0);
	return 1;
}

// Raw MTProto calls.

int Plugin::ApiInvoke(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermMtproto);
	luaL_checktype(L, 1, LUA_TTABLE);
	const auto callback = lua_isfunction(L, 2);
	if (!callback
		&& !(plugin->_threads.contains(L) && lua_isyieldable(L))) {
		return luaL_error(
			L,
			"tg.invoke needs a callback or a tg.async(...) coroutine");
	} else if (plugin->_invokes >= kMaxInvokes) {
		return luaL_error(L, "too many calls at once");
	}
	const auto session = ActiveSession();
	if (!session) {
		return luaL_error(L, "no account is logged in");
	}
	auto body = Tl::Read(L, 1, Tl::Query()); // raises on a bad table
	const auto method = Tl::MethodOf(body);
	const auto type = Tl::ResultOf(method);

	auto callbackRef = LUA_NOREF;
	if (callback) {
		lua_pushvalue(L, 2);
		callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	const auto thread = callback ? nullptr : L;
	const auto generation = plugin->_generation;
	const auto weak = base::make_weak(plugin);
	const auto host = base::make_weak(plugin->_host.get());
	const auto requestId = MTP::details::GetNextRequestId();
	++plugin->_invokes;
	// Filters of plugins never see the calls of plugins: no loops.
	plugin->_host->ownRequest(requestId, method);
	const auto finish = [=](
			const mtpBuffer *reply,
			const MTP::Error *error) {
		if (const auto strong = host.get()) {
			strong->forgetOwnRequest(requestId);
		}
		if (const auto strong = weak.get()) {
			strong->finishInvoke(
				reply,
				error,
				type,
				generation,
				callbackRef,
				thread);
		}
	};
	// An answer always comes: also when the request is dropped with its
	// session (logout, account switch) and neither done nor fail runs.
	struct Pending {
		Fn<void(const mtpBuffer*, const MTP::Error*)> finish;
		bool answered = false;

		~Pending() {
			if (!answered && finish) {
				crl::on_main([finish = finish] {
					const auto error = MTP::Error::Local(
						u"PLUGIN_ABORTED"_q,
						u"The request was dropped."_q);
					finish(nullptr, &error);
				});
			}
		}
	};
	const auto pending = std::make_shared<Pending>();
	pending->finish = finish;
	const auto answer = [=](
			const mtpBuffer *reply,
			const MTP::Error *error) {
		pending->answered = true;
		pending->finish(reply, error);
	};
	// Sent on the next event loop turn, not from inside a running hook.
	crl::on_main(session, [=, body = std::move(body)] {
		auto request = MTP::details::SerializedRequest::Prepare(
			uint32(body.size()));
		request->append(body);
		session->mtp().sendSerialized(
			requestId,
			std::move(request),
			MTP::ResponseHandler{
				.done = [=](const MTP::Response &response) {
					answer(&response.reply, nullptr);
					return true;
				},
				.fail = [=](
						const MTP::Error &error,
						const MTP::Response &) {
					answer(nullptr, &error);
					return true;
				},
			},
			0,  // main dc
			0,  // send now
			0); // after no request
	});
	if (callback) {
		return 0;
	}
	return lua_yield(L, 0); // resumed with (result, error) by finishInvoke()
}

void Plugin::finishInvoke(
		const mtpBuffer *reply,
		const MTP::Error *error,
		Tl::Type type,
		int generation,
		int callbackRef,
		lua_State *thread) {
	if (generation != _generation || !_lua) {
		return;
	}
	--_invokes;
	// (result, nil) or (nil, { code, type, description })
	deliver(callbackRef, thread, 2, [&](lua_State *L) {
		if (reply) {
			auto job = DecodeJob{ .type = type, .data = reply };
			lua_pushcfunction(L, &RunDecode);
			lua_pushlightuserdata(L, &job);
			if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
				lua_pushnil(L);
				return;
			}
			const auto message = ToString(L, -1);
			lua_pop(L, 1);
			lua_pushnil(L);
			PushError(L, 0, u"TL_DECODE_FAILED"_q, message);
		} else {
			lua_pushnil(L);
			PushError(L, error->code(), error->type(), error->description());
		}
	});
}

// Messages and chats.

int Plugin::ApiEditMessage(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermMessagesManage);
	const auto peerId = FromBotApiId(int64(luaL_checkinteger(L, 1)));
	const auto msgId = MsgId(luaL_checkinteger(L, 2));
	const auto text = CheckString(L, 3);
	const auto session = ActiveSession();
	const auto itemId = FullMsgId(peerId, msgId);
	const auto item = session ? session->data().message(itemId) : nullptr;
	if (!item || text.isEmpty() || !item->allowsEdit(base::unixtime::now())) {
		lua_pushboolean(L, 0);
		return 1;
	}
	crl::on_main(session, [=] {
		const auto item = session->data().message(itemId);
		if (!item) {
			return;
		}
		Api::EditTextMessage(
			item,
			TextWithEntities{ text },
			Data::WebPageDraft::FromItem(item),
			Api::SendOptions(),
			[](mtpRequestId) {},
			[=](const QString &error, mtpRequestId) {
				LOG(("Plugins: edit failed: %1").arg(error));
			},
			false);
	});
	lua_pushboolean(L, 1);
	return 1;
}

int Plugin::ApiDeleteMessages(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermMessagesManage);
	const auto peerId = FromBotApiId(int64(luaL_checkinteger(L, 1)));
	auto ids = std::vector<MsgId>();
	if (lua_isinteger(L, 2)) {
		ids.push_back(MsgId(lua_tointeger(L, 2)));
	} else {
		luaL_checktype(L, 2, LUA_TTABLE);
		const auto count = lua_Integer(lua_rawlen(L, 2));
		if (count > kMaxDelete) {
			return luaL_error(L, "up to 100 messages at once");
		}
		for (auto i = lua_Integer(1); i <= count; ++i) {
			lua_rawgeti(L, 2, i);
			if (!lua_isinteger(L, -1)) {
				return luaL_error(L, "message ids must be integers");
			}
			ids.push_back(MsgId(lua_tointeger(L, -1)));
			lua_pop(L, 1);
		}
	}
	auto revoke = (lua_toboolean(L, 3) != 0); // for everyone
	const auto session = ActiveSession();
	auto list = MessageIdsList();
	if (session) {
		const auto now = base::unixtime::now();
		for (const auto id : ids) {
			const auto item = session->data().message(FullMsgId(peerId, id));
			if (item && item->canDelete()) {
				list.push_back(item->fullId());
				revoke = revoke && item->canDeleteForEveryone(now);
			}
		}
	}
	if (list.empty()) {
		lua_pushinteger(L, 0);
		return 1;
	}
	const auto count = int(list.size());
	crl::on_main(session, [=] {
		session->data().histories().deleteMessages(list, revoke);
		session->data().sendHistoryChangeNotifications();
	});
	lua_pushinteger(L, count);
	return 1;
}

int Plugin::ApiGetMessage(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermMessagesRead);
	const auto peerId = FromBotApiId(int64(luaL_checkinteger(L, 1)));
	const auto msgId = MsgId(luaL_checkinteger(L, 2));
	const auto session = ActiveSession();
	const auto item = session
		? session->data().message(FullMsgId(peerId, msgId))
		: nullptr;
	if (item) {
		plugin->pushMessage(L, item);
	} else {
		lua_pushnil(L); // not loaded: tg.invoke can ask the server
	}
	return 1;
}

int Plugin::ApiPeer(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermChatsRead);
	const auto peerId = FromBotApiId(int64(luaL_checkinteger(L, 1)));
	const auto session = ActiveSession();
	const auto peer = (session && peerId)
		? session->data().peerLoaded(peerId)
		: nullptr;
	if (peer) {
		plugin->pushPeer(L, peer);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int Plugin::ApiMe(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermChatsRead);
	if (const auto session = ActiveSession()) {
		plugin->pushPeer(L, session->user());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int Plugin::ApiCurrentChat(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermChatsRead);
	const auto controller = ActiveController();
	const auto peer = controller
		? controller->activeChatCurrent().peer()
		: nullptr;
	if (peer) {
		lua_pushinteger(L, BotApiId(peer->id));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Windows.

Plugin::Waiter Plugin::waiter(lua_State *L, int index, const char *what) {
	auto result = Waiter{
		.callbackRef = LUA_NOREF,
		.generation = _generation,
	};
	if (lua_isfunction(L, index)) {
		lua_pushvalue(L, index);
		result.callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
	} else if (_threads.contains(L) && lua_isyieldable(L)) {
		result.thread = L;
	} else {
		luaL_error(
			L,
			"%s needs a callback or a tg.async(...) coroutine",
			what);
	}
	return result;
}

int Plugin::wait(lua_State *L, const Waiter &waiter) {
	return waiter.thread ? lua_yield(L, 0) : 0;
}

void Plugin::answer(const Waiter &waiter, Fn<void(lua_State*)> push) {
	if (waiter.generation != _generation || !_lua) {
		return;
	}
	deliver(waiter.callbackRef, waiter.thread, 1, std::move(push));
}

int Plugin::ApiAlert(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	const auto text = CheckString(L, 1);
	const auto title = (lua_type(L, 2) == LUA_TSTRING)
		? ToString(L, 2)
		: QString();
	crl::on_main(plugin, [=] {
		if (const auto controller = ActiveController()) {
			auto args = Ui::ConfirmBoxArgs{ .text = text };
			if (!title.isEmpty()) {
				args.title = title;
			}
			controller->show(Ui::MakeInformBox(std::move(args)));
		}
	});
	return 0;
}

// tg.ui.confirm("text") or { title, text, ok, cancel } -> true / false
int Plugin::ApiConfirm(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	auto title = QString();
	auto text = QString();
	auto ok = QString();
	auto cancel = QString();
	if (lua_type(L, 1) == LUA_TSTRING) {
		text = ToString(L, 1);
	} else {
		luaL_checktype(L, 1, LUA_TTABLE);
		title = StringField(L, 1, "title");
		text = StringField(L, 1, "text");
		ok = StringField(L, 1, "ok");
		cancel = StringField(L, 1, "cancel");
	}
	const auto waiter = plugin->waiter(L, 2, "tg.ui.confirm");
	const auto weak = base::make_weak(plugin);
	crl::on_main(plugin, [=] {
		const auto answered = std::make_shared<bool>(false);
		const auto finish = [=](bool value) {
			if (*answered) {
				return;
			}
			*answered = true;
			if (const auto strong = weak.get()) {
				strong->answer(waiter, [=](lua_State *L) {
					lua_pushboolean(L, value ? 1 : 0);
				});
			}
		};
		const auto controller = ActiveController();
		if (!controller) {
			finish(false);
			return;
		}
		const auto okText = ok.isEmpty() ? tr::lng_box_ok(tr::now) : ok;
		const auto cancelText = cancel.isEmpty()
			? tr::lng_cancel(tr::now)
			: cancel;
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			if (!title.isEmpty()) {
				box->setTitle(title);
			}
			box->addRow(object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
			box->addButton(rpl::single(okText), [=] {
				finish(true);
				box->closeBox();
			});
			box->addButton(rpl::single(cancelText), [=] {
				box->closeBox();
			});
			box->boxClosing() | rpl::on_next([=] {
				finish(false);
			}, box->lifetime());
		}));
	});
	return plugin->wait(L, waiter);
}

// tg.ui.input{ title, text, value, placeholder, ok, cancel } -> text / nil
int Plugin::ApiInput(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	luaL_checktype(L, 1, LUA_TTABLE);
	const auto title = StringField(L, 1, "title");
	const auto text = StringField(L, 1, "text");
	const auto value = StringField(L, 1, "value");
	const auto placeholder = StringField(L, 1, "placeholder");
	const auto ok = StringField(L, 1, "ok");
	const auto cancel = StringField(L, 1, "cancel");
	const auto waiter = plugin->waiter(L, 2, "tg.ui.input");
	const auto weak = base::make_weak(plugin);
	crl::on_main(plugin, [=] {
		const auto answered = std::make_shared<bool>(false);
		const auto finish = [=](std::optional<QString> result) {
			if (*answered) {
				return;
			}
			*answered = true;
			if (const auto strong = weak.get()) {
				strong->answer(waiter, [=](lua_State *L) {
					if (result) {
						PushString(L, *result);
					} else {
						lua_pushnil(L);
					}
				});
			}
		};
		const auto controller = ActiveController();
		if (!controller) {
			finish(std::nullopt);
			return;
		}
		const auto okText = ok.isEmpty() ? tr::lng_box_ok(tr::now) : ok;
		const auto cancelText = cancel.isEmpty()
			? tr::lng_cancel(tr::now)
			: cancel;
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			if (!title.isEmpty()) {
				box->setTitle(title);
			}
			if (!text.isEmpty()) {
				box->addRow(
					object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
			}
			const auto field = box->addRow(object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(placeholder),
				value));
			field->setMaxLength(kMaxInput);
			box->setFocusCallback([=] {
				field->setFocusFast();
			});
			const auto submit = [=] {
				finish(field->getLastText());
				box->closeBox();
			};
			field->submits(
			) | rpl::on_next([=](Qt::KeyboardModifiers) {
				submit();
			}, field->lifetime());
			box->addButton(rpl::single(okText), submit);
			box->addButton(rpl::single(cancelText), [=] {
				box->closeBox();
			});
			box->boxClosing() | rpl::on_next([=] {
				finish(std::nullopt);
			}, box->lifetime());
		}));
	});
	return plugin->wait(L, waiter);
}

int Plugin::ApiCopy(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	const auto text = CheckString(L, 1);
	crl::on_main(plugin, [=] {
		QGuiApplication::clipboard()->setText(text);
	});
	return 0;
}

// Package resources and the plugin's own files.

// plugin.resource("icon.png") -> bytes of resources/icon.png or nil.
int Plugin::ApiResource(lua_State *L) {
	const auto plugin = From(L);
	const auto path = CheckString(L, 1);
	const auto &files = plugin->_package.files;
	const auto i = files.find(u"resources/"_q + path);
	if (i == end(files)) {
		lua_pushnil(L);
	} else {
		lua_pushlstring(L, i->second.constData(), size_t(i->second.size()));
	}
	return 1;
}

QString Plugin::filePath(lua_State *L, int index) const {
	const auto name = CheckString(L, index);
	static const auto check = QRegularExpression(
		u"^[A-Za-z0-9_][A-Za-z0-9_.\\-]{0,63}\\z"_q);
	if (!check.match(name).hasMatch() || name.contains(u".."_q)) {
		luaL_error(
			L,
			"bad file name: %s (use 1-64 of A-Z a-z 0-9 _ . -)",
			name.toUtf8().constData());
	}
	return _dataPath + u"files/"_q + name;
}

int Plugin::ApiFileRead(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermStorage);
	auto file = QFile(plugin->filePath(L, 1));
	if (file.size() > kMaxFile || !file.open(QIODevice::ReadOnly)) {
		lua_pushnil(L);
		return 1;
	}
	const auto bytes = file.readAll();
	lua_pushlstring(L, bytes.constData(), size_t(bytes.size()));
	return 1;
}

int Plugin::ApiFileWrite(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermStorage);
	const auto path = plugin->filePath(L, 1);
	auto size = size_t(0);
	const auto data = luaL_checklstring(L, 2, &size);
	if (qint64(size) > kMaxFile) {
		return luaL_error(L, "a file is limited to 16 MB");
	}
	const auto folder = QFileInfo(path).absolutePath();
	auto total = qint64(size);
	const auto others = QDir(folder).entryInfoList(QDir::Files);
	for (const auto &info : others) {
		if (info.absoluteFilePath() != QFileInfo(path).absoluteFilePath()) {
			total += info.size();
		}
	}
	if (total > kMaxFiles) {
		return luaL_error(L, "the files of a plugin are limited to 64 MB");
	}
	QDir().mkpath(folder);
	auto file = QFile(path);
	const auto written = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		&& (file.write(data, qint64(size)) == qint64(size));
	lua_pushboolean(L, written ? 1 : 0);
	return 1;
}

int Plugin::ApiFileRemove(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermStorage);
	lua_pushboolean(L, QFile::remove(plugin->filePath(L, 1)) ? 1 : 0);
	return 1;
}

int Plugin::ApiFileList(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermStorage);
	const auto names = QDir(plugin->_dataPath + u"files/"_q).entryList(
		QDir::Files,
		QDir::Name);
	lua_createtable(L, int(names.size()), 0);
	auto index = lua_Integer(0);
	for (const auto &name : names) {
		PushString(L, name);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

} // namespace Plugins::Flua
