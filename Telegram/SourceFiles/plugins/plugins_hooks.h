/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"
#include "ui/text/text_entity.h"

#include <optional>

class History;
class HistoryItem;
class PeerData;
class QWidget;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

// The only surface between the client and the plugin runtime.
//
// Every call from client code into this header is marked with
// "// PLUGIN-HOOK: <name>", tools/check_plugin_hooks.py verifies after a
// merge of a new AyuGram / Telegram Desktop version that none got lost.
//
// All functions are main-thread only. Without an attached runtime, or
// with a hook nobody subscribed to, every call is a single bit test.
namespace Plugins {

enum class Hook : uint32 {
	SendText,     // filter: cancel / modify outgoing text
	SendMedia,    // filter: cancel outgoing files
	NewMessage,   // event: a new message was added to a chat
	OpenUrl,      // filter: external link or tg:// link
	AppState,     // event: started / stopping / active / inactive
	Screen,       // event: chat list, chat or profile shown
	PeerMenu,     // ui: items in the chat and profile "..." menus
	ClientEvent,  // filter: cancel a user action (reply, reaction, call...)
	MessageMenu,  // ui: items in the message context menu
	DisplayText,  // filter: replace shown text (message, chat name...)
	MainMenu,     // ui: items in the main (drawer) menu

	Count,
};

enum class Verdict {
	Default,
	Cancel,
	Modify,
};

struct SendTextRequest {
	not_null<History*> history;
	TextWithTags text;
	MsgId replyTo = 0;
};

struct SendTextResult {
	Verdict verdict = Verdict::Default;
	TextWithTags text; // used when verdict == Verdict::Modify
};

struct SendMediaRequest {
	not_null<History*> history;
	QStringList paths;
	QString caption;
};

enum class AppState {
	Started,
	Stopping,
	Active,
	Inactive,
};

enum class Screen {
	ChatList,
	Chat,
	Profile,
};

enum class PeerMenuPlace {
	Chat,
	Profile,
};

struct MenuItem {
	QString text;
	Fn<void()> handler;
};

enum class ClientEvent {
	Reply,        // the user starts replying to a message
	Reaction,     // the user sets or removes a reaction
	Call,         // an outgoing call is about to start
	Transcribe,   // voice / video message transcription requested
	Notification, // a notification is about to be shown
	PlayerNext,     // the music player goes to the next track
	PlayerPrevious, // the music player goes to the previous track
	PlayerStop,     // the music player is closed
	PlayMedia,      // an audio, voice or round video message starts playing
	OpenFile,       // a downloaded file is opened (also .flua packages)
	SelectMessage,  // a message gets selected
	MessageMenu,    // a message context menu is about to open
	AvatarClick,    // the chat header (avatar / name) is clicked
};

struct ClientEventArgs {
	ClientEvent event = ClientEvent::Reply;
	HistoryItem *item = nullptr;
	PeerData *peer = nullptr;
	QString emoji;            // Reaction: the emoji, empty for a custom one
	uint64 customEmojiId = 0; // Reaction: custom emoji document id
	bool remove = false;      // Reaction: the chosen reaction is toggled off
	bool video = false;       // Call: a video call
	bool reaction = false;    // Notification: about a reaction, not a message
	QString path;             // OpenFile: local path of the file
};

enum class DisplayTextPlace {
	Message,      // message text in a chat (display only, not sent)
	ChatListName, // chat name in the chat list
	ChatTitle,    // chat name in the chat header
	ChatSubtitle, // status line in the chat header
	CopiedText,   // text put to the clipboard by "Copy text"
};

// Implemented by the plugin runtime.
class Sink {
public:
	virtual ~Sink() = default;

	virtual SendTextResult sendText(const SendTextRequest &) {
		return {};
	}
	// true cancels sending.
	virtual bool sendMedia(const SendMediaRequest &) {
		return false;
	}
	virtual void newMessage(not_null<HistoryItem*>) {
	}
	// true means the plugin handled the link and the client must not.
	// local == true for tg:// links.
	virtual bool openUrl(const QString &, bool) {
		return false;
	}
	virtual void appState(AppState) {
	}
	virtual void screen(Screen, PeerData*) {
	}
	virtual void peerMenu(
			PeerMenuPlace,
			not_null<PeerData*>,
			std::vector<MenuItem> &) {
	}
	// true cancels the action.
	virtual bool clientEvent(const ClientEventArgs &) {
		return false;
	}
	virtual void messageMenu(
			not_null<HistoryItem*>,
			std::vector<MenuItem> &) {
	}
	// A value replaces the text that is shown, std::nullopt keeps it.
	virtual std::optional<QString> displayText(
			DisplayTextPlace,
			PeerData*,
			HistoryItem*,
			const QString &) {
		return std::nullopt;
	}
	virtual void mainMenu(std::vector<MenuItem> &) {
	}
};

// The runtime attaches itself and says which hooks it needs.
void SetSink(Sink *sink); // nullptr detaches and disables every hook
void SetEnabled(Hook hook, bool enabled);

// While alive, the send hooks are skipped. The runtime wraps its own
// sends with it, so a plugin never receives its own messages back.
class BypassScope final {
public:
	BypassScope();
	~BypassScope();

	BypassScope(const BypassScope &) = delete;
	BypassScope &operator=(const BypassScope &) = delete;

};

namespace details {
extern uint64 EnabledMask;
} // namespace details

[[nodiscard]] inline bool Has(Hook hook) {
	return (details::EnabledMask & (uint64(1) << uint32(hook))) != 0;
}

// Safe to call unconditionally: they return the default answer when no
// runtime is attached or the hook is disabled.
[[nodiscard]] SendTextResult FilterSendText(SendTextRequest &&request);
[[nodiscard]] bool SendMedia(SendMediaRequest &&request);
void NewMessage(not_null<HistoryItem*> item);
[[nodiscard]] bool OpenUrl(const QString &url, bool local);
void NotifyAppState(AppState state);
void NotifyScreen(Screen screen, PeerData *peer);

// Adds the plugin items: a "Plugins" submenu in the chat menu, plain
// items in the profile menu.
void FillPeerMenu(
	PeerMenuPlace place,
	not_null<PeerData*> peer,
	const Ui::Menu::MenuCallback &addAction);

// true means a plugin cancelled the action.
[[nodiscard]] bool InterceptClientEvent(ClientEventArgs &&args);

// Adds the plugin items to a message context menu.
void FillMessageMenu(not_null<Ui::PopupMenu*> menu, HistoryItem *item);

// Text to show instead of `text`, std::nullopt keeps the original.
// Applied where the client rebuilds the text, not on every repaint.
[[nodiscard]] std::optional<QString> DisplayText(
	DisplayTextPlace place,
	PeerData *peer,
	HistoryItem *item,
	const QString &text);

// Bumped by RefreshDisplayText(); places that cache a hooked text add it
// to their own version so the cache is rebuilt.
[[nodiscard]] int DisplayGeneration();

// For the runtime: its display rules changed, rebuild what is shown.
// Messages of loaded chats are re-laid out now, chat names and headers
// on their next repaint.
void RefreshDisplayText();

// Items for the main (drawer) menu.
[[nodiscard]] std::vector<MenuItem> MainMenuItems();

// Message input fields register themselves; the runtime reads and
// replaces the text of the current one (focused, else visible).
void RegisterComposeField(
	not_null<QWidget*> widget,
	Fn<TextWithTags()> get,
	Fn<void(const TextWithTags&)> set);
[[nodiscard]] std::optional<TextWithTags> ComposeText();
bool SetComposeText(const TextWithTags &text);

} // namespace Plugins
