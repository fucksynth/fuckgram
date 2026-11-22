/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_package.h"

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

namespace Plugins::Flua {
namespace {

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

	static const auto idCheck = QRegularExpression(
		u"^[a-z0-9][a-z0-9_.\\-]{1,63}$"_q);
	if (!idCheck.match(manifest.id).hasMatch()) {
		package.error = u"bad id, use 2-64 of a-z 0-9 _ . -"_q;
		return;
	} else if (manifest.name.isEmpty()) {
		package.error = u"name is missing"_q;
		return;
	} else if (!GoodPath(manifest.entry)
		|| !package.files.contains(manifest.entry)) {
		package.error = u"entry file is missing: "_q + manifest.entry;
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

	for (const auto value : json.value(u"permissions"_q).toArray()) {
		const auto permission = value.toString();
		const auto &known = KnownPermissions();
		if (std::find(begin(known), end(known), permission) == end(known)) {
			package.error = u"unknown permission: "_q + permission;
			return;
		}
		manifest.permissions.push_back(permission);
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
	};
	return result;
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
		if (name.isEmpty()) {
			result.error = u"broken archive entry"_q;
			return result;
		}
		if (!name.endsWith(u'/')) {
			auto content = zip.readCurrentFileContent(kMaxFileSize);
			if (zip.error() != UNZ_OK) {
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
	return result;
}

} // namespace Plugins::Flua
