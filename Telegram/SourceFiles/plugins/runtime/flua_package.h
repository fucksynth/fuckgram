/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

namespace Plugins::Flua {

inline constexpr auto kApiVersion = 1;

// Permissions a plugin can request in manifest.json. Unknown names make
// the package invalid, so a typo never silently drops a permission.
inline constexpr auto kPermMessagesRead = "messages.read";
inline constexpr auto kPermMessagesSend = "messages.send";
inline constexpr auto kPermUi = "ui";
inline constexpr auto kPermStorage = "storage";
inline constexpr auto kPermLinks = "links";
inline constexpr auto kPermActions = "actions";
inline constexpr auto kPermDisplay = "display";
inline constexpr auto kPermChatsRead = "chats.read";
inline constexpr auto kPermCompose = "compose";
// Edit and delete messages: tg.messages.edit / tg.messages.delete.
inline constexpr auto kPermMessagesManage = "messages.manage";
// Raw MTProto: tg.invoke and the mtproto.* filters. Full account access.
inline constexpr auto kPermMtproto = "mtproto";
// The music player: what plays and its buttons (tg.player).
inline constexpr auto kPermPlayer = "player";

[[nodiscard]] const std::vector<QString> &KnownPermissions();

// Sections of the plugin catalog a plugin can name in "categories".
// Unknown names are skipped: they only sort plugins in the catalog.
[[nodiscard]] const std::vector<QString> &KnownCategories();

// "net:api.example.com" or "net:*.example.com": HTTPS requests to that
// host (or its subdomains). The host part of a valid one, or nullopt.
[[nodiscard]] bool IsNetPermission(const QString &permission);
[[nodiscard]] std::optional<QString> NetHostPattern(const QString &permission);

// Name and description of a plugin in one language.
struct LocalizedText {
	QString name;
	QString description;
};

// The interface language, lowercase: "ru", "en", "pt-br".
[[nodiscard]] QString InterfaceLanguage();

// Languages to look a text up in, best first, always ending with "en":
// "pt-br" -> { "pt-br", "pt", "en" }.
[[nodiscard]] std::vector<QString> LanguageChain(const QString &language);

// Plural form of `count` in `language`: "one", "few", "many" or "other".
[[nodiscard]] QString PluralForm(const QString &language, double count);

struct Manifest {
	QString id;
	QString name;
	QString version;
	QString author;
	QString description;
	QString entry;
	QString icon; // optional picture in the package, e.g. resources/icon.png
	int apiMin = 0;
	int apiMax = 0;
	std::vector<QString> permissions;
	std::vector<QString> categories; // known ones, see KnownCategories()
	// "localized": { "ru": { "name": ..., "description": ... } }; name and
	// description above are the default (English) texts.
	std::map<QString, LocalizedText> localized;

	// Name and description in the interface language (main thread), or
	// in the first of `languages` the manifest has (any thread).
	[[nodiscard]] QString displayName() const;
	[[nodiscard]] QString displayDescription() const;
	[[nodiscard]] QString displayName(
		const std::vector<QString> &languages) const;
	[[nodiscard]] QString displayDescription(
		const std::vector<QString> &languages) const;

	[[nodiscard]] bool has(const char *permission) const;
	[[nodiscard]] bool hasNetwork() const;
	[[nodiscard]] bool hostAllowed(const QString &host) const;
};

// The whole package in memory: a .flua archive (zip) or an unpacked
// development folder. Paths are relative, with '/' separators.
struct Package {
	QString origin; // file or folder it was read from
	std::map<QString, QByteArray> files;
	Manifest manifest;
	QString error; // non-empty if the package is not usable
};

[[nodiscard]] Package ReadPackageFile(const QString &path);
[[nodiscard]] Package ReadPackageFolder(const QString &path);

// The picture at `path` in the package, null if it is not a small image.
[[nodiscard]] QImage ReadIcon(
	const std::map<QString, QByteArray> &files,
	const QString &path);

// Packs the files into a .flua (zip) at `path`.
[[nodiscard]] bool WritePackageFile(
	const std::map<QString, QByteArray> &files,
	const QString &path);

} // namespace Plugins::Flua
