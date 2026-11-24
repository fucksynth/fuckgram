/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_catalog.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "logs.h"
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
#include <QtCore/QCryptographicHash>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <openssl/evp.h>

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

// The official repository: index.json and the .flua files in "main".
constexpr auto kRepository = "fucksynth/fuckgram-plugins";
constexpr auto kRepositoryBranch = "main";
constexpr auto kMaxIndexSize = int64(1024 * 1024);
constexpr auto kMaxSignedIndexSize = int64(2 * 1024 * 1024);
constexpr auto kMaxIndexFiles = 500;

// index.signed.json of the official repository is signed by this Ed25519
// key (its private half is a secret of the repository's CI): a list that
// does not match is not shown, and a package from a verified source
// installs as trusted only when its SHA-256 is in a signed list.
constexpr auto kCatalogPublicKey = std::array<unsigned char, 32>{
	0x0f, 0x34, 0x2e, 0x62, 0xd7, 0x74, 0xe1, 0x1c,
	0xd1, 0xce, 0x53, 0x7a, 0x02, 0x50, 0x9c, 0xda,
	0xb6, 0xfd, 0x32, 0x8c, 0xa1, 0xd8, 0xa9, 0xaf,
	0xfc, 0xe0, 0x0a, 0x4d, 0x5a, 0xa4, 0x63, 0x84,
};

[[nodiscard]] bool GoodCatalogSignature(
		const QByteArray &data,
		const QByteArray &signature) {
	if (signature.size() != 64) {
		return false;
	}
	const auto key = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519,
		nullptr,
		kCatalogPublicKey.data(),
		kCatalogPublicKey.size());
	if (!key) {
		return false;
	}
	const auto context = EVP_MD_CTX_new();
	const auto result = context
		&& (EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1)
		&& (EVP_DigestVerify(
			context,
			reinterpret_cast<const unsigned char*>(signature.constData()),
			size_t(signature.size()),
			reinterpret_cast<const unsigned char*>(data.constData()),
			size_t(data.size())) == 1);
	EVP_MD_CTX_free(context);
	EVP_PKEY_free(key);
	return result;
}

// SHA-256 (hex) of every package in a signed list seen in this run.
[[nodiscard]] base::flat_set<QByteArray> &SignedHashes() {
	static auto result = base::flat_set<QByteArray>();
	return result;
}

[[nodiscard]] QString RepositoryKey(const QString &repository) {
	return u"github:"_q + repository;
}

[[nodiscard]] QString RepositoryFileUrl(
		const QString &repository,
		const QString &path) {
	return u"https://raw.githubusercontent.com/%1/%2/%3"_q.arg(
		repository,
		QString::fromLatin1(kRepositoryBranch),
		path);
}

[[nodiscard]] QNetworkRequest RepositoryRequest(const QString &url) {
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "fuckgram-catalog");
	return request;
}

// "dist/name.flua": no "..", no absolute paths, nothing but names.
[[nodiscard]] bool GoodRepositoryPath(const QString &path) {
	static const auto check = QRegularExpression(
		u"^[A-Za-z0-9_.\\-]+(/[A-Za-z0-9_.\\-]+)*$"_q);
	return check.match(path).hasMatch()
		&& !path.split(u'/').contains(u".."_q)
		&& path.endsWith(u".flua"_q, Qt::CaseInsensitive);
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
// session or the interface; the languages are taken on the main thread.
[[nodiscard]] std::optional<CatalogPlugin> ReadCatalogFile(
		const QString &path,
		int64 size,
		const std::vector<QString> &languages) {
	const auto info = QFileInfo(path);
	if (!info.exists() || info.size() != size) {
		return std::nullopt;
	}
	const auto package = ReadPackageFile(path);
	const auto &manifest = package.manifest;
	auto result = CatalogPlugin{
		.id = manifest.id,
		.name = manifest.displayName(languages),
		.version = manifest.version,
		.author = manifest.author,
		.description = manifest.displayDescription(languages),
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

bool IsSignedCatalogFile(const QString &path) {
	if (SignedHashes().empty()) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFileSize) {
		return false;
	}
	const auto hash = QCryptographicHash::hash(
		file.readAll(),
		QCryptographicHash::Sha256).toHex().toLower();
	return SignedHashes().contains(hash);
}

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
	const auto repository = QString::fromLatin1(kRepository);
	_sources.push_back({
		.username = RepositoryKey(repository),
		.verified = true,
		.title = repository.mid(repository.indexOf(u'/') + 1)
			+ u" \u00B7 GitHub"_q,
		.repository = repository,
	});
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
	if (!source->repository.isEmpty()) {
		requestIndex(source->username, source->generation);
	} else if (source->peer) {
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
						.fileName = file->filename(),
						.size = file->size,
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
		// A repository file is named by its contents: a cached one is
		// the same file.
		file.path = CatalogPath()
			+ (file.document
				? QString::number(file.document->id)
				: (u"github-"_q + QString::fromLatin1(file.sha256)))
			+ u".flua"_q;
		const auto info = QFileInfo(file.path);
		if (info.exists() && info.size() == file.size) {
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
		if (!source || source->generation != file.generation) {
			continue;
		} else if (!file.url.isEmpty()) {
			downloadUrl(file);
			continue;
		} else if (!source->peer || !file.document) {
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
		if (i->document == document.get()) {
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
		const auto size = file.size;
		const auto languages = LanguageChain(InterfaceLanguage());
		crl::async([=] {
			auto result = ReadCatalogFile(path, size, languages);
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
	result.fileName = file.fileName;
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

not_null<QNetworkAccessManager*> Catalog::network() {
	if (!_network) {
		_network = std::make_unique<QNetworkAccessManager>();
	}
	return _network.get();
}

void Catalog::requestIndex(const QString &username, int generation) {
	const auto source = find(username);
	if (!source) {
		return;
	}
	// The raw files are cached for minutes: a new query each minute.
	// The signed list: index.json with its signature in one file, so the
	// two never come from different commits.
	const auto url = RepositoryFileUrl(
		source->repository,
		u"index.signed.json"_q)
		+ u"?t="_q
		+ QString::number(base::unixtime::now() / 60);
	const auto reply = network()->get(RepositoryRequest(url));
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (!weak || !current(username, generation)) {
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			fail(username, Translated(
				"could not read the repository: ",
				"не удалось прочитать репозиторий: ") + reply->errorString());
			return;
		}
		gotIndex(username, generation, reply->read(kMaxSignedIndexSize));
	});
}

// index.signed.json: { "index": base64, "signature": base64 }, where
// index is index.json: { "plugins": [ { "file": "dist/x.flua", "size": 123,
// "sha256": "...", "date": 1700000000 }, ... ] }, the newest first.
void Catalog::gotIndex(
		const QString &username,
		int generation,
		const QByteArray &signedBytes) {
	const auto source = find(username);
	if (!source) {
		return;
	}
	const auto envelope = QJsonDocument::fromJson(signedBytes).object();
	const auto bytes = QByteArray::fromBase64(
		envelope.value(u"index"_q).toString().toLatin1());
	const auto signature = QByteArray::fromBase64(
		envelope.value(u"signature"_q).toString().toLatin1());
	if (bytes.isEmpty()
		|| bytes.size() > kMaxIndexSize
		|| !GoodCatalogSignature(bytes, signature)) {
		LOG(("Plugins Error: the signature of %1 does not match"
			).arg(source->repository));
		fail(username, Translated(
			"the plugin list is not signed by fuckgram",
			"список плагинов не подписан fuckgram"));
		return;
	}
	const auto document = QJsonDocument::fromJson(bytes);
	if (!document.isObject()) {
		fail(username, Translated(
			"the repository has no plugin list",
			"в репозитории нет списка плагинов"));
		return;
	}
	const auto list = document.object().value(u"plugins"_q).toArray();
	auto &found = _found[username];
	found.clear();
	for (const auto value : list) {
		const auto entry = value.toObject();
		const auto path = entry.value(u"file"_q).toString();
		const auto sha256 = entry.value(
			u"sha256"_q).toString().toLower().toLatin1();
		const auto size = int64(entry.value(u"size"_q).toDouble());
		if (!GoodRepositoryPath(path)
			|| sha256.size() != 64
			|| size <= 0
			|| size > kMaxFileSize) {
			continue;
		}
		SignedHashes().emplace(sha256);
		found.push_back({
			.username = username,
			.msgId = MsgId(int64(list.size()) - int64(found.size())),
			.date = TimeId(entry.value(u"date"_q).toDouble()),
			.generation = generation,
			.url = (RepositoryFileUrl(source->repository, path)
				+ u"?sha="_q
				+ QString::fromLatin1(sha256)),
			.fileName = path.mid(path.lastIndexOf(u'/') + 1),
			.sha256 = sha256,
			.size = size,
		});
		if (int(found.size()) >= kMaxIndexFiles) {
			break;
		}
	}
	startFiles(username);
}

void Catalog::downloadUrl(const File &file) {
	_downloading.push_back(file);
	const auto reply = network()->get(RepositoryRequest(file.url));
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (!weak) {
			return;
		}
		const auto good = (reply->error() == QNetworkReply::NoError);
		urlDownloaded(
			file,
			good ? reply->read(kMaxFileSize + 1) : QByteArray());
	});
}

void Catalog::urlDownloaded(const File &file, const QByteArray &bytes) {
	for (auto i = begin(_downloading); i != end(_downloading); ++i) {
		if (i->url == file.url
			&& i->username == file.username
			&& i->generation == file.generation) {
			_downloading.erase(i);
			break;
		}
	}
	const auto hash = QCryptographicHash::hash(
		bytes,
		QCryptographicHash::Sha256).toHex().toLower();
	if (bytes.size() == file.size && hash == file.sha256) {
		// Whole or nothing: a half-written file is never read.
		const auto part = file.path + u".part"_q;
		auto output = QFile(part);
		if (output.open(QIODevice::WriteOnly | QIODevice::Truncate)
			&& output.write(bytes) == bytes.size()) {
			output.close();
			QFile::remove(file.path);
			QFile::rename(part, file.path);
		} else {
			output.close();
			QFile::remove(part);
		}
	} else if (!bytes.isEmpty()) {
		LOG(("Plugins Error: %1 does not match its size or SHA-256"
			).arg(file.url));
	}
	parse(file);
	downloadNext();
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
	} else if (category == u"integrations"_q) {
		return Translated("Integrations", "Интеграции");
	} else if (category == u"libraries"_q) {
		return Translated("Libraries", "Библиотеки");
	}
	return category;
}

} // namespace Plugins::Flua
