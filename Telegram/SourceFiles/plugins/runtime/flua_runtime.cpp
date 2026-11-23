/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_runtime.h"

#include "base/call_delayed.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "plugins/runtime/flua_package.h"
#include "plugins/runtime/flua_plugin.h"
#include "plugins/runtime/flua_tl.h"
#include "plugins/ui/plugins_manager.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

namespace Plugins::Flua {
namespace {

std::unique_ptr<Host> Instance;

[[nodiscard]] MenuPlace PlaceFor(PeerMenuPlace place) {
	return (place == PeerMenuPlace::Profile)
		? MenuPlace::Profile
		: MenuPlace::Chat;
}

[[nodiscard]] QString StateName(AppState state) {
	switch (state) {
	case AppState::Started: return u"started"_q;
	case AppState::Active: return u"active"_q;
	case AppState::Inactive: return u"inactive"_q;
	case AppState::Stopping: return u"stopping"_q;
	}
	return QString();
}

[[nodiscard]] Window::SessionController *ActiveController() {
	const auto window = Core::App().activeWindow();
	return window ? window->sessionController() : nullptr;
}

void Toast(const QString &text) {
	if (const auto controller = ActiveController()) {
		controller->showToast(text);
	}
}

[[nodiscard]] bool Russian() {
	return Lang::GetInstance().id().startsWith(u"ru"_q);
}

} // namespace

QString PermissionText(const QString &permission) {
	const auto is = [&](const char *name) {
		return permission == QString::fromLatin1(name);
	};
	const auto ru = Russian();
	const auto pick = [&](const char *en, const char *russian) {
		return QString::fromUtf8(ru ? russian : en);
	};
	if (const auto host = NetHostPattern(permission)) {
		return pick("connect to ", "подключаться к ") + *host;
	} else if (is(kPermMessagesRead)) {
		return pick("read new messages", "читать новые сообщения");
	} else if (is(kPermMessagesSend)) {
		return pick(
			"send messages and change what you send",
			"отправлять сообщения и менять то, что вы отправляете");
	} else if (is(kPermUi)) {
		return pick(
			"add menu items, rows and notices",
			"добавлять пункты меню, строки и уведомления");
	} else if (is(kPermStorage)) {
		return pick("keep its own settings", "хранить свои настройки");
	} else if (is(kPermLinks)) {
		return pick(
			"handle links you open",
			"обрабатывать ссылки, которые вы открываете");
	} else if (is(kPermActions)) {
		return pick(
			"see and cancel actions: replies, reactions, calls...",
			"видеть и отменять действия: ответы, реакции, звонки...");
	} else if (is(kPermDisplay)) {
		return pick(
			"change how messages and chat names are shown",
			"менять, как показаны сообщения и названия чатов");
	} else if (is(kPermChatsRead)) {
		return pick(
			"see chat names and which chat or profile is open",
			"видеть названия чатов и какой чат или профиль открыт");
	} else if (is(kPermCompose)) {
		return pick(
			"read and replace the text you type",
			"читать и заменять набираемый текст");
	} else if (is(kPermMessagesManage)) {
		return pick(
			"edit and delete messages",
			"редактировать и удалять сообщения");
	} else if (is(kPermMtproto)) {
		return pick(
			"⚠ FULL ACCESS TO YOUR ACCOUNT: send any request to Telegram, "
			"read and change everything the client sends and receives",
			"⚠ ПОЛНЫЙ ДОСТУП К АККАУНТУ: любые запросы к Telegram, "
			"чтение и подмена всего, что клиент отправляет и получает");
	}
	return permission;
}

namespace {

// Our keys are not in the cloud language packs.
void ApplyTranslations() {
	if (!Russian()) {
		return;
	}
	const auto set = [](const char *key, const char *value) {
		Lang::GetInstance().resetValue(QByteArray(key));
		Lang::GetInstance().applyValue(QByteArray(key), QByteArray(value));
	};
	set("fg_plugins", "Плагины");
	set("fg_plugins_enabled", "Включён");
	set("fg_plugins_running", "работает");
	set("fg_plugins_stopped", "остановлен");
	set("fg_plugins_disabled", "выключен");
	set("fg_plugins_not_loaded", "Не загружены:");
	set("fg_plugins_empty", "Плагинов пока нет. Отправьте файл .flua в любой чат и откройте его или положите плагины в папку:");
	set("fg_plugins_open_folder", "Открыть папку");
	set("fg_plugins_install_title", "Установить плагин?");
	set("fg_plugins_install", "Установить");
	set("fg_plugins_installed", "Плагин установлен.");
	set("fg_plugins_install_failed", "Не удалось скопировать файл плагина.");
	set("fg_plugins_bad_package", "Это не плагин:");
	set("fg_plugins_dev_folder", "Уже загружена папка разработки с этим id плагина.");
}

// Exists while plugins start: found at the next launch, it means the
// client crashed then, and the safe mode turns on.
[[nodiscard]] QString StartingMarkPath() {
	return PluginsDataPath() + u"starting"_q;
}

[[nodiscard]] PluginView ViewOf(const Plugin &plugin) {
	const auto &manifest = plugin.manifest();
	return {
		.id = manifest.id,
		.name = manifest.name,
		.version = manifest.version,
		.author = manifest.author,
		.description = manifest.description,
		.permissions = manifest.permissions,
		.origin = plugin.origin(),
		.icon = plugin.icon(),
		.error = plugin.lastError(),
		.dev = QFileInfo(plugin.origin()).isDir(),
		.running = plugin.running(),
	};
}

} // namespace

QString PluginsPath() {
	return cWorkingDir() + u"plugins/"_q;
}

QString PluginsDataPath() {
	return cWorkingDir() + u"plugins_data/"_q;
}

QString Translated(const char *english, const char *russian) {
	return QString::fromUtf8(Russian() ? russian : english);
}

bool IsSensitiveMethod(const QString &name) {
	if (name.startsWith(u"auth."_q)) {
		return true;
	} else if (name.startsWith(u"payments."_q)) {
		// Reading is fine, everything that pays or moves gifts is not.
		const auto action = name.mid(9);
		return !action.startsWith(u"get"_q)
			&& !action.startsWith(u"check"_q)
			&& !action.startsWith(u"validate"_q)
			&& !action.startsWith(u"can"_q);
	}
	static const auto list = base::flat_set<QString>{
		u"messages.sendPaidReaction"_q,
		u"channels.editCreator"_q,
		u"account.updatePasswordSettings"_q,
		u"account.getPasswordSettings"_q,
		u"account.resetPassword"_q,
		u"account.declinePasswordReset"_q,
		u"account.confirmPasswordEmail"_q,
		u"account.resendPasswordEmail"_q,
		u"account.cancelPasswordEmail"_q,
		u"account.getTmpPassword"_q,
		u"account.resetAuthorization"_q,
		u"account.resetWebAuthorization"_q,
		u"account.resetWebAuthorizations"_q,
		u"account.changeAuthorizationSettings"_q,
		u"account.setAuthorizationTTL"_q,
		u"account.sendChangePhoneCode"_q,
		u"account.changePhone"_q,
		u"account.deleteAccount"_q,
		u"account.setAccountTTL"_q,
		u"account.sendVerifyEmailCode"_q,
		u"account.verifyEmail"_q,
		u"account.invalidateSignInCodes"_q,
		u"account.updateDeviceLocked"_q,
		u"account.getAuthorizationForm"_q,
		u"account.acceptAuthorization"_q,
		u"account.getAllSecureValues"_q,
		u"account.getSecureValue"_q,
		u"account.saveSecureValue"_q,
		u"account.deleteSecureValue"_q,
	};
	return list.contains(name);
}

Host *CurrentHost() {
	return Instance.get();
}

void Start() {
	if (Instance) {
		return;
	}
	ApplyTranslations();
	Instance = std::make_unique<Host>();
	Instance->start();
}

void Stop() {
	if (!Instance) {
		return;
	}
	SetSink(nullptr);
	Instance->stop();
	Instance = nullptr;
}

namespace {

// Changes when a file in the folder is added, removed or saved.
[[nodiscard]] QString FolderStamp(const QString &folder) {
	auto count = 0;
	auto newest = qint64(0);
	const auto root = QDir(folder);
	// The same files ReadPackageFolder() reads.
	auto it = QDirIterator(
		folder,
		QDir::Files | QDir::NoSymLinks | QDir::Hidden,
		QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const auto relative = root.relativeFilePath(it.next());
		if (relative.startsWith(u'.') || relative.contains(u"/."_q)) {
			continue;
		}
		++count;
		newest = std::max(
			newest,
			it.fileInfo().lastModified().toMSecsSinceEpoch());
	}
	return QString::number(count) + u':' + QString::number(newest);
}

} // namespace

Host::Host()
: _devTimer([=] { checkDevFolders(); }) {
}

Host::~Host() {
	stop();
}

void Host::start() {
	loadDisabled();
	loadEngine();
	QDir().mkpath(PluginsDataPath());
	const auto crashed = QFile::exists(StartingMarkPath());
	// A mark of an earlier start must not count again later.
	QFile::remove(StartingMarkPath());
	if (crashed && mayRun()) {
		LOG(("Plugins: the last start did not finish, safe mode is on"));
		_engine.safeMode = true;
		saveEngine();
		base::call_delayed(3000, this, [] {
			Toast(Translated(
				"The client closed while plugins were starting. "
				"Safe mode is on: plugins do not run.",
				"Клиент закрылся, пока запускались плагины. "
				"Включён безопасный режим: плагины не работают."));
		});
	}
	if (mayRun()) {
		auto mark = QFile(StartingMarkPath());
		if (mark.open(QIODevice::WriteOnly)) {
			mark.close();
		}
		// Plugin code also runs right after the start, on first events.
		base::call_delayed(10000, this, [] {
			QFile::remove(StartingMarkPath());
		});
	}
	const auto root = PluginsPath();
	QDir().mkpath(root);
	const auto entries = QDir(root).entryInfoList(
		QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
		QDir::Name);
	for (const auto &entry : entries) {
		if (entry.isDir()
			|| entry.suffix().compare(u"flua"_q, Qt::CaseInsensitive) == 0) {
			load(entry.absoluteFilePath());
		}
	}
	SetSink(this);
	refreshHooks();
	if (_engine.devMode) {
		watchDevFolders();
	}
	LOG(("Plugins: %1 loaded, %2 invalid, from %3"
		).arg(_plugins.size()
		).arg(_invalid.size()
		).arg(root));
}

bool Host::mayRun() const {
	return _engine.enabled && !_engine.safeMode;
}

void Host::startAll() {
	for (const auto &plugin : _plugins) {
		if (!_disabled.contains(plugin->manifest().id)) {
			plugin->start();
		}
	}
	refreshHooks();
	Plugins::RefreshDisplayText();
}

void Host::stopAll() {
	for (const auto &plugin : _plugins) {
		plugin->stop();
	}
	refreshHooks();
	Plugins::RefreshDisplayText();
}

void Host::loadEngine() {
	_engine = EngineSettings();
	auto file = QFile(PluginsDataPath() + u"engine.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto json = QJsonDocument::fromJson(file.readAll()).object();
	const auto read = [&](const char *key, bool &value) {
		const auto stored = json.value(QString::fromLatin1(key));
		if (stored.isBool()) {
			value = stored.toBool();
		}
	};
	read("enabled", _engine.enabled);
	read("dev_mode", _engine.devMode);
	read("compact", _engine.compact);
	read("unknown_sources", _engine.unknownSources);
	read("confirm_sensitive", _engine.confirmSensitive);
	read("safe_mode", _engine.safeMode);
}

void Host::saveEngine() {
	auto json = QJsonObject();
	json.insert(u"enabled"_q, _engine.enabled);
	json.insert(u"dev_mode"_q, _engine.devMode);
	json.insert(u"compact"_q, _engine.compact);
	json.insert(u"unknown_sources"_q, _engine.unknownSources);
	json.insert(u"confirm_sensitive"_q, _engine.confirmSensitive);
	json.insert(u"safe_mode"_q, _engine.safeMode);
	QDir().mkpath(PluginsDataPath());
	auto file = QFile(PluginsDataPath() + u"engine.json"_q);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
	}
}

const EngineSettings &Host::engine() const {
	return _engine;
}

void Host::setEngine(const EngineSettings &settings) {
	if (_engine == settings) {
		return;
	}
	const auto couldRun = mayRun();
	const auto hadDevMode = _engine.devMode;
	_engine = settings;
	saveEngine();
	if (couldRun != mayRun()) {
		if (mayRun()) {
			startAll();
		} else {
			stopAll();
		}
	}
	if (hadDevMode != _engine.devMode) {
		if (_engine.devMode) {
			watchDevFolders();
		} else {
			_devTimer.cancel();
			_devStamps.clear();
		}
	}
	notifyChanged();
}

rpl::producer<> Host::changes() const {
	return _changes.events();
}

void Host::notifyChanged() {
	// Many plugins start or stop at once: one update for all of them.
	if (_changeQueued) {
		return;
	}
	_changeQueued = true;
	crl::on_main(this, [=] {
		_changeQueued = false;
		_changes.fire({});
	});
}

std::vector<PluginView> Host::pluginViews() const {
	auto result = std::vector<PluginView>();
	result.reserve(_plugins.size());
	for (const auto &plugin : _plugins) {
		auto view = ViewOf(*plugin);
		view.enabled = !_disabled.contains(view.id);
		result.push_back(std::move(view));
	}
	return result;
}

std::vector<InvalidView> Host::invalidViews() const {
	auto result = std::vector<InvalidView>();
	result.reserve(_invalid.size());
	for (const auto &entry : _invalid) {
		result.push_back({ .origin = entry.origin, .error = entry.error });
	}
	return result;
}

std::deque<QString> Host::journal(const QString &id) const {
	for (const auto &plugin : _plugins) {
		if (plugin->manifest().id == id) {
			return plugin->journal();
		}
	}
	return {};
}

void Host::removePlugin(const QString &id) {
	for (auto i = begin(_plugins); i != end(_plugins); ++i) {
		if ((*i)->manifest().id != id) {
			continue;
		}
		const auto origin = (*i)->origin();
		if (QFileInfo(origin).isDir()) {
			return; // The sources of a developer are not ours to delete.
		}
		const auto display = (*i)->handles(Event::Display);
		(*i)->stop();
		_plugins.erase(i);
		QFile::remove(origin);
		QDir(PluginsDataPath() + id).removeRecursively();
		_disabled.remove(id);
		saveDisabled();
		refreshHooks();
		if (display) {
			Plugins::RefreshDisplayText();
		}
		notifyChanged();
		return;
	}
}

QString Host::sharePath(const QString &id) {
	for (const auto &plugin : _plugins) {
		if (plugin->manifest().id != id) {
			continue;
		}
		const auto origin = plugin->origin();
		if (!QFileInfo(origin).isDir()) {
			return origin;
		}
		const auto folder = PluginsDataPath() + u"share/"_q;
		QDir().mkpath(folder);
		const auto path = folder + id + u".flua"_q;
		return WritePackageFile(plugin->files(), path) ? path : QString();
	}
	return QString();
}

void Host::confirmSensitive(
		const QString &pluginName,
		const QString &method,
		Fn<void(bool allowed)> done) {
	const auto controller = ActiveController();
	if (!controller) {
		done(false);
		return;
	}
	const auto what = (method.startsWith(u"payments."_q)
		|| method.startsWith(u"messages.sendPaid"_q))
		? Translated(
			"spend Stars, pay or move gifts",
			"потратить звёзды, заплатить или передать подарки")
		: method.startsWith(u"channels.editCreator"_q)
		? Translated(
			"give your channel or group to another owner",
			"передать ваш канал или группу другому владельцу")
		: (method.contains(u"SecureValue"_q)
			|| method == u"account.getAuthorizationForm"_q
			|| method == u"account.acceptAuthorization"_q)
		? Translated(
			"see or share your Telegram Passport data",
			"посмотреть или передать данные Telegram Passport")
		: Translated(
			"get to the sign in, sessions or security of your account",
			"получить доступ ко входу, сеансам или защите аккаунта");
	const auto text = Translated(
		"The plugin \"%1\" wants to %2 (%3).\n\n"
		"Allow it only if you asked the plugin to do this right now.",
		"Плагин «%1» хочет %2 (%3).\n\n"
		"Разрешайте, только если сами только что попросили "
		"плагин об этом.").arg(pluginName, what, method);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto answered = std::make_shared<bool>(false);
		const auto answer = [=](bool allowed) {
			if (!*answered) {
				*answered = true;
				done(allowed);
			}
		};
		box->setTitle(rpl::single(Translated(
			"Sensitive action",
			"Важное действие")));
		box->addRow(object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
		box->addButton(rpl::single(Translated("Allow", "Разрешить")), [=] {
			answer(true);
			box->closeBox();
		}, st::attentionBoxButton);
		box->addButton(rpl::single(Translated("Deny", "Запретить")), [=] {
			box->closeBox();
		});
		box->boxClosing() | rpl::on_next([=] {
			answer(false);
		}, box->lifetime());
	}));
}

void Host::load(const QString &path) {
	auto package = QFileInfo(path).isDir()
		? ReadPackageFolder(path)
		: ReadPackageFile(path);
	if (!package.error.isEmpty()) {
		LOG(("Plugins Error: %1: %2").arg(path, package.error));
		_invalid.push_back({ path, package.error });
		return;
	}
	const auto id = package.manifest.id;
	for (const auto &plugin : _plugins) {
		if (plugin->manifest().id == id) {
			_invalid.push_back({ path, u"duplicate id: "_q + id });
			return;
		}
	}
	const auto data = QString(PluginsDataPath() + id + u'/');
	_plugins.push_back(std::make_unique<Plugin>(
		this,
		std::move(package),
		data));
	if (!_disabled.contains(id) && mayRun()) {
		_plugins.back()->start();
	}
	notifyChanged();
}

void Host::watchDevFolders() {
	_devStamps.clear();
	const auto entries = QDir(PluginsPath()).entryInfoList(
		QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto &entry : entries) {
		const auto path = entry.absoluteFilePath();
		_devStamps.emplace(path, FolderStamp(path));
	}
	// Also notices folders added or removed later.
	_devTimer.callEach(1000);
}

void Host::checkDevFolders() {
	auto current = base::flat_set<QString>();
	const auto entries = QDir(PluginsPath()).entryInfoList(
		QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto &entry : entries) {
		current.emplace(entry.absoluteFilePath());
	}
	for (auto i = begin(_devStamps); i != end(_devStamps);) {
		if (current.contains(i->first)) {
			++i;
		} else {
			unload(i->first);
			i = _devStamps.erase(i);
		}
	}
	auto changed = std::vector<QString>();
	for (const auto &path : current) {
		auto now = FolderStamp(path);
		const auto i = _devStamps.find(path);
		if (i == end(_devStamps)) {
			_devStamps.emplace(path, std::move(now));
			changed.push_back(path);
		} else if (i->second != now) {
			i->second = std::move(now);
			changed.push_back(path);
		}
	}
	for (const auto &path : changed) {
		reload(path);
	}
}

void Host::unload(const QString &origin) {
	for (auto i = begin(_plugins); i != end(_plugins); ++i) {
		if ((*i)->origin() == origin) {
			(*i)->stop();
			_plugins.erase(i);
			break;
		}
	}
	_invalid.erase(
		ranges::remove(_invalid, origin, &Invalid::origin),
		end(_invalid));
	refreshHooks();
}

void Host::reload(const QString &origin) {
	auto name = QFileInfo(origin).fileName();
	auto display = false;
	for (const auto &plugin : _plugins) {
		if (plugin->origin() == origin) {
			name = plugin->manifest().name;
			display = plugin->handles(Event::Display);
		}
	}
	unload(origin);
	const auto invalid = _invalid.size();
	load(origin);
	refreshHooks();
	const auto loaded = (!_plugins.empty()
		&& _plugins.back()->origin() == origin)
		? _plugins.back().get()
		: nullptr;
	if (display || (loaded && loaded->handles(Event::Display))) {
		// Texts the old or the new handlers changed are shown again.
		Plugins::RefreshDisplayText();
	}
	const auto ru = Russian();
	if (_invalid.size() > invalid) {
		Toast((ru
			? QString::fromUtf8("Плагин не загружен: ")
			: u"Plugin not loaded: "_q) + _invalid.back().error);
	} else if (loaded && !loaded->lastError().isEmpty()) {
		Toast((ru
			? QString::fromUtf8("Ошибка плагина: ")
			: u"Plugin error: "_q) + loaded->lastError());
	} else {
		Toast((ru
			? QString::fromUtf8("Плагин перезагружен: ")
			: u"Plugin reloaded: "_q) + name);
	}
}

void Host::stop() {
	SetSink(nullptr);
	QFile::remove(StartingMarkPath());
	_devTimer.cancel();
	_devStamps.clear();
	for (const auto &plugin : _plugins) {
		plugin->stop();
	}
	_plugins.clear();
	_invalid.clear();
	SetSink(nullptr); // stop() above enabled hooks again
}

void Host::refreshHooks() {
	auto has = [&](Event event) {
		for (const auto &plugin : _plugins) {
			if (plugin->handles(event)) {
				return true;
			}
		}
		return false;
	};
	auto menu = [&](MenuPlace a, MenuPlace b) {
		for (const auto &plugin : _plugins) {
			if (!plugin->running()) {
				continue;
			}
			for (const auto &entry : plugin->menu()) {
				if (entry.place == a || entry.place == b) {
					return true;
				}
			}
		}
		return false;
	};
	SetEnabled(Hook::NewMessage, has(Event::MessageNew));
	SetEnabled(Hook::SendText, has(Event::MessageSend));
	SetEnabled(Hook::AppState, has(Event::AppState));
	SetEnabled(Hook::OpenUrl, has(Event::UrlOpen));
	SetEnabled(Hook::Screen, has(Event::Screen));
	SetEnabled(Hook::DisplayText, has(Event::Display));
	SetEnabled(Hook::ProfileRows, has(Event::ProfileRows));
	SetEnabled(Hook::MessageMenu, menu(MenuPlace::Message, MenuPlace::Message));
	SetEnabled(Hook::MainMenu, menu(MenuPlace::Main, MenuPlace::Main));
	SetEnabled(Hook::PeerMenu, menu(MenuPlace::Chat, MenuPlace::Profile));
	const auto requests = has(Event::MtprotoRequest);
	const auto responses = has(Event::MtprotoResponse);
	// Responses are decoded by the method of their request: watch those.
	SetEnabled(Hook::Request, requests || responses);
	SetEnabled(Hook::Response, responses);
	SetEnabled(Hook::Updates, has(Event::MtprotoUpdates));
	if (!responses) {
		_methods.clear();
	}
	// Always on: opening a .flua file offers to install it.
	SetEnabled(Hook::ClientEvent, true);
}

SendTextResult Host::sendText(const SendTextRequest &request) {
	auto text = request.text.text;
	auto modified = false;
	for (const auto &plugin : _plugins) {
		if (!plugin->handles(Event::MessageSend)) {
			continue;
		}
		auto verdict = plugin->messageSend(request.history->peer, text);
		if (verdict.cancel) {
			return { .verdict = Verdict::Cancel };
		} else if (verdict.text) {
			text = std::move(*verdict.text);
			modified = true;
		}
	}
	if (!modified) {
		return {};
	}
	return {
		.verdict = Verdict::Modify,
		.text = TextWithTags{ text },
	};
}

void Host::newMessage(not_null<HistoryItem*> item) {
	for (const auto &plugin : _plugins) {
		if (plugin->handles(Event::MessageNew)) {
			plugin->messageNew(item);
		}
	}
}

bool Host::openUrl(const QString &url, bool local) {
	for (const auto &plugin : _plugins) {
		if (plugin->handles(Event::UrlOpen) && plugin->urlOpen(url, local)) {
			return true;
		}
	}
	return false;
}

void Host::appState(AppState state) {
	const auto name = StateName(state);
	for (const auto &plugin : _plugins) {
		if (plugin->handles(Event::AppState)) {
			plugin->appState(name);
		}
	}
}

void Host::screen(Plugins::Screen screen, PeerData *peer) {
	for (const auto &plugin : _plugins) {
		if (plugin->handles(Event::Screen)) {
			plugin->screen(screen, peer);
		}
	}
}

bool Host::clientEvent(const ClientEventArgs &args) {
	if (args.event == ClientEvent::OpenFile
		&& args.path.endsWith(u".flua"_q, Qt::CaseInsensitive)) {
		offerInstall(args.path);
		return true;
	}
	for (const auto &plugin : _plugins) {
		if (plugin->handles(Event::Action) && plugin->action(args)) {
			return true;
		}
	}
	return false;
}

std::optional<QString> Host::displayText(
		DisplayTextPlace place,
		PeerData *peer,
		HistoryItem *item,
		const QString &text) {
	auto result = std::optional<QString>();
	for (const auto &plugin : _plugins) {
		if (!plugin->handles(Event::Display)) {
			continue;
		}
		if (auto replaced = plugin->display(
				place,
				peer,
				item,
				result ? *result : text)) {
			result = std::move(replaced);
		}
	}
	return result;
}

void Host::ownRequest(mtpRequestId requestId, mtpTypeId method) {
	_own.emplace(requestId, method);
}

void Host::forgetOwnRequest(mtpRequestId requestId) {
	_own.erase(requestId);
}

RawResult Host::request(const RawRequest &request) {
	if (_own.contains(request.requestId)) {
		return {};
	}
	auto method = Tl::MethodOf(request.body);
	auto body = std::optional<mtpBuffer>();
	for (const auto &plugin : _plugins) {
		if (!plugin->wants(Event::MtprotoRequest, method)) {
			continue;
		}
		auto result = plugin->mtprotoRequest(
			body ? *body : request.body,
			method);
		if (result.verdict == Verdict::Cancel) {
			return result;
		} else if (result.verdict == Verdict::Modify) {
			// A filter may stop a sensitive request, never write one:
			// no new recipient of a gift, no request turned into a payment.
			const auto modified = Tl::MethodOf(result.body);
			const auto was = QString::fromLatin1(Tl::Name(method));
			const auto now = QString::fromLatin1(Tl::Name(modified));
			if (_engine.confirmSensitive
				&& (IsSensitiveMethod(was) || IsSensitiveMethod(now))) {
				LOG(("Plugins: %1 may not change %2 into %3"
					).arg(plugin->manifest().id, was, now));
				continue;
			}
			body = std::move(result.body);
			method = modified;
		}
	}
	if (Has(Hook::Response)) {
		_methods[request.requestId] = method;
		// Requests that never get an answer must not pile up.
		while (_methods.size() > kMaxWatched) {
			_methods.erase(_methods.begin()); // the oldest id
		}
	}
	return body
		? RawResult{ .verdict = Verdict::Modify, .body = std::move(*body) }
		: RawResult();
}

std::optional<mtpBuffer> Host::response(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const mtpBuffer &reply) {
	if (_own.contains(requestId)) {
		return std::nullopt;
	}
	auto method = mtpTypeId(0);
	if (const auto i = _methods.find(requestId); i != end(_methods)) {
		method = i->second;
		// After an error the same request may be sent again.
		if (reply.isEmpty() || mtpTypeId(reply[0]) != Tl::kRpcError) {
			_methods.erase(i);
		}
	}
	auto result = std::optional<mtpBuffer>();
	for (const auto &plugin : _plugins) {
		if (!plugin->wants(Event::MtprotoResponse, method)) {
			continue;
		}
		if (auto replaced = plugin->mtprotoResponse(
				result ? *result : reply,
				method)) {
			result = std::move(replaced);
		}
	}
	return result;
}

RawResult Host::updates(
		not_null<MTP::Instance*> instance,
		const mtpBuffer &updates) {
	auto body = std::optional<mtpBuffer>();
	for (const auto &plugin : _plugins) {
		if (!plugin->handles(Event::MtprotoUpdates)) {
			continue;
		}
		auto result = plugin->mtprotoUpdates(body ? *body : updates);
		if (result.verdict == Verdict::Cancel) {
			return result;
		} else if (result.verdict == Verdict::Modify) {
			body = std::move(result.body);
		}
	}
	return body
		? RawResult{ .verdict = Verdict::Modify, .body = std::move(*body) }
		: RawResult();
}

void Host::profileRows(
		not_null<PeerData*> peer,
		std::vector<ProfileRow> &rows) {
	for (const auto &plugin : _plugins) {
		if (plugin->handles(Event::ProfileRows)) {
			plugin->profileRows(peer, rows);
		}
	}
}

void Host::messageMenu(
		not_null<HistoryItem*> item,
		std::vector<MenuItem> &items) {
	const auto peer = item->history()->peer;
	const auto msgId = item->id;
	for (const auto &plugin : _plugins) {
		if (!plugin->running()) {
			continue;
		}
		for (const auto &entry : plugin->menu()) {
			if (entry.place != MenuPlace::Message) {
				continue;
			}
			const auto weak = base::make_weak(plugin.get());
			const auto ref = entry.ref;
			items.push_back({
				.text = entry.text,
				.handler = [=] {
					if (const auto strong = weak.get()) {
						strong->menuClicked(
							ref,
							peer,
							peer->owner().message(peer->id, msgId));
					}
				},
			});
		}
	}
}

void Host::peerMenu(
		PeerMenuPlace place,
		not_null<PeerData*> peer,
		std::vector<MenuItem> &items) {
	const auto wanted = PlaceFor(place);
	for (const auto &plugin : _plugins) {
		if (!plugin->running()) {
			continue;
		}
		for (const auto &entry : plugin->menu()) {
			if (entry.place != wanted) {
				continue;
			}
			const auto weak = base::make_weak(plugin.get());
			const auto ref = entry.ref;
			const auto raw = peer.get();
			items.push_back({
				.text = entry.text,
				.handler = [=] {
					if (const auto strong = weak.get()) {
						strong->menuClicked(ref, raw, nullptr);
					}
				},
			});
		}
	}
}

void Host::mainMenu(std::vector<MenuItem> &items) {
	for (const auto &plugin : _plugins) {
		if (!plugin->running()) {
			continue;
		}
		for (const auto &entry : plugin->menu()) {
			if (entry.place != MenuPlace::Main) {
				continue;
			}
			const auto weak = base::make_weak(plugin.get());
			const auto ref = entry.ref;
			items.push_back({
				.text = entry.text,
				.handler = [=] {
					if (const auto strong = weak.get()) {
						strong->menuClicked(ref, nullptr, nullptr);
					}
				},
			});
		}
	}
}

void Host::setPluginEnabled(const QString &id, bool enabled) {
	for (const auto &plugin : _plugins) {
		if (plugin->manifest().id != id) {
			continue;
		}
		if (enabled) {
			_disabled.remove(id);
			if (mayRun() && !plugin->start()) {
				Toast(plugin->manifest().name
					+ u": "_q
					+ plugin->lastError());
			}
		} else {
			_disabled.emplace(id);
			plugin->stop();
		}
		saveDisabled();
		refreshHooks();
		Plugins::RefreshDisplayText();
		notifyChanged();
		return;
	}
}

void Host::offerInstall(const QString &path) {
	const auto controller = ActiveController();
	if (!controller) {
		return;
	}
	auto package = ReadPackageFile(path);
	if (!package.error.isEmpty()) {
		controller->showToast(
			tr::fg_plugins_bad_package(tr::now) + u' ' + package.error);
		return;
	}
	const auto manifest = package.manifest;
	for (const auto &plugin : _plugins) {
		if (plugin->manifest().id == manifest.id
			&& QFileInfo(plugin->origin()).isDir()) {
			controller->showToast(tr::fg_plugins_dev_folder(tr::now));
			return;
		}
	}
	if (!_engine.unknownSources) {
		controller->showToast(Translated(
			"Installing plugins from files is off: Settings > Plugins > "
			"Plugin engine > Unknown sources.",
			"Установка плагинов из файлов выключена: Настройки > Плагины > "
			"Движок плагинов > Неизвестные источники."));
		return;
	}
	auto view = PluginView{
		.id = manifest.id,
		.name = manifest.name,
		.version = manifest.version,
		.author = manifest.author,
		.description = manifest.description,
		.permissions = manifest.permissions,
		.origin = path,
		.icon = ReadIcon(package.files, manifest.icon),
	};
	const auto weak = base::make_weak(this);
	ShowInstallBox(controller, std::move(view), [=](bool enable) {
		if (const auto strong = weak.get()) {
			strong->install(path, manifest.id, enable);
		}
	});
}

void Host::install(const QString &path, const QString &id, bool enable) {
	const auto target = QString(PluginsPath() + id + u".flua"_q);
	for (auto i = begin(_plugins); i != end(_plugins); ++i) {
		if ((*i)->manifest().id == id) {
			(*i)->stop();
			_plugins.erase(i);
			break;
		}
	}
	const auto from = QFileInfo(path).absoluteFilePath();
	if (from != QFileInfo(target).absoluteFilePath()) {
		QDir().mkpath(PluginsPath());
		QFile::remove(target);
		if (!QFile::copy(path, target)) {
			Toast(tr::fg_plugins_install_failed(tr::now));
			return;
		}
	}
	if (enable) {
		_disabled.remove(id);
	} else {
		_disabled.emplace(id);
	}
	saveDisabled();
	load(target);
	refreshHooks();
	Plugins::RefreshDisplayText();
	Toast(tr::fg_plugins_installed(tr::now));
}

void Host::loadDisabled() {
	_disabled.clear();
	auto file = QFile(PluginsDataPath() + u"disabled.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto list = QJsonDocument::fromJson(file.readAll()).array();
	for (const auto value : list) {
		const auto id = value.toString();
		if (!id.isEmpty()) {
			_disabled.emplace(id);
		}
	}
}

void Host::saveDisabled() {
	auto list = QJsonArray();
	for (const auto &id : _disabled) {
		list.push_back(id);
	}
	QDir().mkpath(PluginsDataPath());
	auto file = QFile(PluginsDataPath() + u"disabled.json"_q);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(list).toJson(QJsonDocument::Indented));
	}
}

bool Host::hasPluginManager() {
	return true;
}

void Host::openPluginManager(Window::SessionController *controller) {
	if (controller) {
		controller->showSettings(Settings::PluginsId());
	}
}

} // namespace Plugins::Flua
