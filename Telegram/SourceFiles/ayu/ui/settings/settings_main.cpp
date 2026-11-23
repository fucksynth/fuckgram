// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_main.h"

#include "settings/sections/settings_main.h"
#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/ui/ayu_logo.h"
#include "ayu/ui/settings/settings_appearance.h"
#include "ayu/ui/settings/settings_ayu.h"
#include "ayu/ui/settings/settings_chats.h"
#include "ayu/ui/settings/settings_filters.h"
#include "ayu/ui/settings/settings_general.h"
#include "ayu/ui/settings/settings_other.h"
#include "core/version.h"
#include "fuckgram/fuckgram_updater.h"
#include "plugins/runtime/flua_runtime.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_ayu_settings.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"

#include <QDesktopServices>

namespace Settings {

using namespace Builder;

namespace {

void BuildLogo(SectionBuilder &builder) {
	builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		auto logo = object_ptr<Ui::RpWidget>(ctx.container);
		const auto logoRaw = logo.data();
		logoRaw->resize(
			QSize(st::settingsCloudPasswordIconSize,
				st::settingsCloudPasswordIconSize));
		logoRaw->setNaturalWidth(st::settingsCloudPasswordIconSize);
		logoRaw->paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(logoRaw);
			const auto image = AyuAssets::currentAppLogoPad();
			if (!image.isNull()) {
				const auto size = st::settingsCloudPasswordIconSize;
				const auto scaled = image.scaled(
					size * style::DevicePixelRatio(),
					size * style::DevicePixelRatio(),
					Qt::KeepAspectRatio,
					Qt::SmoothTransformation);
				p.drawImage(QRect(0, 0, size, size), scaled);
			}
		}, logoRaw->lifetime());
		return { .widget = std::move(logo), .align = style::al_top };
	});
}

void BuildVersionInfo(SectionBuilder &builder) {
	builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		return {
			.widget = object_ptr<Ui::FlatLabel>(
				ctx.container,
				rpl::single(
					QString("fuckgram v")
					+ QString::fromLatin1(AppVersionStr)),
				st::boxTitle),
			.align = style::al_top,
		};
	});

	builder.addSkip();

	builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		return {
			.widget = object_ptr<Ui::FlatLabel>(
				ctx.container,
				tr::ayu_SettingsDescription(),
				st::centeredBoxLabel),
			.align = style::al_top,
		};
	});
}

void BuildCategories(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSkip();
	builder.addSkip();
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(tr::ayu_CategoriesHeader());

	builder.addSectionButton({
		.title = rpl::single(QString("fuckgram")),
		.targetSection = AyuGhost::Id(),
		.icon = { &st::menuIconGroupReactions },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryFilters(),
		.targetSection = AyuFilters::Id(),
		.icon = { &st::menuIconTagFilter },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryGeneral(),
		.targetSection = AyuGeneral::Id(),
		.icon = { &st::menuIconShowAll },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryAppearance(),
		.targetSection = AyuAppearance::Id(),
		.icon = { &st::menuIconPalette },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryChats(),
		.targetSection = AyuChats::Id(),
		.icon = { &st::menuIconChatBubble },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryOther(),
		.targetSection = AyuOther::Id(),
		.icon = { &st::menuIconFave },
	});
}

void BuildUpdates(SectionBuilder &builder) {
	using namespace Fuckgram;

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(rpl::single(UpdateText(
		"Updates",
		"Обновления")));

	builder.addButton({
		.id = u"fuckgram/update"_q,
		.title = UpdateStatusValue(
		) | rpl::map([](const UpdateStatus &status) {
			return (status.state == UpdateState::Ready)
				? UpdateText("Restart and update", "Перезапустить и обновить")
				: UpdateText("Check for updates", "Проверить обновления");
		}),
		.icon = { &st::menuIconDownload },
		.label = UpdateStatusValue() | rpl::map(UpdateStatusText),
		.onClick = [] {
			if (CurrentUpdateStatus().state == UpdateState::Ready) {
				InstallUpdate();
			} else {
				CheckForUpdates(true);
			}
		},
		.keywords = { u"update"_q, u"обновление"_q, u"version"_q },
	});
	const auto automatic = builder.addButton({
		.id = u"fuckgram/update_auto"_q,
		.title = rpl::single(UpdateText(
			"Check automatically",
			"Проверять автоматически")),
		.icon = { &st::menuIconSchedule },
		.toggled = rpl::single(AutoUpdates()),
		.shown = rpl::single(
			CurrentUpdateStatus().state != UpdateState::Disabled),
	});
	if (automatic) {
		automatic->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != AutoUpdates());
		}) | rpl::on_next([](bool enabled) {
			SetAutoUpdates(enabled);
		}, automatic->lifetime());
	}
	builder.addButton({
		.id = u"fuckgram/releases"_q,
		.title = rpl::single(UpdateText("All versions", "Все версии")),
		.icon = { &st::menuIconInfo },
		.label = rpl::single(QString("GitHub")),
		.onClick = [] {
			QDesktopServices::openUrl(QString(
				"https://github.com/fucksynth/fuckgram/releases"));
		},
	});
}

void BuildLinks(SectionBuilder &builder) {
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(tr::ayu_LinksHeader());

	const auto controller = builder.controller();

	builder.addButton({
		.id = u"ayu/channel"_q,
		.title = tr::ayu_LinksChannel(),
		.icon = { &st::menuIconChannel },
		.label = rpl::single(QString("@fuckgramdesktop")),
		.onClick = [=] {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = QString("fuckgramdesktop"),
			});
		},
	});
	builder.addButton({
		.id = u"ayu/plugins_channel"_q,
		.title = rpl::single(Plugins::Flua::Translated(
			"Plugins",
			"Плагины")),
		.icon = { &st::menuIconBotCommands },
		.label = rpl::single(QString("@fuckgramplugins")),
		.onClick = [=] {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = QString("fuckgramplugins"),
			});
		},
	});
	builder.addButton({
		.id = u"ayu/website"_q,
		.title = tr::ayu_LinksDocumentation(),
		.icon = { &st::menuIconIpAddress },
		.label = rpl::single(QString("GitHub")),
		.onClick = [=] {
			QDesktopServices::openUrl(QString(
				"https://github.com/fucksynth/fuckgram/tree/dev/docs"));
		},
	});

	builder.addSkip();
}

const auto kMeta = BuildHelper({
	.id = AyuMain::Id(),
	.parentId = MainId(),
	.title = &tr::ayu_AyuPreferences,
	.icon = &st::menuIconPremium,
}, [](SectionBuilder &builder) {
	BuildLogo(builder);
	builder.addSkip();
	BuildVersionInfo(builder);
	BuildCategories(builder);
	BuildUpdates(builder);
	BuildLinks(builder);
});

} // namespace

rpl::producer<QString> AyuMain::title() {
	return rpl::single(QString(""));
}

AyuMain::AyuMain(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuMain::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuMainId() {
	return AyuMain::Id();
}

} // namespace Settings
