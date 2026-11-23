-- Raw MTProto from Lua: objects are tables with the constructor name in "_",
-- as in api.tl of the client's layer.

-- Others never see "typing...": the request is not sent at all.
plugin.on("mtproto.request", function(call)
	if plugin.settings.get("hide_typing", true) then
		return false
	end
	return nil
end, { "messages.setTyping" })

-- Asks Telegram for the data centers; runs inside tg.async.
local function data_center()
	local dc, err = tg.invoke{ _ = "help.getNearestDc" }
	if not dc then
		tg.log("help.getNearestDc failed:", err.type)
		return nil, plugin.t("error.dc", err.type)
	end
	return plugin.t("dc.reply", {
		country = dc.country,
		this_dc = dc.this_dc,
		nearest = dc.nearest_dc,
	})
end

-- "/dc" is not sent: the plugin asks Telegram and sends the answer instead.
plugin.on("message.send", function(draft)
	if draft.text ~= "/dc" or not plugin.settings.get("dc_command", true) then
		return nil
	end
	tg.async(function()
		local text, problem = data_center()
		if text then
			tg.messages.send(draft.peer, text)
		else
			tg.toast(problem)
		end
	end)
	return false
end)

plugin.settings.page(function()
	return {
		{ type = "header", text = plugin.t("settings.privacy") },
		{
			type = "switch",
			key = "hide_typing",
			text = plugin.t("settings.hide_typing"),
			subtext = plugin.t("settings.hide_typing.about"),
			icon = "ghost",
			default = true,
		},
		{ type = "header", text = plugin.t("settings.data_center") },
		{
			type = "switch",
			key = "dc_command",
			text = plugin.t("settings.dc_command"),
			subtext = plugin.t("settings.dc_command.about"),
			icon = "code",
			default = true,
		},
		{
			type = "text",
			text = plugin.t("settings.check"),
			subtext = plugin.t("settings.check.about"),
			icon = "network",
			accent = true,
			on_click = function()
				tg.async(function()
					local text, problem = data_center()
					tg.ui.alert(text or problem, plugin.t("settings.data_center"))
				end)
			end,
		},
	}
end)
