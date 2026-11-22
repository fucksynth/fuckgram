/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_runtime.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "plugins/runtime/flua_package.h"
#include "plugins/runtime/flua_plugin.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

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
	_plugins.back()->start();
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
	auto newMessage = false;
	auto send = false;
	auto state = false;
	auto messageMenu = false;
	auto mainMenu = false;
	auto peerMenu = false;
	for (const auto &plugin : _plugins) {
		if (!plugin->running()) {
			continue;
		}
		newMessage |= plugin->handles(Event::MessageNew);
		send |= plugin->handles(Event::MessageSend);
		state |= plugin->handles(Event::AppState);
		for (const auto &entry : plugin->menu()) {
			switch (entry.place) {
			case MenuPlace::Message: messageMenu = true; break;
			case MenuPlace::Main: mainMenu = true; break;
			case MenuPlace::Chat:
			case MenuPlace::Profile: peerMenu = true; break;
			}
		}
	}
	SetEnabled(Hook::NewMessage, newMessage);
	SetEnabled(Hook::SendText, send);
	SetEnabled(Hook::AppState, state);
	SetEnabled(Hook::MessageMenu, messageMenu);
	SetEnabled(Hook::MainMenu, mainMenu);
	SetEnabled(Hook::PeerMenu, peerMenu);
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

void Host::appState(AppState state) {
	const auto name = StateName(state);
	for (const auto &plugin : _plugins) {
		if (plugin->handles(Event::AppState)) {
			plugin->appState(name);
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

bool Host::hasPluginManager() {
	return true;
}

void Host::openPluginManager(Window::SessionController *controller) {
	if (!controller) {
		return;
	}
	auto lines = QStringList();
	for (const auto &plugin : _plugins) {
		const auto &manifest = plugin->manifest();
		auto line = QString(manifest.name + u' ' + manifest.version);
		if (!manifest.author.isEmpty()) {
			line += u" · "_q + manifest.author;
		}
		line += plugin->running()
			? u"\nrunning"_q
			: (u"\nstopped: "_q + plugin->lastError());
		if (!manifest.permissions.empty()) {
			auto names = QStringList();
			for (const auto &permission : manifest.permissions) {
				names.push_back(permission);
			}
			line += u"\npermissions: "_q + names.join(u", "_q);
		}
		lines.push_back(line);
	}
	for (const auto &invalid : _invalid) {
		lines.push_back(QFileInfo(invalid.origin).fileName()
			+ u"\nnot loaded: "_q
			+ invalid.error);
	}
	if (lines.isEmpty()) {
		lines.push_back(u"No plugins yet. Put .flua files or plugin "
			"folders into:\n"_q + QDir::toNativeSeparators(PluginsPath()));
	}
	const auto text = lines.join(u"\n\n"_q);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::fg_plugins());
		box->addRow(object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Plugins::Flua
