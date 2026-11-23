/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/ui/plugins_manager.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "data/data_thread.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "plugins/runtime/flua_package.h"
#include "plugins/runtime/flua_runtime.h"
#include "settings/detailed_settings_button.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_plugins.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>

namespace Plugins::Flua {
namespace {

constexpr auto kMaxErrorLines = 3;
constexpr auto kDocsUrl
	= "https://github.com/fucksynth/fuckgram/blob/dev/docs/plugins.md";

struct Badge {
	QString text;
	QColor color;
};

[[nodiscard]] bool IsMtproto(const QString &permission) {
	return (permission == QString::fromLatin1(kPermMtproto));
}

// Two or three words for a card; the install box has the full text.
[[nodiscard]] QString ShortPermission(const QString &permission) {
	const auto is = [&](const char *name) {
		return (permission == QString::fromLatin1(name));
	};
	if (const auto host = NetHostPattern(permission)) {
		return Translated("network: ", "сеть: ") + *host;
	} else if (is(kPermMessagesRead)) {
		return Translated("reads messages", "читает сообщения");
	} else if (is(kPermMessagesSend)) {
		return Translated("sends messages", "отправляет сообщения");
	} else if (is(kPermMessagesManage)) {
		return Translated("edits, deletes", "правит, удаляет");
	} else if (is(kPermUi)) {
		return Translated("menus", "меню");
	} else if (is(kPermStorage)) {
		return Translated("storage", "хранилище");
	} else if (is(kPermLinks)) {
		return Translated("links", "ссылки");
	} else if (is(kPermActions)) {
		return Translated("actions", "действия");
	} else if (is(kPermDisplay)) {
		return Translated("display", "отображение");
	} else if (is(kPermChatsRead)) {
		return Translated("chats", "чаты");
	} else if (is(kPermCompose)) {
		return Translated("text field", "поле ввода");
	} else if (IsMtproto(permission)) {
		return Translated("full account access", "полный доступ к аккаунту");
	}
	return permission;
}

[[nodiscard]] const style::icon &PermissionIcon(const QString &permission) {
	const auto is = [&](const char *name) {
		return (permission == QString::fromLatin1(name));
	};
	if (NetHostPattern(permission)) {
		return st::menuIconNetwork;
	} else if (is(kPermMessagesRead)) {
		return st::menuIconChatBubble;
	} else if (is(kPermMessagesSend)) {
		return st::menuIconSend;
	} else if (is(kPermMessagesManage) || is(kPermCompose)) {
		return st::menuIconEdit;
	} else if (is(kPermUi)) {
		return st::menuIconCustomize;
	} else if (is(kPermStorage)) {
		return st::menuIconStorage;
	} else if (is(kPermLinks)) {
		return st::menuIconLink;
	} else if (is(kPermActions)) {
		return st::menuIconReply;
	} else if (is(kPermDisplay)) {
		return st::menuIconPalette;
	} else if (is(kPermChatsRead)) {
		return st::menuIconChats;
	} else if (IsMtproto(permission)) {
		return st::menuIconAdmin;
	}
	return st::menuIconPermissions;
}

// The picture of the plugin or its first letter on a color.
[[nodiscard]] QImage PrepareIcon(
		const PluginView &view,
		int size,
		int radius) {
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(
		QSize(size, size) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);

	auto p = QPainter(&result);
	PainterHighQualityEnabler hq(p);
	auto path = QPainterPath();
	path.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
	p.setClipPath(path);
	const auto rect = QRect(0, 0, size, size);
	if (!view.icon.isNull()) {
		const auto scaled = view.icon.scaled(
			QSize(size, size) * ratio,
			Qt::KeepAspectRatioByExpanding,
			Qt::SmoothTransformation);
		const auto x = (scaled.width() - size * ratio) / 2;
		const auto y = (scaled.height() - size * ratio) / 2;
		p.drawImage(
			rect,
			scaled,
			QRect(x, y, size * ratio, size * ratio));
	} else {
		const style::color *colors[] = {
			&st::historyPeer1UserpicBg,
			&st::historyPeer2UserpicBg,
			&st::historyPeer3UserpicBg,
			&st::historyPeer4UserpicBg,
			&st::historyPeer5UserpicBg,
			&st::historyPeer6UserpicBg,
			&st::historyPeer7UserpicBg,
			&st::historyPeer8UserpicBg,
		};
		const auto index = qHash(view.id) % uint(std::size(colors));
		p.fillRect(rect, (*colors[index])->c);
		auto font = st::semiboldFont->f;
		font.setPixelSize(std::max(size * 45 / 100, 1));
		p.setFont(font);
		p.setPen(QColor(255, 255, 255));
		const auto letter = view.name.trimmed().isEmpty()
			? u"?"_q
			: view.name.trimmed().left(1).toUpper();
		p.drawText(rect, Qt::AlignCenter, letter);
	}
	p.end();
	return result;
}

// Lays out badges in lines of `width`, paints them if `p` is given.
int LayoutBadges(
		const std::vector<Badge> &badges,
		int width,
		QPainter *p = nullptr,
		QPoint origin = QPoint()) {
	if (badges.empty()) {
		return 0;
	}
	const auto &font = st::pluginsCardBadgeFont;
	const auto height = st::pluginsCardBadgeHeight;
	const auto skip = st::pluginsCardBadgeSkip;
	const auto padding = st::pluginsCardBadgePadding;
	const auto radius = st::pluginsCardBadgeRadius;
	auto x = 0;
	auto y = 0;
	for (const auto &badge : badges) {
		const auto full = font->width(badge.text) + 2 * padding;
		const auto w = std::min(full, width);
		if (x > 0 && x + w > width) {
			x = 0;
			y += height + skip;
		}
		if (p) {
			const auto rect = QRect(origin.x() + x, origin.y() + y, w, height);
			auto background = badge.color;
			background.setAlphaF(0.14);
			p->setPen(Qt::NoPen);
			p->setBrush(background);
			p->drawRoundedRect(rect, radius, radius);
			p->setPen(badge.color);
			p->setFont(font);
			p->drawText(
				rect,
				Qt::AlignCenter,
				font->elided(badge.text, std::max(w - 2 * padding, 1)));
		}
		x += w + skip;
	}
	return y + height;
}

struct CardCallbacks {
	Fn<void(bool)> toggle;
	Fn<void()> share;
	Fn<void()> journal;
	Fn<void()> folder;
	Fn<void()> remove;
};

class PluginCard final : public Ui::RpWidget {
public:
	PluginCard(
		QWidget *parent,
		PluginView view,
		bool compact,
		bool engineRuns,
		CardCallbacks callbacks);

	// Everything but the state is the same: the card can be updated.
	[[nodiscard]] static QString LookKey(const PluginView &view);

	// Updates the state (on/off, running, error) without building again.
	void setState(PluginView view, bool engineRuns);

protected:
	int resizeGetHeight(int newWidth) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] int textLeft() const;
	[[nodiscard]] QRect toggleRect() const;
	[[nodiscard]] QRect toggleArea() const;
	void refreshState();
	void addAction(
		const style::icon &icon,
		const style::icon &over,
		Fn<void()> callback,
		bool right);

	PluginView _view;
	const bool _compact = false;
	bool _engineRuns = false;
	const CardCallbacks _callbacks;
	QImage _icon;
	Ui::Text::String _name;
	Ui::Text::String _meta;
	// Without a minimum resize width a String counts its height as if it
	// never wraps, while it does wrap when drawn.
	Ui::Text::String _description = { 1 };
	Ui::Text::String _status = { 1 };
	bool _statusIsError = false;
	std::vector<Badge> _badges;
	std::unique_ptr<Ui::ToggleView> _toggle;
	std::vector<not_null<Ui::IconButton*>> _leftActions;
	std::vector<not_null<Ui::IconButton*>> _rightActions;

	int _layoutWidth = -1;
	int _headerHeight = 0;
	int _descriptionTop = 0;
	int _badgesTop = 0;
	int _statusTop = 0;

};

PluginCard::PluginCard(
	QWidget *parent,
	PluginView view,
	bool compact,
	bool engineRuns,
	CardCallbacks callbacks)
: RpWidget(parent)
, _view(std::move(view))
, _compact(compact)
, _engineRuns(engineRuns)
, _callbacks(std::move(callbacks))
, _icon(PrepareIcon(
	_view,
	st::pluginsCardIconSize,
	st::pluginsCardIconRadius))
, _toggle(std::make_unique<Ui::ToggleView>(
	st::pluginsCardToggle,
	_view.enabled,
	[=] { update(); })) {
	_name.setText(st::pluginsCardNameStyle, _view.name, kPlainTextOptions);

	auto meta = QStringList();
	if (!_view.version.isEmpty()) {
		meta.push_back(_view.version);
	}
	if (!_view.author.isEmpty()) {
		meta.push_back(_view.author);
	}
	if (_view.dev) {
		meta.push_back(Translated("development folder", "папка разработки"));
	}
	_meta.setText(
		st::pluginsCardMetaStyle,
		meta.join(u" · "_q),
		kPlainTextOptions);

	refreshState();
	if (!compact) {
		_description.setText(
			st::pluginsCardTextStyle,
			_view.description,
			kPlainTextOptions);
		if (_callbacks.share) {
			addAction(
				st::pluginsActionShare,
				st::pluginsActionShareOver,
				_callbacks.share,
				false);
		}
		if (_callbacks.journal) {
			addAction(
				st::pluginsActionJournal,
				st::pluginsActionJournalOver,
				_callbacks.journal,
				false);
		}
		if (_callbacks.folder) {
			addAction(
				st::pluginsActionFolder,
				st::pluginsActionFolderOver,
				_callbacks.folder,
				false);
		}
		if (_callbacks.remove) {
			addAction(
				st::pluginsActionDelete,
				st::pluginsActionDeleteOver,
				_callbacks.remove,
				true);
		}
	}
	setMouseTracking(true);
}

QString PluginCard::LookKey(const PluginView &view) {
	auto result = QStringList{
		view.id,
		view.name,
		view.version,
		view.author,
		view.description,
		view.origin,
		view.dev ? u"dev"_q : QString(),
		QString::number(view.icon.cacheKey()),
	};
	for (const auto &permission : view.permissions) {
		result.push_back(permission);
	}
	return result.join(QChar(0));
}

void PluginCard::setState(PluginView view, bool engineRuns) {
	_view = std::move(view);
	_engineRuns = engineRuns;
	if (_toggle->checked() != _view.enabled) {
		_toggle->setChecked(_view.enabled, anim::type::normal);
	}
	refreshState();
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void PluginCard::refreshState() {
	auto status = QString();
	_statusIsError = false;
	if (_view.enabled && !_view.running) {
		if (!_engineRuns) {
			status = Translated(
				"Does not run: plugins are off.",
				"Не запущен: плагины выключены.");
		} else {
			status = _view.error.isEmpty()
				? Translated("Stopped.", "Остановлен.")
				: (Translated("Stopped: ", "Остановлен: ") + _view.error);
			_statusIsError = true;
		}
	} else if (_view.running && !_view.error.isEmpty()) {
		status = Translated("Last error: ", "Последняя ошибка: ")
			+ _view.error;
		_statusIsError = true;
	}
	_status.setText(st::pluginsCardTextStyle, status, kPlainTextOptions);

	_badges.clear();
	if (_compact) {
		return;
	}
	if (!_view.enabled) {
		_badges.push_back({
			Translated("turned off", "выключен"),
			st::windowSubTextFg->c,
		});
	}
	for (const auto &permission : _view.permissions) {
		_badges.push_back({
			ShortPermission(permission),
			(IsMtproto(permission)
				? st::attentionButtonFg->c
				: st::windowActiveTextFg->c),
		});
	}
}

void PluginCard::addAction(
		const style::icon &icon,
		const style::icon &over,
		Fn<void()> callback,
		bool right) {
	const auto button = Ui::CreateChild<Ui::IconButton>(
		this,
		st::pluginsCardAction);
	button->setIconOverride(&icon, &over);
	button->setRippleColorOverride(&st::windowBgRipple);
	button->setClickedCallback(std::move(callback));
	(right ? _rightActions : _leftActions).push_back(button);
}

int PluginCard::textLeft() const {
	return st::pluginsCardPadding.left()
		+ st::pluginsCardIconSize
		+ st::pluginsCardIconSkip;
}

QRect PluginCard::toggleRect() const {
	const auto size = _toggle->getSize();
	return QRect(
		width() - st::pluginsCardPadding.right() - size.width(),
		st::pluginsCardPadding.top() + (_headerHeight - size.height()) / 2,
		size.width(),
		size.height());
}

QRect PluginCard::toggleArea() const {
	const auto extend = st::pluginsCardIconSkip;
	return toggleRect().marginsAdded({ extend, extend, extend, extend });
}

// Whoever changes the width, the text and the badges are laid out for it.
void PluginCard::resizeEvent(QResizeEvent *e) {
	if (width() == _layoutWidth) {
		return;
	}
	const auto height = resizeGetHeight(width());
	if (height != this->height()) {
		resize(width(), height);
	}
	update();
}

int PluginCard::resizeGetHeight(int newWidth) {
	_layoutWidth = newWidth;
	const auto &padding = st::pluginsCardPadding;
	const auto inner = std::max(
		newWidth - padding.left() - padding.right(),
		1);
	const auto textHeight = st::pluginsCardNameStyle.font->height
		+ st::pluginsCardLineSkip
		+ st::pluginsCardMetaStyle.font->height;
	_headerHeight = std::max(st::pluginsCardIconSize, textHeight);
	auto top = padding.top() + _headerHeight;
	if (!_description.isEmpty()) {
		top += st::pluginsCardBlockSkip;
		_descriptionTop = top;
		top += _description.countHeight(inner);
	}
	if (!_badges.empty()) {
		top += st::pluginsCardBlockSkip;
		_badgesTop = top;
		top += LayoutBadges(_badges, inner);
	}
	if (!_status.isEmpty()) {
		top += st::pluginsCardBlockSkip;
		_statusTop = top;
		top += std::min(
			_status.countHeight(inner),
			kMaxErrorLines * st::pluginsCardTextStyle.font->height);
	}
	if (_leftActions.empty() && _rightActions.empty()) {
		return top + padding.bottom();
	}
	top += st::pluginsCardActionsTop;
	const auto shift = (st::pluginsCardAction.width
		- st::pluginsCardAction.rippleAreaSize) / 2;
	auto left = padding.left() - shift;
	for (const auto button : _leftActions) {
		button->moveToLeft(left, top, newWidth);
		left += button->width();
	}
	auto right = padding.right() - shift;
	for (const auto button : _rightActions) {
		button->moveToRight(right, top, newWidth);
		right += button->width();
	}
	return top + st::pluginsCardAction.height + (padding.bottom() / 2);
}

void PluginCard::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto &padding = st::pluginsCardPadding;
	{
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(
			rect(),
			st::pluginsCardRadius,
			st::pluginsCardRadius);
	}
	p.drawImage(
		QPoint(
			padding.left(),
			padding.top() + (_headerHeight - st::pluginsCardIconSize) / 2),
		_icon);

	const auto left = textLeft();
	const auto toggle = toggleRect();
	const auto available = std::max(
		toggle.x() - st::pluginsCardIconSkip - left,
		1);
	const auto textHeight = st::pluginsCardNameStyle.font->height
		+ st::pluginsCardLineSkip
		+ st::pluginsCardMetaStyle.font->height;
	auto top = padding.top() + (_headerHeight - textHeight) / 2;
	p.setPen(st::windowBoldFg);
	_name.drawLeftElided(p, left, top, available, width());
	top += st::pluginsCardNameStyle.font->height + st::pluginsCardLineSkip;
	p.setPen(st::windowSubTextFg);
	_meta.drawLeftElided(p, left, top, available, width());

	_toggle->paint(p, toggle.x(), toggle.y(), width());

	const auto inner = std::max(
		width() - padding.left() - padding.right(),
		1);
	if (!_description.isEmpty()) {
		p.setPen(st::windowFg);
		_description.drawLeft(p, padding.left(), _descriptionTop, inner, width());
	}
	if (!_badges.empty()) {
		LayoutBadges(
			_badges,
			inner,
			&p,
			QPoint(padding.left(), _badgesTop));
	}
	if (!_status.isEmpty()) {
		p.setPen(_statusIsError ? st::attentionButtonFg : st::windowSubTextFg);
		_status.drawLeftElided(
			p,
			padding.left(),
			_statusTop,
			inner,
			width(),
			kMaxErrorLines);
	}
}

void PluginCard::mouseMoveEvent(QMouseEvent *e) {
	setCursor(toggleArea().contains(e->pos())
		? style::cur_pointer
		: style::cur_default);
}

void PluginCard::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton
		|| !toggleArea().contains(e->pos())
		|| !_callbacks.toggle) {
		return;
	}
	const auto checked = !_toggle->checked();
	_toggle->setChecked(checked, anim::type::normal);
	// The list is built again on the change: let the toggle move first.
	// Not guarded by the card: it may be built again before that, and the
	// callback needs only the plugin id.
	const auto toggle = _callbacks.toggle;
	base::call_delayed(st::pluginsCardToggle.duration, [=] {
		toggle(checked);
	});
}

// A permission with its icon, for the install box.
[[nodiscard]] object_ptr<Ui::RpWidget> MakePermissionRow(
		QWidget *parent,
		const QString &permission) {
	auto result = object_ptr<Ui::RpWidget>(parent);
	const auto raw = result.data();
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		raw,
		PermissionText(permission),
		st::boxLabel);
	if (IsMtproto(permission)) {
		label->setTextColorOverride(st::attentionButtonFg->c);
	}
	const auto icon = &PermissionIcon(permission);
	raw->widthValue() | rpl::on_next([=](int width) {
		const auto left = st::pluginsPermissionTextLeft;
		label->resizeToWidth(std::max(width - left, 1));
		label->moveToLeft(left, 0, width);
		raw->resize(
			width,
			std::max(label->height(), icon->height())
				+ st::pluginsPermissionSkip);
	}, raw->lifetime());
	raw->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(raw);
		const auto top = std::max(
			(st::boxLabel.style.font->height - icon->height()) / 2,
			0);
		icon->paint(p, st::pluginsPermissionIconLeft, top, raw->width());
	}, raw->lifetime());
	return result;
}

[[nodiscard]] object_ptr<Ui::RpWidget> MakeBadgesRow(
		QWidget *parent,
		std::vector<Badge> badges) {
	auto result = object_ptr<Ui::RpWidget>(parent);
	const auto raw = result.data();
	const auto shared = std::make_shared<std::vector<Badge>>(
		std::move(badges));
	raw->widthValue() | rpl::on_next([=](int width) {
		raw->resize(width, LayoutBadges(*shared, std::max(width, 1)));
	}, raw->lifetime());
	raw->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(raw);
		PainterHighQualityEnabler hq(p);
		// Centered: the badges fit one line in the install box.
		auto full = 0;
		for (const auto &badge : *shared) {
			full += st::pluginsCardBadgeFont->width(badge.text)
				+ 2 * st::pluginsCardBadgePadding
				+ st::pluginsCardBadgeSkip;
		}
		full = std::max(full - st::pluginsCardBadgeSkip, 0);
		const auto left = std::max((raw->width() - full) / 2, 0);
		LayoutBadges(
			*shared,
			std::max(raw->width() - left, 1),
			&p,
			QPoint(left, 0));
	}, raw->lifetime());
	return result;
}

} // namespace

void ShowInstallBox(
		not_null<Window::SessionController*> controller,
		PluginView view,
		Fn<void(bool enable)> install) {
	const auto shared = std::make_shared<PluginView>(std::move(view));
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto &view = *shared;
		box->setWidth(st::boxWideWidth);

		const auto size = st::pluginsInstallIconSize;
		auto icon = object_ptr<Ui::RpWidget>(box);
		const auto iconRaw = icon.data();
		iconRaw->resize(size, size);
		iconRaw->setNaturalWidth(size);
		const auto image = PrepareIcon(
			view,
			size,
			st::pluginsInstallIconRadius);
		iconRaw->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(iconRaw);
			p.drawImage(QPoint(), image);
		}, iconRaw->lifetime());
		box->addSkip(st::pluginsInstallIconTop);
		box->addRow(std::move(icon), style::al_top);

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				view.name,
				st::pluginsInstallName),
			style::al_top);
		auto meta = view.version.isEmpty()
			? QString()
			: (Translated("Version ", "Версия ") + view.version);
		if (!view.author.isEmpty()) {
			meta += (meta.isEmpty() ? QString() : u" · "_q) + view.author;
		}
		if (!meta.isEmpty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					meta,
					st::pluginsInstallMeta),
				style::al_top);
		}
		box->addSkip(st::pluginsCardBlockSkip);
		box->addRow(MakeBadgesRow(box, {
			{
				Translated("code checked", "код проверен"),
				st::boxTextFgGood->c,
			},
			{
				Translated("unknown source", "неизвестный источник"),
				st::historyPeer8UserpicBg->c,
			},
		}));
		if (!view.description.isEmpty()) {
			box->addSkip(st::pluginsCardBlockSkip);
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				view.description,
				st::pluginsInstallText));
		}
		box->addSkip(st::pluginsCardBlockSkip * 2);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			(view.permissions.empty()
				? Translated(
					"Needs no permissions.",
					"Ничего не требует.")
				: Translated(
					"The plugin will be able to",
					"Что сможет плагин")),
			st::pluginsInstallSection));
		box->addSkip(st::pluginsPermissionSkip);
		for (const auto &permission : view.permissions) {
			box->addRow(MakePermissionRow(box, permission));
		}
		box->addSkip(st::pluginsCardBlockSkip);
		const auto enable = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			Translated("Turn on after installing", "Включить после установки"),
			true,
			st::defaultBoxCheckbox));

		box->addButton(tr::fg_plugins_install(), [=] {
			const auto on = enable->checked();
			box->closeBox();
			install(on);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Plugins::Flua

namespace Settings {
namespace {

namespace Flua = Plugins::Flua;
using Flua::Translated;

void SharePlugin(
		not_null<Window::SessionController*> controller,
		const QString &id) {
	const auto host = Flua::CurrentHost();
	const auto path = host ? host->sharePath(id) : QString();
	if (path.isEmpty()) {
		controller->showToast(Translated(
			"Could not prepare the plugin file.",
			"Не удалось подготовить файл плагина."));
		return;
	}
	Window::ShowChooseRecipientBox(
		controller,
		[=](not_null<Data::Thread*> thread) {
			auto list = Storage::PrepareMediaList(
				QStringList{ path },
				st::sendMediaPreviewSize,
				thread->session().premium());
			if (list.error != Ui::PreparedList::Error::None) {
				controller->showToast(Translated(
					"Could not send the plugin file.",
					"Не удалось отправить файл плагина."));
				return true;
			}
			thread->session().api().sendFiles(
				std::move(list),
				SendMediaType::File,
				nullptr,
				Api::SendAction(thread));
			controller->showToast(Translated(
				"Plugin sent.",
				"Плагин отправлен."));
			return true;
		},
		rpl::single(Translated("Send the plugin", "Отправить плагин")));
}

void ShowJournal(
		not_null<Window::SessionController*> controller,
		const QString &id,
		const QString &name) {
	const auto host = Flua::CurrentHost();
	auto lines = QStringList();
	if (host) {
		for (const auto &line : host->journal(id)) {
			lines.push_back(line);
		}
	}
	const auto text = lines.isEmpty()
		? Translated(
			"Empty for now: here are tg.log, print and errors.",
			"Пока пусто: здесь будут tg.log, print и ошибки.")
		: lines.join(u'\n');
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(name));
		box->setWidth(st::boxWideWidth);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			text,
			st::pluginsJournalText));
		label->setSelectable(true);
		box->addButton(rpl::single(Translated("Copy", "Копировать")), [=] {
			QGuiApplication::clipboard()->setText(text);
			controller->showToast(Translated("Copied.", "Скопировано."));
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ConfirmRemove(
		not_null<Window::SessionController*> controller,
		const QString &id,
		const QString &name) {
	controller->show(Ui::MakeConfirmBox({
		.text = Translated(
			"Delete the plugin \"%1\" and its data?",
			"Удалить плагин «%1» и его данные?").arg(name),
		.confirmed = [=](Fn<void()> close) {
			if (const auto host = Flua::CurrentHost()) {
				host->removePlugin(id);
			}
			close();
		},
		.confirmText = Translated("Delete", "Удалить"),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

class PluginsSection final : public Section<PluginsSection> {
public:
	PluginsSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
	void refresh();
	void rebuild();
	[[nodiscard]] std::vector<Flua::PluginView> shownViews() const;
	[[nodiscard]] QString listKey(
		const std::vector<Flua::PluginView> &views) const;

	Ui::VerticalLayout *_list = nullptr;
	std::vector<not_null<Flua::PluginCard*>> _cards;
	QString _builtKey;
	QString _query;

};

PluginsSection::PluginsSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> PluginsSection::title() {
	return tr::fg_plugins();
}

void PluginsSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto host = Flua::CurrentHost();

	Ui::AddSkip(content);
	if (host) {
		const auto master = AddButtonWithIcon(
			content,
			rpl::single(Translated("Plugins are on", "Плагины включены")),
			st::settingsButton,
			{ &st::menuIconBotCommands });
		master->toggleOn(rpl::single(
			rpl::empty
		) | rpl::then(
			host->changes()
		) | rpl::map([] {
			const auto host = Flua::CurrentHost();
			return host && host->engine().enabled;
		}));
		master->toggledChanges(
		) | rpl::on_next([=](bool enabled) {
			if (const auto host = Flua::CurrentHost()) {
				auto engine = host->engine();
				engine.enabled = enabled;
				host->setEngine(engine);
			}
		}, master->lifetime());
	}
	AddButtonWithIcon(
		content,
		rpl::single(Translated("Plugin engine", "Движок плагинов")),
		st::settingsButton,
		{ &st::menuIconSettings }
	)->setClickedCallback([=] {
		showOther(PluginsEngineId());
	});
	AddButtonWithIcon(
		content,
		tr::fg_plugins_open_folder(),
		st::settingsButton,
		{ &st::menuIconShowInFolder }
	)->setClickedCallback([] {
		QDir().mkpath(Flua::PluginsPath());
		QDesktopServices::openUrl(QUrl::fromLocalFile(Flua::PluginsPath()));
	});
	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);

	const auto search = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			rpl::single(Translated("Search plugins", "Поиск плагинов"))),
		st::boxRowPadding);
	search->changes() | rpl::on_next([=] {
		_query = search->getLastText().trimmed();
		rebuild();
	}, search->lifetime());
	Ui::AddSkip(content);

	_list = content->add(object_ptr<Ui::VerticalLayout>(content));
	rebuild();
	if (host) {
		host->changes() | rpl::on_next([=] {
			refresh();
		}, lifetime());
	}
	Ui::AddSkip(content);

	Ui::ResizeFitChild(this, content);
}

std::vector<Flua::PluginView> PluginsSection::shownViews() const {
	auto result = std::vector<Flua::PluginView>();
	const auto host = Flua::CurrentHost();
	if (!host) {
		return result;
	}
	const auto query = _query.toLower();
	for (auto &view : host->pluginViews()) {
		if (query.isEmpty()
			|| view.name.toLower().contains(query)
			|| view.id.contains(query)
			|| view.description.toLower().contains(query)) {
			result.push_back(std::move(view));
		}
	}
	return result;
}

// What the list is built of, except the states the cards update by
// themselves: when it is the same, nothing is built again.
QString PluginsSection::listKey(
		const std::vector<Flua::PluginView> &views) const {
	const auto host = Flua::CurrentHost();
	if (!host) {
		return u"none"_q;
	}
	const auto &engine = host->engine();
	auto parts = QStringList{
		engine.compact ? u"compact"_q : QString(),
		engine.safeMode ? u"safe"_q : QString(),
	};
	for (const auto &view : views) {
		parts.push_back(Flua::PluginCard::LookKey(view));
	}
	for (const auto &entry : host->invalidViews()) {
		parts.push_back(entry.origin + QChar(0) + entry.error);
	}
	return parts.join(QChar(1));
}

void PluginsSection::refresh() {
	const auto host = Flua::CurrentHost();
	auto views = shownViews();
	if (!host
		|| listKey(views) != _builtKey
		|| views.size() != _cards.size()) {
		rebuild();
		return;
	}
	const auto &engine = host->engine();
	const auto runs = engine.enabled && !engine.safeMode;
	for (auto i = 0, count = int(views.size()); i != count; ++i) {
		_cards[i]->setState(std::move(views[i]), runs);
	}
}

void PluginsSection::rebuild() {
	// The list keeps its height while it is built again, so the scroll
	// position does not jump to the top.
	_list->setMinimumHeight(_list->height());
	const auto restoreHeight = gsl::finally([&] {
		_list->setMinimumHeight(0);
		if (_list->width() > 0) {
			_list->resizeToWidth(_list->width());
		}
	});
	_list->clear();
	_cards.clear();
	auto views = shownViews();
	_builtKey = listKey(views);
	const auto host = Flua::CurrentHost();
	if (!host) {
		Ui::AddDividerText(
			_list,
			rpl::single(Translated(
				"Plugins did not start.",
				"Плагины не запустились.")));
		return;
	}
	const auto &engine = host->engine();
	if (engine.safeMode) {
		Ui::AddDividerText(
			_list,
			rpl::single(Translated(
				"Safe mode is on: plugins do not run. Turn it off in "
				"Plugin engine when the problem is found.",
				"Включён безопасный режим: плагины не запускаются. "
				"Выключите его в «Движке плагинов», когда найдёте "
				"проблему.")));
		Ui::AddSkip(_list);
	}
	const auto runs = engine.enabled && !engine.safeMode;
	const auto controller = this->controller();
	auto shown = 0;
	for (auto &view : views) {
		const auto id = view.id;
		const auto name = view.name;
		const auto origin = view.origin;
		auto callbacks = Flua::CardCallbacks{
			.toggle = [=](bool enabled) {
				if (const auto host = Flua::CurrentHost()) {
					host->setPluginEnabled(id, enabled);
				}
			},
			.share = [=] { SharePlugin(controller, id); },
			.journal = [=] { ShowJournal(controller, id, name); },
		};
		if (view.dev) {
			callbacks.folder = [=] {
				QDesktopServices::openUrl(QUrl::fromLocalFile(origin));
			};
		} else {
			callbacks.remove = [=] { ConfirmRemove(controller, id, name); };
		}
		_cards.push_back(_list->add(
			object_ptr<Flua::PluginCard>(
				_list,
				std::move(view),
				engine.compact,
				runs,
				std::move(callbacks)),
			st::pluginsCardMargin));
		++shown;
	}
	const auto invalid = host->invalidViews();
	if (!shown && invalid.empty()) {
		Ui::AddDividerText(
			_list,
			rpl::single(_query.isEmpty()
				? (tr::fg_plugins_empty(tr::now)
					+ u'\n'
					+ QDir::toNativeSeparators(Flua::PluginsPath()))
				: Translated("Nothing found.", "Ничего не найдено.")));
	}
	if (!invalid.empty()) {
		Ui::AddSkip(_list);
		Ui::AddSubsectionTitle(_list, tr::fg_plugins_not_loaded());
		for (const auto &entry : invalid) {
			const auto label = _list->add(
				object_ptr<Ui::FlatLabel>(
					_list,
					QFileInfo(entry.origin).fileName()
						+ u": "_q
						+ entry.error,
					st::boxLabel),
				st::boxRowPadding);
			label->setTextColorOverride(st::attentionButtonFg->c);
			label->setSelectable(true);
			Ui::AddSkip(_list);
		}
	}
}

class PluginsEngineSection final : public Section<PluginsEngineSection> {
public:
	PluginsEngineSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

PluginsEngineSection::PluginsEngineSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> PluginsEngineSection::title() {
	return rpl::single(Translated("Plugin engine", "Движок плагинов"));
}

void PluginsEngineSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto host = Flua::CurrentHost();
	const auto engine = host ? host->engine() : Flua::EngineSettings();

	using Setter = void(*)(Flua::EngineSettings&, bool);
	const auto add = [&](
			const char *title,
			const char *titleRu,
			const char *about,
			const char *aboutRu,
			const style::icon &icon,
			const style::color &background,
			bool value,
			Setter setter) {
		const auto button = AddDetailedSettingsButton(
			content,
			rpl::single(Translated(title, titleRu)),
			rpl::single(Translated(about, aboutRu)),
			{ .icon = &icon, .background = &background },
			rpl::single(value),
			st::detailedSettingsButtonStyle);
		if (!setter) {
			button->setToggleLocked(true);
			return;
		}
		button->toggledChanges() | rpl::on_next([=](bool on) {
			if (const auto host = Flua::CurrentHost()) {
				auto engine = host->engine();
				setter(engine, on);
				host->setEngine(engine);
			}
		}, button->lifetime());
	};

	Ui::AddSkip(content);
	add(
		"Developer mode",
		"Режим разработчика",
		"Plugin folders reload as soon as a file is saved.",
		"Папки плагинов перезагружаются, как только сохранён файл.",
		st::pluginsEngineIconCode,
		st::settingsIconBg5,
		engine.devMode,
		[](Flua::EngineSettings &e, bool on) { e.devMode = on; });
	add(
		"Compact view",
		"Компактный вид",
		"Plugin cards without descriptions and buttons.",
		"Карточки плагинов без описаний и кнопок.",
		st::pluginsEngineIconCompact,
		st::settingsIconBg4,
		engine.compact,
		[](Flua::EngineSettings &e, bool on) { e.compact = on; });
	add(
		"Unknown sources",
		"Неизвестные источники",
		"Install .flua files sent in chats.",
		"Ставить файлы .flua, присланные в чатах.",
		st::pluginsEngineIconSources,
		st::settingsIconBg3,
		engine.unknownSources,
		[](Flua::EngineSettings &e, bool on) { e.unknownSources = on; });
	add(
		"Code check",
		"Проверка кода",
		"Always on: a plugin loads only if its code uses exactly the "
		"permissions of its manifest.",
		"Всегда включена: плагин загружается, только если его код "
		"использует ровно те права, что в манифесте.",
		st::pluginsEngineIconCheck,
		st::settingsIconBg2,
		true,
		nullptr);
	add(
		"Confirm sensitive actions",
		"Подтверждать важные действия",
		"Stars, gifts, payments, sessions, the password: plugins do them "
		"only after your \"yes\".",
		"Звёзды, подарки, платежи, сеансы, пароль: плагины делают это "
		"только после вашего «да».",
		st::pluginsEngineIconConfirm,
		st::settingsIconBg1,
		engine.confirmSensitive,
		[](Flua::EngineSettings &e, bool on) { e.confirmSensitive = on; });
	add(
		"Safe mode",
		"Безопасный режим",
		"Plugins do not run, nothing is deleted. Turns on by itself if "
		"the client closes while plugins start.",
		"Плагины не запускаются, ничего не удаляется. Включается сам, "
		"если клиент закрылся во время запуска плагинов.",
		st::pluginsEngineIconSafe,
		st::settingsIconBg6,
		engine.safeMode,
		[](Flua::EngineSettings &e, bool on) { e.safeMode = on; });

	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	AddButtonWithIcon(
		content,
		rpl::single(Translated("Documentation", "Документация")),
		st::settingsButton,
		{ &st::menuIconFaq }
	)->setClickedCallback([] {
		QDesktopServices::openUrl(QUrl(QString::fromLatin1(Flua::kDocsUrl)));
	});
	Ui::AddSkip(content);

	Ui::ResizeFitChild(this, content);
}

} // namespace

Type PluginsId() {
	return PluginsSection::Id();
}

Type PluginsEngineId() {
	return PluginsEngineSection::Id();
}

} // namespace Settings
