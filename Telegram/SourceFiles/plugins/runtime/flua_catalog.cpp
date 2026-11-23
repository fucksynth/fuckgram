/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_catalog.h"

#include "apiwrap.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "plugins/runtime/flua_package.h"
#include "plugins/runtime/flua_runtime.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

namespace Plugins::Flua {
namespace {

constexpr auto kPerPage = 100;
constexpr auto kMaxPages = 5; // the newest 500 documents of a channel
constexpr auto kMaxFileSize = int64(4 * 1024 * 1024);
constexpr auto kParallel = 4;

// Built in: plugins from these channels install without "Unknown sources"
// and show "verified source".
[[nodiscard]] const std::vector<QString> &VerifiedChannels() {
	static const auto result = std::vector<QString>{
		u"fuckgramplugins"_q,
	};
	return result;
}

[[nodiscard]] QString CatalogPath() {
	return PluginsDataPath() + u"catalog/"_q;
}

[[nodiscard]] QString CustomSourcesPath() {
	return PluginsDataPath() + u"catalog.json"_q;
}

[[nodiscard]] bool SameUsername(const QString &a, const QString &b) {
	return (a.compare(b, Qt::CaseInsensitive) == 0);
}

[[nodiscard]] auto Catalogs()
-> std::map<not_null<Main::Session*>, std::unique_ptr<Catalog>> & {
	static auto result = std::map<
		not_null<Main::Session*>,
		std::unique_ptr<Catalog>>();
	return result;
}

// What the package in a downloaded file is; nullopt if the file is not
// there or not whole. Runs off the main thread: nothing here touches the
// session or the interface.
[[nodiscard]] std::optional<CatalogPlugin> ReadCatalogFile(
		const QString &path,
		int64 size) {
	const auto info = QFileInfo(path);
	if (!info.exists() || info.size() != size) {
		return std::nullopt;
	}
	const auto package = ReadPackageFile(path);
	const auto &manifest = package.manifest;
	auto result = CatalogPlugin{
		.id = manifest.id,
		.name = manifest.name,
		.version = manifest.version,
		.author = manifest.author,
		.description = manifest.description,
		.permissions = manifest.permissions,
		.categories = manifest.categories,
		.error = package.error,
	};
	if (result.error.isEmpty()) {
		result.icon = ReadIcon(package.files, manifest.icon);
	}
	return result;
}

} // namespace

int CatalogSource::validCount() const {
	return int(ranges::count_if(plugins, [](const CatalogPlugin &plugin) {
		return plugin.error.isEmpty();
	}));
}

Catalog::Catalog(not_null<Main::Session*> session)
: _session(session) {
	for (const auto &name : VerifiedChannels()) {
		_sources.push_back({ .username = name, .verified = true });
	}
	loadCustom();

	_session->data().documentLoadProgress(
	) | rpl::on_next([=](not_null<DocumentData*> document) {
		if (!document->loading()) {
			downloaded(document);
		}
	}, _lifetime);
}

Catalog::~Catalog() = default;

not_null<Main::Session*> Catalog::session() const {
	return _session;
}

const std::vector<CatalogSource> &Catalog::sources() const {
	return _sources;
}

const CatalogSource *Catalog::source(const QString &username) const {
	for (const auto &source : _sources) {
		if (SameUsername(source.username, username)) {
			return &source;
		}
	}
	return nullptr;
}

bool Catalog::current(const QString &username, int generation) const {
	const auto found = source(username);
	return found && (found->generation == generation);
}

CatalogSource *Catalog::find(const QString &username) {
	for (auto &source : _sources) {
		if (SameUsername(source.username, username)) {
			return &source;
		}
	}
	return nullptr;
}

rpl::producer<> Catalog::changes() const {
	return _changes.events();
}

void Catalog::notify() {
	// Many files are checked at once: one update for all of them.
	if (_changeQueued) {
		return;
	}
	_changeQueued = true;
	crl::on_main(this, [=] {
		_changeQueued = false;
		_changes.fire({});
	});
}

void Catalog::loadCustom() {
	auto file = QFile(CustomSourcesPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto json = QJsonDocument::fromJson(file.readAll()).object();
	for (const auto value : json.value(u"sources"_q).toArray()) {
		const auto username = ParseChannelUsername(value.toString());
		if (username && !find(*username)) {
			_sources.push_back({ .username = *username });
		}
	}
}

void Catalog::saveCustom() const {
	auto list = QJsonArray();
	for (const auto &source : _sources) {
		if (!source.verified) {
			list.push_back(source.username);
		}
	}
	QDir().mkpath(PluginsDataPath());
	auto file = QFile(CustomSourcesPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(QJsonObject{
			{ u"sources"_q, list },
		}).toJson());
	}
}

void Catalog::load(const QString &username, bool force) {
	const auto source = find(username);
	if (!source
		|| source->state == CatalogState::Loading
		|| (source->state == CatalogState::Ready && !force)) {
		return;
	}
	source->state = CatalogState::Loading;
	source->error = QString();
	source->filesTotal = source->filesDone = 0;
	// Unique across sources: a channel removed and added again while it
	// loads does not take the answers of its older load.
	source->generation = ++_generations;
	_found.erase(source->username);
	_parsed.erase(source->username);
	notify();
	if (source->peer) {
		requestPage(source->username, source->generation, 0, 0);
	} else {
		resolve(source->username, source->generation);
	}
}

void Catalog::loadAll() {
	auto names = QStringList();
	for (const auto &source : _sources) {
		names.push_back(source.username);
	}
	for (const auto &name : names) {
		load(name);
	}
}

QString Catalog::addSource(const QString &text, QString *error) {
	const auto username = ParseChannelUsername(text);
	if (!username) {
		if (error) {
			*error = Translated(
				"Enter a channel username or a t.me link.",
				"Введите имя канала или ссылку t.me.");
		}
		return QString();
	} else if (find(*username)) {
		if (error) {
			*error = Translated(
				"This channel is in the catalog already.",
				"Этот канал уже есть в каталоге.");
		}
		return QString();
	}
	_sources.push_back({ .username = *username });
	saveCustom();
	notify();
	load(*username);
	return *username;
}

void Catalog::removeSource(const QString &username) {
	const auto i = ranges::find_if(_sources, [&](const CatalogSource &s) {
		return !s.verified && SameUsername(s.username, username);
	});
	if (i == end(_sources)) {
		return;
	}
	const auto name = i->username;
	_sources.erase(i);
	_found.erase(name);
	_parsed.erase(name);
	_queue.erase(
		ranges::remove(_queue, name, &File::username),
		end(_queue));
	saveCustom();
	notify();
}

void Catalog::fail(const QString &username, const QString &error) {
	if (const auto source = find(username)) {
		source->state = CatalogState::Failed;
		source->error = error;
		notify();
	}
}

void Catalog::resolve(const QString &username, int generation) {
	const auto gotPeer = [=](PeerData *peer) {
		const auto source = find(username);
		if (!source || source->generation != generation) {
			return;
		} else if (!peer || !peer->isChannel()) {
			fail(username, Translated("not a channel", "это не канал"));
			return;
		}
		source->peer = peer;
		source->title = peer->name();
		peer->loadUserpic();
		requestPage(username, generation, 0, 0);
	};
	if (const auto peer = _session->data().peerByUsername(username)) {
		gotPeer(peer);
		return;
	}
	const auto weak = base::make_weak(this);
	_session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		if (!weak) {
			return;
		}
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		gotPeer(_session->data().peerLoaded(peerFromMTP(data.vpeer())));
	}).fail([=](const MTP::Error &error) {
		if (weak && current(username, generation)) {
			fail(username, Translated(
				"the channel is not found",
				"канал не найден"));
		}
	}).send();
}

void Catalog::requestPage(
		const QString &username,
		int generation,
		MsgId offsetId,
		int page) {
	const auto source = find(username);
	if (!source || !source->peer || source->generation != generation) {
		return;
	}
	const auto peer = source->peer;
	const auto weak = base::make_weak(this);
	_session->api().request(MTPmessages_Search(
		MTP_flags(0),
		peer->input(),
		MTP_string(),
		MTP_inputPeerEmpty(), // from_id
		MTP_inputPeerEmpty(), // saved_peer_id
		MTP_vector<MTPReaction>(), // saved_reaction
		MTP_int(0), // top_msg_id
		MTP_inputMessagesFilterDocument(),
		MTP_int(0), // min_date
		MTP_int(0), // max_date
		MTP_int(int(offsetId.bare)),
		MTP_int(0), // add_offset
		MTP_int(kPerPage),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (!weak || !current(username, generation)) {
			return;
		}
		const auto list = result.match([](
				const MTPDmessages_messagesNotModified &) {
			return QVector<MTPMessage>();
		}, [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			return data.vmessages().v;
		});
		auto &found = _found[username];
		auto last = MsgId();
		for (const auto &message : list) {
			message.match([&](const MTPDmessage &data) {
				last = data.vid().v;
				const auto media = data.vmedia();
				if (!media) {
					return;
				}
				media->match([&](const MTPDmessageMediaDocument &fields) {
					const auto document = fields.vdocument();
					if (!document) {
						return;
					}
					const auto file = _session->data().processDocument(
						*document);
					if (!file->filename().endsWith(
							u".flua"_q,
							Qt::CaseInsensitive)
						|| file->size <= 0
						|| file->size > kMaxFileSize) {
						return;
					}
					found.push_back({
						.username = username,
						.document = file,
						.msgId = data.vid().v,
						.date = data.vdate().v,
						.generation = generation,
					});
				}, [](const auto &) {});
			}, [&](const auto &data) {
				last = data.vid().v;
			});
		}
		if (list.size() >= kPerPage && page + 1 < kMaxPages && last) {
			requestPage(username, generation, last, page + 1);
		} else {
			startFiles(username);
		}
	}).fail([=](const MTP::Error &error) {
		if (weak && current(username, generation)) {
			fail(username, Translated(
				"could not read the channel: ",
				"не удалось прочитать канал: ") + error.type());
		}
	}).send();
}

void Catalog::startFiles(const QString &username) {
	const auto source = find(username);
	if (!source) {
		return;
	}
	auto files = base::take(_found[username]);
	_found.erase(username);
	_parsed[username].clear();
	source->filesTotal = int(files.size());
	source->filesDone = 0;
	notify();
	if (files.empty()) {
		finish(username);
		return;
	}
	QDir().mkpath(CatalogPath());
	for (auto &file : files) {
		file.path = CatalogPath()
			+ QString::number(file.document->id)
			+ u".flua"_q;
		const auto info = QFileInfo(file.path);
		if (info.exists() && info.size() == file.document->size) {
			parse(file);
		} else {
			_queue.push_back(file);
		}
	}
	downloadNext();
}

void Catalog::downloadNext() {
	while (int(_downloading.size()) < kParallel && !_queue.empty()) {
		auto file = _queue.front();
		_queue.pop_front();
		const auto source = find(file.username);
		if (!source
			|| !source->peer
			|| source->generation != file.generation) {
			continue;
		}
		// The same document in two posts or channels: one download.
		const auto joined = ranges::contains(
			_downloading,
			file.document,
			&File::document);
		_downloading.push_back(file);
		if (joined) {
			continue;
		}
		if (!_media.contains(file.document)) {
			_media.emplace(file.document, file.document->createMediaView());
		}
		// An empty name loads into memory and joins a download that is
		// already going, instead of cancelling it.
		file.document->save(
			Data::FileOriginMessage(source->peer->id, file.msgId),
			QString());
		if (!file.document->loading()) {
			// It was loaded already, or did not start.
			downloaded(file.document);
		}
	}
}

void Catalog::downloaded(not_null<DocumentData*> document) {
	auto finished = std::vector<File>();
	for (auto i = begin(_downloading); i != end(_downloading);) {
		if (i->document == document) {
			finished.push_back(*i);
			i = _downloading.erase(i);
		} else {
			++i;
		}
	}
	if (finished.empty()) {
		return;
	}
	const auto media = _media.take(document);
	auto bytes = (media && *media) ? (*media)->bytes() : QByteArray();
	if (bytes.size() != document->size) {
		// Downloaded to a file before, in a chat.
		const auto &location = document->location(true);
		if (!location.isEmpty() && location.accessEnable()) {
			auto file = QFile(location.name());
			if (file.open(QIODevice::ReadOnly)) {
				bytes = file.readAll();
			}
			location.accessDisable();
		}
	}
	if (bytes.size() == document->size) {
		// Whole or nothing: a half-written file is never read.
		const auto path = finished.front().path;
		const auto part = path + u".part"_q;
		auto file = QFile(part);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)
			&& file.write(bytes) == bytes.size()) {
			file.close();
			QFile::remove(path);
			QFile::rename(part, path);
		} else {
			file.close();
			QFile::remove(part);
		}
	} else if (document->status != FileDownloadFailed) {
		// Cancelled, e.g. by a download of the same file in a chat: the
		// file is asked for once more.
		auto again = std::vector<File>();
		for (auto i = begin(finished); i != end(finished);) {
			if (i->attempts < 1) {
				++i->attempts;
				again.push_back(*i);
				i = finished.erase(i);
			} else {
				++i;
			}
		}
		for (const auto &file : again) {
			_queue.push_front(file);
		}
	}
	for (const auto &file : finished) {
		parse(file);
	}
	downloadNext();
}

void Catalog::parse(const File &file) {
	if (!current(file.username, file.generation)) {
		return;
	}
	_parseQueue.push_back(file);
	parseNext();
}

void Catalog::parseNext() {
	// One package at a time, off the main thread: hundreds of cached files
	// must not freeze the window.
	if (_parsing) {
		return;
	}
	while (!_parseQueue.empty()) {
		const auto file = _parseQueue.front();
		_parseQueue.pop_front();
		if (!current(file.username, file.generation)) {
			continue;
		}
		_parsing = true;
		const auto weak = base::make_weak(this);
		const auto path = file.path;
		const auto size = int64(file.document->size);
		crl::async([=] {
			auto result = ReadCatalogFile(path, size);
			crl::on_main(weak, [=, result = std::move(result)]() mutable {
				_parsing = false;
				parsed(file, std::move(result));
				parseNext();
			});
		});
		return;
	}
}

void Catalog::parsed(const File &file, std::optional<CatalogPlugin> plugin) {
	if (!current(file.username, file.generation)) {
		return;
	}
	auto result = plugin
		? std::move(*plugin)
		: CatalogPlugin{
			.error = Translated(
				"could not download the file",
				"не удалось скачать файл"),
		};
	result.fileName = file.document->filename();
	result.path = file.path;
	result.msgId = file.msgId;
	result.date = file.date;
	_parsed[file.username].push_back(std::move(result));
	fileDone(file.username);
}

void Catalog::fileDone(const QString &username) {
	const auto source = find(username);
	if (!source) {
		return;
	}
	++source->filesDone;
	if (source->filesDone >= source->filesTotal) {
		finish(username);
	} else {
		notify();
	}
}

void Catalog::finish(const QString &username) {
	const auto source = find(username);
	if (!source) {
		return;
	}
	auto list = base::take(_parsed[username]);
	_parsed.erase(username);
	ranges::sort(list, [](const CatalogPlugin &a, const CatalogPlugin &b) {
		return a.msgId > b.msgId;
	});
	auto result = std::vector<CatalogPlugin>();
	auto broken = std::vector<CatalogPlugin>();
	auto kept = base::flat_map<QString, MsgId>(); // id -> its shown post
	for (auto &plugin : list) {
		if (!plugin.error.isEmpty()) {
			broken.push_back(std::move(plugin));
		} else if (!kept.contains(plugin.id)) {
			kept.emplace(plugin.id, plugin.msgId);
			result.push_back(std::move(plugin));
		}
	}
	auto brokenShown = base::flat_set<QString>();
	for (auto &plugin : broken) {
		if (!plugin.id.isEmpty()) {
			// An older broken post of a plugin that has a good one is
			// noise; a newer one is what the author tries to release.
			const auto i = kept.find(plugin.id);
			if ((i != end(kept) && plugin.msgId < i->second)
				|| !brokenShown.emplace(plugin.id).second) {
				continue;
			}
		}
		result.push_back(std::move(plugin));
	}
	source->plugins = std::move(result);
	source->state = CatalogState::Ready;
	notify();
}

not_null<Catalog*> CatalogFor(not_null<Main::Session*> session) {
	auto &catalogs = Catalogs();
	const auto i = catalogs.find(session);
	if (i != end(catalogs)) {
		return i->second.get();
	}
	const auto result = catalogs.emplace(
		session,
		std::make_unique<Catalog>(session)).first->second.get();
	session->lifetime().add([=] {
		Catalogs().erase(session);
	});
	return result;
}

std::optional<QString> ParseChannelUsername(const QString &text) {
	auto value = text.trimmed();
	static const auto link = QRegularExpression(
		u"^(?:https?://)?(?:www\\.)?(?:t|telegram)\\.me/(?:s/)?"
		"([A-Za-z0-9_]+)(?:[/?#].*)?$"_q);
	if (const auto match = link.match(value); match.hasMatch()) {
		value = match.captured(1);
	} else if (value.startsWith(u'@')) {
		value = value.mid(1);
	}
	static const auto check = QRegularExpression(
		u"^[A-Za-z][A-Za-z0-9_]{3,31}$"_q);
	if (!check.match(value).hasMatch()) {
		return std::nullopt;
	}
	return value;
}

int CompareVersions(const QString &a, const QString &b) {
	static const auto number = QRegularExpression(u"\\d+"_q);
	const auto numbers = [](const QString &version) {
		auto result = std::vector<qint64>();
		auto i = number.globalMatch(version);
		while (i.hasNext()) {
			result.push_back(i.next().captured().left(15).toLongLong());
		}
		return result;
	};
	const auto first = numbers(a);
	const auto second = numbers(b);
	for (auto i = 0; i != int(std::max(first.size(), second.size())); ++i) {
		const auto x = (i < int(first.size())) ? first[i] : qint64(0);
		const auto y = (i < int(second.size())) ? second[i] : qint64(0);
		if (x != y) {
			return (x < y) ? -1 : 1;
		}
	}
	return 0;
}

QString CategoryName(const QString &category) {
	if (category == u"appearance"_q) {
		return Translated("Appearance", "Оформление");
	} else if (category == u"chats"_q) {
		return Translated("Chats", "Чаты");
	} else if (category == u"media"_q) {
		return Translated("Media", "Медиа");
	} else if (category == u"privacy"_q) {
		return Translated("Privacy", "Приватность");
	} else if (category == u"tools"_q) {
		return Translated("Tools", "Инструменты");
	} else if (category == u"fun"_q) {
		return Translated("Fun", "Развлечения");
	}
	return category;
}

} // namespace Plugins::Flua
