/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/ui/plugins_catalog.h"

#include "ayu/ui/ayu_logo.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "plugins/runtime/flua_catalog.h"
#include "plugins/runtime/flua_package.h"
#include "plugins/runtime/flua_runtime.h"
#include "plugins/ui/plugins_ui.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"
#include "styles/style_ayu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_plugins.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

namespace Settings {
namespace {

namespace Flua = Plugins::Flua;
using Flua::CatalogPlugin;
using Flua::CatalogSource;
using Flua::CatalogState;
using Flua::Translated;

constexpr auto kDescriptionLines = 2;

// The channel the source section shows: sections are made by type only.
[[nodiscard]] QString &CurrentSource() {
	static auto result = QString();
	return result;
}

[[nodiscard]] QString PluginsCountText(int count) {
	const auto mod10 = count % 10;
	const auto mod100 = count % 100;
	const auto russian = (mod10 == 1 && mod100 != 11)
		? "плагин"
		: (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14))
		? "плагина"
		: "плагинов";
	return QString::number(count)
		+ u' '
		+ Translated((count == 1) ? "plugin" : "plugins", russian);
}

[[nodiscard]] QString SourceStatus(const CatalogSource *source) {
	if (!source) {
		return QString();
	}
	switch (source->state) {
	case CatalogState::Failed:
		return source->error;
	case CatalogState::Ready:
		return PluginsCountText(source->validCount());
	case CatalogState::Loading:
		if (source->filesTotal > 0) {
			return Translated("Checking plugins: ", "Проверяю плагины: ")
				+ QString::number(source->filesDone)
				+ Translated(" of ", " из ")
				+ QString::number(source->filesTotal);
		}
		break;
	case CatalogState::Idle:
		break;
	}
	return Translated("Loading…", "Загрузка…");
}

[[nodiscard]] QString SourceTitle(
		const CatalogSource *source,
		const QString &username) {
	return (source && !source->title.isEmpty())
		? source->title
		: (u"@"_q + username);
}

// A channel of the catalog: its picture, name and how many plugins.
class SourceRow final : public Ui::RippleButton {
public:
	SourceRow(
		QWidget *parent,
		not_null<Flua::Catalog*> catalog,
		QString username,
		bool big);

	void setRemoveCallback(Fn<void()> callback);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const not_null<Flua::Catalog*> _catalog;
	const QString _username;
	const bool _big = false;
	Ui::PeerUserpicView _userpic;
	std::unique_ptr<Ui::EmptyUserpic> _empty;
	Ui::IconButton *_remove = nullptr;

};

SourceRow::SourceRow(
	QWidget *parent,
	not_null<Flua::Catalog*> catalog,
	QString username,
	bool big)
: RippleButton(parent, st::defaultRippleAnimation)
, _catalog(catalog)
, _username(std::move(username))
, _big(big) {
	_catalog->session()->downloaderTaskFinished(
	) | rpl::on_next([=] {
		update();
	}, lifetime());
	if (_big) {
		setDisabled(true);
	}
}

void SourceRow::setRemoveCallback(Fn<void()> callback) {
	if (!_remove) {
		_remove = Ui::CreateChild<Ui::IconButton>(this, st::pluginsCardAction);
		_remove->setIconOverride(
			&st::pluginsActionRemoveSource,
			&st::pluginsActionRemoveSourceOver);
		_remove->setRippleColorOverride(&st::windowBgRipple);
	}
	_remove->setClickedCallback(std::move(callback));
	if (width() > 0) {
		resizeToWidth(width());
	}
}

int SourceRow::resizeGetHeight(int newWidth) {
	const auto height = _big
		? st::pluginsSourceBigHeight
		: st::pluginsSourceHeight;
	if (_remove) {
		_remove->moveToRight(
			st::pluginsSourceRight,
			(height - _remove->height()) / 2,
			newWidth);
	}
	return height;
}

void SourceRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (!_big) {
		paintRipple(p, 0, 0);
	}
	const auto source = _catalog->source(_username);
	const auto size = _big
		? st::pluginsSourceBigUserpic
		: st::pluginsSourceUserpic;
	const auto x = st::pluginsSourceLeft;
	const auto y = (height() - size) / 2;
	if (const auto peer = source ? source->peer : nullptr) {
		peer->paintUserpicLeft(p, _userpic, x, y, width(), size);
	} else {
		if (!_empty) {
			_empty = std::make_unique<Ui::EmptyUserpic>(
				Ui::EmptyUserpic::UserpicColor(
					Ui::EmptyUserpic::ColorIndex(qHash(_username))),
				_username);
		}
		_empty->paintCircle(p, x, y, width(), size);
	}

	const auto left = x + size + st::pluginsSourceTextSkip;
	const auto right = st::pluginsSourceRight
		+ (_remove ? _remove->width() : 0);
	const auto available = std::max(width() - left - right, 1);
	const auto &nameStyle = _big
		? st::pluginsSourceBigNameStyle
		: st::pluginsSourceNameStyle;
	const auto nameHeight = nameStyle.font->height;
	const auto &statusFont = st::pluginsSourceStatusFont;
	const auto top = (height()
		- nameHeight
		- st::pluginsSourceLineSkip
		- statusFont->height) / 2;

	const auto verified = source && source->verified;
	const auto &badge = st::dialogsExteraOfficialIcon.icon;
	const auto badgeWidth = verified
		? (st::pluginsSourceBadgeSkip + badge.width())
		: 0;
	const auto name = nameStyle.font->elided(
		SourceTitle(source, _username),
		std::max(available - badgeWidth, 1));
	p.setFont(nameStyle.font);
	p.setPen(st::windowBoldFg);
	p.drawTextLeft(left, top, width(), name);
	if (verified) {
		badge.paint(
			p,
			left + nameStyle.font->width(name) + st::pluginsSourceBadgeSkip,
			top + (nameHeight - badge.height()) / 2,
			width());
	}

	const auto failed = source && (source->state == CatalogState::Failed);
	p.setFont(statusFont);
	p.setPen(failed ? st::attentionButtonFg : st::windowSubTextFg);
	p.drawTextLeft(
		left,
		top + nameHeight + st::pluginsSourceLineSkip,
		width(),
		statusFont->elided(SourceStatus(source), available));
}

// "All" and the categories the channel has; one of them is chosen.
class CategoryChips final : public Ui::RpWidget {
public:
	CategoryChips(
		QWidget *parent,
		const std::vector<QString> &categories,
		QString selected,
		Fn<void(QString)> changed);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;

private:
	struct Chip {
		QString key;
		QString text;
		QRect rect;
	};

	[[nodiscard]] const Chip *chipAt(QPoint point) const;

	std::vector<Chip> _chips;
	QString _selected;
	const Fn<void(QString)> _changed;

};

CategoryChips::CategoryChips(
	QWidget *parent,
	const std::vector<QString> &categories,
	QString selected,
	Fn<void(QString)> changed)
: RpWidget(parent)
, _selected(std::move(selected))
, _changed(std::move(changed)) {
	_chips.push_back({ QString(), Translated("All", "Все") });
	for (const auto &category : categories) {
		_chips.push_back({ category, Flua::CategoryName(category) });
	}
	setMouseTracking(true);
}

int CategoryChips::resizeGetHeight(int newWidth) {
	const auto &font = st::pluginsCatalogChipFont;
	const auto height = st::pluginsCatalogChipHeight;
	const auto skip = st::pluginsCatalogChipSkip;
	auto x = 0;
	auto y = 0;
	for (auto &chip : _chips) {
		const auto w = std::min(
			font->width(chip.text) + 2 * st::pluginsCatalogChipPadding,
			std::max(newWidth, 1));
		if (x > 0 && x + w > newWidth) {
			x = 0;
			y += height + skip;
		}
		chip.rect = QRect(x, y, w, height);
		x += w + skip;
	}
	return y + height;
}

void CategoryChips::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	PainterHighQualityEnabler hq(p);
	const auto &font = st::pluginsCatalogChipFont;
	const auto radius = st::pluginsCatalogChipHeight / 2.;
	for (const auto &chip : _chips) {
		const auto selected = (chip.key == _selected);
		auto fg = (selected ? st::windowActiveTextFg : st::windowSubTextFg)->c;
		auto bg = selected ? fg : st::windowBgOver->c;
		if (selected) {
			bg.setAlphaF(0.15);
		}
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawRoundedRect(chip.rect, radius, radius);
		p.setPen(fg);
		p.setFont(font);
		p.drawText(
			chip.rect,
			Qt::AlignCenter,
			font->elided(
				chip.text,
				std::max(
					chip.rect.width() - 2 * st::pluginsCatalogChipPadding,
					1)));
	}
}

const CategoryChips::Chip *CategoryChips::chipAt(QPoint point) const {
	for (const auto &chip : _chips) {
		if (chip.rect.contains(point)) {
			return &chip;
		}
	}
	return nullptr;
}

void CategoryChips::mouseMoveEvent(QMouseEvent *e) {
	setCursor(chipAt(e->pos()) ? style::cur_pointer : style::cur_default);
}

void CategoryChips::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto chip = chipAt(e->pos());
	if (!chip || chip->key == _selected) {
		return;
	}
	_selected = chip->key;
	update();
	// The list with these chips is built again: not while in the handler.
	const auto key = _selected;
	const auto changed = _changed;
	crl::on_main(this, [=] {
		changed(key);
	});
}

enum class CardKind {
	Available,
	Update,
	Installed,
	Broken,
};

// A plugin of the channel: picture, name and version, two lines of the
// description, the authors and the categories; one action on the right.
class CatalogCard final : public Ui::RpWidget {
public:
	CatalogCard(
		QWidget *parent,
		const CatalogPlugin &plugin,
		CardKind kind,
		const QString &installedVersion,
		Fn<void()> open,
		Fn<void()> action);

protected:
	int resizeGetHeight(int newWidth) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] int textLeft() const;
	[[nodiscard]] int textWidth(int outer) const;

	const CardKind _kind;
	const Fn<void()> _open;
	QImage _icon;
	Ui::Text::String _name;
	Ui::Text::String _description = { 1 };
	Ui::Text::String _author = { 1 };
	QString _version;
	std::vector<Flua::Badge> _badges;
	Ui::IconButton *_action = nullptr;

	int _layoutWidth = -1;
	int _descriptionTop = 0;
	int _authorTop = 0;
	int _badgesTop = 0;
	bool _pressed = false;

};

CatalogCard::CatalogCard(
	QWidget *parent,
	const CatalogPlugin &plugin,
	CardKind kind,
	const QString &installedVersion,
	Fn<void()> open,
	Fn<void()> action)
: RpWidget(parent)
, _kind(kind)
, _open(std::move(open)) {
	const auto name = plugin.name.isEmpty() ? plugin.fileName : plugin.name;
	_icon = Flua::PrepareIcon(
		Flua::PluginView{
			.id = plugin.id.isEmpty() ? plugin.fileName : plugin.id,
			.name = name,
			.icon = plugin.icon,
		},
		st::pluginsCardIconSize,
		st::pluginsCardIconRadius);
	_name.setText(st::pluginsCardNameStyle, name, kPlainTextOptions);
	_description.setText(
		st::pluginsCardTextStyle,
		(kind == CardKind::Broken) ? plugin.error : plugin.description,
		kPlainTextOptions);
	if (!plugin.author.isEmpty()) {
		_author.setText(
			st::pluginsCatalogAuthorStyle,
			plugin.author,
			kPlainTextOptions);
	}
	_version = (kind == CardKind::Update && !installedVersion.isEmpty())
		? (installedVersion + u" → "_q + plugin.version)
		: plugin.version;
	for (const auto &category : plugin.categories) {
		_badges.push_back({
			Flua::CategoryName(category).toUpper(),
			st::windowSubTextFg->c,
		});
	}
	if (action) {
		_action = Ui::CreateChild<Ui::IconButton>(this, st::pluginsCardAction);
		_action->setRippleColorOverride(&st::windowBgRipple);
		switch (kind) {
		case CardKind::Update:
			_action->setIconOverride(
				&st::pluginsActionUpdate,
				&st::pluginsActionUpdateOver);
			break;
		case CardKind::Installed:
			_action->setIconOverride(
				&st::pluginsActionDelete,
				&st::pluginsActionDeleteOver);
			break;
		default:
			_action->setIconOverride(
				&st::pluginsActionInstall,
				&st::pluginsActionInstallOver);
			break;
		}
		_action->setClickedCallback(std::move(action));
	}
	if (_open) {
		setCursor(style::cur_pointer);
	}
}

int CatalogCard::textLeft() const {
	return st::pluginsCatalogCardPadding.left()
		+ st::pluginsCardIconSize
		+ st::pluginsCardIconSkip;
}

int CatalogCard::textWidth(int outer) const {
	return std::max(
		outer
			- textLeft()
			- st::pluginsCatalogCardPadding.right()
			- (_action ? _action->width() : 0)
			- st::pluginsCatalogTextRight,
		1);
}

void CatalogCard::resizeEvent(QResizeEvent *e) {
	if (width() == _layoutWidth) {
		return;
	}
	const auto height = resizeGetHeight(width());
	if (height != this->height()) {
		resize(width(), height);
	}
	update();
}

int CatalogCard::resizeGetHeight(int newWidth) {
	_layoutWidth = newWidth;
	const auto &padding = st::pluginsCatalogCardPadding;
	const auto inner = textWidth(newWidth);
	auto top = padding.top() + st::pluginsCardNameStyle.font->height;
	if (!_description.isEmpty()) {
		top += st::pluginsCardLineSkip;
		_descriptionTop = top;
		top += std::min(
			_description.countHeight(inner),
			kDescriptionLines * st::pluginsCardTextStyle.font->height);
	}
	if (!_author.isEmpty()) {
		top += st::pluginsCardLineSkip;
		_authorTop = top;
		top += st::pluginsCatalogAuthorStyle.font->height;
	}
	if (!_badges.empty()) {
		top += st::pluginsCardBlockSkip;
		_badgesTop = top;
		top += Flua::LayoutBadges(_badges, inner);
	}
	const auto height = std::max(
		top,
		padding.top() + st::pluginsCardIconSize) + padding.bottom();
	if (_action) {
		_action->moveToRight(
			padding.right(),
			(height - _action->height()) / 2,
			newWidth);
	}
	return height;
}

void CatalogCard::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto &padding = st::pluginsCatalogCardPadding;
	{
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(
			rect(),
			st::pluginsCardRadius,
			st::pluginsCardRadius);
	}
	p.drawImage(QPoint(padding.left(), padding.top()), _icon);

	const auto left = textLeft();
	const auto inner = textWidth(width());
	const auto &versionFont = st::pluginsCatalogVersionFont;
	const auto versionWidth = _version.isEmpty()
		? 0
		: std::min(
			versionFont->width(_version)
				+ 2 * st::pluginsCatalogVersionPadding,
			inner / 2);
	const auto nameAvailable = std::max(
		inner - (versionWidth
			? (versionWidth + st::pluginsCatalogVersionSkip)
			: 0),
		1);
	const auto nameWidth = std::min(_name.maxWidth(), nameAvailable);
	p.setPen(st::windowBoldFg);
	_name.drawLeftElided(p, left, padding.top(), nameAvailable, width());
	if (versionWidth) {
		const auto nameHeight = st::pluginsCardNameStyle.font->height;
		const auto height = st::pluginsCatalogVersionHeight;
		const auto pill = QRect(
			left + nameWidth + st::pluginsCatalogVersionSkip,
			padding.top() + (nameHeight - height) / 2,
			versionWidth,
			height);
		auto fg = ((_kind == CardKind::Update)
			? st::windowActiveTextFg
			: st::windowSubTextFg)->c;
		auto bg = fg;
		bg.setAlphaF(0.14);
		{
			PainterHighQualityEnabler hq(p);
			p.setPen(Qt::NoPen);
			p.setBrush(bg);
			p.drawRoundedRect(pill, height / 2., height / 2.);
		}
		p.setPen(fg);
		p.setFont(versionFont);
		p.drawText(
			pill,
			Qt::AlignCenter,
			versionFont->elided(
				_version,
				std::max(
					pill.width() - 2 * st::pluginsCatalogVersionPadding,
					1)));
	}
	if (!_description.isEmpty()) {
		p.setPen((_kind == CardKind::Broken)
			? st::attentionButtonFg
			: st::windowFg);
		_description.drawLeftElided(
			p,
			left,
			_descriptionTop,
			inner,
			width(),
			kDescriptionLines);
	}
	if (!_author.isEmpty()) {
		p.setPen(st::windowSubTextFg);
		_author.drawLeftElided(p, left, _authorTop, inner, width());
	}
	if (!_badges.empty()) {
		Flua::LayoutBadges(_badges, inner, &p, QPoint(left, _badgesTop));
	}
}

void CatalogCard::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) && _open;
}

void CatalogCard::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, false);
	if (pressed && rect().contains(e->pos()) && _open) {
		const auto open = _open;
		open();
	}
}

void ConfirmRemovePlugin(
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

void ShowAddSourceBox(
		not_null<Window::SessionController*> controller,
		not_null<Flua::Catalog*> catalog) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(
			Translated("Add a channel", "Добавить канал")));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			Translated(
				"Plugins are the .flua files posted in the channel.",
				"Плагины — это файлы .flua, выложенные в канале."),
			st::boxLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"@channel, t.me/channel"_q)));
		const auto submit = [=] {
			auto error = QString();
			const auto added = catalog->addSource(
				field->getLastText(),
				&error);
			if (added.isEmpty()) {
				field->showError();
				controller->showToast(error);
				return;
			}
			box->closeBox();
		};
		field->submits(
		) | rpl::on_next([=](Qt::KeyboardModifiers) {
			submit();
		}, field->lifetime());
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		box->addButton(rpl::single(Translated("Add", "Добавить")), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

class PluginsCatalogSourceSection final
	: public Section<PluginsCatalogSourceSection> {
public:
	PluginsCatalogSourceSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
	void refresh();
	void rebuild();
	[[nodiscard]] QString stateKey() const;

	const not_null<Flua::Catalog*> _catalog;
	const QString _username;
	SourceRow *_header = nullptr;
	Ui::VerticalLayout *_list = nullptr;
	QString _query;
	QString _category;
	QString _builtKey;

};

class PluginsCatalogSection final : public Section<PluginsCatalogSection> {
public:
	PluginsCatalogSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
	void rebuildSources();

	const not_null<Flua::Catalog*> _catalog;
	Ui::VerticalLayout *_verified = nullptr;
	Ui::VerticalLayout *_custom = nullptr;
	Ui::FlatLabel *_noCustom = nullptr;
	std::vector<not_null<SourceRow*>> _rows;
	QStringList _shown;

};

PluginsCatalogSection::PluginsCatalogSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _catalog(Flua::CatalogFor(&controller->session())) {
	setupContent();
}

rpl::producer<QString> PluginsCatalogSection::title() {
	return rpl::single(Translated("Plugin catalog", "Каталог плагинов"));
}

void PluginsCatalogSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	const auto size = st::pluginsCatalogLogoSize;
	auto logo = object_ptr<Ui::RpWidget>(content);
	const auto logoRaw = logo.data();
	logoRaw->resize(size, size);
	logoRaw->setNaturalWidth(size);
	logoRaw->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(logoRaw);
		PainterHighQualityEnabler hq(p);
		p.drawImage(QRect(0, 0, size, size), AyuAssets::currentAppLogo());
	}, logoRaw->lifetime());
	content->add(
		std::move(logo),
		style::margins(0, st::pluginsCatalogLogoTop, 0, 0),
		style::al_top);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			Translated("Plugin catalog", "Каталог плагинов"),
			st::pluginsCatalogTitle),
		style::margins(
			st::boxRowPadding.left(),
			st::pluginsCardBlockSkip,
			st::boxRowPadding.right(),
			0),
		style::al_top);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			Translated(
				"Plugins posted in Telegram channels: install them in "
				"one click, each one is checked before it is installed.",
				"Плагины из Telegram-каналов: установка в одно нажатие, "
				"каждый проверяется перед установкой."),
			st::pluginsCatalogAbout),
		st::pluginsCatalogAboutPadding,
		style::al_top);

	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(
		content,
		rpl::single(Translated("Verified channels", "Проверенные каналы")));
	_verified = content->add(object_ptr<Ui::VerticalLayout>(content));

	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(
		content,
		rpl::single(Translated("Your channels", "Свои каналы")));
	_custom = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto controller = this->controller();
	const auto catalog = _catalog;
	AddButtonWithIcon(
		content,
		rpl::single(Translated("Add a channel", "Добавить канал")),
		st::settingsButton,
		{ &st::menuIconAdd }
	)->setClickedCallback([=] {
		ShowAddSourceBox(controller, catalog);
	});
	Ui::AddSkip(content);
	Ui::AddDividerText(
		content,
		rpl::single(Translated(
			"Post .flua files in your channel and add it here. Plugins "
			"from your channels install only with \"Unknown sources\" on "
			"in Plugin engine.",
			"Выложите файлы .flua в своём канале и добавьте его сюда. "
			"Плагины из своих каналов ставятся, только если в «Движке "
			"плагинов» включены «Неизвестные источники».")));

	rebuildSources();
	_catalog->changes() | rpl::on_next([=] {
		rebuildSources();
	}, lifetime());
	_catalog->loadAll();

	Ui::ResizeFitChild(this, content);
}

void PluginsCatalogSection::rebuildSources() {
	auto names = QStringList();
	for (const auto &source : _catalog->sources()) {
		names.push_back(source.username);
	}
	if (names == _shown) {
		for (const auto row : _rows) {
			row->update();
		}
		return;
	}
	_shown = names;
	_rows.clear();
	_verified->clear();
	_custom->clear();
	const auto controller = this->controller();
	const auto catalog = _catalog;
	auto custom = 0;
	for (const auto &source : _catalog->sources()) {
		const auto list = source.verified ? _verified : _custom;
		const auto username = source.username;
		const auto row = list->add(object_ptr<SourceRow>(
			list,
			_catalog,
			username,
			false));
		row->setClickedCallback([=] {
			CurrentSource() = username;
			showOther(PluginsCatalogSourceSection::Id());
		});
		if (!source.verified) {
			++custom;
			row->setRemoveCallback([=] {
				controller->show(Ui::MakeConfirmBox({
					.text = Translated(
						"Remove @%1 from the catalog? Installed plugins "
						"stay.",
						"Убрать @%1 из каталога? Установленные плагины "
						"останутся.").arg(username),
					.confirmed = [=](Fn<void()> close) {
						catalog->removeSource(username);
						close();
					},
					.confirmText = Translated("Remove", "Убрать"),
					.confirmStyle = &st::attentionBoxButton,
				}));
			});
		}
		_rows.push_back(row);
	}
	if (!custom) {
		_custom->add(
			object_ptr<Ui::FlatLabel>(
				_custom,
				Translated(
					"No channels of your own yet.",
					"Своих каналов пока нет."),
				st::pluginsCatalogAbout),
			st::pluginsCatalogEmptyPadding);
	}
	for (const auto list : { _verified, _custom }) {
		if (list->width() > 0) {
			list->resizeToWidth(list->width());
		}
	}
}

PluginsCatalogSourceSection::PluginsCatalogSourceSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _catalog(Flua::CatalogFor(&controller->session()))
, _username(CurrentSource()) {
	setupContent();
}

rpl::producer<QString> PluginsCatalogSourceSection::title() {
	const auto catalog = _catalog;
	const auto username = _username;
	return rpl::single(
		rpl::empty
	) | rpl::then(
		catalog->changes()
	) | rpl::map([=] {
		return SourceTitle(catalog->source(username), username);
	});
}

void PluginsCatalogSourceSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	const auto controller = this->controller();
	const auto username = _username;

	_header = content->add(object_ptr<SourceRow>(
		content,
		_catalog,
		username,
		true));
	const auto opened = _catalog->source(username);
	const auto repository = opened ? opened->repository : QString();
	AddButtonWithIcon(
		content,
		rpl::single(repository.isEmpty()
			? Translated("Open the channel", "Открыть канал")
			: Translated("Open on GitHub", "Открыть на GitHub")),
		st::settingsButton,
		{ repository.isEmpty() ? &st::menuIconChannel : &st::menuIconIpAddress }
	)->setClickedCallback([=] {
		if (!repository.isEmpty()) {
			QDesktopServices::openUrl(
				QUrl(u"https://github.com/"_q + repository));
			return;
		}
		controller->showPeerByLink(Window::PeerByLinkInfo{
			.usernameOrId = username,
		});
	});
	const auto catalog = _catalog;
	AddButtonWithIcon(
		content,
		rpl::single(Translated("Check for updates", "Проверить обновления")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->setClickedCallback([=] {
		catalog->load(username, true);
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
	Ui::AddSkip(content);

	_catalog->changes() | rpl::on_next([=] {
		refresh();
	}, lifetime());
	if (const auto host = Flua::CurrentHost()) {
		host->changes() | rpl::on_next([=] {
			refresh();
		}, lifetime());
	}
	_catalog->load(_username);
	rebuild();

	Ui::ResizeFitChild(this, content);
}

QString PluginsCatalogSourceSection::stateKey() const {
	const auto source = _catalog->source(_username);
	if (!source) {
		return u"none"_q;
	}
	auto parts = QStringList{ _query, _category };
	switch (source->state) {
	case CatalogState::Ready:
		parts.push_back(u"ready"_q);
		for (const auto &plugin : source->plugins) {
			parts.push_back(plugin.id
				+ u':'
				+ plugin.version
				+ u':'
				+ QString::number(plugin.msgId.bare)
				+ u':'
				+ plugin.error);
		}
		break;
	case CatalogState::Failed:
		parts.push_back(u"failed:"_q + source->error);
		break;
	default:
		parts.push_back(u"loading"_q);
		break;
	}
	if (const auto host = Flua::CurrentHost()) {
		for (const auto &view : host->pluginViews()) {
			parts.push_back(u"installed:"_q
				+ view.id
				+ u':'
				+ view.version
				+ (view.dev ? u":dev"_q : QString()));
		}
	}
	return parts.join(QChar(0));
}

void PluginsCatalogSourceSection::refresh() {
	if (_header) {
		_header->update();
	}
	if (stateKey() != _builtKey) {
		rebuild();
	}
}

void PluginsCatalogSourceSection::rebuild() {
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

	const auto controller = this->controller();
	const auto catalog = _catalog;
	const auto username = _username;
	const auto source = _catalog->source(_username);

	// The categories the channel has; a chosen one that is gone is reset
	// before the state is remembered, or the next change builds again.
	auto categories = std::vector<QString>();
	if (source && source->state == CatalogState::Ready) {
		for (const auto &category : Plugins::Flua::KnownCategories()) {
			const auto used = ranges::any_of(source->plugins, [&](
					const CatalogPlugin &plugin) {
				return plugin.error.isEmpty()
					&& ranges::contains(plugin.categories, category);
			});
			if (used) {
				categories.push_back(category);
			}
		}
		if (!ranges::contains(categories, _category)) {
			_category = QString();
		}
	}
	_builtKey = stateKey();

	if (!source) {
		Ui::AddDividerText(
			_list,
			rpl::single(Translated(
				"The channel is not in the catalog.",
				"Этого канала нет в каталоге.")));
		return;
	} else if (source->state == CatalogState::Failed) {
		Ui::AddDividerText(
			_list,
			rpl::single(Translated(
				"Could not load the plugins: ",
				"Не удалось загрузить плагины: ") + source->error));
		Ui::AddSkip(_list);
		AddButtonWithIcon(
			_list,
			rpl::single(Translated("Try again", "Повторить")),
			st::settingsButton,
			{ &st::menuIconRestore }
		)->setClickedCallback([=] {
			catalog->load(username, true);
		});
		return;
	} else if (source->state != CatalogState::Ready) {
		_list->add(
			object_ptr<Ui::FlatLabel>(
				_list,
				Translated(
					"Loading the plugins of the channel…",
					"Загружаю плагины канала…"),
				st::pluginsCatalogAbout),
			st::pluginsCatalogEmptyPadding);
		return;
	}

	struct Installed {
		QString version;
		bool dev = false;
	};
	auto installed = base::flat_map<QString, Installed>();
	if (const auto host = Flua::CurrentHost()) {
		for (const auto &view : host->pluginViews()) {
			installed[view.id] = Installed{ view.version, view.dev };
		}
	}

	if (!categories.empty()) {
		const auto weak = base::make_weak(this);
		_list->add(
			object_ptr<CategoryChips>(
				_list,
				categories,
				_category,
				[=](QString category) {
					if (const auto strong = weak.get()) {
						strong->_category = category;
						strong->rebuild();
					}
				}),
			st::pluginsCatalogChipsPadding);
	}

	auto updates = std::vector<const CatalogPlugin*>();
	auto installedList = std::vector<const CatalogPlugin*>();
	auto available = std::vector<const CatalogPlugin*>();
	auto broken = std::vector<const CatalogPlugin*>();
	const auto query = _query.toLower();
	for (const auto &plugin : source->plugins) {
		if (!query.isEmpty()
			&& !plugin.name.toLower().contains(query)
			&& !plugin.description.toLower().contains(query)
			&& !plugin.author.toLower().contains(query)
			&& !plugin.id.contains(query, Qt::CaseInsensitive)
			&& !plugin.fileName.toLower().contains(query)) {
			continue;
		} else if (!plugin.error.isEmpty()) {
			if (_category.isEmpty()) {
				broken.push_back(&plugin);
			}
			continue;
		} else if (!_category.isEmpty()
			&& !ranges::contains(plugin.categories, _category)) {
			continue;
		}
		const auto i = installed.find(plugin.id);
		if (i == end(installed)) {
			available.push_back(&plugin);
		} else if (!i->second.dev
			&& Flua::CompareVersions(plugin.version, i->second.version) > 0) {
			updates.push_back(&plugin);
		} else {
			installedList.push_back(&plugin);
		}
	}

	const auto verified = source->verified;
	const auto addGroup = [&](
			const QString &title,
			const std::vector<const CatalogPlugin*> &list,
			CardKind kind) {
		if (list.empty()) {
			return;
		}
		Ui::AddSubsectionTitle(_list, rpl::single(title));
		for (const auto plugin : list) {
			const auto path = plugin->path;
			const auto id = plugin->id;
			const auto name = plugin->name;
			const auto error = plugin->error;
			const auto install = [=] {
				if (const auto host = Flua::CurrentHost()) {
					host->offerInstall(path, verified
						? Flua::Host::InstallSource::VerifiedChannel
						: Flua::Host::InstallSource::Channel);
				}
			};
			const auto i = installed.find(id);
			const auto installedVersion = (i != end(installed))
				? i->second.version
				: QString();
			const auto dev = (i != end(installed)) && i->second.dev;
			auto open = Fn<void()>();
			auto action = Fn<void()>();
			switch (kind) {
			case CardKind::Available:
			case CardKind::Update:
				open = install;
				action = install;
				break;
			case CardKind::Installed:
				if (!dev) {
					action = [=] {
						ConfirmRemovePlugin(controller, id, name);
					};
				}
				break;
			case CardKind::Broken:
				open = [=] {
					controller->show(Ui::MakeInformBox(error));
				};
				break;
			}
			_list->add(
				object_ptr<CatalogCard>(
					_list,
					*plugin,
					kind,
					installedVersion,
					std::move(open),
					std::move(action)),
				st::pluginsCardMargin);
		}
		Ui::AddSkip(_list);
	};
	addGroup(Translated("Updates", "Обновления"), updates, CardKind::Update);
	addGroup(
		Translated("Installed", "Установленные"),
		installedList,
		CardKind::Installed);
	addGroup(
		Translated("Available", "Доступные"),
		available,
		CardKind::Available);
	addGroup(
		Translated("Did not pass the checks", "Не прошли проверку"),
		broken,
		CardKind::Broken);

	if (updates.empty()
		&& installedList.empty()
		&& available.empty()
		&& broken.empty()) {
		_list->add(
			object_ptr<Ui::FlatLabel>(
				_list,
				((query.isEmpty() && _category.isEmpty())
					? Translated(
						"There are no .flua plugins in this channel yet.",
						"В этом канале пока нет плагинов .flua.")
					: Translated("Nothing found.", "Ничего не найдено.")),
				st::pluginsCatalogAbout),
			st::pluginsCatalogEmptyPadding);
	}
}

} // namespace

Type PluginsCatalogId() {
	return PluginsCatalogSection::Id();
}

} // namespace Settings
