/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "fuckgram/fuckgram_updater.h"

#include "fuckgram/fuckgram_build.h"
#include "base/call_delayed.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/zlib_help.h"
#include "core/application.h"
#include "lang/lang_instance.h"
#include "logs.h"
#include "settings.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <openssl/evp.h>

namespace Fuckgram {
namespace {

constexpr auto kRepository = "fucksynth/fuckgram";
constexpr auto kCheckPeriod = TimeId(24 * 60 * 60);
constexpr auto kFirstCheckDelay = 30 * crl::time(1000);
constexpr auto kTimerPeriod = 60 * 60 * crl::time(1000);
constexpr auto kToastDelay = 5 * crl::time(1000);
constexpr auto kMaxArchive = qint64(400) * 1024 * 1024;
constexpr auto kMaxSignature = qint64(1024);

// Every release archive is signed by this Ed25519 key: "<archive>.sig"
// next to it in the release is the base64 signature of the archive's bytes.
// The private key is kept only on the releaser's computer, not on GitHub:
// a release made by whoever gets into the account does not install.
constexpr auto kReleasePublicKey = std::array<unsigned char, 32>{
	0x43, 0x83, 0x28, 0x3f, 0x6e, 0xb4, 0xd6, 0xef,
	0xe4, 0x70, 0x08, 0x99, 0x74, 0x8d, 0x8f, 0x68,
	0xc2, 0x76, 0xf7, 0xb3, 0xf3, 0xc1, 0x34, 0x89,
	0x2c, 0x74, 0x43, 0x92, 0xd5, 0x25, 0xf7, 0x9c,
};

[[nodiscard]] bool GoodReleaseSignature(
		const QByteArray &data,
		const QByteArray &signature) {
	if (signature.size() != 64) {
		return false;
	}
	const auto key = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519,
		nullptr,
		kReleasePublicKey.data(),
		kReleasePublicKey.size());
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
constexpr auto kMaxProgram = 900 * 1024 * 1024;
constexpr auto kMinProgram = 1024 * 1024;
constexpr auto kMaxNotes = 6000;
constexpr auto kNotesDelay = 3 * crl::time(1000);

[[nodiscard]] QString Commit() {
	return QString::fromLatin1(FUCKGRAM_COMMIT).trimmed().toLower();
}

[[nodiscard]] TimeId CommitTime() {
	return TimeId(FUCKGRAM_COMMIT_TIME);
}

[[nodiscard]] QString SettingsPath() {
	return cWorkingDir() + u"tdata/fuckgram_updates.json"_q;
}

[[nodiscard]] QString ProgramPath() {
	return cExeDir() + cExeName();
}

[[nodiscard]] QString DownloadedPath() {
	return ProgramPath() + u".new"_q;
}

[[nodiscard]] QString ApiUrl(const QString &path) {
	return u"https://api.github.com/repos/%1/%2"_q.arg(
		QString::fromLatin1(kRepository),
		path);
}

[[nodiscard]] QString DownloadsUrl() {
	return u"https://github.com/%1/releases/download/"_q.arg(
		QString::fromLatin1(kRepository));
}

// The Windows x64 zip of a release: "fuckgram-v7.2.9-win-x64-portable.zip".
[[nodiscard]] bool WindowsArchive(const QString &name) {
	const auto lower = name.toLower();
	return lower.endsWith(u".zip"_q)
		&& lower.contains(u"win"_q)
		&& (lower.contains(u"x64"_q) || lower.contains(u"win64"_q));
}

// The release notes are Markdown: headings and **bold** become bold,
// "- " items get bullets, [text](url) and bare links are clickable.
[[nodiscard]] TextWithEntities NotesText(QString markdown) {
	markdown.replace(u"\r\n"_q, u"\n"_q);
	if (markdown.size() > kMaxNotes) {
		markdown = markdown.left(kMaxNotes) + QString::fromUtf8("\xE2\x80\xA6");
	}
	static const auto inline_ = QRegularExpression(
		u"\\*\\*(.+?)\\*\\*"
		"|\\[([^\\]]+)\\]\\((https?://[^)\\s]+)\\)"
		"|(https?://[^\\s<>()]+)"_q);
	auto result = TextWithEntities();
	auto first = true;
	for (auto line : markdown.split(u'\n')) {
		if (!first) {
			result.append(u"\n"_q);
		}
		first = false;
		auto trimmed = line.trimmed();
		auto heading = false;
		if (trimmed.startsWith(u'#')) {
			while (trimmed.startsWith(u'#')) {
				trimmed = trimmed.mid(1);
			}
			line = trimmed.trimmed();
			heading = true;
		} else if (trimmed.startsWith(u"- "_q) || trimmed.startsWith(u"* "_q)) {
			line = QString::fromUtf8("\xE2\x80\xA2 ") + trimmed.mid(2);
		}
		auto part = TextWithEntities();
		auto from = 0;
		auto matches = inline_.globalMatch(line);
		while (matches.hasNext()) {
			const auto match = matches.next();
			part.append(line.mid(from, match.capturedStart() - from));
			if (!match.captured(1).isEmpty()) {
				part.append(Ui::Text::Bold(match.captured(1)));
			} else if (!match.captured(2).isEmpty()) {
				part.append(Ui::Text::Link(
					match.captured(2),
					match.captured(3)));
			} else {
				const auto url = match.captured(4);
				part.append(Ui::Text::Link(url, url));
			}
			from = match.capturedEnd();
		}
		part.append(line.mid(from));
		if (heading) {
			part = Ui::Text::Wrapped(std::move(part), EntityType::Bold);
		}
		result.append(std::move(part));
	}
	return result;
}

void ShowNotesBox(
		const QString &title,
		const QString &notes,
		const QString &page,
		const QString &action,
		Fn<void()> callback) {
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		box->setWidth(st::boxWideWidth);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			st::boxLabel));
		label->setMarkedText(notes.trimmed().isEmpty()
			? TextWithEntities{ UpdateText(
				"The release has no description.",
				"У релиза нет описания.") }
			: NotesText(notes));
		label->setSelectable(true);
		box->addButton(rpl::single(action), [=] {
			box->closeBox();
			if (callback) {
				callback();
			}
		});
		if (!page.isEmpty()) {
			box->addLeftButton(rpl::single(QString("GitHub")), [=] {
				QDesktopServices::openUrl(QUrl(page));
			});
		}
	}));
}

void ShowToast(const QString &text) {
	if (const auto window = Core::App().activeWindow()) {
		window->showToast(text);
	}
}

enum class UnpackError {
	None,
	Size,
	Checksum,
	Signature,
	NoProgram,
	Open,
	Write,
};

struct UnpackResult {
	UnpackError error = UnpackError::None;
	QString details;
};

// Runs off the main thread: checks the archive and writes the program
// from it next to the running one. No interface texts here.
[[nodiscard]] UnpackResult Unpack(
		QByteArray archive,
		qint64 size,
		QByteArray sha256,
		QByteArray signature,
		QString programName,
		QString target) {
	if (archive.size() != size) {
		return { UnpackError::Size };
	} else if (!GoodReleaseSignature(archive, signature)) {
		return { UnpackError::Signature };
	}
	if (!sha256.isEmpty()) {
		const auto hash = QCryptographicHash::hash(
			archive,
			QCryptographicHash::Sha256).toHex().toLower();
		if (hash != sha256) {
			return { UnpackError::Checksum };
		}
	}
	// fuckgram.exe or the program of the same name as the running one,
	// nothing else; the archive may keep it in a folder.
	auto zip = zlib::FileToRead(archive);
	auto program = QByteArray();
	if (zip.goToFirstFile() == UNZ_OK) {
		do {
			const auto path = zip.getCurrentFileName();
			const auto name = path.mid(path.lastIndexOf(u'/') + 1);
			if (!name.endsWith(u".exe"_q, Qt::CaseInsensitive)
				|| !name.compare(u"Updater.exe"_q, Qt::CaseInsensitive)) {
				continue;
			}
			const auto same = !name.compare(programName, Qt::CaseInsensitive)
				|| !name.compare(u"fuckgram.exe"_q, Qt::CaseInsensitive);
			if (same) {
				program = zip.readCurrentFileContent(kMaxProgram);
				break;
			}
		} while (zip.goToNextFile() == UNZ_OK);
	}
	if (program.size() < kMinProgram || !program.startsWith("MZ")) {
		return { UnpackError::NoProgram };
	}
	QFile::remove(target);
	auto file = QFile(target);
	if (!file.open(QIODevice::WriteOnly)) {
		return { UnpackError::Open, file.errorString() };
	} else if (file.write(program) != program.size() || !file.flush()) {
		const auto error = file.errorString();
		file.close();
		QFile::remove(target);
		return { UnpackError::Write, error };
	}
	file.close();
	return {};
}

[[nodiscard]] QString UnpackErrorText(const UnpackResult &result) {
	switch (result.error) {
	case UnpackError::None: return QString();
	case UnpackError::Size:
		return UpdateText(
			"the downloaded file has a wrong size",
			"у скачанного файла не тот размер");
	case UnpackError::Checksum:
		return UpdateText(
			"the checksum of the downloaded file does not match",
			"контрольная сумма скачанного файла не совпала");
	case UnpackError::Signature:
		return UpdateText(
			"the release is not signed by fuckgram, it is not installed",
			"релиз не подписан fuckgram, он не ставится");
	case UnpackError::NoProgram:
		return UpdateText(
			"there is no program in the archive",
			"в архиве нет программы");
	case UnpackError::Open:
		return UpdateText(
			"could not write to the program folder: ",
			"не удалось записать в папку программы: ") + result.details;
	case UnpackError::Write:
		return UpdateText(
			"could not write the new version: ",
			"не удалось записать новую версию: ") + result.details;
	}
	Unexpected("Error in UnpackErrorText.");
}

class Updater final : public QObject {
public:
	explicit Updater(QObject *parent);

	[[nodiscard]] const UpdateStatus &status() const;
	[[nodiscard]] rpl::producer<UpdateStatus> value() const;
	[[nodiscard]] bool autoUpdates() const;
	void setAutoUpdates(bool enabled);

	void start();
	void check(bool manual);
	void install();
	void showWhatsNew();

private:
	struct Release {
		QString id;
		QString tag;
		QString page;
		QString notes;
		TimeId published = 0;
		QString archiveUrl;
		qint64 archiveSize = 0;
		QByteArray sha256; // hex, lowercase; empty if GitHub gave none
		QString signatureUrl; // "<archive>.sig"
	};

	void setState(UpdateState state);
	void changed();
	void readSettings();
	void writeSettings() const;
	void checkIfDue();
	void fail(const QString &error);
	void latest(const Release &release, bool same);
	void cleanup();
	void restart();
	[[nodiscard]] QNetworkReply *get(
		const QString &url,
		const QByteArray &accept = QByteArray());
	void gotRelease(const QByteArray &bytes);
	void gotTagCommit(const QString &commit);
	void download();
	void downloadArchive();
	void downloaded(const QByteArray &bytes);
	void unpacked(const QString &error);

	QNetworkAccessManager _manager;
	UpdateStatus _status;
	rpl::event_stream<> _changes;
	base::Timer _timer;
	QPointer<QNetworkReply> _reply;
	Release _release;
	QByteArray _signature;
	int _percent = -1;
	bool _started = false;
	bool _manual = false;
	bool _swapped = false;

	// Kept in tdata/fuckgram_updates.json.
	bool _auto = true;
	TimeId _lastCheck = 0;
	QString _installedId; // the release this program came from
	QString _installedTag;
	QString _installedNotes;
	QString _installedPage;
	QString _justInstalled; // a tag to tell about after the restart
	QString _readyId; // the release downloaded and waiting
	QString _readyTag;
	QString _readyNotes;
	QString _readyPage;
	qint64 _readySize = 0;

};

Updater::Updater(QObject *parent)
: QObject(parent)
, _timer([=] { checkIfDue(); }) {
	_status.commit = Commit();
#ifdef Q_OS_WIN
	const auto supported = !_status.commit.isEmpty();
#else // Q_OS_WIN
	const auto supported = false;
#endif // Q_OS_WIN
	_status.state = supported ? UpdateState::Idle : UpdateState::Disabled;
	readSettings();
	_status.current = _installedTag;
	_status.notes = _installedNotes;
	_status.page = _installedPage;
}

const UpdateStatus &Updater::status() const {
	return _status;
}

rpl::producer<UpdateStatus> Updater::value() const {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		_changes.events()
	) | rpl::map([=] {
		return _status;
	});
}

bool Updater::autoUpdates() const {
	return _auto;
}

void Updater::setAutoUpdates(bool enabled) {
	if (_auto == enabled) {
		return;
	}
	_auto = enabled;
	writeSettings();
	changed();
	if (enabled) {
		checkIfDue();
	}
}

void Updater::readSettings() {
	auto file = QFile(SettingsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto json = QJsonDocument::fromJson(file.readAll()).object();
	const auto text = [&](const char *key) {
		return json.value(QString::fromLatin1(key)).toString();
	};
	_auto = json.value(u"auto"_q).toBool(true);
	_lastCheck = TimeId(json.value(u"last_check"_q).toDouble());
	_installedId = text("installed_id");
	_installedTag = text("installed_tag");
	_installedNotes = text("installed_notes");
	_installedPage = text("installed_page");
	_justInstalled = text("just_installed");
	_readyId = text("ready_id");
	_readyTag = text("ready_tag");
	_readyNotes = text("ready_notes");
	_readyPage = text("ready_page");
	_readySize = qint64(json.value(u"ready_size"_q).toDouble());

	// The program was replaced by hand: what was kept is about another.
	// After an update the new program finds "just_installed" instead.
	if (text("commit") != Commit() && _justInstalled.isEmpty()) {
		_installedId = _installedTag = _justInstalled = QString();
		_installedNotes = _installedPage = QString();
		_readyId = _readyTag = _readyNotes = _readyPage = QString();
		_readySize = 0;
		_lastCheck = 0;
	}
}

void Updater::writeSettings() const {
	auto json = QJsonObject();
	json.insert(u"auto"_q, _auto);
	json.insert(u"last_check"_q, double(_lastCheck));
	json.insert(u"commit"_q, Commit());
	json.insert(u"installed_id"_q, _installedId);
	json.insert(u"installed_tag"_q, _installedTag);
	json.insert(u"installed_notes"_q, _installedNotes);
	json.insert(u"installed_page"_q, _installedPage);
	json.insert(u"just_installed"_q, _justInstalled);
	json.insert(u"ready_id"_q, _readyId);
	json.insert(u"ready_tag"_q, _readyTag);
	json.insert(u"ready_notes"_q, _readyNotes);
	json.insert(u"ready_page"_q, _readyPage);
	json.insert(u"ready_size"_q, double(_readySize));
	auto file = QFile(SettingsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
	}
}

void Updater::start() {
	if (_started) {
		return;
	}
	_started = true;
	if (_status.state == UpdateState::Disabled) {
		cleanup();
		return;
	}
	// An update downloaded in an earlier run and not installed yet.
	const auto downloaded = QFileInfo(DownloadedPath());
	if (!_readyId.isEmpty()
		&& _readyId != _installedId
		&& downloaded.exists()
		&& downloaded.size() == _readySize) {
		_status.available = _readyTag;
		_status.notes = _readyNotes;
		_status.page = _readyPage;
		// Widgets made before the start learn it too.
		setState(UpdateState::Ready);
	} else if (!_readyId.isEmpty() || _readySize) {
		_readyId = _readyTag = _readyNotes = _readyPage = QString();
		_readySize = 0;
		writeSettings();
	}
	cleanup();
	if (!_justInstalled.isEmpty()) {
		const auto tag = base::take(_justInstalled);
		writeSettings();
		LOG(("Updater: updated to %1").arg(tag));
		const auto notes = _installedNotes;
		const auto page = _installedPage;
		base::call_delayed(kNotesDelay, this, [=] {
			ShowNotesBox(
				UpdateText(
					"fuckgram is updated to %1",
					"fuckgram обновлён до версии %1").arg(tag),
				notes,
				page,
				UpdateText("Great", "Отлично"),
				nullptr);
		});
	}
	_timer.callEach(kTimerPeriod);
	base::call_delayed(kFirstCheckDelay, this, [=] {
		cleanup();
		checkIfDue();
	});
}

// The previous program stays as "<name>.old" after an update: it was
// running, so it could only be renamed. It is free after the restart.
void Updater::cleanup() {
	const auto program = QFileInfo(ProgramPath());
	auto folder = QDir(program.absolutePath());
	const auto old = folder.entryList(
		{ program.fileName() + u".old*"_q },
		QDir::Files);
	for (const auto &name : old) {
		QFile::remove(folder.filePath(name));
	}
	if (_status.state != UpdateState::Ready
		&& _status.state != UpdateState::Downloading) {
		QFile::remove(DownloadedPath());
	}
}

void Updater::checkIfDue() {
	if (!_auto
		|| _status.state == UpdateState::Disabled
		|| (base::unixtime::now() - _lastCheck) < kCheckPeriod) {
		return;
	}
	check(false);
}

void Updater::setState(UpdateState state) {
	_status.state = state;
	changed();
}

void Updater::changed() {
	_changes.fire({});
}

void Updater::fail(const QString &error) {
	LOG(("Updater Error: %1").arg(error));
	_status.error = error;
	setState(UpdateState::Failed);
	if (_manual) {
		ShowToast(UpdateText(
			"Could not update: ",
			"Не удалось обновиться: ") + error);
	}
}

QNetworkReply *Updater::get(const QString &url, const QByteArray &accept) {
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "fuckgram-updater");
	if (!accept.isEmpty()) {
		request.setRawHeader("Accept", accept);
	}
	// An aborted request finishes right away: it is not current by then.
	if (const auto previous = base::take(_reply)) {
		previous->abort();
	}
	_reply = _manager.get(request);
	return _reply.data();
}

void Updater::check(bool manual) {
	switch (_status.state) {
	case UpdateState::Disabled:
		if (manual) {
			ShowToast(UpdateText(
				"This build does not update itself: "
				"only release builds do.",
				"Эта сборка не обновляется сама: "
				"обновляются только релизные сборки."));
		}
		return;
	case UpdateState::Checking:
	case UpdateState::Downloading:
		return;
	case UpdateState::Ready:
		if (manual) {
			install();
		}
		return;
	default:
		break;
	}
	_manual = manual;
	_status.error = QString();
	_status.received = _status.total = 0;
	setState(UpdateState::Checking);

	// The newest release that is not a draft and not a pre-release.
	const auto reply = get(
		ApiUrl(u"releases/latest"_q),
		"application/vnd.github+json");
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (_reply != reply) {
			return;
		}
		_reply = nullptr;
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code == 404) {
			// No releases yet.
			latest({}, false);
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString());
			return;
		}
		gotRelease(reply->readAll());
	});
}

// `same`: this program is the release, not only newer than it.
void Updater::latest(const Release &release, bool same) {
	if (same && !release.id.isEmpty() && _installedId != release.id) {
		// Say in the settings which release this is.
		_installedId = release.id;
		_installedTag = _status.current = release.tag;
		_installedNotes = _status.notes = release.notes;
		_installedPage = _status.page = release.page;
	}
	_lastCheck = base::unixtime::now();
	writeSettings();
	setState(UpdateState::Latest);
	if (_manual) {
		ShowToast(UpdateText(
			"fuckgram is up to date.",
			"У вас последняя версия fuckgram."));
	}
}

void Updater::gotRelease(const QByteArray &bytes) {
	const auto json = QJsonDocument::fromJson(bytes).object();
	auto release = Release{
		.id = QString::number(qint64(json.value(u"id"_q).toDouble())),
		.tag = json.value(u"tag_name"_q).toString(),
		.page = json.value(u"html_url"_q).toString(),
		.notes = json.value(u"body"_q).toString(),
		.published = TimeId(QDateTime::fromString(
			json.value(u"published_at"_q).toString(),
			Qt::ISODate).toSecsSinceEpoch()),
	};
	if (release.tag.isEmpty() || release.id == u"0"_q) {
		fail(UpdateText(
			"GitHub answered something unexpected",
			"GitHub ответил что-то не то"));
		return;
	}
	for (const auto value : json.value(u"assets"_q).toArray()) {
		const auto asset = value.toObject();
		const auto name = asset.value(u"name"_q).toString();
		const auto url = asset.value(u"browser_download_url"_q).toString();
		if (!WindowsArchive(name) || !url.startsWith(DownloadsUrl())) {
			continue;
		}
		const auto digest = asset.value(u"digest"_q).toString().toLower();
		release.archiveUrl = url;
		release.archiveSize = qint64(asset.value(u"size"_q).toDouble());
		release.sha256 = digest.startsWith(u"sha256:"_q)
			? digest.mid(7).toLatin1()
			: QByteArray();
		break;
	}
	for (const auto value : json.value(u"assets"_q).toArray()) {
		const auto asset = value.toObject();
		const auto name = asset.value(u"name"_q).toString();
		const auto url = asset.value(u"browser_download_url"_q).toString();
		if (!release.archiveUrl.isEmpty()
			&& url.startsWith(DownloadsUrl())
			&& url == release.archiveUrl + u".sig"_q) {
			release.signatureUrl = url;
			break;
		}
	}
	_status.available = release.tag;
	_release = release;

	if (release.id == _installedId) {
		latest(release, true);
		return;
	} else if (release.published > 0 && release.published <= CommitTime()) {
		// This program is newer than the release.
		latest(release, false);
		return;
	} else if (release.archiveUrl.isEmpty()) {
		fail(UpdateText(
			"the newest release has no build for Windows x64",
			"в последнем релизе нет сборки для Windows x64"));
		return;
	} else if (release.signatureUrl.isEmpty()) {
		fail(UpdateText(
			"the newest release is not signed",
			"последний релиз не подписан"));
		return;
	} else if (release.archiveSize <= 0
		|| release.archiveSize > kMaxArchive
		|| (!release.sha256.isEmpty() && release.sha256.size() != 64)) {
		fail(UpdateText(
			"the archive of the release looks wrong",
			"архив релиза выглядит не так"));
		return;
	}

	// The commit of the tag: a program made of it needs no update.
	const auto reply = get(
		ApiUrl(u"commits/"_q
			+ QString::fromLatin1(QUrl::toPercentEncoding(release.tag))),
		"application/vnd.github.sha");
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (_reply != reply) {
			return;
		}
		_reply = nullptr;
		const auto good = (reply->error() == QNetworkReply::NoError);
		gotTagCommit(good
			? QString::fromLatin1(reply->read(64)).trimmed().toLower()
			: QString());
	});
}

void Updater::gotTagCommit(const QString &commit) {
	if (!commit.isEmpty() && commit == Commit()) {
		latest(_release, true);
		return;
	}
	download();
}

void Updater::download() {
	_status.received = 0;
	_status.total = _release.archiveSize;
	_percent = -1;
	_signature = QByteArray();
	setState(UpdateState::Downloading);

	// The signature first: it is tiny, and without it nothing installs.
	const auto reply = get(_release.signatureUrl);
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (_reply != reply) {
			return;
		}
		_reply = nullptr;
		if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString());
			return;
		}
		_signature = QByteArray::fromBase64(
			reply->read(kMaxSignature).trimmed());
		downloadArchive();
	});
}

void Updater::downloadArchive() {
	const auto reply = get(_release.archiveUrl);
	connect(reply, &QNetworkReply::downloadProgress, this, [=](
			qint64 received,
			qint64 total) {
		if (_reply != reply) {
			return;
		} else if (received > kMaxArchive) {
			reply->abort();
			return;
		}
		_status.received = received;
		if (total > 0) {
			_status.total = total;
		}
		const auto percent = (_status.total > 0)
			? int(received * 100 / _status.total)
			: 0;
		if (_percent != percent) {
			_percent = percent;
			changed();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (_reply != reply) {
			return;
		}
		_reply = nullptr;
		if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString());
			return;
		}
		downloaded(reply->readAll());
	});
}

void Updater::downloaded(const QByteArray &bytes) {
	const auto size = _release.archiveSize;
	const auto sha256 = _release.sha256;
	const auto signature = _signature;
	const auto name = QFileInfo(ProgramPath()).fileName();
	const auto target = DownloadedPath();
	const auto guard = QPointer<Updater>(this);
	crl::async([=] {
		const auto result = Unpack(
			bytes,
			size,
			sha256,
			signature,
			name,
			target);
		crl::on_main([=] {
			if (const auto strong = guard.data()) {
				strong->unpacked(UnpackErrorText(result));
			}
		});
	});
}

void Updater::unpacked(const QString &error) {
	if (_status.state != UpdateState::Downloading) {
		return;
	} else if (!error.isEmpty()) {
		QFile::remove(DownloadedPath());
		fail(error);
		return;
	}
	LOG(("Updater: %1 is ready").arg(_release.tag));
	_lastCheck = base::unixtime::now();
	_readyId = _release.id;
	_readyTag = _release.tag;
	_readyNotes = _status.notes = _release.notes;
	_readyPage = _status.page = _release.page;
	_readySize = QFileInfo(DownloadedPath()).size();
	writeSettings();
	setState(UpdateState::Ready);
	ShowToast(UpdateText(
		"fuckgram %1 is downloaded: "
		"press \"Update fuckgram\" under the chat list.",
		"fuckgram %1 скачан: "
		"нажмите «Обновить fuckgram» под списком чатов."
	).arg(_release.tag));
}

// The running program can not be overwritten on Windows, but can be
// renamed: it becomes "<name>.old" and the downloaded one takes its name.
void Updater::install() {
	if (_status.state != UpdateState::Ready) {
		return;
	} else if (_swapped) {
		// The files are in place already, the quit was cancelled before.
		restart();
		return;
	}
	const auto program = ProgramPath();
	const auto fresh = DownloadedPath();
	if (!QFile::exists(fresh)) {
		_readyId = _readyTag = QString();
		_readySize = 0;
		writeSettings();
		fail(UpdateText(
			"the downloaded update is gone",
			"скачанное обновление пропало"));
		return;
	}
	auto old = program + u".old"_q;
	if (QFile::exists(old) && !QFile::remove(old)) {
		old += QString::number(base::unixtime::now());
	}
	if (!QFile::rename(program, old)) {
		fail(UpdateText(
			"could not move the current version aside",
			"не удалось отодвинуть текущую версию"));
		return;
	} else if (!QFile::rename(fresh, program)) {
		if (!QFile::rename(old, program) && !QFile::copy(old, program)) {
			LOG(("Updater Error: could not put %1 back").arg(program));
		}
		fail(UpdateText(
			"could not put the new version in place",
			"не удалось поставить новую версию"));
		return;
	}
	_swapped = true;
	// The new program reads this: it is the release just installed.
	_installedId = _readyId;
	_installedTag = _justInstalled = _readyTag;
	_installedNotes = _readyNotes;
	_installedPage = _readyPage;
	_readyId = _readyTag = _readyNotes = _readyPage = QString();
	_readySize = 0;
	writeSettings();
	LOG(("Updater: %1 is in place, restarting").arg(_installedTag));
	restart();
}

// A quit can be cancelled (a download or an upload is going): then the
// client goes on, and only the next press of the button restarts it.
void Updater::restart() {
	cSetRestarting(true);
	cSetRestartingToSettings(false);
	Core::Quit();
	if (!Core::Quitting()) {
		cSetRestarting(false);
	}
}

void Updater::showWhatsNew() {
	if (_status.state == UpdateState::Ready) {
		ShowNotesBox(
			UpdateText("What's new in %1", "Что нового в %1").arg(
				_status.available),
			_status.notes,
			_status.page,
			UpdateText("Restart and update", "Перезапустить и обновить"),
			[=] { install(); });
	} else if (!_installedTag.isEmpty()) {
		ShowNotesBox(
			UpdateText("What's new in %1", "Что нового в %1").arg(
				_installedTag),
			_installedNotes,
			_installedPage,
			UpdateText("Close", "Закрыть"),
			nullptr);
	} else {
		QDesktopServices::openUrl(QUrl(u"https://github.com/%1/releases"_q
			.arg(QString::fromLatin1(kRepository))));
	}
}

[[nodiscard]] Updater *Instance() {
	static auto instance = QPointer<Updater>();
	if (!instance) {
		instance = new Updater(QCoreApplication::instance());
	}
	return instance.data();
}

} // namespace

void StartUpdater() {
	Instance()->start();
}

void CheckForUpdates(bool manual) {
	Instance()->check(manual);
}

void InstallUpdate() {
	Instance()->install();
}

void ShowWhatsNew() {
	Instance()->showWhatsNew();
}

UpdateStatus CurrentUpdateStatus() {
	return Instance()->status();
}

rpl::producer<UpdateStatus> UpdateStatusValue() {
	return Instance()->value();
}

bool AutoUpdates() {
	return Instance()->autoUpdates();
}

void SetAutoUpdates(bool enabled) {
	Instance()->setAutoUpdates(enabled);
}

QString UpdateStatusText(const UpdateStatus &status) {
	switch (status.state) {
	case UpdateState::Disabled:
		return UpdateText("Release builds only", "Только релизные сборки");
	case UpdateState::Idle:
	case UpdateState::Latest:
		if (!status.current.isEmpty()) {
			return UpdateText("Version %1", "Версия %1").arg(status.current);
		}
		return UpdateText("Build %1", "Сборка %1").arg(
			status.commit.left(7));
	case UpdateState::Checking:
		return UpdateText("Checking…", "Проверка…");
	case UpdateState::Downloading: {
		const auto percent = (status.total > 0)
			? int(status.received * 100 / status.total)
			: 0;
		return UpdateText(
			"Downloading %1%",
			"Загрузка %1%").arg(std::clamp(percent, 0, 100));
	}
	case UpdateState::Ready:
		return UpdateText(
			"Version %1 is ready",
			"Версия %1 готова").arg(status.available);
	case UpdateState::Failed:
		return UpdateText("Error", "Ошибка");
	}
	Unexpected("State in UpdateStatusText.");
}

QString UpdateText(const char *english, const char *russian) {
	const auto &instance = Lang::GetInstance();
	const auto id = instance.id();
	const auto russianInterface = id.startsWith(u"ru"_q)
		|| (id.startsWith(u'#') && instance.baseId().startsWith(u"ru"_q));
	return QString::fromUtf8(russianInterface ? russian : english);
}

} // namespace Fuckgram
