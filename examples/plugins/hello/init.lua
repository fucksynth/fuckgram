-- Example fuckgram plugin.
-- Copy the "hello" folder into <fuckgram data>/plugins/ and restart.
-- Settings → Plugins shows whether it runs and why not; the gear on its
-- card opens the settings page below.
--
-- Texts are in locales/en.json and locales/ru.json: plugin.t(key, ...)
-- takes the one of the interface language, English when there is none.

local shrug = require("source.shrug")

function on_enable()
	local starts = plugin.settings.get("starts", 0) + 1
	plugin.settings.set("starts", starts)
	tg.log("hello started", starts, "times")
end

-- Outgoing text: return false to cancel, a string to replace, nil to keep.
plugin.on("message.send", function(draft)
	if draft.text == "/nope" and plugin.settings.get("nope", true) then
		return false
	end
	if plugin.settings.get("shrug", true) then
		return shrug.apply(draft.text)
	end
	return nil
end)

-- Built every time the page opens, so the texts follow the language.
local function settings_page()
	local starts = plugin.settings.get("starts", 0)
	return {
		{ type = "header", text = plugin.t("settings.filter") },
		{
			type = "switch",
			key = "shrug",
			text = plugin.t("settings.shrug"),
			subtext = plugin.t("settings.shrug.about"),
			icon = "emoji",
			default = true,
		},
		{
			type = "switch",
			key = "nope",
			text = plugin.t("settings.nope"),
			subtext = plugin.t("settings.nope.about"),
			icon = "block",
			default = true,
		},
		{ type = "header", text = plugin.t("settings.about") },
		{
			type = "text",
			-- A plural text: the number picks "one", "few", "many" or "other".
			text = plugin.t("settings.starts", starts),
			-- A named placeholder: {lang} in the text.
			subtext = plugin.t("settings.language", { lang = tg.lang() }),
			icon = "info",
		},
	}
end

plugin.settings.page(settings_page)

plugin.menu{
	place = "message",
	text = plugin.t("menu.message_id"),
	icon = "info",
	on_click = function(ctx)
		if ctx.message then
			tg.toast(plugin.t("toast.message_id", {
				id = ctx.message.id,
				peer = ctx.peer,
			}))
		end
	end,
}

plugin.menu{
	place = "main",
	text = plugin.t("menu.hello"),
	icon = "plugins",
	on_click = function()
		tg.toast(plugin.t("toast.hello", plugin.settings.get("starts", 0)))
	end,
}
