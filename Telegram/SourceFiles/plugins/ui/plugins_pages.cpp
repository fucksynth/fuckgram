/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/ui/plugins_pages.h"

#include "plugins/runtime/flua_canvas.h"
#include "plugins/runtime/flua_plugin.h"
#include "plugins/runtime/flua_runtime.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_plugins.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Plugins::Flua {
namespace {

constexpr auto kMaxSliderValues = 1001;
constexpr auto kSubtitleLines = 3;

struct PageRequest {
	QString pluginId;
	int builder = kNoRef;
	int generation = 0;
	QString title;
};

// A row of a plugin page: icon, title, subtitle, and a toggle or a value
// on the right.
class PageRow final : public Ui::RippleButton {
public:
	PageRow(QWidget *parent, const PageItem &item, bool toggle);

	void setValue(const QString &value);
	void setChecked(bool checked);
	[[nodiscard]] bool checked() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	[[nodiscard]] int textLeft() const;
	[[nodiscard]] int rightWidth() const;

	const style::icon *_icon = nullptr;
	Ui::Text::String _title = { 1 };
	Ui::Text::String _subtitle = { 1 };
	QString _value;
	std::unique_ptr<Ui::ToggleView> _toggle;
	bool _accent = false;
	bool _red = false;
	int _titleHeight = 0;

};

PageRow::PageRow(QWidget *parent, const PageItem &item, bool toggle)
: RippleButton(parent, st::defaultRippleAnimation)
, _icon(item.icon.isEmpty() ? nullptr : IconByName(item.icon))
, _accent(item.accent)
, _red(item.red) {
	_title.setText(st::pluginsPageTitleStyle, item.text, kPlainTextOptions);
	if (!item.subtext.isEmpty()) {
		_subtitle.setText(
			st::pluginsPageSubtitleStyle,
			item.subtext,
			kPlainTextOptions);
	}
	if (toggle) {
		_toggle = std::make_unique<Ui::ToggleView>(
			st::pluginsCardToggle,
			false,
			[=] { update(); });
	}
}

void PageRow::setValue(const QString &value) {
	_value = value;
	update();
}

void PageRow::setChecked(bool checked) {
	if (_toggle) {
		_toggle->setChecked(checked, anim::type::normal);
	}
}

bool PageRow::checked() const {
	return _toggle && _toggle->checked();
}

int PageRow::textLeft() const {
	return st::pluginsPageRowPadding.left()
		+ (_icon ? st::pluginsPageIconSkip : 0);
}

int PageRow::rightWidth() const {
	if (_toggle) {
		return _toggle->getSize().width() + st::pluginsPageRowPadding.right();
	} else if (!_value.isEmpty()) {
		return std::min(
			st::pluginsPageValueFont->width(_value),
			width() * 2 / 5) + st::pluginsPageRowPadding.right() * 2;
	}
	return st::pluginsPageRowPadding.right();
}

int PageRow::resizeGetHeight(int newWidth) {
	const auto &padding = st::pluginsPageRowPadding;
	const auto available = std::max(
		newWidth - textLeft() - (_toggle
			? (_toggle->getSize().width() + padding.right() * 2)
			: (newWidth * 2 / 5 + padding.right())),
		1);
	_titleHeight = std::min(
		_title.countHeight(available),
		2 * st::pluginsPageTitleStyle.font->height);
	auto height = padding.top() + _titleHeight;
	if (!_subtitle.isEmpty()) {
		height += st::pluginsPageLineSkip + std::min(
			_subtitle.countHeight(available),
			kSubtitleLines * st::pluginsPageSubtitleStyle.font->height);
	}
	return std::max(height + padding.bottom(), st::pluginsPageRowMinHeight);
}

void PageRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	paintRipple(p, 0, 0);
	const auto &padding = st::pluginsPageRowPadding;
	if (_icon) {
		_icon->paint(
			p,
			padding.left(),
			(height() - _icon->height()) / 2,
			width());
	}
	const auto left = textLeft();
	const auto available = std::max(width() - left - rightWidth(), 1);
	const auto subtitleHeight = _subtitle.isEmpty()
		? 0
		: (st::pluginsPageLineSkip + std::min(
			_subtitle.countHeight(available),
			kSubtitleLines * st::pluginsPageSubtitleStyle.font->height));
	const auto content = _titleHeight + subtitleHeight;
	auto top = std::max((height() - content) / 2, padding.top());
	p.setPen(_red
		? st::attentionButtonFg
		: _accent
		? st::windowActiveTextFg
		: st::windowBoldFg);
	_title.drawLeftElided(p, left, top, available, width(), 2);
	top += _titleHeight;
	if (!_subtitle.isEmpty()) {
		top += st::pluginsPageLineSkip;
		p.setPen(st::windowSubTextFg);
		_subtitle.drawLeftElided(
			p,
			left,
			top,
			available,
			width(),
			kSubtitleLines);
	}
	if (_toggle) {
		const auto size = _toggle->getSize();
		_toggle->paint(
			p,
			width() - padding.right() - size.width(),
			(height() - size.height()) / 2,
			width());
	} else if (!_value.isEmpty()) {
		const auto &font = st::pluginsPageValueFont;
		const auto value = font->elided(_value, width() * 2 / 5);
		p.setFont(font);
		p.setPen(st::windowActiveTextFg);
		p.drawTextRight(
			padding.right(),
			(height() - font->height) / 2,
			width(),
			value);
	}
}

// A custom row: the plugin paints it.
class CanvasRow final : public Ui::RpWidget {
public:
	CanvasRow(
		QWidget *parent,
		int height,
		Fn<void(QPainter&, QSize)> paint,
		Fn<void(QPoint)> click);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	const int _height = 0;
	const Fn<void(QPainter&, QSize)> _paint;
	const Fn<void(QPoint)> _click;
	bool _pressed = false;

};

CanvasRow::CanvasRow(
	QWidget *parent,
	int height,
	Fn<void(QPainter&, QSize)> paint,
	Fn<void(QPoint)> click)
: RpWidget(parent)
, _height(height)
, _paint(std::move(paint))
, _click(std::move(click)) {
	if (_click) {
		setCursor(style::cur_pointer);
	}
}

int CanvasRow::resizeGetHeight(int newWidth) {
	return style::ConvertScale(_height);
}

void CanvasRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto scale = style::Scale() / 100.;
	p.scale(scale, scale);
	_paint(p, QSize(int(std::round(width() / scale)), _height));
}

void CanvasRow::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) && _click;
}

void CanvasRow::mouseReleaseEvent(QMouseEvent *e) {
	if (std::exchange(_pressed, false) && rect().contains(e->pos())) {
		const auto scale = style::Scale() / 100.;
		_click(QPoint(
			int(std::round(e->pos().x() / scale)),
			int(std::round(e->pos().y() / scale))));
	}
}

[[nodiscard]] QString NumberText(double value) {
	return (std::floor(value) == value && std::abs(value) < 1e15)
		? QString::number(qint64(value))
		: QString::number(value, 'g', 6);
}

class PageSection final : public Settings::AbstractSection {
public:
	PageSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Settings::Type type,
		PageRequest request);
	~PageSection();

	[[nodiscard]] Settings::Type id() const override;
	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
	void rebuild();
	void release();
	void fill(not_null<Plugin*> plugin);
	void addRow(not_null<Plugin*> plugin, const PageItem &item);
	void showText(const QString &text);

	const Settings::Type _type;
	const PageRequest _request;
	Ui::VerticalLayout *_content = nullptr;
	std::optional<Page> _page;
	// Turned off when the page is built again: boxes it opened stay
	// alive and must not call the released handlers.
	std::shared_ptr<bool> _alive;
	rpl::variable<QString> _title;
	rpl::lifetime _pluginLifetime;
	base::weak_ptr<Plugin> _watched; // its refreshes are watched
	int _builtGeneration = -1;
	bool _builtRunning = false;

};

struct PageFactory final
	: Settings::AbstractSectionFactory
	, std::enable_shared_from_this<PageFactory> {
	explicit PageFactory(PageRequest request)
	: request(std::move(request)) {
	}

	object_ptr<Settings::AbstractSection> create(
			not_null<QWidget*> parent,
			not_null<Window::SessionController*> controller,
			not_null<Ui::ScrollArea*> scroll,
			rpl::producer<Settings::Container> containerValue
	) const override {
		return object_ptr<PageSection>(
			parent,
			controller,
			std::const_pointer_cast<PageFactory>(shared_from_this()),
			request);
	}

	const PageRequest request;
};

PageSection::PageSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	Settings::Type type,
	PageRequest request)
: AbstractSection(parent, controller)
, _type(std::move(type))
, _request(std::move(request))
, _title(_request.title) {
	setupContent();
}

PageSection::~PageSection() {
	release();
}

Settings::Type PageSection::id() const {
	return _type;
}

rpl::producer<QString> PageSection::title() {
	return _title.value();
}

void PageSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	_content = content->add(object_ptr<Ui::VerticalLayout>(content));
	Ui::AddSkip(content);
	rebuild();
	if (const auto host = CurrentHost()) {
		// Started, stopped or restarted: the page is of another run.
		host->changes() | rpl::on_next([=] {
			const auto host = CurrentHost();
			const auto plugin = host
				? host->plugin(_request.pluginId)
				: nullptr;
			const auto running = plugin && plugin->running();
			const auto generation = plugin ? plugin->generation() : -1;
			if (running != _builtRunning
				|| (running && generation != _builtGeneration)) {
				rebuild();
			}
		}, lifetime());
	}
	Ui::ResizeFitChild(this, content);
}

void PageSection::release() {
	if (_alive) {
		*_alive = false;
		_alive = nullptr;
	}
	if (_page) {
		const auto host = CurrentHost();
		if (const auto plugin = host ? host->plugin(_request.pluginId) : nullptr) {
			plugin->releasePage(*_page);
		}
		_page = std::nullopt;
	}
}

void PageSection::showText(const QString &text) {
	Ui::AddDividerText(_content, rpl::single(text));
}

void PageSection::rebuild() {
	release();
	_content->setMinimumHeight(_content->height());
	const auto restoreHeight = gsl::finally([&] {
		_content->setMinimumHeight(0);
		if (_content->width() > 0) {
			_content->resizeToWidth(_content->width());
		}
	});
	_content->clear();

	const auto host = CurrentHost();
	const auto plugin = host ? host->plugin(_request.pluginId) : nullptr;
	_builtRunning = plugin && plugin->running();
	_builtGeneration = plugin ? plugin->generation() : -1;
	if (!plugin) {
		showText(Translated(
			"The plugin is not installed.",
			"Плагин не установлен."));
		return;
	} else if (!plugin->running()) {
		showText(Translated(
			"The plugin is turned off.",
			"Плагин выключен."));
		return;
	} else if (_request.builder != kNoRef
		&& plugin->generation() != _request.generation) {
		showText(Translated(
			"The plugin restarted: open this page again.",
			"Плагин перезапустился: откройте эту страницу заново."));
		return;
	}
	if (_watched.get() != plugin) {
		// A development folder reloads as a new plugin object.
		_watched = base::make_weak(plugin);
		_pluginLifetime.destroy();
		plugin->pageRefreshes() | rpl::on_next([=] {
			rebuild();
		}, _pluginLifetime);
	}
	auto error = QString();
	_page = plugin->buildPage(_request.builder, &error);
	if (!_page) {
		showText(Translated(
			"The page could not be built: ",
			"Страница не собралась: ") + error);
		return;
	}
	_alive = std::make_shared<bool>(true);
	if (!_page->title.isEmpty()) {
		_title = _page->title;
	}
	fill(plugin);
}

void PageSection::fill(not_null<Plugin*> plugin) {
	for (const auto &item : _page->items) {
		addRow(plugin, item);
	}
}

void PageSection::addRow(not_null<Plugin*> plugin, const PageItem &item) {
	using Type = PageItem::Type;
	const auto controller = this->controller();
	const auto weak = base::make_weak(plugin.get());
	const auto alive = _alive;
	const auto generation = _page->generation;
	const auto pluginId = _request.pluginId;
	const auto key = item.key;
	const auto callback = [=](int ref, QJsonValue value) {
		if (!*alive) {
			return;
		}
		if (const auto strong = weak.get()) {
			strong->pageCallback(generation, ref, value);
		}
	};
	const auto store = [=](QJsonValue value) {
		if (const auto strong = weak.get(); strong && *alive) {
			strong->setSetting(key, value);
		}
	};
	const auto current = [&]() {
		const auto value = plugin->setting(key);
		return value.isUndefined() ? item.fallback : value;
	};
	switch (item.type) {
	case Type::Header:
		Ui::AddSubsectionTitle(_content, rpl::single(item.text));
		return;
	case Type::Divider:
		if (item.text.isEmpty()) {
			Ui::AddSkip(_content);
			Ui::AddDivider(_content);
			Ui::AddSkip(_content);
		} else {
			Ui::AddDividerText(_content, rpl::single(item.text));
		}
		return;
	case Type::Switch: {
		const auto row = _content->add(
			object_ptr<PageRow>(_content, item, true));
		row->setChecked(current().toBool());
		const auto onChange = item.onChange;
		row->setClickedCallback([=] {
			const auto now = !row->checked();
			row->setChecked(now);
			store(now);
			callback(onChange, now);
		});
		return;
	}
	case Type::Selector: {
		const auto row = _content->add(
			object_ptr<PageRow>(_content, item, false));
		const auto items = item.items;
		const auto index = [=](const QJsonValue &value) {
			const auto number = int(value.toDouble(1.));
			return std::clamp(number, 1, int(items.size()));
		};
		row->setValue(items[index(current()) - 1]);
		const auto onChange = item.onChange;
		const auto title = item.text;
		row->setClickedCallback([=] {
			const auto strong = weak.get();
			if (!strong || !*alive) {
				return;
			}
			const auto selected = index(strong->setting(key).isUndefined()
				? QJsonValue(1.)
				: strong->setting(key));
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				SingleChoiceBox(box, {
					.title = rpl::single(title),
					.options = items,
					.initialSelection = selected - 1,
					.callback = [=](int chosen) {
						if (!*alive) {
							return;
						}
						const auto value = chosen + 1;
						row->setValue(items[chosen]);
						store(double(value));
						callback(onChange, double(value));
					},
				});
			}));
		});
		return;
	}
	case Type::Input: {
		const auto row = _content->add(
			object_ptr<PageRow>(_content, item, false));
		row->setValue(current().toString());
		const auto onChange = item.onChange;
		const auto title = item.text;
		const auto placeholder = item.placeholder;
		row->setClickedCallback([=] {
			const auto strong = weak.get();
			if (!strong || !*alive) {
				return;
			}
			const auto value = strong->setting(key).toString();
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(rpl::single(title));
				const auto field = box->addRow(object_ptr<Ui::InputField>(
					box,
					st::defaultInputField,
					rpl::single(placeholder),
					value));
				box->setFocusCallback([=] {
					field->setFocusFast();
				});
				const auto save = [=] {
					if (!*alive) {
						box->closeBox();
						return;
					}
					const auto text = field->getLastText();
					row->setValue(text);
					store(text);
					callback(onChange, text);
					box->closeBox();
				};
				field->submits(
				) | rpl::on_next([=](Qt::KeyboardModifiers) {
					save();
				}, field->lifetime());
				box->addButton(rpl::single(Translated("Save", "Сохранить")), save);
				box->addButton(
					rpl::single(Translated("Cancel", "Отмена")),
					[=] { box->closeBox(); });
			}));
		});
		return;
	}
	case Type::Text: {
		const auto row = _content->add(
			object_ptr<PageRow>(_content, item, false));
		const auto onClick = item.onClick;
		const auto page = item.page;
		const auto title = item.text;
		if (onClick == kNoRef && page == kNoRef) {
			row->setDisabled(true);
			return;
		}
		row->setClickedCallback([=] {
			if (onClick != kNoRef) {
				callback(onClick, QJsonValue(QJsonValue::Undefined));
			} else if (*alive) {
				OpenPluginPage(controller, pluginId, page, generation, title);
			}
		});
		return;
	}
	case Type::Slider: {
		const auto row = _content->add(
			object_ptr<PageRow>(_content, item, false));
		row->setDisabled(true);
		const auto min = item.min;
		const auto max = item.max;
		const auto step = item.step;
		const auto count = std::clamp(
			int(std::round((max - min) / step)) + 1,
			2,
			kMaxSliderValues);
		const auto convert = [=](int index) {
			return std::min(min + index * step, max);
		};
		const auto value = std::clamp(current().toDouble(min), min, max);
		row->setValue(NumberText(value));
		const auto slider = _content->add(
			object_ptr<Ui::MediaSlider>(_content, st::settingsScale),
			st::pluginsPageSliderPadding);
		const auto onChange = item.onChange;
		slider->setPseudoDiscrete(
			count,
			convert,
			value,
			[=](double now) { row->setValue(NumberText(now)); },
			[=](double now) {
				row->setValue(NumberText(now));
				store(now);
				callback(onChange, now);
			});
		return;
	}
	case Type::Custom: {
		const auto paint = item.paint;
		const auto onClick = item.onClick;
		_content->add(object_ptr<CanvasRow>(
			_content,
			item.height,
			[=](QPainter &p, QSize size) {
				if (const auto strong = weak.get(); strong && *alive) {
					strong->paintCanvas(generation, paint, p, size);
				}
			},
			(onClick == kNoRef
				? Fn<void(QPoint)>()
				: Fn<void(QPoint)>([=](QPoint) {
					callback(onClick, QJsonValue(QJsonValue::Undefined));
				}))));
		return;
	}
	}
}

} // namespace

void AddSettingsRows(
		not_null<Ui::VerticalLayout*> container,
		std::vector<SettingsRow> rows) {
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);
	for (auto &row : rows) {
		Settings::AddButtonWithIcon(
			container,
			rpl::single(row.text),
			st::settingsButton,
			{ row.icon ? row.icon : &st::menuIconBotCommands }
		)->setClickedCallback(std::move(row.handler));
	}
	Ui::AddSkip(container);
}

Settings::Type PluginPageType(
		const QString &pluginId,
		int builder,
		int generation,
		const QString &title) {
	return std::make_shared<PageFactory>(PageRequest{
		.pluginId = pluginId,
		.builder = builder,
		.generation = generation,
		.title = title,
	});
}

void OpenPluginPage(
		not_null<Window::SessionController*> controller,
		const QString &pluginId,
		int builder,
		int generation,
		const QString &title) {
	controller->showSettings(
		PluginPageType(pluginId, builder, generation, title));
}

} // namespace Plugins::Flua
