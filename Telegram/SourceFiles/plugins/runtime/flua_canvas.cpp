/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_canvas.h"

#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "styles/style_basic.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QPolygonF>

// Lua is built as C++ (see cmake/lib_lua.cmake): no extern "C" here.
#include "lua.h"
#include "lauxlib.h"

namespace Plugins::Flua {
namespace {

constexpr auto kCanvasMeta = "fuckgram.canvas";
constexpr auto kMaxTextSize = 200;

struct CanvasData {
	QPainter *painter = nullptr;
	int width = 0;
	int height = 0;
	const ImageLookup *images = nullptr;
};

[[nodiscard]] QString ToQString(lua_State *L, int index) {
	auto size = size_t(0);
	const auto data = lua_tolstring(L, index, &size);
	return data ? QString::fromUtf8(data, int(size)) : QString();
}

[[nodiscard]] double Scale() {
	return style::Scale() / 100.;
}

[[nodiscard]] CanvasData *Check(lua_State *L) {
	const auto data = static_cast<CanvasData*>(
		luaL_checkudata(L, 1, kCanvasMeta));
	if (!data->painter) {
		luaL_error(L, "the canvas is used outside of its paint function");
	}
	return data;
}

[[nodiscard]] double Number(lua_State *L, int index, double fallback = 0.) {
	return lua_isnumber(L, index) ? lua_tonumber(L, index) : fallback;
}

[[nodiscard]] QColor Color(lua_State *L, int index) {
	return ReadColor(L, index).value_or(st::windowFg->c);
}

struct TextOptions {
	QFont font;
	QColor color;
	int align = Qt::AlignLeft;
	double width = 0.;
	double height = 0.;
};

// { color, size, bold, italic, align = "left" | "center" | "right",
//   width, height } at `index`, if it is a table.
[[nodiscard]] TextOptions ReadTextOptions(lua_State *L, int index) {
	auto result = TextOptions{
		.font = st::normalFont->f,
		.color = st::windowFg->c,
	};
	auto size = 13.;
	index = lua_absindex(L, index);
	if (lua_istable(L, index)) {
		lua_getfield(L, index, "size");
		size = std::clamp(Number(L, -1, size), 1., double(kMaxTextSize));
		lua_getfield(L, index, "bold");
		if (lua_toboolean(L, -1)) {
			result.font.setWeight(QFont::DemiBold);
		}
		lua_getfield(L, index, "italic");
		result.font.setItalic(lua_toboolean(L, -1) != 0);
		lua_getfield(L, index, "color");
		if (const auto color = ReadColor(L, -1)) {
			result.color = *color;
		}
		lua_getfield(L, index, "align");
		const auto align = ToQString(L, -1);
		if (align == u"center"_q) {
			result.align = Qt::AlignHCenter;
		} else if (align == u"right"_q) {
			result.align = Qt::AlignRight;
		}
		lua_getfield(L, index, "width");
		result.width = std::max(Number(L, -1), 0.);
		lua_getfield(L, index, "height");
		result.height = std::max(Number(L, -1), 0.);
		lua_pop(L, 7);
	}
	result.font.setPixelSize(std::max(int(std::round(size)), 1));
	return result;
}

// The rectangle a text takes, with wrapping when a width is given.
[[nodiscard]] QRectF TextRect(
		const QString &text,
		const TextOptions &options,
		QPointF position) {
	const auto metrics = QFontMetricsF(options.font);
	if (options.width > 0.) {
		const auto bounds = metrics.boundingRect(
			QRectF(0., 0., options.width, 1e6),
			options.align | Qt::TextWordWrap,
			text);
		const auto height = (options.height > 0.)
			? std::min(options.height, bounds.height())
			: bounds.height();
		return QRectF(position, QSizeF(options.width, height));
	}
	return QRectF(
		position,
		QSizeF(metrics.horizontalAdvance(text), metrics.height()));
}

int CanvasSize(lua_State *L) {
	const auto data = Check(L);
	lua_pushinteger(L, data->width);
	lua_pushinteger(L, data->height);
	return 2;
}

int CanvasFill(lua_State *L) {
	const auto data = Check(L);
	data->painter->fillRect(
		QRectF(0., 0., data->width, data->height),
		Color(L, 2));
	return 0;
}

// c:rect(x, y, w, h, color, radius)
int CanvasRect(lua_State *L) {
	const auto data = Check(L);
	const auto rect = QRectF(
		Number(L, 2),
		Number(L, 3),
		Number(L, 4),
		Number(L, 5));
	const auto radius = Number(L, 7);
	auto &p = *data->painter;
	if (radius > 0.) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(Color(L, 6));
		p.drawRoundedRect(rect, radius, radius);
	} else {
		p.fillRect(rect, Color(L, 6));
	}
	return 0;
}

// c:stroke(x, y, w, h, color, width, radius)
int CanvasStroke(lua_State *L) {
	const auto data = Check(L);
	const auto width = std::max(Number(L, 7, 1.), 0.1);
	const auto half = width / 2.;
	const auto rect = QRectF(
		Number(L, 2) + half,
		Number(L, 3) + half,
		Number(L, 4) - width,
		Number(L, 5) - width);
	const auto radius = Number(L, 8);
	auto &p = *data->painter;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(QPen(Color(L, 6), width));
	p.setBrush(Qt::NoBrush);
	if (radius > 0.) {
		p.drawRoundedRect(rect, radius, radius);
	} else {
		p.drawRect(rect);
	}
	return 0;
}

// c:circle(x, y, radius, color)
int CanvasCircle(lua_State *L) {
	const auto data = Check(L);
	const auto radius = std::max(Number(L, 4), 0.);
	auto &p = *data->painter;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(Color(L, 5));
	p.drawEllipse(QPointF(Number(L, 2), Number(L, 3)), radius, radius);
	return 0;
}

// c:line(x1, y1, x2, y2, color, width)
int CanvasLine(lua_State *L) {
	const auto data = Check(L);
	auto &p = *data->painter;
	auto hq = PainterHighQualityEnabler(p);
	auto pen = QPen(Color(L, 6), std::max(Number(L, 7, 1.), 0.1));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.drawLine(
		QPointF(Number(L, 2), Number(L, 3)),
		QPointF(Number(L, 4), Number(L, 5)));
	return 0;
}

// c:text(x, y, text, options) -> width, height it took
int CanvasText(lua_State *L) {
	const auto data = Check(L);
	const auto text = ToQString(L, 4);
	const auto options = ReadTextOptions(L, 5);
	const auto rect = TextRect(
		text,
		options,
		QPointF(Number(L, 2), Number(L, 3)));
	auto &p = *data->painter;
	p.setFont(options.font);
	p.setPen(options.color);
	if (options.width > 0.) {
		p.save();
		p.setClipRect(rect);
		p.drawText(
			QRectF(rect.topLeft(), QSizeF(rect.width(), 1e6)),
			options.align | Qt::TextWordWrap,
			text);
		p.restore();
	} else {
		const auto metrics = QFontMetricsF(options.font);
		p.drawText(
			QPointF(rect.x(), rect.y() + metrics.ascent()),
			text);
	}
	lua_pushnumber(L, rect.width());
	lua_pushnumber(L, rect.height());
	return 2;
}

// c:measure(text, options) -> width, height
int CanvasMeasure(lua_State *L) {
	[[maybe_unused]] const auto data = Check(L);
	const auto rect = TextRect(
		ToQString(L, 2),
		ReadTextOptions(L, 3),
		QPointF());
	lua_pushnumber(L, rect.width());
	lua_pushnumber(L, rect.height());
	return 2;
}

// c:image(id, x, y, w, h, radius)
int CanvasImage(lua_State *L) {
	const auto data = Check(L);
	const auto id = int(luaL_checkinteger(L, 2));
	const auto image = (data->images && *data->images)
		? (*data->images)(id)
		: nullptr;
	if (!image || image->isNull()) {
		return luaL_error(L, "no image %d", id);
	}
	const auto x = Number(L, 3);
	const auto y = Number(L, 4);
	const auto w = Number(L, 5, image->width());
	const auto h = Number(L, 6, image->height());
	const auto radius = Number(L, 7);
	const auto rect = QRectF(x, y, w, h);
	auto &p = *data->painter;
	auto hq = PainterHighQualityEnabler(p);
	if (radius > 0.) {
		auto path = QPainterPath();
		path.addRoundedRect(rect, radius, radius);
		p.save();
		p.setClipPath(path);
		p.drawImage(rect, *image);
		p.restore();
	} else {
		p.drawImage(rect, *image);
	}
	return 0;
}

// c:polygon({ x1, y1, x2, y2, ... }, color): a filled polygon.
int CanvasPolygon(lua_State *L) {
	const auto data = Check(L);
	luaL_checktype(L, 2, LUA_TTABLE);
	const auto count = std::min(int(lua_rawlen(L, 2)), 2048) / 2;
	if (count < 3) {
		return 0;
	}
	auto polygon = QPolygonF();
	polygon.reserve(count);
	for (auto i = 0; i != count; ++i) {
		lua_rawgeti(L, 2, i * 2 + 1);
		lua_rawgeti(L, 2, i * 2 + 2);
		polygon.push_back(QPointF(Number(L, -2), Number(L, -1)));
		lua_pop(L, 2);
	}
	auto &p = *data->painter;
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(Color(L, 3));
	p.drawPolygon(polygon);
	return 0;
}

// c:opacity(value): for everything drawn after it in this call.
int CanvasOpacity(lua_State *L) {
	const auto data = Check(L);
	data->painter->setOpacity(std::clamp(Number(L, 2, 1.), 0., 1.));
	return 0;
}

void EnsureCanvasMeta(lua_State *L) {
	if (!luaL_newmetatable(L, kCanvasMeta)) {
		return; // already made, on the stack
	}
	const luaL_Reg methods[] = {
		{ "size", CanvasSize },
		{ "fill", CanvasFill },
		{ "rect", CanvasRect },
		{ "stroke", CanvasStroke },
		{ "circle", CanvasCircle },
		{ "line", CanvasLine },
		{ "text", CanvasText },
		{ "measure", CanvasMeasure },
		{ "image", CanvasImage },
		{ "polygon", CanvasPolygon },
		{ "opacity", CanvasOpacity },
		{ nullptr, nullptr },
	};
	lua_newtable(L);
	luaL_setfuncs(L, methods, 0);
	lua_setfield(L, -2, "__index");
	// getmetatable() of a canvas gives false: plugins can't change it.
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
}

class Overlay final : public Ui::RpWidget {
public:
	Overlay(not_null<Ui::RpWidget*> parent, OverlayDescriptor &&descriptor)
	: RpWidget(parent)
	, _paint(std::move(descriptor.paint))
	, _click(std::move(descriptor.click)) {
		if (descriptor.clickThrough || !_click) {
			setAttribute(Qt::WA_TransparentForMouseEvents);
		} else {
			setCursor(style::cur_pointer);
		}
		setPosition(descriptor.geometry, descriptor.corner);
		parent->sizeValue(
		) | rpl::on_next([=] {
			place();
		}, lifetime());
	}

	void setPosition(QRect geometry, Qt::Corner corner) {
		_geometry = geometry;
		_corner = corner;
		place();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		if (!_paint) {
			return;
		}
		auto p = QPainter(this);
		const auto scale = Scale();
		p.scale(scale, scale);
		_paint(p, _geometry.size());
	}

	void mousePressEvent(QMouseEvent *e) override {
		_pressed = (e->button() == Qt::LeftButton);
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (std::exchange(_pressed, false)
			&& rect().contains(e->pos())
			&& _click) {
			const auto scale = Scale();
			_click(QPoint(
				int(std::round(e->pos().x() / scale)),
				int(std::round(e->pos().y() / scale))));
		}
	}

private:
	void place() {
		const auto parent = parentWidget();
		if (!parent) {
			return;
		}
		const auto scaled = QRect(
			style::ConvertScale(_geometry.x()),
			style::ConvertScale(_geometry.y()),
			style::ConvertScale(_geometry.width()),
			style::ConvertScale(_geometry.height()));
		const auto outer = parent->size();
		const auto right = (_corner == Qt::TopRightCorner)
			|| (_corner == Qt::BottomRightCorner);
		const auto bottom = (_corner == Qt::BottomLeftCorner)
			|| (_corner == Qt::BottomRightCorner);
		const auto x = right
			? (outer.width() - scaled.x() - scaled.width())
			: scaled.x();
		const auto y = bottom
			? (outer.height() - scaled.y() - scaled.height())
			: scaled.y();
		setGeometry(x, y, scaled.width(), scaled.height());
		raise();
		update();
	}

	Fn<void(QPainter&, QSize)> _paint;
	Fn<void(QPoint)> _click;
	QRect _geometry;
	Qt::Corner _corner = Qt::TopLeftCorner;
	bool _pressed = false;

};

} // namespace

std::optional<QColor> ThemeColor(const QString &name) {
	static const auto colors = std::vector<std::pair<QString, const style::color*>>{
		{ u"windowBg"_q, &st::windowBg },
		{ u"windowBgOver"_q, &st::windowBgOver },
		{ u"windowBgRipple"_q, &st::windowBgRipple },
		{ u"windowFg"_q, &st::windowFg },
		{ u"windowSubTextFg"_q, &st::windowSubTextFg },
		{ u"windowBoldFg"_q, &st::windowBoldFg },
		{ u"windowActiveTextFg"_q, &st::windowActiveTextFg },
		{ u"windowShadowFg"_q, &st::windowShadowFg },
		{ u"activeButtonBg"_q, &st::activeButtonBg },
		{ u"activeButtonFg"_q, &st::activeButtonFg },
		{ u"lightButtonFg"_q, &st::lightButtonFg },
		{ u"attentionButtonFg"_q, &st::attentionButtonFg },
		{ u"boxBg"_q, &st::boxBg },
		{ u"boxTextFgGood"_q, &st::boxTextFgGood },
		{ u"menuBg"_q, &st::menuBg },
		{ u"menuIconFg"_q, &st::menuIconFg },
		{ u"shadowFg"_q, &st::shadowFg },
		{ u"placeholderFg"_q, &st::placeholderFg },
		{ u"dialogsBg"_q, &st::dialogsBg },
		{ u"sideBarBg"_q, &st::sideBarBg },
		{ u"titleBg"_q, &st::titleBg },
		{ u"historyComposeAreaBg"_q, &st::historyComposeAreaBg },
		{ u"msgInBg"_q, &st::msgInBg },
		{ u"msgOutBg"_q, &st::msgOutBg },
		{ u"historyTextInFg"_q, &st::historyTextInFg },
		{ u"historyTextOutFg"_q, &st::historyTextOutFg },
		{ u"historyPeer1UserpicBg"_q, &st::historyPeer1UserpicBg },
		{ u"historyPeer2UserpicBg"_q, &st::historyPeer2UserpicBg },
		{ u"historyPeer3UserpicBg"_q, &st::historyPeer3UserpicBg },
		{ u"historyPeer4UserpicBg"_q, &st::historyPeer4UserpicBg },
		{ u"historyPeer5UserpicBg"_q, &st::historyPeer5UserpicBg },
		{ u"historyPeer6UserpicBg"_q, &st::historyPeer6UserpicBg },
		{ u"historyPeer7UserpicBg"_q, &st::historyPeer7UserpicBg },
		{ u"historyPeer8UserpicBg"_q, &st::historyPeer8UserpicBg },
	};
	for (const auto &[key, color] : colors) {
		if (key == name) {
			return (*color)->c;
		}
	}
	return std::nullopt;
}

const std::vector<QString> &ThemeColorNames() {
	static const auto result = std::vector<QString>{
		u"windowBg"_q, u"windowBgOver"_q, u"windowBgRipple"_q,
		u"windowFg"_q, u"windowSubTextFg"_q, u"windowBoldFg"_q,
		u"windowActiveTextFg"_q, u"windowShadowFg"_q,
		u"activeButtonBg"_q, u"activeButtonFg"_q, u"lightButtonFg"_q,
		u"attentionButtonFg"_q, u"boxBg"_q, u"boxTextFgGood"_q,
		u"menuBg"_q, u"menuIconFg"_q, u"shadowFg"_q, u"placeholderFg"_q,
		u"dialogsBg"_q, u"sideBarBg"_q, u"titleBg"_q,
		u"historyComposeAreaBg"_q, u"msgInBg"_q, u"msgOutBg"_q,
		u"historyTextInFg"_q, u"historyTextOutFg"_q,
		u"historyPeer1UserpicBg"_q, u"historyPeer2UserpicBg"_q,
		u"historyPeer3UserpicBg"_q, u"historyPeer4UserpicBg"_q,
		u"historyPeer5UserpicBg"_q, u"historyPeer6UserpicBg"_q,
		u"historyPeer7UserpicBg"_q, u"historyPeer8UserpicBg"_q,
	};
	return result;
}

std::optional<QColor> ReadColor(lua_State *L, int index) {
	index = lua_absindex(L, index);
	if (lua_type(L, index) == LUA_TSTRING) {
		const auto text = ToQString(L, index).trimmed();
		if (text.startsWith(u'#')) {
			const auto color = QColor(text);
			return color.isValid() ? std::make_optional(color) : std::nullopt;
		}
		return ThemeColor(text);
	} else if (lua_istable(L, index)) {
		const auto part = [&](int i, const char *name, int fallback) {
			lua_getfield(L, index, name);
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				lua_rawgeti(L, index, i);
			}
			const auto value = lua_isnumber(L, -1)
				? int(lua_tointeger(L, -1))
				: fallback;
			lua_pop(L, 1);
			return std::clamp(value, 0, 255);
		};
		return QColor(
			part(1, "r", 0),
			part(2, "g", 0),
			part(3, "b", 0),
			part(4, "a", 255));
	}
	return std::nullopt;
}

const style::icon *IconByName(const QString &name) {
	static const auto icons = std::vector<std::pair<QString, const style::icon*>>{
		{ u"settings"_q, &st::menuIconSettings },
		{ u"palette"_q, &st::menuIconPalette },
		{ u"customize"_q, &st::menuIconCustomize },
		{ u"font"_q, &st::menuIconFont },
		{ u"chat"_q, &st::menuIconChatBubble },
		{ u"chats"_q, &st::menuIconChats },
		{ u"reply"_q, &st::menuIconReply },
		{ u"forward"_q, &st::menuIconForward },
		{ u"edit"_q, &st::menuIconEdit },
		{ u"draw"_q, &st::menuIconDraw },
		{ u"copy"_q, &st::menuIconCopy },
		{ u"delete"_q, &st::menuIconDelete },
		{ u"pin"_q, &st::menuIconPin },
		{ u"unpin"_q, &st::menuIconUnpin },
		{ u"select"_q, &st::menuIconSelect },
		{ u"send"_q, &st::menuIconSend },
		{ u"schedule"_q, &st::menuIconSchedule },
		{ u"silent"_q, &st::menuIconSilent },
		{ u"mute"_q, &st::menuIconMute },
		{ u"unmute"_q, &st::menuIconUnmute },
		{ u"link"_q, &st::menuIconLink },
		{ u"share"_q, &st::menuIconShare },
		{ u"download"_q, &st::menuIconDownload },
		{ u"folder"_q, &st::menuIconShowInFolder },
		{ u"file"_q, &st::menuIconFile },
		{ u"photo"_q, &st::menuIconPhoto },
		{ u"gif"_q, &st::menuIconGif },
		{ u"sticker"_q, &st::menuIconStickers },
		{ u"emoji"_q, &st::menuIconEmoji },
		{ u"reactions"_q, &st::menuIconReactions },
		{ u"search"_q, &st::menuIconSearch },
		{ u"translate"_q, &st::menuIconTranslate },
		{ u"info"_q, &st::menuIconInfo },
		{ u"faq"_q, &st::menuIconFaq },
		{ u"user"_q, &st::menuIconProfile },
		{ u"users"_q, &st::menuIconGroups },
		{ u"add_user"_q, &st::menuIconInvite },
		{ u"admin"_q, &st::menuIconAdmin },
		{ u"channel"_q, &st::menuIconChannel },
		{ u"bot"_q, &st::menuIconBot },
		{ u"plugins"_q, &st::menuIconBotCommands },
		{ u"lock"_q, &st::menuIconLock },
		{ u"key"_q, &st::menuIcon2SV },
		{ u"shield"_q, &st::menuIconAntispam },
		{ u"privacy"_q, &st::menuIconPermissions },
		{ u"block"_q, &st::menuIconBlock },
		{ u"eye"_q, &st::menuIconUserShow },
		{ u"eye_off"_q, &st::menuIconUserHide },
		{ u"ghost"_q, &st::menuIconStealth },
		{ u"bell"_q, &st::menuIconNotifications },
		{ u"sound"_q, &st::menuIconSoundOn },
		{ u"sound_off"_q, &st::menuIconSoundOff },
		{ u"phone"_q, &st::menuIconPhone },
		{ u"video"_q, &st::menuIconVideoChat },
		{ u"star"_q, &st::menuIconStar },
		{ u"fave"_q, &st::menuIconFave },
		{ u"like"_q, &st::menuIconLike },
		{ u"premium"_q, &st::menuIconPremium },
		{ u"gift"_q, &st::menuIconGiftPremium },
		{ u"shop"_q, &st::menuIconShop },
		{ u"stats"_q, &st::menuIconStats },
		{ u"timer"_q, &st::menuIconTimer },
		{ u"hourglass"_q, &st::menuIconHourglass },
		{ u"archive"_q, &st::menuIconArchive },
		{ u"saved"_q, &st::menuIconSavedMessages },
		{ u"night"_q, &st::menuIconNightMode },
		{ u"network"_q, &st::menuIconNetwork },
		{ u"storage"_q, &st::menuIconStorage },
		{ u"devices"_q, &st::menuIconDevices },
		{ u"experimental"_q, &st::menuIconExperimental },
		{ u"power"_q, &st::menuIconPowerUsage },
		{ u"restore"_q, &st::menuIconRestore },
		{ u"refresh"_q, &st::menuIconRestartBot },
		{ u"add"_q, &st::menuIconAdd },
		{ u"remove"_q, &st::menuIconRemove },
		{ u"cancel"_q, &st::menuIconCancel },
		{ u"expand"_q, &st::menuIconExpand },
		{ u"collapse"_q, &st::menuIconCollapse },
		{ u"new_window"_q, &st::menuIconNewWindow },
		{ u"qr"_q, &st::menuIconQrCode },
		{ u"location"_q, &st::menuIconAddress },
		{ u"ip"_q, &st::menuIconIpAddress },
		{ u"username"_q, &st::menuIconUsername },
		{ u"topics"_q, &st::menuIconTopics },
		{ u"poll"_q, &st::menuIconCreatePoll },
		{ u"article"_q, &st::menuIconArticle },
		{ u"spoiler"_q, &st::menuIconSpoiler },
		{ u"tag"_q, &st::menuIconTagFilter },
		{ u"payment"_q, &st::menuIconPayment },
		{ u"code"_q, &st::menuIconBotCommands },
	};
	for (const auto &[key, icon] : icons) {
		if (key == name) {
			return icon;
		}
	}
	return nullptr;
}

std::vector<QString> IconNames() {
	return {
		u"settings"_q, u"palette"_q, u"customize"_q, u"font"_q, u"chat"_q,
		u"chats"_q, u"reply"_q, u"forward"_q, u"edit"_q, u"draw"_q,
		u"copy"_q, u"delete"_q, u"pin"_q, u"unpin"_q, u"select"_q,
		u"send"_q, u"schedule"_q, u"silent"_q, u"mute"_q, u"unmute"_q,
		u"link"_q, u"share"_q, u"download"_q, u"folder"_q, u"file"_q,
		u"photo"_q, u"gif"_q, u"sticker"_q, u"emoji"_q, u"reactions"_q,
		u"search"_q, u"translate"_q, u"info"_q, u"faq"_q, u"user"_q,
		u"users"_q, u"add_user"_q, u"admin"_q, u"channel"_q, u"bot"_q,
		u"plugins"_q, u"lock"_q, u"key"_q, u"shield"_q, u"privacy"_q,
		u"block"_q, u"eye"_q, u"eye_off"_q, u"ghost"_q, u"bell"_q,
		u"sound"_q, u"sound_off"_q, u"phone"_q, u"video"_q, u"star"_q,
		u"fave"_q, u"like"_q, u"premium"_q, u"gift"_q, u"shop"_q,
		u"stats"_q, u"timer"_q, u"hourglass"_q, u"archive"_q, u"saved"_q,
		u"night"_q, u"network"_q, u"storage"_q, u"devices"_q,
		u"experimental"_q, u"power"_q, u"restore"_q, u"refresh"_q,
		u"add"_q, u"remove"_q, u"cancel"_q, u"expand"_q, u"collapse"_q,
		u"new_window"_q, u"qr"_q, u"location"_q, u"ip"_q, u"username"_q,
		u"topics"_q, u"poll"_q, u"article"_q, u"spoiler"_q, u"tag"_q,
		u"payment"_q, u"code"_q,
	};
}

void PushCanvas(
		lua_State *L,
		not_null<QPainter*> p,
		QSize size,
		not_null<const ImageLookup*> images) {
	const auto data = static_cast<CanvasData*>(
		lua_newuserdatauv(L, sizeof(CanvasData), 0));
	new (data) CanvasData{
		.painter = p.get(),
		.width = size.width(),
		.height = size.height(),
		.images = images.get(),
	};
	EnsureCanvasMeta(L);
	lua_setmetatable(L, -2);
}

void InvalidateCanvas(lua_State *L, int index) {
	if (const auto data = static_cast<CanvasData*>(
			luaL_testudata(L, index, kCanvasMeta))) {
		data->painter = nullptr;
		data->images = nullptr;
	}
}

base::unique_qptr<Ui::RpWidget> CreateOverlay(
		not_null<Ui::RpWidget*> parent,
		OverlayDescriptor &&descriptor) {
	auto result = base::make_unique_q<Overlay>(
		parent,
		std::move(descriptor));
	result->show();
	return result;
}

void MoveOverlay(
		not_null<Ui::RpWidget*> overlay,
		QRect geometry,
		Qt::Corner corner) {
	if (const auto raw = dynamic_cast<Overlay*>(overlay.get())) {
		raw->setPosition(geometry, corner);
	}
}

} // namespace Plugins::Flua
