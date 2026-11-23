/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_package.h"

#include "plugins/runtime/flua_analyzer.h"

#include "base/zlib_help.h"
#include "logs.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QBuffer>
#include <QtGui/QImageReader>

namespace Plugins::Flua {
namespace {

constexpr auto kMaxIconSide = 1024;
constexpr auto kMaxFileSize = 4 * 1024 * 1024;
constexpr auto kMaxPackageSize = 16 * 1024 * 1024;
constexpr auto kMaxFiles = 512;

[[nodiscard]] bool GoodPath(const QString &path) {
	if (path.isEmpty()
		|| path.startsWith(u'/')
		|| path.contains(u'\\')
		|| path.contains(u':')) {
		return false;
	}
	for (const auto &part : path.split(u'/')) {
		if (part.isEmpty() || part == u"."_q || part == u".."_q) {
			return false;
		}
	}
	return true;
}

// Native code is not allowed anywhere in a package.
[[nodiscard]] bool NativeFile(const QString &path) {
	const auto lower = path.toLower();
	for (const auto ext : { ".dll", ".so", ".dylib", ".exe", ".pyd" }) {
		if (lower.endsWith(QString::fromLatin1(ext))) {
			return true;
		}
	}
	return false;
}

void ParseManifest(Package &package) {
	const auto i = package.files.find(u"manifest.json"_q);
	if (i == end(package.files)) {
		package.error = u"manifest.json is missing"_q;
		return;
	}
	auto parseError = QJsonParseError();
	const auto document = QJsonDocument::fromJson(i->second, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		package.error = u"manifest.json is not a JSON object"_q;
		return;
	}
	const auto json = document.object();
	auto &manifest = package.manifest;
	manifest.id = json.value(u"id"_q).toString();
	manifest.name = json.value(u"name"_q).toString();
	manifest.version = json.value(u"version"_q).toString();
	manifest.author = json.value(u"author"_q).toString();
	manifest.description = json.value(u"description"_q).toString();
	manifest.entry = json.value(u"entry"_q).toString(u"init.lua"_q);
	manifest.icon = json.value(u"icon"_q).toString();

	static const auto idCheck = QRegularExpression(
		u"^[a-z0-9][a-z0-9_.\\-]{1,63}$"_q);
	if (!idCheck.match(manifest.id).hasMatch()) {
		package.error = u"bad id, use 2-64 of a-z 0-9 _ . -"_q;
		return;
	} else if (manifest.name.isEmpty()) {
		package.error = u"name is missing"_q;
		return;
	} else if (!GoodPath(manifest.entry)
		|| !manifest.entry.endsWith(u".lua"_q)
		|| !package.files.contains(manifest.entry)) {
		package.error = u"entry file is missing: "_q + manifest.entry;
		return;
	} else if (!manifest.icon.isEmpty()
		&& (!GoodPath(manifest.icon)
			|| !package.files.contains(manifest.icon))) {
		package.error = u"icon file is missing: "_q + manifest.icon;
		return;
	}

	const auto api = json.value(u"api"_q).toObject();
	manifest.apiMin = api.value(u"min"_q).toInt(1);
	manifest.apiMax = api.value(u"max"_q).toInt(manifest.apiMin);
	if (manifest.apiMin > kApiVersion || manifest.apiMax < kApiVersion) {
		package.error = u"needs plugin API %1-%2, the client has %3"_q
			.arg(manifest.apiMin)
			.arg(manifest.apiMax)
			.arg(kApiVersion);
		return;
	}

	auto categories = json.value(u"categories"_q).toArray();
	if (const auto single = json.value(u"category"_q); single.isString()) {
		categories.push_back(single);
	}
	for (const auto value : categories) {
		const auto category = value.toString().trimmed().toLower();
		const auto &known = KnownCategories();
		if (std::find(begin(known), end(known), category) != end(known)
			&& std::find(
				begin(manifest.categories),
				end(manifest.categories),
				category) == end(manifest.categories)) {
			manifest.categories.push_back(category);
		}
	}

	for (const auto value : json.value(u"permissions"_q).toArray()) {
		const auto permission = value.toString();
		const auto &known = KnownPermissions();
		if (IsNetPermission(permission)) {
			if (!NetHostPattern(permission)) {
				package.error = u"bad network permission: "_q + permission;
				return;
			}
		} else if (std::find(begin(known), end(known), permission)
			== end(known)) {
			package.error = u"unknown permission: "_q + permission;
			return;
		}
		if (std::find(
				begin(manifest.permissions),
				end(manifest.permissions),
				permission) == end(manifest.permissions)) {
			manifest.permissions.push_back(permission);
		}
	}
}

[[nodiscard]] bool AddFile(
		Package &package,
		const QString &path,
		QByteArray &&content,
		int &total) {
	if (!GoodPath(path)) {
		package.error = u"bad file path in package: "_q + path;
		return false;
	} else if (NativeFile(path)) {
		package.error = u"native code is not allowed: "_q + path;
		return false;
	} else if (content.size() > kMaxFileSize) {
		package.error = u"file is too large: "_q + path;
		return false;
	} else if (int(package.files.size()) >= kMaxFiles) {
		package.error = u"too many files"_q;
		return false;
	} else if (package.files.contains(path)) {
		package.error = u"duplicate file: "_q + path;
		return false;
	}
	total += content.size();
	if (total > kMaxPackageSize) {
		package.error = u"package is too large"_q;
		return false;
	}
	package.files.emplace(path, std::move(content));
	return true;
}

} // namespace

const std::vector<QString> &KnownPermissions() {
	static const auto result = std::vector<QString>{
		QString::fromLatin1(kPermMessagesRead),
		QString::fromLatin1(kPermMessagesSend),
		QString::fromLatin1(kPermUi),
		QString::fromLatin1(kPermStorage),
		QString::fromLatin1(kPermLinks),
		QString::fromLatin1(kPermActions),
		QString::fromLatin1(kPermDisplay),
		QString::fromLatin1(kPermChatsRead),
		QString::fromLatin1(kPermCompose),
		QString::fromLatin1(kPermMtproto),
		QString::fromLatin1(kPermMessagesManage),
		QString::fromLatin1(kPermPlayer),
	};
	return result;
}

const std::vector<QString> &KnownCategories() {
	static const auto result = std::vector<QString>{
		u"appearance"_q,
		u"chats"_q,
		u"media"_q,
		u"privacy"_q,
		u"tools"_q,
		u"integrations"_q,
		u"fun"_q,
		u"libraries"_q,
	};
	return result;
}

bool IsNetPermission(const QString &permission) {
	return permission.startsWith(u"net:"_q);
}

std::optional<QString> NetHostPattern(const QString &permission) {
	if (!IsNetPermission(permission)) {
		return std::nullopt;
	}
	static const auto check = QRegularExpression(
		u"^(\\*\\.)?[a-z0-9]([a-z0-9\\-]*[a-z0-9])?"
		"(\\.[a-z0-9]([a-z0-9\\-]*[a-z0-9])?)+$"_q);
	const auto host = permission.mid(4);
	if (!check.match(host).hasMatch()) {
		return std::nullopt;
	}
	return host;
}

bool Manifest::hostAllowed(const QString &host) const {
	const auto lower = host.toLower();
	for (const auto &permission : permissions) {
		const auto pattern = NetHostPattern(permission);
		if (!pattern) {
			continue;
		} else if (pattern->startsWith(u"*."_q)) {
			// *.example.com: any subdomain, not example.com itself.
			if (lower.endsWith(pattern->mid(1))
				&& lower.size() > pattern->size() - 1) {
				return true;
			}
		} else if (lower == *pattern) {
			return true;
		}
	}
	return false;
}

bool Manifest::hasNetwork() const {
	for (const auto &permission : permissions) {
		if (IsNetPermission(permission)) {
			return true;
		}
	}
	return false;
}

bool Manifest::has(const char *permission) const {
	const auto name = QString::fromLatin1(permission);
	return std::find(begin(permissions), end(permissions), name)
		!= end(permissions);
}

Package ReadPackageFile(const QString &path) {
	auto result = Package{ .origin = path };
	auto file = QFile(path);
	if (file.size() > kMaxPackageSize || !file.open(QIODevice::ReadOnly)) {
		result.error = u"could not read the package file"_q;
		return result;
	}
	const auto bytes = file.readAll();
	file.close();

	auto zip = zlib::FileToRead(bytes);
	if (zip.goToFirstFile() != UNZ_OK) {
		result.error = u"not a .flua archive"_q;
		return result;
	}
	auto total = 0;
	while (true) {
		const auto name = zip.getCurrentFileName();
		if (name.isEmpty() || name.toUtf8().size() >= 128) {
			// getCurrentFileName() cuts names at 128 bytes.
			result.error = u"broken archive entry"_q;
			return result;
		}
		if (!name.endsWith(u'/')) {
			auto info = unz_file_info();
			zip.getCurrentFileInfo(&info, nullptr, 0, nullptr, 0, nullptr, 0);
			auto content = zip.readCurrentFileContent(kMaxFileSize);
			if (zip.error() != UNZ_OK
				|| content.size() != int(info.uncompressed_size)) {
				result.error = u"could not unpack: "_q + name;
				return result;
			} else if (!AddFile(result, name, std::move(content), total)) {
				return result;
			}
		}
		const auto next = zip.goToNextFile();
		if (next == UNZ_END_OF_LIST_OF_FILE) {
			break;
		} else if (next != UNZ_OK) {
			result.error = u"broken archive"_q;
			return result;
		}
	}
	ParseManifest(result);
	if (result.error.isEmpty()) {
		result.error = AnalyzePackage(result);
	}
	return result;
}

Package ReadPackageFolder(const QString &path) {
	auto result = Package{ .origin = path };
	const auto root = QDir(path);
	auto total = 0;
	auto it = QDirIterator(
		path,
		QDir::Files | QDir::NoSymLinks | QDir::Hidden,
		QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const auto full = it.next();
		const auto relative = root.relativeFilePath(full);
		if (relative.startsWith(u'.') || relative.contains(u"/."_q)) {
			continue; // .git, editor settings: not part of the plugin
		}
		auto file = QFile(full);
		if (file.size() > kMaxFileSize || !file.open(QIODevice::ReadOnly)) {
			result.error = u"could not read: "_q + relative;
			return result;
		}
		if (!AddFile(result, relative, file.readAll(), total)) {
			return result;
		}
	}
	ParseManifest(result);
	if (result.error.isEmpty()) {
		result.error = AnalyzePackage(result);
	}
	return result;
}

QImage ReadIcon(
		const std::map<QString, QByteArray> &files,
		const QString &path) {
	if (path.isEmpty()) {
		return QImage();
	}
	const auto i = files.find(path);
	if (i == end(files)) {
		return QImage();
	}
	auto bytes = i->second;
	QBuffer buffer(&bytes);
	if (!buffer.open(QIODevice::ReadOnly)) {
		return QImage();
	}
	QImageReader reader(&buffer);
	// The size first: a small file may unpack into a huge picture.
	const auto size = reader.size();
	if (!size.isValid()
		|| size.width() > kMaxIconSide
		|| size.height() > kMaxIconSide) {
		return QImage();
	}
	const auto result = reader.read();
	return result.isNull()
		? QImage()
		: result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

bool WritePackageFile(
		const std::map<QString, QByteArray> &files,
		const QString &path) {
	zlib::FileToWrite zip;
	zip_fileinfo zfi = { { 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
	for (const auto &[name, data] : files) {
		const auto utf8 = name.toUtf8();
		zip.openNewFile(
			utf8.constData(),
			&zfi,
			nullptr,
			0,
			nullptr,
			0,
			nullptr,
			Z_DEFLATED,
			Z_DEFAULT_COMPRESSION);
		zip.writeInFile(data.constData(), unsigned(data.size()));
		zip.closeFile();
	}
	zip.close();
	if (zip.error() != ZIP_OK) {
		LOG(("Plugins Error: could not pack %1, status: %2"
			).arg(path
			).arg(zip.error()));
		return false;
	}
	const auto bytes = zip.result();
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		&& (file.write(bytes) == bytes.size());
}

} // namespace Plugins::Flua
