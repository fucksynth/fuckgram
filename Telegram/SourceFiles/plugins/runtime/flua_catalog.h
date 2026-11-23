/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/weak_ptr.h"

class PeerData;
class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace Plugins::Flua {

// The plugin catalog: .flua files posted in Telegram channels. Verified
// channels are built in, the user adds own ones. Every file is downloaded
// into plugins_data/catalog and checked like any package (manifest and
// the static analysis) before it is shown.

struct CatalogPlugin {
	QString id;
	QString name;
	QString version;
	QString author;
	QString description;
	std::vector<QString> permissions;
	std::vector<QString> categories;
	QImage icon;
	QString error; // the package does not pass the checks
	QString fileName; // as posted
	QString path; // the downloaded .flua
	MsgId msgId = 0;
	TimeId date = 0;
};

enum class CatalogState {
	Idle,
	Loading,
	Ready,
	Failed,
};

struct CatalogSource {
	QString username;
	bool verified = false;
	PeerData *peer = nullptr;
	QString title;
	CatalogState state = CatalogState::Idle;
	QString error;
	int filesTotal = 0;
	int filesDone = 0;
	// One per plugin id (the newest post), newest first; then the files
	// that do not pass the checks.
	std::vector<CatalogPlugin> plugins;

	[[nodiscard]] int validCount() const;
};

class Catalog final : public base::has_weak_ptr {
public:
	explicit Catalog(not_null<Main::Session*> session);
	~Catalog();

	[[nodiscard]] not_null<Main::Session*> session() const;
	[[nodiscard]] const std::vector<CatalogSource> &sources() const;
	[[nodiscard]] const CatalogSource *source(const QString &username) const;
	[[nodiscard]] rpl::producer<> changes() const;

	// Loads what is not loaded yet, everything again with `force`.
	void load(const QString &username, bool force = false);
	void loadAll();

	// A channel of the user's own, by username or t.me link. Returns the
	// username, or an error text in `error`.
	[[nodiscard]] QString addSource(const QString &text, QString *error);
	void removeSource(const QString &username);

private:
	struct File {
		QString username;
		not_null<DocumentData*> document;
		MsgId msgId = 0;
		TimeId date = 0;
		QString path;
	};

	[[nodiscard]] CatalogSource *find(const QString &username);
	void notify();
	void loadCustom();
	void saveCustom() const;
	void resolve(const QString &username);
	void requestPage(const QString &username, MsgId offsetId, int page);
	void fail(const QString &username, const QString &error);
	void startFiles(const QString &username);
	void downloadNext();
	void fileReady(const File &file);
	void fileDone(const QString &username);
	void finish(const QString &username);

	const not_null<Main::Session*> _session;
	std::vector<CatalogSource> _sources;
	std::map<QString, std::vector<File>> _found; // username -> its files
	std::map<QString, std::vector<CatalogPlugin>> _parsed;
	std::deque<File> _queue;
	std::vector<File> _downloading;
	rpl::event_stream<> _changes;
	bool _changeQueued = false;
	rpl::lifetime _lifetime;

};

// The catalog of the session, made on the first call.
[[nodiscard]] not_null<Catalog*> CatalogFor(not_null<Main::Session*> session);

// "name", "@name", "t.me/name", "https://t.me/name/123" -> "name".
[[nodiscard]] std::optional<QString> ParseChannelUsername(const QString &text);

// Compares "1.2.10" and "1.2.9" by numbers: < 0, 0 or > 0.
[[nodiscard]] int CompareVersions(const QString &a, const QString &b);

// Name of a category from KnownCategories() for the interface.
[[nodiscard]] QString CategoryName(const QString &category);

} // namespace Plugins::Flua
