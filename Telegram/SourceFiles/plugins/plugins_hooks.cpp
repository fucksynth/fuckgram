/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/plugins_hooks.h"

#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "ayu/ui/settings/settings_appearance.h"
#include "ayu/ui/settings/settings_ayu.h"
#include "ayu/ui/settings/settings_chats.h"
#include "ayu/ui/settings/settings_filters.h"
#include "ayu/ui/settings/settings_general.h"
#include "ayu/ui/settings/settings_main.h"
#include "ayu/ui/settings/settings_other.h"
#include "settings/sections/settings_active_sessions.h"
#include "settings/sections/settings_advanced.h"
#include "settings/sections/settings_blocked_peers.h"
#include "settings/sections/settings_calls.h"
#include "settings/sections/settings_chat.h"
#include "settings/sections/settings_folders.h"
#include "settings/sections/settings_information.h"
#include "settings/sections/settings_main.h"
#include "settings/sections/settings_notifications.h"
#include "settings/sections/settings_privacy_security.h"
#include "settings/sections/settings_shortcuts.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_menu_icons.h"

#include <crl/crl.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace Plugins {
namespace details {

uint64 EnabledMask = 0;

} // namespace details
namespace {

constexpr auto kSlowCall = crl::time(50);

Sink *CurrentSink = nullptr;
uint64 InFlightMask = 0;
int BypassDepth = 0;
int DisplayGenerationValue = 0;

struct ComposeEntry {
	QPointer<QWidget> widget;
	Fn<TextWithTags()> get;
	Fn<void(const TextWithTags&)> set;
};

[[nodiscard]] std::vector<ComposeEntry> &ComposeFields() {
	static auto result = std::vector<ComposeEntry>();
	std::erase_if(result, [](const ComposeEntry &entry) {
		return !entry.widget;
	});
	return result;
}

// The focused field, else the most recently registered visible one.
[[nodiscard]] ComposeEntry *CurrentCompose() {
	auto &list = ComposeFields();
	const auto focus = QApplication::focusWidget();
	if (focus) {
		for (auto &entry : list) {
			if (entry.widget == focus || entry.widget->isAncestorOf(focus)) {
				return &entry;
			}
		}
	}
	for (auto i = list.rbegin(); i != list.rend(); ++i) {
		if (i->widget->isVisible()) {
			return &*i;
		}
	}
	return nullptr;
}

[[nodiscard]] uint64 Bit(Hook hook) {
	return uint64(1) << uint32(hook);
}

[[nodiscard]] const char *Name(Hook hook) {
	switch (hook) {
	case Hook::SendText: return "send_text";
	case Hook::SendMedia: return "send_media";
	case Hook::NewMessage: return "new_message";
	case Hook::OpenUrl: return "open_url";
	case Hook::AppState: return "app_state";
	case Hook::Screen: return "screen";
	case Hook::PeerMenu: return "peer_menu";
	case Hook::ClientEvent: return "client_event";
	case Hook::MessageMenu: return "message_menu";
	case Hook::DisplayText: return "display_text";
	case Hook::MainMenu: return "main_menu";
	case Hook::Update: return "update";
	case Hook::ProfileRows: return "profile_rows";
	case Hook::Request: return "request";
	case Hook::Response: return "response";
	case Hook::Updates: return "updates";
	case Hook::SendMenu: return "send_menu";
	case Hook::SettingsSection: return "settings_section";
	case Hook::Count: break;
	}
	return "unknown";
}

// Marks a hook as running, so a plugin that triggers the same hook from
// inside its own handler does not recurse into itself.
class InFlight final {
public:
	explicit InFlight(Hook hook)
	: _bit(Bit(hook))
	, _hook(hook)
	, _started(crl::now()) {
		InFlightMask |= _bit;
	}
	~InFlight() {
		InFlightMask &= ~_bit;
		if (const auto spent = crl::now() - _started; spent > kSlowCall) {
			LOG(("Plugins Warning: %1 hook took %2 ms"
				).arg(Name(_hook)
				).arg(spent));
		}
	}

private:
	const uint64 _bit = 0;
	const Hook _hook = Hook::Count;
	const crl::time _started = 0;

};

[[nodiscard]] bool Ready(Hook hook) {
	return CurrentSink && Has(hook) && !(InFlightMask & Bit(hook));
}

} // namespace

void SetSink(Sink *sink) {
	CurrentSink = sink;
	if (!sink) {
		details::EnabledMask = 0;
	}
}

void SetEnabled(Hook hook, bool enabled) {
	if (hook == Hook::Count) {
		return;
	} else if (enabled) {
		details::EnabledMask |= Bit(hook);
	} else {
		details::EnabledMask &= ~Bit(hook);
	}
}

BypassScope::BypassScope() {
	++BypassDepth;
}

BypassScope::~BypassScope() {
	--BypassDepth;
}

SendTextResult FilterSendText(SendTextRequest &&request) {
	if (BypassDepth > 0 || !Ready(Hook::SendText)) {
		return {};
	}
	const auto guard = InFlight(Hook::SendText);
	auto result = CurrentSink->sendText(request);
	if (result.verdict == Verdict::Modify && result.text.text.isEmpty()) {
		// Nothing left to send.
		result.verdict = Verdict::Cancel;
	}
	return result;
}

bool SendMedia(SendMediaRequest &&request) {
	if (BypassDepth > 0 || !Ready(Hook::SendMedia)) {
		return false;
	}
	const auto guard = InFlight(Hook::SendMedia);
	return CurrentSink->sendMedia(request);
}

void NewMessage(not_null<HistoryItem*> item) {
	if (!Ready(Hook::NewMessage)) {
		return;
	}
	const auto guard = InFlight(Hook::NewMessage);
	CurrentSink->newMessage(item);
}

bool OpenUrl(const QString &url, bool local) {
	if (!Ready(Hook::OpenUrl)) {
		return false;
	}
	const auto guard = InFlight(Hook::OpenUrl);
	return CurrentSink->openUrl(url, local);
}

void NotifyAppState(AppState state) {
	if (!Ready(Hook::AppState)) {
		return;
	}
	const auto guard = InFlight(Hook::AppState);
	CurrentSink->appState(state);
}

void NotifyScreen(Screen screen, PeerData *peer) {
	if (!Ready(Hook::Screen)) {
		return;
	}
	const auto guard = InFlight(Hook::Screen);
	CurrentSink->screen(screen, peer);
}

void FillPeerMenu(
		PeerMenuPlace place,
		not_null<PeerData*> peer,
		const Ui::Menu::MenuCallback &addAction) {
	if (!Ready(Hook::PeerMenu)) {
		return;
	}
	auto items = std::vector<MenuItem>();
	{
		const auto guard = InFlight(Hook::PeerMenu);
		CurrentSink->peerMenu(place, peer, items);
	}
	std::erase_if(items, [](const MenuItem &item) {
		return item.text.isEmpty() || !item.handler;
	});
	if (items.empty()) {
		return;
	}
	if (place == PeerMenuPlace::Profile) {
		for (auto &item : items) {
			addAction(
				item.text,
				std::move(item.handler),
				item.icon ? item.icon : &st::menuIconBotCommands);
		}
		return;
	}
	addAction(Ui::Menu::MenuCallback::Args{
		.text = tr::fg_plugins(tr::now),
		.handler = nullptr,
		.icon = &st::menuIconBotCommands,
		.fillSubmenu = [items = std::move(items)](
				not_null<Ui::PopupMenu*> menu) mutable {
			for (auto &item : items) {
				menu->addAction(
					item.text,
					std::move(item.handler),
					item.icon ? item.icon : &st::menuIconBotCommands);
			}
		},
	});
}

bool InterceptClientEvent(ClientEventArgs &&args) {
	if (!Ready(Hook::ClientEvent)) {
		return false;
	}
	const auto guard = InFlight(Hook::ClientEvent);
	return CurrentSink->clientEvent(args);
}

void FillMessageMenu(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	if (!item || !Ready(Hook::MessageMenu)) {
		return;
	}
	auto items = std::vector<MenuItem>();
	{
		const auto guard = InFlight(Hook::MessageMenu);
		CurrentSink->messageMenu(item, items);
	}
	for (auto &entry : items) {
		if (entry.text.isEmpty() || !entry.handler) {
			continue;
		}
		menu->addAction(
			entry.text,
			std::move(entry.handler),
			entry.icon ? entry.icon : &st::menuIconBotCommands);
	}
}

std::optional<QString> DisplayText(
		DisplayTextPlace place,
		PeerData *peer,
		HistoryItem *item,
		const QString &text) {
	if (!Ready(Hook::DisplayText)) {
		return std::nullopt;
	}
	const auto guard = InFlight(Hook::DisplayText);
	auto result = CurrentSink->displayText(place, peer, item, text);
	if (result && *result == text) {
		return std::nullopt;
	}
	return result;
}

int DisplayGeneration() {
	return DisplayGenerationValue;
}

void RefreshDisplayText() {
	++DisplayGenerationValue;
	for (const auto &entry : Core::App().domain().accounts()) {
		if (!entry.account->sessionExists()) {
			continue;
		}
		auto &data = entry.account->session().data();
		const auto refresh = [&](not_null<PeerData*> peer) {
			const auto history = data.historyLoaded(peer->id);
			if (!history) {
				return;
			}
			for (const auto &block : history->blocks) {
				for (const auto &view : block->messages) {
					data.requestItemTextRefresh(view->data());
				}
			}
		};
		// Fn<void(not_null<UserData*>)> can't take a PeerData handler.
		data.enumerateUsers([&](not_null<UserData*> user) {
			refresh(user);
		});
		data.enumerateGroups(refresh);
		data.enumerateBroadcasts([&](not_null<ChannelData*> channel) {
			refresh(channel);
		});
	}
}

void FillSendMenu(not_null<Ui::PopupMenu*> menu, PeerData *peer) {
	if (!Ready(Hook::SendMenu)) {
		return;
	}
	auto items = std::vector<MenuItem>();
	{
		const auto guard = InFlight(Hook::SendMenu);
		CurrentSink->sendMenu(peer, items);
	}
	for (auto &entry : items) {
		if (entry.text.isEmpty() || !entry.handler) {
			continue;
		}
		menu->addAction(
			entry.text,
			std::move(entry.handler),
			entry.icon ? entry.icon : &st::menuIconBotCommands);
	}
}

QString SettingsSectionName(const Settings::Type &type) {
	const auto known = std::vector<std::pair<Settings::Type, const char*>>{
		{ Settings::MainId(), "main" },
		{ Settings::InformationId(), "account" },
		{ Settings::NotificationsId(), "notifications" },
		{ Settings::PrivacySecurityId(), "privacy" },
		{ Settings::ChatId(), "chats" },
		{ Settings::FoldersId(), "folders" },
		{ Settings::AdvancedId(), "advanced" },
		{ Settings::CallsId(), "calls" },
		{ Settings::SessionsId(), "sessions" },
		{ Settings::BlockedPeersId(), "blocked" },
		{ Settings::ShortcutsId(), "shortcuts" },
		{ Settings::AyuMainId(), "fuckgram" },
		{ Settings::AyuGhostId(), "fuckgram.ghost" },
		{ Settings::AyuFiltersId(), "fuckgram.filters" },
		{ Settings::AyuGeneralId(), "fuckgram.general" },
		{ Settings::AyuAppearanceId(), "fuckgram.appearance" },
		{ Settings::AyuChatsId(), "fuckgram.chats" },
		{ Settings::AyuOtherId(), "fuckgram.other" },
	};
	for (const auto &[id, name] : known) {
		if (id == type) {
			return QString::fromLatin1(name);
		}
	}
	return QString();
}

void FillSettingsSection(
		const Settings::Type &type,
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	if (!Ready(Hook::SettingsSection)) {
		return;
	}
	if (const auto name = SettingsSectionName(type); !name.isEmpty()) {
		FillSettingsSection(name, container, controller);
	}
}

void FillSettingsSection(
		const QString &section,
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	if (!Ready(Hook::SettingsSection)) {
		return;
	}
	const auto guard = InFlight(Hook::SettingsSection);
	CurrentSink->settingsSection(section, container, controller);
}

std::vector<MenuItem> MainMenuItems() {
	if (!Ready(Hook::MainMenu)) {
		return {};
	}
	auto items = std::vector<MenuItem>();
	{
		const auto guard = InFlight(Hook::MainMenu);
		CurrentSink->mainMenu(items);
	}
	std::erase_if(items, [](const MenuItem &item) {
		return item.text.isEmpty() || !item.handler;
	});
	return items;
}

void RegisterComposeField(
		not_null<QWidget*> widget,
		Fn<TextWithTags()> get,
		Fn<void(const TextWithTags&)> set) {
	ComposeFields().push_back({
		.widget = widget.get(),
		.get = std::move(get),
		.set = std::move(set),
	});
}

std::optional<TextWithTags> ComposeText() {
	if (const auto entry = CurrentCompose()) {
		return entry->get();
	}
	return std::nullopt;
}

bool SetComposeText(const TextWithTags &text) {
	if (const auto entry = CurrentCompose()) {
		entry->set(text);
		return true;
	}
	return false;
}

bool FilterUpdate(
		not_null<Main::Session*> session,
		const MTPUpdate &update) {
	if (!Ready(Hook::Update)) {
		return false;
	}
	const auto guard = InFlight(Hook::Update);
	return CurrentSink->update(session, update);
}

std::vector<ProfileRow> ProfileRows(not_null<PeerData*> peer) {
	if (!Ready(Hook::ProfileRows)) {
		return {};
	}
	auto rows = std::vector<ProfileRow>();
	{
		const auto guard = InFlight(Hook::ProfileRows);
		CurrentSink->profileRows(peer, rows);
	}
	std::erase_if(rows, [](const ProfileRow &row) {
		return row.label.isEmpty() || row.value.isEmpty();
	});
	return rows;
}

namespace {

[[nodiscard]] bool OnMainThread() {
	const auto app = QCoreApplication::instance();
	return app && (QThread::currentThread() == app->thread());
}

} // namespace

RawResult FilterRequest(const RawRequest &request) {
	if (!OnMainThread() || !Ready(Hook::Request)) {
		return {};
	}
	const auto guard = InFlight(Hook::Request);
	auto result = CurrentSink->request(request);
	if (result.verdict == Verdict::Modify && result.body.isEmpty()) {
		result.verdict = Verdict::Default;
	}
	return result;
}

std::optional<mtpBuffer> FilterResponse(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const mtpBuffer &reply) {
	if (!OnMainThread() || !Ready(Hook::Response)) {
		return std::nullopt;
	}
	const auto guard = InFlight(Hook::Response);
	auto result = CurrentSink->response(instance, requestId, reply);
	if (result && result->isEmpty()) {
		return std::nullopt;
	}
	return result;
}

RawResult FilterUpdates(
		not_null<MTP::Instance*> instance,
		const mtpBuffer &updates) {
	if (!OnMainThread() || !Ready(Hook::Updates)) {
		return {};
	}
	const auto guard = InFlight(Hook::Updates);
	auto result = CurrentSink->updates(instance, updates);
	if (result.verdict == Verdict::Modify && result.body.isEmpty()) {
		result.verdict = Verdict::Default;
	}
	return result;
}

bool HasPluginManager() {
	return CurrentSink && CurrentSink->hasPluginManager();
}

void OpenPluginManager(Window::SessionController *controller) {
	if (HasPluginManager()) {
		CurrentSink->openPluginManager(controller);
	}
}

} // namespace Plugins
