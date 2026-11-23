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
#include "data/data_document.h"
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
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "plugins/plugins_hooks.h"
#include "plugins/runtime/flua_runtime.h"
#include "plugins/runtime/flua_tl.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/basic_click_handlers.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"
#include "styles/style_boxes.h"
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
constexpr auto kPaintBudget = crl::time(100);
constexpr auto kMaxPageRows = 500;
constexpr auto kMaxCustomHeight = 1000;
constexpr auto kMaxImages = 64;
constexpr auto kMaxImageSide = 2048;
constexpr auto kMaxOverlays = 16;
constexpr auto kMaxOverlaySide = 4096;
constexpr auto kMaxDumpDepth = 8;
constexpr auto kMaxDumpItems = 100;

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

[[nodiscard]] std::optional<MenuPlace> ParseMenuPlace(const QString &place) {
	if (place == u"message"_q) {
		return MenuPlace::Message;
	} else if (place == u"main"_q) {
		return MenuPlace::Main;
	} else if (place == u"chat"_q) {
		return MenuPlace::Chat;
	} else if (place == u"profile"_q) {
		return MenuPlace::Profile;
	} else if (place == u"send"_q) {
		return MenuPlace::Send;
	}
	return std::nullopt;
}

// <b>, <i>, <u>, <s>, <code>, <pre>, <a href>, <tg-spoiler>, <blockquote>
// and &amp; &lt; &gt; &quot; &#NN; to text with entities. Unknown tags
// are dropped, their text is kept.
[[nodiscard]] TextWithEntities ParseHtml(const QString &html) {
	struct Open {
		QString tag;
		EntityType type = EntityType::Invalid;
		int offset = 0;
		QString data;
	};
	static const auto href = QRegularExpression(
		u"href\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')"_q,
		QRegularExpression::CaseInsensitiveOption);
	auto result = TextWithEntities();
	auto stack = std::vector<Open>();
	auto i = 0;
	const auto size = int(html.size());
	while (i < size) {
		const auto ch = html[i];
		if (ch == u'<') {
			const auto close = html.indexOf(u'>', i);
			if (close < 0) {
				result.text += html.mid(i);
				break;
			}
			auto inside = html.mid(i + 1, close - i - 1).trimmed();
			i = close + 1;
			const auto closing = inside.startsWith(u'/');
			if (closing) {
				inside = inside.mid(1).trimmed();
			}
			if (inside.endsWith(u'/')) {
				inside.chop(1);
			}
			const auto space = inside.indexOf(u' ');
			const auto name = ((space < 0) ? inside : inside.left(space))
				.trimmed()
				.toLower();
			const auto attributes = (space < 0) ? QString() : inside.mid(space);
			if (name == u"br"_q) {
				result.text += u'\n';
				continue;
			}
			auto type = EntityType::Invalid;
			auto data = QString();
			if (name == u"b"_q || name == u"strong"_q) {
				type = EntityType::Bold;
			} else if (name == u"i"_q || name == u"em"_q) {
				type = EntityType::Italic;
			} else if (name == u"u"_q || name == u"ins"_q) {
				type = EntityType::Underline;
			} else if (name == u"s"_q
				|| name == u"strike"_q
				|| name == u"del"_q) {
				type = EntityType::StrikeOut;
			} else if (name == u"code"_q) {
				type = EntityType::Code;
			} else if (name == u"pre"_q) {
				type = EntityType::Pre;
			} else if (name == u"blockquote"_q) {
				type = EntityType::Blockquote;
			} else if (name == u"tg-spoiler"_q
				|| name == u"spoiler"_q
				|| (name == u"span"_q
					&& attributes.contains(u"tg-spoiler"_q))) {
				type = EntityType::Spoiler;
			} else if (name == u"a"_q) {
				type = EntityType::CustomUrl;
				const auto match = href.match(attributes);
				if (match.hasMatch()) {
					data = match.captured(1).isEmpty()
						? match.captured(2)
						: match.captured(1);
				}
			}
			if (type == EntityType::Invalid) {
				continue;
			}
			if (!closing) {
				stack.push_back({
					.tag = name,
					.type = type,
					.offset = int(result.text.size()),
					.data = data,
				});
				continue;
			}
			for (auto j = int(stack.size()) - 1; j >= 0; --j) {
				if (stack[j].tag != name) {
					continue;
				}
				const auto open = stack[j];
				stack.erase(begin(stack) + j);
				const auto length = int(result.text.size()) - open.offset;
				if (length > 0
					&& (open.type != EntityType::CustomUrl
						|| !open.data.isEmpty())) {
					result.entities.push_back(EntityInText(
						open.type,
						open.offset,
						length,
						open.data));
				}
				break;
			}
		} else if (ch == u'&') {
			const auto semicolon = html.indexOf(u';', i);
			auto decoded = QString();
			if (semicolon > i && semicolon - i <= 10) {
				const auto entity = html.mid(i + 1, semicolon - i - 1);
				if (entity == u"amp"_q) {
					decoded = u"&"_q;
				} else if (entity == u"lt"_q) {
					decoded = u"<"_q;
				} else if (entity == u"gt"_q) {
					decoded = u">"_q;
				} else if (entity == u"quot"_q) {
					decoded = u"\""_q;
				} else if (entity == u"apos"_q) {
					decoded = u"'"_q;
				} else if (entity == u"nbsp"_q) {
					decoded = QString(QChar(0xA0));
				} else if (entity.startsWith(u'#')) {
					auto ok = false;
					const auto hex = entity.startsWith(u"#x"_q, Qt::CaseInsensitive);
					const auto code = hex
						? entity.mid(2).toUInt(&ok, 16)
						: entity.mid(1).toUInt(&ok, 10);
					if (ok && code > 0 && code < 0x110000) {
						decoded = QString::fromUcs4(&code, 1);
					}
				}
			}
			if (decoded.isEmpty()) {
				result.text += ch;
				++i;
			} else {
				result.text += decoded;
				i = semicolon + 1;
			}
		} else {
			result.text += ch;
			++i;
		}
	}
	ranges::sort(result.entities, [](
			const EntityInText &a,
			const EntityInText &b) {
		return a.offset() < b.offset();
	});
	return result;
}

// parse_mode = "markdown" | "html" | nil (plain text).
[[nodiscard]] TextWithEntities Formatted(
		const QString &text,
		const QString &mode) {
	if (mode == u"markdown"_q) {
		return TextUtilities::ParseEntities(
			text,
			TextParseLinks | TextParseMarkdown);
	} else if (mode == u"html"_q) {
		return ParseHtml(text);
	}
	return TextWithEntities{ text };
}

[[nodiscard]] TextWithTags ToTags(TextWithEntities &&text) {
	return TextWithTags{
		text.text,
		TextUtilities::ConvertEntitiesToTextTags(text.entities),
	};
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

const std::map<QString, QByteArray> &Plugin::files() const {
	return _package.files;
}

bool Plugin::running() const {
	return _lua != nullptr;
}

const QString &Plugin::lastError() const {
	return _lastError;
}

QImage Plugin::icon() const {
	if (!_icon) {
		_icon = ReadIcon(_package.files, manifest().icon);
	}
	return *_icon;
}

const std::deque<QString> &Plugin::journal() const {
	return _journal;
}

void Plugin::addToJournal(const QString &text) {
	constexpr auto kMaxLines = 300;
	constexpr auto kMaxLine = 2000;
	_journal.push_back(QTime::currentTime().toString(u"HH:mm:ss"_q)
		+ u"  "_q
		+ text.left(kMaxLine));
	while (_journal.size() > kMaxLines) {
		_journal.pop_front();
	}
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
	_imageLookup = [=](int id) -> const QImage* {
		const auto i = _images.find(id);
		return (i != end(_images)) ? &i->second : nullptr;
	};

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
	addToJournal(u"started"_q);
	_host->notifyChanged();
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
	// Not right now: stop() may come from a click on an overlay.
	for (auto &[id, overlay] : base::take(_overlays)) {
		if (const auto widget = overlay.widget.release()) {
			widget->hide();
			widget->deleteLater();
		}
	}
	_images.clear();
	_settingsPage = kNoRef;
	_pageBuilders.clear();
	lua_close(_lua);
	_lua = nullptr;
	for (auto &list : _handlers) {
		list.clear();
	}
	_methodFilters.clear();
	_menu.clear();
	addToJournal(u"stopped"_q);
	_host->refreshHooks();
	_host->notifyChanged();
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
	set("menu_remove", &Plugin::ApiMenuRemove);
	lua_newtable(_lua);
	set("get", &Plugin::ApiSettingsGet);
	set("set", &Plugin::ApiSettingsSet);
	set("all", &Plugin::ApiSettingsAll);
	set("reset", &Plugin::ApiSettingsReset);
	set("page", &Plugin::ApiSettingsPage);
	set("refresh", &Plugin::ApiSettingsRefresh);
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
	set("dump", &Plugin::ApiDump);
	set("now", &Plugin::ApiNow);
	set("toast", &Plugin::ApiToast);
	lua_newtable(_lua);
	set("send", &Plugin::ApiSendMessage);
	set("send_file", &Plugin::ApiSendFile);
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
	set("dialog", &Plugin::ApiDialog);
	set("current", &Plugin::ApiCurrent);
	set("open", &Plugin::ApiOpen);
	set("color", &Plugin::ApiColor);
	set("image", &Plugin::ApiImage);
	set("image_free", &Plugin::ApiImageFree);
	set("overlay", &Plugin::ApiOverlay);
	set("window_size", &Plugin::ApiWindowSize);
	lua_setfield(_lua, -2, "ui");
	lua_newtable(_lua);
	set("state", &Plugin::ApiPlayerState);
	set("toggle", &Plugin::ApiPlayerToggle);
	set("play", &Plugin::ApiPlayerPlay);
	set("pause", &Plugin::ApiPlayerPause);
	set("next", &Plugin::ApiPlayerNext);
	set("previous", &Plugin::ApiPlayerPrevious);
	set("stop", &Plugin::ApiPlayerStop);
	set("seek", &Plugin::ApiPlayerSeek);
	lua_setfield(_lua, -2, "player");
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
	addToJournal(u"ERROR: "_q + error);
	_host->notifyChanged();
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
	const auto text = parts.join(u' ');
	LOG(("Plugins [%1]: %2").arg(plugin->manifest().id, text));
	plugin->addToJournal(text);
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

// plugin.menu(place, text, fn) or plugin.menu{ place, text, icon,
// section, on_click, page } -> id for plugin.menu_remove(id)
int Plugin::ApiMenu(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	auto entry = MenuEntry();
	if (lua_istable(L, 1)) {
		entry = plugin->readMenuEntry(L, 1);
	} else {
		const auto place = CheckString(L, 1);
		entry.text = CheckString(L, 2);
		luaL_checktype(L, 3, LUA_TFUNCTION);
		if (const auto found = ParseMenuPlace(place)) {
			entry.place = *found;
		} else {
			return luaL_error(
				L,
				"unknown menu: %s",
				place.toUtf8().constData());
		}
		if (entry.text.isEmpty()) {
			return luaL_error(L, "menu item text is empty");
		}
		lua_pushvalue(L, 3);
		entry.ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	entry.id = ++plugin->_nextMenuId;
	const auto id = entry.id;
	plugin->_menu.push_back(std::move(entry));
	crl::on_main(plugin, [host = plugin->_host] {
		host->refreshHooks();
	});
	lua_pushinteger(L, id);
	return 1;
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

// tg.toast("text") or tg.toast{ text, title, duration = ms }
int Plugin::ApiToast(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	auto text = QString();
	auto title = QString();
	auto duration = crl::time(0);
	if (lua_istable(L, 1)) {
		text = StringField(L, 1, "text");
		title = StringField(L, 1, "title");
		lua_getfield(L, 1, "duration");
		if (lua_isinteger(L, -1)) {
			duration = std::clamp(
				crl::time(lua_tointeger(L, -1)),
				crl::time(500),
				crl::time(60000));
		}
		lua_pop(L, 1);
	} else {
		text = CheckString(L, 1);
	}
	crl::on_main(plugin, [=] {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto controller = window->sessionController()) {
				auto config = Ui::Toast::Config{
					.title = title,
					.text = TextWithEntities{ text },
				};
				if (duration) {
					config.duration = duration;
				}
				controller->showToast(std::move(config));
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
	auto mode = QString();
	if (!lua_isnoneornil(L, 3)) {
		luaL_checktype(L, 3, LUA_TTABLE);
		mode = StringField(L, 3, "parse_mode").toLower();
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
		message.textWithTags = ToTags(Formatted(text, mode));
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
	const auto send = [=, body = std::move(body)] {
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
	};
	const auto name = QString::fromLatin1(Tl::Name(method));
	const auto pluginName = plugin->manifest().name;
	// Sent on the next event loop turn, not from inside a running hook.
	crl::on_main(session, [=] {
		const auto strong = host.get();
		if (!strong
			|| !strong->engine().confirmSensitive
			|| !IsSensitiveMethod(name)) {
			send();
			return;
		}
		// Stars, gifts, sessions, the password: only after a "yes".
		strong->confirmSensitive(pluginName, name, [=](bool allowed) {
			if (allowed) {
				crl::on_main(session, send);
			} else {
				const auto error = MTP::Error::Local(
					u"PLUGIN_DENIED"_q,
					u"The user did not allow this request."_q);
				answer(nullptr, &error);
			}
		});
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
	const auto mode = lua_istable(L, 4)
		? StringField(L, 4, "parse_mode").toLower()
		: QString();
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
			Formatted(text, mode),
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

// Pages, menus anywhere, canvas, overlays, dialogs (API 2 of the UI).

int Plugin::generation() const {
	return _generation;
}

bool Plugin::hasSettingsPage() const {
	return _lua && (_settingsPage != kNoRef);
}

rpl::producer<> Plugin::pageRefreshes() const {
	return _pageRefreshes.events();
}

QJsonValue Plugin::setting(const QString &key) const {
	return _settings.value(key);
}

void Plugin::setSetting(const QString &key, const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) {
		_settings.remove(key);
	} else {
		_settings.insert(key, value);
	}
	saveSettings();
}

MenuEntry Plugin::readMenuEntry(lua_State *L, int index) {
	auto entry = MenuEntry();
	const auto place = StringField(L, index, "place");
	auto section = StringField(L, index, "section");
	if (place.startsWith(u"settings"_q)) {
		entry.place = MenuPlace::Settings;
		if (place.startsWith(u"settings."_q)) {
			section = place.mid(9);
		}
		entry.section = section.isEmpty() ? u"main"_q : section;
	} else if (const auto found = ParseMenuPlace(place)) {
		entry.place = *found;
	} else {
		luaL_error(L, "unknown menu place: %s", place.toUtf8().constData());
	}
	entry.text = StringField(L, index, "text");
	if (entry.text.isEmpty()) {
		luaL_error(L, "menu item text is empty");
	}
	entry.icon = StringField(L, index, "icon");
	if (!entry.icon.isEmpty() && !IconByName(entry.icon)) {
		luaL_error(L, "unknown icon: %s", entry.icon.toUtf8().constData());
	}
	lua_getfield(L, index, "on_click");
	if (lua_isfunction(L, -1)) {
		entry.ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_pop(L, 1);
	}
	lua_getfield(L, index, "page");
	if (lua_isfunction(L, -1)) {
		entry.page = luaL_ref(L, LUA_REGISTRYINDEX);
		_pageBuilders.emplace(entry.page);
	} else {
		lua_pop(L, 1);
	}
	if (entry.ref == kNoRef && entry.page == kNoRef) {
		luaL_error(L, "a menu item needs on_click or page");
	}
	return entry;
}

int Plugin::ApiMenuRemove(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	const auto id = int(luaL_checkinteger(L, 1));
	const auto i = ranges::find(plugin->_menu, id, &MenuEntry::id);
	if (i == end(plugin->_menu)) {
		lua_pushboolean(L, 0);
		return 1;
	}
	if (i->ref != kNoRef) {
		luaL_unref(L, LUA_REGISTRYINDEX, i->ref);
	}
	// i->page stays: a page opened from the item may still be shown.
	plugin->_menu.erase(i);
	crl::on_main(plugin, [host = plugin->_host] {
		host->refreshHooks();
	});
	lua_pushboolean(L, 1);
	return 1;
}

// plugin.settings.page(function() return { rows } end)
int Plugin::ApiSettingsPage(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	if (lua_isnil(L, 1)) {
		if (plugin->_settingsPage != kNoRef) {
			luaL_unref(L, LUA_REGISTRYINDEX, plugin->_settingsPage);
			plugin->_settingsPage = kNoRef;
		}
	} else {
		luaL_checktype(L, 1, LUA_TFUNCTION);
		if (plugin->_settingsPage != kNoRef) {
			luaL_unref(L, LUA_REGISTRYINDEX, plugin->_settingsPage);
		}
		lua_pushvalue(L, 1);
		plugin->_settingsPage = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	crl::on_main(plugin, [=] {
		plugin->_host->notifyChanged();
		plugin->_pageRefreshes.fire({});
	});
	return 0;
}

int Plugin::ApiSettingsRefresh(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	// Not now: the page may call this from its own handler.
	crl::on_main(plugin, [=] {
		plugin->_pageRefreshes.fire({});
	});
	return 0;
}

int Plugin::ApiSettingsAll(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermStorage);
	lua_createtable(L, 0, int(plugin->_settings.size()));
	for (auto i = plugin->_settings.begin(); i != plugin->_settings.end(); ++i) {
		PushString(L, i.key());
		PushJson(L, i.value(), 0);
		lua_settable(L, -3);
	}
	return 1;
}

int Plugin::ApiSettingsReset(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermStorage);
	plugin->_settings = QJsonObject();
	plugin->saveSettings();
	crl::on_main(plugin, [=] {
		plugin->_pageRefreshes.fire({});
	});
	return 0;
}

void Plugin::readPage(lua_State *L, int index, Page &page) {
	luaL_checktype(L, index, LUA_TTABLE);
	page.title = StringField(L, index, "title");
	const auto count = int(lua_rawlen(L, index));
	if (count > kMaxPageRows) {
		luaL_error(L, "a page has more than %d rows", kMaxPageRows);
	}
	const auto ref = [&](lua_State *L, int row, const char *name) {
		lua_getfield(L, row, name);
		if (!lua_isfunction(L, -1)) {
			lua_pop(L, 1);
			return kNoRef;
		}
		const auto result = luaL_ref(L, LUA_REGISTRYINDEX);
		page.refs.push_back(result);
		return result;
	};
	const auto number = [&](lua_State *L, int row, const char *name, double fallback) {
		lua_getfield(L, row, name);
		const auto result = lua_isnumber(L, -1)
			? double(lua_tonumber(L, -1))
			: fallback;
		lua_pop(L, 1);
		return result;
	};
	const auto flag = [&](lua_State *L, int row, const char *name) {
		lua_getfield(L, row, name);
		const auto result = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		return result;
	};
	for (auto i = 1; i <= count; ++i) {
		lua_rawgeti(L, index, i);
		if (!lua_istable(L, -1)) {
			luaL_error(L, "row %d of the page is not a table", i);
		}
		const auto row = lua_gettop(L);
		auto item = PageItem();
		const auto type = StringField(L, row, "type");
		using Type = PageItem::Type;
		if (type == u"header"_q) {
			item.type = Type::Header;
		} else if (type == u"divider"_q) {
			item.type = Type::Divider;
		} else if (type == u"switch"_q) {
			item.type = Type::Switch;
		} else if (type == u"selector"_q) {
			item.type = Type::Selector;
		} else if (type == u"input"_q) {
			item.type = Type::Input;
		} else if (type == u"text"_q || type.isEmpty()) {
			item.type = Type::Text;
		} else if (type == u"slider"_q) {
			item.type = Type::Slider;
		} else if (type == u"custom"_q) {
			item.type = Type::Custom;
		} else {
			luaL_error(L, "row %d: unknown type %s", i, type.toUtf8().constData());
		}
		item.key = StringField(L, row, "key");
		item.text = StringField(L, row, "text");
		item.subtext = StringField(L, row, "subtext");
		item.icon = StringField(L, row, "icon");
		item.placeholder = StringField(L, row, "placeholder");
		if (!item.icon.isEmpty() && !IconByName(item.icon)) {
			luaL_error(L, "row %d: unknown icon %s", i, item.icon.toUtf8().constData());
		}
		item.accent = flag(L, row, "accent");
		item.red = flag(L, row, "red");
		item.min = number(L, row, "min", 0.);
		item.max = number(L, row, "max", 100.);
		item.step = std::max(number(L, row, "step", 1.), 1e-9);
		item.height = int(number(L, row, "height", 0.));
		lua_getfield(L, row, "items");
		if (lua_istable(L, -1)) {
			const auto items = lua_gettop(L);
			const auto size = std::min(int(lua_rawlen(L, items)), kMaxPageRows);
			for (auto j = 1; j <= size; ++j) {
				lua_rawgeti(L, items, j);
				item.items.push_back(ToString(L, -1));
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		lua_getfield(L, row, "default");
		switch (lua_type(L, -1)) {
		case LUA_TBOOLEAN:
			item.fallback = (lua_toboolean(L, -1) != 0);
			break;
		case LUA_TNUMBER:
			item.fallback = double(lua_tonumber(L, -1));
			break;
		case LUA_TSTRING:
			item.fallback = ToString(L, -1);
			break;
		}
		lua_pop(L, 1);
		item.onChange = ref(L, row, "on_change");
		item.onClick = ref(L, row, "on_click");
		lua_getfield(L, row, "page");
		if (lua_isfunction(L, -1)) {
			item.page = luaL_ref(L, LUA_REGISTRYINDEX);
			_pageBuilders.emplace(item.page);
		} else {
			lua_pop(L, 1);
		}
		item.paint = ref(L, row, "paint");

		const auto keyed = (item.type == Type::Switch)
			|| (item.type == Type::Selector)
			|| (item.type == Type::Input)
			|| (item.type == Type::Slider);
		if (keyed && item.key.isEmpty()) {
			luaL_error(L, "row %d: a %s needs a key", i, type.toUtf8().constData());
		} else if (item.type == Type::Selector && item.items.empty()) {
			luaL_error(L, "row %d: a selector needs items", i);
		} else if (item.type == Type::Custom
			&& (item.paint == kNoRef
				|| item.height <= 0
				|| item.height > kMaxCustomHeight)) {
			luaL_error(
				L,
				"row %d: a custom row needs paint and a height 1..%d",
				i,
				kMaxCustomHeight);
		} else if (item.type == Type::Slider && !(item.max > item.min)) {
			luaL_error(L, "row %d: a slider needs max > min", i);
		} else if (item.text.isEmpty()
			&& item.type != Type::Divider
			&& item.type != Type::Custom) {
			luaL_error(L, "row %d has no text", i);
		}
		page.items.push_back(std::move(item));
		lua_pop(L, 1);
	}
}

std::optional<Page> Plugin::buildPage(int ref, QString *error) {
	const auto fail = [&](const QString &text) {
		if (error) {
			*error = text;
		}
		return std::optional<Page>();
	};
	if (!_lua) {
		return fail(u"the plugin is not running"_q);
	}
	const auto builder = (ref == kNoRef) ? _settingsPage : ref;
	if (builder == kNoRef) {
		return fail(u"the plugin has no settings page"_q);
	} else if (ref != kNoRef && !_pageBuilders.contains(ref)) {
		return fail(u"the page is gone"_q);
	}
	auto page = Page{ .generation = _generation };
	lua_rawgeti(_lua, LUA_REGISTRYINDEX, builder);
	if (!call(0, 1, kClickBudget)) {
		return fail(_lastError);
	}
	auto readError = QString();
	const auto read = protect(1, 0, [&](lua_State *L) {
		readPage(L, 1, page);
	}, &readError);
	if (!read) {
		if (_lua) {
			for (const auto handler : page.refs) {
				luaL_unref(_lua, LUA_REGISTRYINDEX, handler);
			}
		}
		addToJournal(u"ERROR: page: "_q + readError);
		return fail(readError);
	}
	return page;
}

void Plugin::releasePage(const Page &page) {
	if (!_lua || page.generation != _generation) {
		return;
	}
	for (const auto handler : page.refs) {
		luaL_unref(_lua, LUA_REGISTRYINDEX, handler);
	}
}

void Plugin::pageCallback(
		int generation,
		int ref,
		const QJsonValue &value) {
	if (!_lua || generation != _generation || ref == kNoRef) {
		return;
	}
	lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
	if (value.isUndefined()) {
		call(0, 0, kClickBudget);
	} else {
		auto pushed = protect(0, 1, [&](lua_State *L) {
			PushJson(L, value, 0);
		});
		if (!pushed) {
			lua_pop(_lua, 1);
			return;
		}
		call(1, 0, kClickBudget);
	}
}

void Plugin::paintCanvas(
		int generation,
		int ref,
		QPainter &p,
		QSize size) {
	if (!_lua || generation != _generation || ref == kNoRef || _running) {
		return;
	}
	lua_rawgeti(_lua, LUA_REGISTRYINDEX, ref);
	PushCanvas(_lua, &p, size, &_imageLookup);
	lua_pushvalue(_lua, -1);
	lua_insert(_lua, -3); // canvas, function, canvas
	p.save();
	call(1, 0, kPaintBudget);
	p.restore();
	if (_lua) {
		InvalidateCanvas(_lua, -1);
		lua_pop(_lua, 1);
	}
}

// tg.ui.dialog{ title, text, items = { ... }, buttons = { ... }, danger }
// -> the number of the chosen item or button, nil if closed.
int Plugin::ApiDialog(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	luaL_checktype(L, 1, LUA_TTABLE);
	const auto title = StringField(L, 1, "title");
	const auto text = StringField(L, 1, "text");
	const auto list = [&](const char *name) {
		auto result = std::vector<QString>();
		lua_getfield(L, 1, name);
		if (lua_istable(L, -1)) {
			const auto size = std::min(int(lua_rawlen(L, -1)), kMaxPageRows);
			for (auto i = 1; i <= size; ++i) {
				lua_rawgeti(L, -1, i);
				result.push_back(ToString(L, -1));
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		return result;
	};
	const auto items = list("items");
	auto buttons = list("buttons");
	if (buttons.size() > 3) {
		return luaL_error(L, "a dialog has up to 3 buttons");
	}
	lua_getfield(L, 1, "danger");
	const auto danger = lua_isinteger(L, -1) ? int(lua_tointeger(L, -1)) : 0;
	lua_pop(L, 1);
	const auto waiter = plugin->waiter(L, 2, "tg.ui.dialog");
	const auto weak = base::make_weak(plugin);
	crl::on_main(plugin, [=] {
		const auto answered = std::make_shared<bool>(false);
		const auto finish = [=](int value) {
			if (*answered) {
				return;
			}
			*answered = true;
			if (const auto strong = weak.get()) {
				strong->answer(waiter, [=](lua_State *L) {
					if (value > 0) {
						lua_pushinteger(L, value);
					} else {
						lua_pushnil(L);
					}
				});
			}
		};
		const auto controller = ActiveController();
		if (!controller) {
			finish(0);
			return;
		}
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			if (!title.isEmpty()) {
				box->setTitle(rpl::single(title));
			}
			if (!text.isEmpty()) {
				box->addRow(
					object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
			}
			for (auto i = 0; i != int(items.size()); ++i) {
				const auto button = box->addRow(
					object_ptr<Ui::SettingsButton>(
						box,
						rpl::single(items[i]),
						st::defaultSettingsButton),
					style::margins());
				button->setClickedCallback([=] {
					finish(i + 1);
					box->closeBox();
				});
			}
			const auto shown = buttons.empty()
				? std::vector<QString>{ items.empty()
					? tr::lng_box_ok(tr::now)
					: tr::lng_cancel(tr::now) }
				: buttons;
			for (auto i = 0; i != int(shown.size()); ++i) {
				const auto chosen = buttons.empty() ? 0 : (i + 1);
				const auto clicked = [=] {
					finish(chosen);
					box->closeBox();
				};
				if (chosen && chosen == danger) {
					box->addButton(
						rpl::single(shown[i]),
						clicked,
						st::attentionBoxButton);
				} else {
					box->addButton(rpl::single(shown[i]), clicked);
				}
			}
			box->boxClosing() | rpl::on_next([=] {
				finish(0);
			}, box->lifetime());
		}));
	});
	return plugin->wait(L, waiter);
}

// tg.ui.current() -> { screen, peer, title, window }
int Plugin::ApiCurrent(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermChatsRead);
	lua_createtable(L, 0, 4);
	const auto window = Core::App().activeWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (window) {
		PushString(L, window->widget()->windowTitle());
		lua_setfield(L, -2, "window");
	}
	const auto peer = controller
		? controller->activeChatCurrent().peer()
		: nullptr;
	PushString(L, !controller
		? u"none"_q
		: peer
		? u"chat"_q
		: u"chats"_q);
	lua_setfield(L, -2, "screen");
	if (peer) {
		lua_pushinteger(L, BotApiId(peer->id));
		lua_setfield(L, -2, "peer");
		PushString(L, peer->name());
		lua_setfield(L, -2, "title");
	}
	return 1;
}

// tg.ui.open(chat id | "@username" | "https://t.me/..." | "tg://...")
int Plugin::ApiOpen(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	if (lua_isinteger(L, 1)) {
		const auto peerId = FromBotApiId(int64(lua_tointeger(L, 1)));
		crl::on_main(plugin, [=] {
			if (const auto controller = ActiveController()) {
				if (const auto peer = controller->session().data().peerLoaded(peerId)) {
					controller->showPeerHistory(peer);
				}
			}
		});
		return 0;
	}
	const auto target = CheckString(L, 1).trimmed();
	crl::on_main(plugin, [=] {
		if (target.startsWith(u'@')) {
			if (const auto controller = ActiveController()) {
				controller->showPeerByLink(Window::PeerByLinkInfo{
					.usernameOrId = target.mid(1),
				});
			}
		} else if (!target.isEmpty()) {
			UrlClickHandler::Open(target);
		}
	});
	return 0;
}

// tg.ui.color("windowBg") -> "#aarrggbb" of the current theme
int Plugin::ApiColor(lua_State *L) {
	const auto name = CheckString(L, 1);
	if (const auto color = ThemeColor(name)) {
		PushString(L, color->name(QColor::HexArgb));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// tg.ui.image(bytes) -> id, width, height (PNG, JPEG, WebP, GIF frame)
int Plugin::ApiImage(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	auto size = size_t(0);
	const auto data = luaL_checklstring(L, 1, &size);
	if (int(plugin->_images.size()) >= kMaxImages) {
		return luaL_error(L, "a plugin keeps up to %d images", kMaxImages);
	}
	auto image = QImage::fromData(QByteArray(data, int(size)));
	if (image.isNull()) {
		lua_pushnil(L);
		return 1;
	}
	if (image.width() > kMaxImageSide || image.height() > kMaxImageSide) {
		image = image.scaled(
			kMaxImageSide,
			kMaxImageSide,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	image = std::move(image).convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	const auto id = ++plugin->_nextImageId;
	lua_pushinteger(L, id);
	lua_pushinteger(L, image.width());
	lua_pushinteger(L, image.height());
	plugin->_images.emplace(id, std::move(image));
	return 3;
}

int Plugin::ApiImageFree(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	plugin->_images.erase(int(luaL_checkinteger(L, 1)));
	return 0;
}

namespace {

[[nodiscard]] Qt::Corner ReadCorner(
		lua_State *L,
		int index,
		Qt::Corner fallback) {
	lua_getfield(L, index, "corner");
	const auto corner = ToString(L, -1);
	lua_pop(L, 1);
	if (corner == u"top-left"_q) {
		return Qt::TopLeftCorner;
	} else if (corner == u"top-right"_q) {
		return Qt::TopRightCorner;
	} else if (corner == u"bottom-left"_q) {
		return Qt::BottomLeftCorner;
	} else if (corner == u"bottom-right"_q) {
		return Qt::BottomRightCorner;
	}
	return fallback;
}

[[nodiscard]] QRect ReadGeometry(lua_State *L, int index, QRect fallback) {
	const auto field = [&](const char *name, int value) {
		lua_getfield(L, index, name);
		const auto result = lua_isnumber(L, -1)
			? int(lua_tointeger(L, -1))
			: value;
		lua_pop(L, 1);
		return result;
	};
	return QRect(
		field("x", fallback.x()),
		field("y", fallback.y()),
		std::clamp(field("width", fallback.width()), 1, kMaxOverlaySide),
		std::clamp(field("height", fallback.height()), 1, kMaxOverlaySide));
}

// The table of o:update() / o:move{...} / o:close(): the last argument.
[[nodiscard]] int OverlayId(lua_State *L) {
	return int(lua_tointeger(L, lua_upvalueindex(1)));
}

} // namespace

// tg.ui.overlay{ x, y, width, height, corner, paint, on_click }
// -> { id, update(), move{ x, y, width, height, corner }, close() }
int Plugin::ApiOverlay(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	luaL_checktype(L, 1, LUA_TTABLE);
	if (int(plugin->_overlays.size()) >= kMaxOverlays) {
		return luaL_error(L, "a plugin shows up to %d overlays", kMaxOverlays);
	}
	const auto window = Core::App().activeWindow();
	const auto body = window ? window->widget()->bodyWidget() : nullptr;
	if (!body) {
		lua_pushnil(L);
		return 1;
	}
	const auto geometry = ReadGeometry(L, 1, QRect(0, 0, 100, 40));
	const auto corner = ReadCorner(L, 1, Qt::TopLeftCorner);
	lua_getfield(L, 1, "paint");
	luaL_checktype(L, -1, LUA_TFUNCTION);
	const auto paint = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_getfield(L, 1, "on_click");
	const auto click = lua_isfunction(L, -1)
		? luaL_ref(L, LUA_REGISTRYINDEX)
		: (lua_pop(L, 1), kNoRef);

	const auto id = ++plugin->_nextOverlayId;
	const auto weak = base::make_weak(plugin);
	auto &state = plugin->_overlays[id];
	state.geometry = geometry;
	state.corner = corner;
	state.paint = paint;
	state.click = click;
	state.widget = CreateOverlay(body, {
		.geometry = geometry,
		.corner = corner,
		.clickThrough = (click == kNoRef),
		.paint = [=](QPainter &p, QSize size) {
			if (const auto strong = weak.get()) {
				strong->paintOverlay(id, p, size);
			}
		},
		.click = [=](QPoint point) {
			if (const auto strong = weak.get()) {
				strong->clickOverlay(id, point);
			}
		},
	});

	lua_createtable(L, 0, 4);
	lua_pushinteger(L, id);
	lua_setfield(L, -2, "id");
	const auto method = [&](const char *name, lua_CFunction function) {
		lua_pushinteger(L, id);
		lua_pushcclosure(L, function, 1);
		lua_setfield(L, -2, name);
	};
	method("update", &Plugin::OverlayUpdate);
	method("move", &Plugin::OverlayMove);
	method("close", &Plugin::OverlayClose);
	return 1;
}

int Plugin::OverlayUpdate(lua_State *L) {
	const auto plugin = From(L);
	const auto i = plugin->_overlays.find(OverlayId(L));
	if (i != end(plugin->_overlays) && i->second.widget) {
		i->second.widget->update();
	}
	return 0;
}

int Plugin::OverlayMove(lua_State *L) {
	const auto plugin = From(L);
	const auto i = plugin->_overlays.find(OverlayId(L));
	const auto index = lua_gettop(L);
	if (i == end(plugin->_overlays) || !lua_istable(L, index)) {
		return 0;
	}
	auto &state = i->second;
	state.geometry = ReadGeometry(L, index, state.geometry);
	state.corner = ReadCorner(L, index, state.corner);
	if (state.widget) {
		MoveOverlay(state.widget.get(), state.geometry, state.corner);
	}
	return 0;
}

int Plugin::OverlayClose(lua_State *L) {
	From(L)->closeOverlay(OverlayId(L));
	return 0;
}

void Plugin::closeOverlay(int id) {
	const auto i = _overlays.find(id);
	if (i == end(_overlays)) {
		return;
	}
	if (_lua) {
		luaL_unref(_lua, LUA_REGISTRYINDEX, i->second.paint);
		luaL_unref(_lua, LUA_REGISTRYINDEX, i->second.click);
	}
	// Not right now: it may be closed from its own click.
	if (const auto widget = i->second.widget.release()) {
		widget->hide();
		widget->deleteLater();
	}
	_overlays.erase(i);
}

void Plugin::paintOverlay(int id, QPainter &p, QSize size) {
	const auto i = _overlays.find(id);
	if (i != end(_overlays)) {
		paintCanvas(_generation, i->second.paint, p, size);
	}
}

void Plugin::clickOverlay(int id, QPoint point) {
	const auto i = _overlays.find(id);
	if (!_lua || i == end(_overlays) || i->second.click == kNoRef) {
		return;
	}
	lua_rawgeti(_lua, LUA_REGISTRYINDEX, i->second.click);
	lua_pushinteger(_lua, point.x());
	lua_pushinteger(_lua, point.y());
	call(2, 0, kClickBudget);
}

// tg.messages.send_file(peer, { bytes, name, caption, photo, silent })
int Plugin::ApiSendFile(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermMessagesSend);
	const auto peerId = FromBotApiId(int64(luaL_checkinteger(L, 1)));
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_getfield(L, 2, "bytes");
	auto size = size_t(0);
	const auto data = luaL_checklstring(L, -1, &size);
	if (size == 0 || qint64(size) > kMaxFile) {
		return luaL_error(L, "a file to send is 1 byte to 16 MB");
	}
	const auto bytes = QByteArray(data, int(size));
	lua_pop(L, 1);
	auto name = StringField(L, 2, "name");
	name.replace(QRegularExpression(u"[^A-Za-z0-9_.\\- ]"_q), u"_"_q);
	if (name.isEmpty() || name.startsWith(u'.')) {
		name = u"file"_q + name;
	}
	const auto caption = StringField(L, 2, "caption");
	lua_getfield(L, 2, "photo");
	const auto photo = lua_toboolean(L, -1) != 0;
	lua_getfield(L, 2, "silent");
	const auto silent = lua_toboolean(L, -1) != 0;
	lua_pop(L, 2);

	const auto session = ActiveSession();
	if (!peerId || !session) {
		lua_pushboolean(L, 0);
		return 1;
	}
	// A folder per file: two files of one name do not meet.
	const auto folder = plugin->_dataPath
		+ u"outbox/"_q
		+ QString::number(++plugin->_nextImageId)
		+ u'_'
		+ QString::number(crl::now())
		+ u'/';
	QDir().mkpath(folder);
	const auto path = folder + name;
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
		lua_pushboolean(L, 0);
		return 1;
	}
	file.close();
	crl::on_main(session, [=] {
		auto list = Storage::PrepareMediaList(
			QStringList{ path },
			st::sendMediaPreviewSize,
			session->premium());
		if (list.error != Ui::PreparedList::Error::None
			|| list.files.empty()) {
			LOG(("Plugins: could not send %1").arg(path));
			return;
		}
		if (!caption.isEmpty()) {
			list.files.front().caption = TextWithTags{ caption };
		}
		const auto history = session->data().history(peerId);
		auto action = Api::SendAction(history);
		action.options.silent = silent;
		const auto bypass = Plugins::BypassScope();
		session->api().sendFiles(
			std::move(list),
			(photo ? SendMediaType::Photo : SendMediaType::File),
			nullptr,
			action);
	});
	lua_pushboolean(L, 1);
	return 1;
}

namespace {

void DumpValue(
		lua_State *L,
		int index,
		int depth,
		QString &out,
		base::flat_set<const void*> &seen) {
	index = lua_absindex(L, index);
	switch (lua_type(L, index)) {
	case LUA_TNIL: out += u"nil"_q; return;
	case LUA_TBOOLEAN:
		out += lua_toboolean(L, index) ? u"true"_q : u"false"_q;
		return;
	case LUA_TNUMBER:
		out += lua_isinteger(L, index)
			? QString::number(lua_tointeger(L, index))
			: QString::number(lua_tonumber(L, index), 'g', 14);
		return;
	case LUA_TSTRING: {
		auto text = ToString(L, index);
		if (text.size() > 200) {
			text = text.left(200) + u"…"_q;
		}
		out += u'"' + text.replace(u'"', u"\\\""_q) + u'"';
		return;
	}
	case LUA_TTABLE: break;
	default:
		out += QString::fromLatin1(luaL_typename(L, index));
		return;
	}
	const auto pointer = lua_topointer(L, index);
	if (!seen.emplace(pointer).second) {
		out += u"<cycle>"_q;
		return;
	} else if (depth >= kMaxDumpDepth) {
		out += u"{…}"_q;
		return;
	}
	const auto indent = QString(depth + 1, u'\t');
	out += u"{\n"_q;
	auto shown = 0;
	lua_pushnil(L);
	while (lua_next(L, index)) {
		if (++shown > kMaxDumpItems) {
			out += indent + u"…\n"_q;
			lua_pop(L, 2);
			break;
		}
		out += indent;
		if (lua_type(L, -2) == LUA_TSTRING) {
			out += ToString(L, -2);
		} else {
			out += u'[';
			DumpValue(L, -2, depth + 1, out, seen);
			out += u']';
		}
		out += u" = "_q;
		DumpValue(L, -1, depth + 1, out, seen);
		out += u",\n"_q;
		lua_pop(L, 1);
	}
	out += QString(depth, u'\t') + u'}';
}

} // namespace

// tg.dump(value) -> readable text of a value, tables with their fields.
int Plugin::ApiDump(lua_State *L) {
	luaL_checkany(L, 1);
	luaL_checkstack(L, kMaxDumpDepth * 3 + 8, "tg.dump");
	auto out = QString();
	auto seen = base::flat_set<const void*>();
	DumpValue(L, 1, 0, out, seen);
	PushString(L, out);
	return 1;
}

// tg.now() -> milliseconds of a steady clock, for animations.
int Plugin::ApiNow(lua_State *L) {
	lua_pushinteger(L, lua_Integer(crl::now()));
	return 1;
}

// tg.ui.window_size() -> width, height of the window body in unscaled
// pixels, nil without a window.
int Plugin::ApiWindowSize(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermUi);
	const auto window = Core::App().activeWindow();
	const auto body = window ? window->widget()->bodyWidget() : nullptr;
	if (!body) {
		lua_pushnil(L);
		return 1;
	}
	const auto size = body->size();
	const auto scale = style::Scale() / 100.;
	lua_pushinteger(L, lua_Integer(std::round(size.width() / scale)));
	lua_pushinteger(L, lua_Integer(std::round(size.height() / scale)));
	return 2;
}

namespace {

using PlayerType = AudioMsgId::Type;

// The music, or a voice message if no music plays.
[[nodiscard]] PlayerType ActivePlayerType() {
	const auto player = Media::Player::instance();
	const auto song = player->getState(PlayerType::Song);
	if (song.id.audio()
		&& !Media::Player::IsStoppedOrStopping(song.state)) {
		return PlayerType::Song;
	}
	const auto voice = player->getState(PlayerType::Voice);
	if (voice.id.audio()
		&& !Media::Player::IsStoppedOrStopping(voice.state)) {
		return PlayerType::Voice;
	}
	return PlayerType::Song;
}

} // namespace

// tg.player.state() -> { title, performer, playing, position, duration,
// voice, peer, message, has_next, has_previous } or nil
int Plugin::ApiPlayerState(lua_State *L) {
	const auto plugin = From(L);
	plugin->requirePermission(L, kPermPlayer);
	const auto player = Media::Player::instance();
	const auto type = ActivePlayerType();
	const auto state = player->getState(type);
	const auto document = state.id.audio();
	if (!document || Media::Player::IsStoppedOrStopping(state.state)) {
		lua_pushnil(L);
		return 1;
	}
	const auto song = document->song();
	const auto title = (song && !song->title.isEmpty())
		? song->title
		: document->filename();
	const auto frequency = std::max(state.frequency, 1);
	const auto position = double(state.position) / frequency;
	const auto duration = (state.length > 0)
		? (double(state.length) / frequency)
		: (document->duration() / 1000.);
	lua_createtable(L, 0, 10);
	PushString(L, title);
	lua_setfield(L, -2, "title");
	PushString(L, song ? song->performer : QString());
	lua_setfield(L, -2, "performer");
	lua_pushboolean(L, Media::Player::ShowPauseIcon(state.state) ? 1 : 0);
	lua_setfield(L, -2, "playing");
	lua_pushnumber(L, position);
	lua_setfield(L, -2, "position");
	lua_pushnumber(L, duration);
	lua_setfield(L, -2, "duration");
	lua_pushboolean(L, (type == PlayerType::Voice) ? 1 : 0);
	lua_setfield(L, -2, "voice");
	lua_pushboolean(L, player->nextAvailable(type) ? 1 : 0);
	lua_setfield(L, -2, "has_next");
	lua_pushboolean(L, player->previousAvailable(type) ? 1 : 0);
	lua_setfield(L, -2, "has_previous");
	const auto context = state.id.contextId();
	if (context.peer) {
		lua_pushinteger(L, BotApiId(context.peer));
		lua_setfield(L, -2, "peer");
		lua_pushinteger(L, lua_Integer(context.msg.bare));
		lua_setfield(L, -2, "message");
	}
	return 1;
}

namespace {

// Player buttons run on the next event loop turn, never inside a hook.
int PlayerAction(lua_State *L, Fn<void(not_null<Media::Player::Instance*>, PlayerType)> action) {
	const auto type = ActivePlayerType();
	crl::on_main([=] {
		action(Media::Player::instance(), type);
	});
	return 0;
}

} // namespace

int Plugin::ApiPlayerToggle(lua_State *L) {
	From(L)->requirePermission(L, kPermPlayer);
	return PlayerAction(L, [](auto player, auto type) {
		player->playPause(type);
	});
}

int Plugin::ApiPlayerPlay(lua_State *L) {
	From(L)->requirePermission(L, kPermPlayer);
	return PlayerAction(L, [](auto player, auto type) {
		player->play(type);
	});
}

int Plugin::ApiPlayerPause(lua_State *L) {
	From(L)->requirePermission(L, kPermPlayer);
	return PlayerAction(L, [](auto player, auto type) {
		player->pause(type);
	});
}

int Plugin::ApiPlayerNext(lua_State *L) {
	From(L)->requirePermission(L, kPermPlayer);
	return PlayerAction(L, [](auto player, auto type) {
		player->next(type);
	});
}

int Plugin::ApiPlayerPrevious(lua_State *L) {
	From(L)->requirePermission(L, kPermPlayer);
	return PlayerAction(L, [](auto player, auto type) {
		player->previous(type);
	});
}

int Plugin::ApiPlayerStop(lua_State *L) {
	From(L)->requirePermission(L, kPermPlayer);
	return PlayerAction(L, [](auto player, auto type) {
		player->stop(type);
	});
}

// tg.player.seek(fraction): 0 is the start, 1 the end.
int Plugin::ApiPlayerSeek(lua_State *L) {
	From(L)->requirePermission(L, kPermPlayer);
	const auto progress = std::clamp(
		double(luaL_checknumber(L, 1)),
		0.,
		1.);
	return PlayerAction(L, [=](auto player, auto type) {
		player->startSeeking(type);
		player->finishSeeking(type, progress);
	});
}

} // namespace Plugins::Flua
