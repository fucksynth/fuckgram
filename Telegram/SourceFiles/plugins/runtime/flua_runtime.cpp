/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_runtime.h"

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
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
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

// What a permission lets a plugin do, for the install box.
[[nodiscard]] QString PermissionText(const QString &permission) {
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
			"know which chat or profile is open",
			"знать, какой чат или профиль открыт");
	} else if (is(kPermCompose)) {
		return pick(
			"read and replace the text you type",
			"читать и заменять набираемый текст");
	}
	return permission;
}

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

[[nodiscard]] QString Describe(const Manifest &manifest) {
	auto result = QString(manifest.name + u' ' + manifest.version);
	if (!manifest.author.isEmpty()) {
		result += u" · "_q + manifest.author;
	}
	if (!manifest.description.isEmpty()) {
		result += u'\n' + manifest.description;
	}
	if (manifest.permissions.empty()) {
		result += Russian()
			? QString::fromUtf8("\n\nБез разрешений.")
			: u"\n\nNo permissions."_q;
	} else {
		result += Russian()
			? QString::fromUtf8("\n\nОн сможет:")
			: u"\n\nIt will be able to:"_q;
		for (const auto &permission : manifest.permissions) {
			result += u"\n• "_q + PermissionText(permission);
		}
	}
	return result;
}

} // namespace

QString PluginsPath() {
	return cWorkingDir() + u"plugins/"_q;
}

QString PluginsDataPath() {
	return cWorkingDir() + u"plugins_data/"_q;
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

Host::Host() = default;

Host::~Host() {
	stop();
}

void Host::start() {
	loadDisabled();
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
	LOG(("Plugins: %1 loaded, %2 invalid, from %3"
		).arg(_plugins.size()
		).arg(_invalid.size()
		).arg(root));
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
	if (!_disabled.contains(id)) {
		_plugins.back()->start();
	}
}

void Host::stop() {
	SetSink(nullptr);
	for (const auto &plugin : _plugins) {
		plugin->stop();
	}
	_plugins.clear();
	_invalid.clear();
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
			if (!plugin->start()) {
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
	const auto text = Describe(manifest);
	const auto weak = base::make_weak(this);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::fg_plugins_install_title());
		box->addRow(object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
		box->addButton(tr::fg_plugins_install(), [=] {
			box->closeBox();
			if (const auto strong = weak.get()) {
				strong->install(path, manifest.id);
			}
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void Host::install(const QString &path, const QString &id) {
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
	_disabled.remove(id);
	saveDisabled();
	load(target);
	refreshHooks();
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
	if (!controller) {
		return;
	}
	struct Row {
		QString id;
		QString text;
		bool enabled = false;
	};
	auto rows = std::vector<Row>();
	for (const auto &plugin : _plugins) {
		const auto &manifest = plugin->manifest();
		auto text = QString(manifest.name + u' ' + manifest.version);
		if (!manifest.author.isEmpty()) {
			text += u" · "_q + manifest.author;
		}
		if (_disabled.contains(manifest.id)) {
			text += u'\n' + tr::fg_plugins_disabled(tr::now);
		} else if (plugin->running()) {
			text += u'\n' + tr::fg_plugins_running(tr::now);
		} else {
			text += u'\n' + tr::fg_plugins_stopped(tr::now)
				+ u": "_q
				+ plugin->lastError();
		}
		rows.push_back({
			.id = manifest.id,
			.text = text,
			.enabled = !_disabled.contains(manifest.id),
		});
	}
	auto invalid = QStringList();
	for (const auto &entry : _invalid) {
		invalid.push_back(QFileInfo(entry.origin).fileName()
			+ u": "_q
			+ entry.error);
	}
	const auto empty = rows.empty() && invalid.isEmpty();
	const auto weak = base::make_weak(this);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::fg_plugins());
		if (empty) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				tr::fg_plugins_empty(tr::now)
					+ u'\n'
					+ QDir::toNativeSeparators(PluginsPath()),
				st::boxLabel));
		}
		for (const auto &row : rows) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				row.text,
				st::boxLabel));
			const auto toggle = box->addRow(object_ptr<Ui::Checkbox>(
				box,
				tr::fg_plugins_enabled(tr::now),
				row.enabled,
				st::defaultBoxCheckbox));
			const auto id = row.id;
			toggle->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				if (const auto strong = weak.get()) {
					strong->setPluginEnabled(id, checked);
				}
			}, toggle->lifetime());
		}
		if (!invalid.isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				tr::fg_plugins_not_loaded(tr::now)
					+ u'\n'
					+ invalid.join(u'\n'),
				st::boxLabel));
		}
		box->addLeftButton(tr::fg_plugins_open_folder(), [] {
			QDir().mkpath(PluginsPath());
			QDesktopServices::openUrl(QUrl::fromLocalFile(PluginsPath()));
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Plugins::Flua
