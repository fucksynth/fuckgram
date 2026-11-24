/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "base/weak_ptr.h"

class PeerData;
class DocumentData;
class QNetworkAccessManager;

namespace Data {
class DocumentMedia;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Plugins::Flua {

// The plugin catalog: .flua files posted in Telegram channels, and the
// official GitHub repository (index.json with the files, their sizes and
// SHA-256). Verified sources are built in, the user adds own channels.
// Every file is downloaded into plugins_data/catalog and checked like any
// package (manifest and the static analysis) before it is shown.

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
	// Grows with every load: answers of an older load are dropped.
	int generation = 0;
	// "owner/name" for a GitHub repository; `username` is its key then.
	QString repository;

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
		DocumentData *document = nullptr; // a post in a channel
		MsgId msgId = 0;
		TimeId date = 0;
		QString path;
		int generation = 0;
		int attempts = 0;
		QString url; // a file of a GitHub repository
		QString fileName;
		QByteArray sha256; // hex, of a repository file
		int64 size = 0;
	};

	[[nodiscard]] CatalogSource *find(const QString &username);
	[[nodiscard]] bool current(const QString &username, int generation) const;
	void notify();
	void loadCustom();
	void saveCustom() const;
	void resolve(const QString &username, int generation);
	void requestPage(
		const QString &username,
		int generation,
		MsgId offsetId,
		int page);
	void fail(const QString &username, const QString &error);
	[[nodiscard]] not_null<QNetworkAccessManager*> network();
	void requestIndex(const QString &username, int generation);
	void gotIndex(
		const QString &username,
		int generation,
		const QByteArray &bytes);
	void downloadUrl(const File &file);
	void urlDownloaded(const File &file, const QByteArray &bytes);
	void startFiles(const QString &username);
	void downloadNext();
	void downloaded(not_null<DocumentData*> document);
	void parse(const File &file);
	void parseNext();
	void parsed(const File &file, std::optional<CatalogPlugin> plugin);
	void fileDone(const QString &username);
	void finish(const QString &username);

	const not_null<Main::Session*> _session;
	std::vector<CatalogSource> _sources;
	std::map<QString, std::vector<File>> _found; // username -> its files
	std::map<QString, std::vector<CatalogPlugin>> _parsed;
	std::deque<File> _queue;
	std::vector<File> _downloading;
	// Downloads go to memory: the media view keeps the bytes, and a file
	// the user downloads in a chat is not touched.
	base::flat_map<
		not_null<DocumentData*>,
		std::shared_ptr<Data::DocumentMedia>> _media;
	std::deque<File> _parseQueue;
	bool _parsing = false;
	int _generations = 0;
	rpl::event_stream<> _changes;
	bool _changeQueued = false;
	std::unique_ptr<QNetworkAccessManager> _network;
	rpl::lifetime _lifetime;

};

// The catalog of the session, made on the first call.
[[nodiscard]] not_null<Catalog*> CatalogFor(not_null<Main::Session*> session);

// The file is a package listed in a signed index of the official
// repository (loaded in this run): only such a package from a verified
// source installs as trusted.
[[nodiscard]] bool IsSignedCatalogFile(const QString &path);

// "name", "@name", "t.me/name", "https://t.me/name/123" -> "name".
[[nodiscard]] std::optional<QString> ParseChannelUsername(const QString &text);

// Compares "1.2.10" and "1.2.9" by numbers: < 0, 0 or > 0.
[[nodiscard]] int CompareVersions(const QString &a, const QString &b);

// Name of a category from KnownCategories() for the interface.
[[nodiscard]] QString CategoryName(const QString &category);

} // namespace Plugins::Flua
