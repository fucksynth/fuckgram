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
#include "window/window_controller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Fuckgram {
namespace {

constexpr auto kRepository = "fucksynth/fuckgram";
constexpr auto kArchive = "fuckgram-win-x64.zip";
constexpr auto kDescription = "update.json";
constexpr auto kTagPrefix = "build-";
constexpr auto kCheckPeriod = TimeId(24 * 60 * 60);
constexpr auto kFirstCheckDelay = 30 * crl::time(1000);
constexpr auto kTimerPeriod = 60 * 60 * crl::time(1000);
constexpr auto kToastDelay = 5 * crl::time(1000);
constexpr auto kMaxArchive = qint64(400) * 1024 * 1024;
constexpr auto kMaxProgram = 900 * 1024 * 1024;
constexpr auto kMinProgram = 1024 * 1024;

[[nodiscard]] QString SettingsPath() {
	return cWorkingDir() + u"tdata/fuckgram_updates.json"_q;
}

[[nodiscard]] QString ProgramPath() {
	return cExeDir() + cExeName();
}

[[nodiscard]] QString DownloadedPath() {
	return ProgramPath() + u".new"_q;
}

[[nodiscard]] QString ReleasesUrl() {
	return u"https://github.com/%1/releases/download/"_q.arg(kRepository);
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
		QString target) {
	if (archive.size() != size) {
		return { UnpackError::Size };
	}
	const auto hash = QCryptographicHash::hash(
		archive,
		QCryptographicHash::Sha256).toHex().toLower();
	if (hash != sha256) {
		return { UnpackError::Checksum };
	}
	auto zip = zlib::FileToRead(archive);
	auto program = QByteArray();
	if (zip.goToFirstFile() == UNZ_OK) {
		do {
			const auto name = zip.getCurrentFileName();
			if (name.endsWith(u".exe"_q, Qt::CaseInsensitive)
				&& !name.contains(u'/')
				&& !name.contains(u'\\')
				&& name.compare(u"Updater.exe"_q, Qt::CaseInsensitive)) {
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

private:
	void restart();
	void setState(UpdateState state);
	void changed();
	void readSettings();
	void writeSettings() const;
	void checkIfDue();
	void fail(const QString &error);
	void cleanup();
	[[nodiscard]] QNetworkReply *get(const QString &url, bool api);
	void gotRelease(const QByteArray &bytes);
	void gotDescription(const QByteArray &bytes);
	void download();
	void downloaded(const QByteArray &bytes);
	void unpacked(const QString &error);

	QNetworkAccessManager _manager;
	UpdateStatus _status;
	rpl::event_stream<> _changes;
	base::Timer _timer;
	bool _started = false;
	bool _auto = true;
	bool _manual = false;
	TimeId _lastCheck = 0;
	int _installed = 0;
	// The downloaded program waiting for a restart, kept between runs.
	int _readyBuild = 0;
	qint64 _readySize = 0;
	bool _swapped = false;
	QString _archiveUrl;
	qint64 _archiveSize = 0;
	QString _descriptionUrl;
	QByteArray _sha256; // hex, lowercase
	QPointer<QNetworkReply> _reply;
	int _percent = -1;

};

Updater::Updater(QObject *parent)
: QObject(parent)
, _timer([=] { checkIfDue(); }) {
	_status.current = CurrentBuild();
#ifdef Q_OS_WIN
	const auto supported = (_status.current > 0);
#else // Q_OS_WIN
	const auto supported = false;
#endif // Q_OS_WIN
	_status.state = supported ? UpdateState::Idle : UpdateState::Disabled;
	readSettings();
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
	_auto = json.value(u"auto"_q).toBool(true);
	_lastCheck = TimeId(json.value(u"last_check"_q).toDouble());
	_installed = json.value(u"installed"_q).toInt();
	_readyBuild = json.value(u"ready_build"_q).toInt();
	_readySize = qint64(json.value(u"ready_size"_q).toDouble());
}

void Updater::writeSettings() const {
	auto json = QJsonObject();
	json.insert(u"auto"_q, _auto);
	json.insert(u"last_check"_q, double(_lastCheck));
	json.insert(u"installed"_q, _installed);
	json.insert(u"ready_build"_q, _readyBuild);
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
	// An update downloaded in an earlier run and not installed yet.
	const auto downloaded = QFileInfo(DownloadedPath());
	if (_status.state != UpdateState::Disabled
		&& _readyBuild > _status.current
		&& downloaded.exists()
		&& downloaded.size() == _readySize) {
		_status.available = _readyBuild;
		// Widgets made before the start learn it too.
		setState(UpdateState::Ready);
	} else if (_readyBuild || _readySize) {
		_readyBuild = 0;
		_readySize = 0;
		writeSettings();
	}
	cleanup();
	if (_status.state == UpdateState::Disabled) {
		return;
	}
	if (_installed != _status.current) {
		const auto was = _installed;
		_installed = _status.current;
		writeSettings();
		if (was > 0 && was < _status.current) {
			LOG(("Updater: updated from build %1 to %2"
				).arg(was
				).arg(_status.current));
			const auto build = _status.current;
			base::call_delayed(kToastDelay, this, [=] {
				ShowToast(UpdateText(
					"fuckgram is updated to build %1",
					"fuckgram обновлён до сборки %1").arg(build));
			});
		}
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

QNetworkReply *Updater::get(const QString &url, bool api) {
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "fuckgram-updater");
	if (api) {
		request.setRawHeader("Accept", "application/vnd.github+json");
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

	const auto reply = get(
		u"https://api.github.com/repos/%1/releases/latest"_q.arg(
			kRepository),
		true);
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
			_lastCheck = base::unixtime::now();
			writeSettings();
			setState(UpdateState::Latest);
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			fail(reply->errorString());
			return;
		}
		gotRelease(reply->readAll());
	});
}

void Updater::gotRelease(const QByteArray &bytes) {
	const auto release = QJsonDocument::fromJson(bytes).object();
	const auto tag = release.value(u"tag_name"_q).toString();
	const auto prefix = QString::fromLatin1(kTagPrefix);
	const auto build = tag.startsWith(prefix)
		? tag.mid(prefix.size()).toInt()
		: 0;
	if (build <= 0) {
		fail(UpdateText(
			"the newest release is not a build: ",
			"последний релиз — не сборка: ") + tag);
		return;
	}
	_status.available = build;
	_status.page = release.value(u"html_url"_q).toString();
	if (build <= _status.current) {
		_lastCheck = base::unixtime::now();
		writeSettings();
		setState(UpdateState::Latest);
		if (_manual) {
			ShowToast(UpdateText(
				"fuckgram is up to date.",
				"У вас последняя версия fuckgram."));
		}
		return;
	}

	_archiveUrl = _descriptionUrl = QString();
	_archiveSize = 0;
	for (const auto value : release.value(u"assets"_q).toArray()) {
		const auto asset = value.toObject();
		const auto name = asset.value(u"name"_q).toString();
		const auto url = asset.value(u"browser_download_url"_q).toString();
		if (!url.startsWith(ReleasesUrl())) {
			continue;
		} else if (name == QString::fromLatin1(kArchive)) {
			_archiveUrl = url;
			_archiveSize = qint64(asset.value(u"size"_q).toDouble());
		} else if (name == QString::fromLatin1(kDescription)) {
			_descriptionUrl = url;
		}
	}
	if (_archiveUrl.isEmpty() || _descriptionUrl.isEmpty()) {
		fail(UpdateText(
			"the release has no files for Windows",
			"в релизе нет файлов для Windows"));
		return;
	}
	const auto reply = get(_descriptionUrl, false);
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
		gotDescription(reply->readAll());
	});
}

void Updater::gotDescription(const QByteArray &bytes) {
	const auto json = QJsonDocument::fromJson(bytes).object();
	const auto build = json.value(u"build"_q).toInt();
	const auto size = qint64(json.value(u"size"_q).toDouble());
	const auto sha256 = json.value(u"sha256"_q).toString().toLower().toLatin1();
	if (build != _status.available
		|| size != _archiveSize
		|| size <= 0
		|| size > kMaxArchive
		|| sha256.size() != 64) {
		fail(UpdateText(
			"the description of the update does not match the release",
			"описание обновления не совпадает с релизом"));
		return;
	}
	_sha256 = sha256;
	download();
}

void Updater::download() {
	_status.received = 0;
	_status.total = _archiveSize;
	_percent = -1;
	setState(UpdateState::Downloading);

	const auto reply = get(_archiveUrl, false);
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
	const auto size = _archiveSize;
	const auto sha256 = _sha256;
	const auto target = DownloadedPath();
	const auto guard = QPointer<Updater>(this);
	crl::async([=] {
		const auto result = Unpack(bytes, size, sha256, target);
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
	LOG(("Updater: build %1 is ready").arg(_status.available));
	_lastCheck = base::unixtime::now();
	_readyBuild = _status.available;
	_readySize = QFileInfo(DownloadedPath()).size();
	writeSettings();
	setState(UpdateState::Ready);
	ShowToast(UpdateText(
		"The fuckgram update is downloaded: "
		"press \"Update fuckgram\" under the chat list.",
		"Обновление fuckgram скачано: "
		"нажмите «Обновить fuckgram» под списком чатов."));
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
	_readyBuild = 0;
	_readySize = 0;
	writeSettings();
	LOG(("Updater: build %1 is in place, restarting"
		).arg(_status.available));
	restart();
}

// A quit can be cancelled (a download or an upload is going): then the
// client goes on, and only the next quit of this button restarts it.
void Updater::restart() {
	cSetRestarting(true);
	cSetRestartingToSettings(false);
	Core::Quit();
	if (!Core::Quitting()) {
		cSetRestarting(false);
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

int CurrentBuild() {
	return FUCKGRAM_BUILD;
}

void StartUpdater() {
	Instance()->start();
}

void CheckForUpdates(bool manual) {
	Instance()->check(manual);
}

void InstallUpdate() {
	Instance()->install();
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
		return UpdateText("Build %1", "Сборка %1").arg(status.current);
	case UpdateState::Checking:
		return UpdateText("Checking…", "Проверка…");
	case UpdateState::Latest:
		return UpdateText("Up to date", "Последняя версия");
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
			"Build %1 is ready",
			"Сборка %1 готова").arg(status.available);
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
