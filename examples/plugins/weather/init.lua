-- "/weather Moscow" is not sent: the plugin asks wttr.in and sends the
-- answer instead. Shows tg.async + tg.http.request: inside tg.async the
-- request reads like a normal call, the client keeps running meanwhile.
-- "/weather" alone takes the city from the settings page.

-- wttr.in options of the "units" selector: metric, metric with the wind
-- in m/s, US units.
local UNITS = { "m", "m&M", "u" }
-- wttr.in one-line formats of the "format" selector.
local FORMATS = { "3", "4" }

local function urlencode(text)
	return (text:gsub("[^A-Za-z0-9%-_.~]", function(c)
		return string.format("%%%02X", c:byte())
	end))
end

local function trim(text)
	return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function default_city()
	return trim(plugin.settings.get("city", ""))
end

local function weather_url(city)
	local units = UNITS[plugin.settings.get("units", 1)] or UNITS[1]
	local format = FORMATS[plugin.settings.get("format", 1)] or FORMATS[1]
	return "https://wttr.in/" .. urlencode(city)
		.. "?format=" .. format .. "&" .. units
end

-- Runs inside tg.async.
local function send_weather(peer, city)
	local response = tg.http.request{
		url = weather_url(city),
		headers = { ["User-Agent"] = "curl" },
	}
	local body = trim(response.body or "")
	if response.status == 200 and body ~= "" then
		tg.messages.send(peer, body)
	else
		tg.log("weather failed:", response.status, response.error)
		tg.toast{
			title = plugin.t("title"),
			text = plugin.t("error.request", {
				city = city,
				problem = response.error or tostring(response.status),
			}),
		}
	end
end

plugin.on("message.send", function(draft)
	local rest = draft.text:match("^/weather(.*)$")
	if not rest or (rest ~= "" and not rest:match("^%s")) then
		return nil -- not the command, "/weathers" too
	end
	local city = trim(rest)
	if city == "" then
		city = default_city()
	end
	if city == "" then
		tg.toast{ title = plugin.t("title"), text = plugin.t("error.no_city") }
	else
		tg.async(send_weather, draft.peer, city)
	end
	return false -- do not send the command itself
end)

local function settings_page()
	return {
		{ type = "header", text = plugin.t("title") },
		{
			type = "input",
			key = "city",
			text = plugin.t("settings.city"),
			subtext = plugin.t("settings.city.about"),
			placeholder = plugin.t("settings.city.placeholder"),
			icon = "location",
		},
		{
			type = "selector",
			key = "units",
			text = plugin.t("settings.units"),
			items = {
				plugin.t("settings.units.metric"),
				plugin.t("settings.units.metric_ms"),
				plugin.t("settings.units.us"),
			},
			default = 1,
			icon = "customize",
		},
		{
			type = "selector",
			key = "format",
			text = plugin.t("settings.format"),
			items = {
				plugin.t("settings.format.short"),
				plugin.t("settings.format.wind"),
			},
			default = 1,
			icon = "article",
		},
		{ type = "divider", text = plugin.t("settings.usage") },
	}
end

plugin.settings.page(settings_page)

-- Right click on the send button: the weather of the default city, or of
-- the city typed in a box.
plugin.menu{
	place = "send",
	text = plugin.t("menu.send"),
	icon = "location",
	on_click = function(ctx)
		if not ctx.peer then
			return
		end
		tg.async(function()
			local city = default_city()
			if city == "" then
				city = trim(tg.ui.input{
					title = plugin.t("title"),
					text = plugin.t("dialog.text"),
					placeholder = plugin.t("settings.city.placeholder"),
					ok = plugin.t("dialog.send"),
				} or "")
			end
			if city ~= "" then
				send_weather(ctx.peer, city)
			end
		end)
	end,
}
