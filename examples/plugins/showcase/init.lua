-- Витрина API: всё, что плагин может добавить в интерфейс клиента.
--
-- • страница настроек (шестерёнка на карточке плагина) со всеми видами
--   строк, вложенной страницей и строкой, нарисованной на холсте;
-- • пункты в главном меню, в меню чата (⋮), у кнопки отправки и в
--   разделах настроек;
-- • плашка поверх окна с названием открытого чата;
-- • окно с кнопками и отправка с разметкой.

local GREETINGS = { "Привет", "Здравствуйте", "Йо" }

local function escape_html(text)
	return (text:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"))
end

local overlay

local function current_title()
	local now = tg.ui.current()
	return now.title or "Список чатов"
end

local function show_overlay()
	if overlay then
		return
	end
	overlay = tg.ui.overlay{
		corner = "top-right",
		x = 16,
		y = 64,
		width = 230,
		height = 46,
		paint = function(c)
			local w, h = c:size()
			c:rect(0, 0, w, h, "windowBgOver", 12)
			c:stroke(0, 0, w, h, "windowActiveTextFg", 1, 12)
			c:text(12, 6, "Сейчас открыто", { size = 11, color = "windowSubTextFg" })
			c:text(12, 22, current_title(), {
				size = 13,
				bold = true,
				width = w - 24,
				height = 18,
			})
		end,
		on_click = function()
			tg.toast{ title = "Витрина API", text = "Это плашка плагина, она рисуется на Lua." }
		end,
	}
end

local function hide_overlay()
	if overlay then
		overlay:close()
		overlay = nil
	end
end

-- Открыли другой чат: плашка перерисовывается.
plugin.on("screen", function()
	if overlay then
		overlay:update()
	end
end)

local function about_page()
	return {
		title = "О витрине",
		{ type = "header", text = "Что здесь" },
		{
			type = "text",
			text = "Вложенная страница",
			subtext = "Строка с page = function() ... end открывает такую страницу.",
			icon = "info",
		},
		{
			type = "text",
			text = "Канал с плагинами",
			subtext = "@fuckgramplugins",
			icon = "channel",
			accent = true,
			on_click = function()
				tg.ui.open("@fuckgramplugins")
			end,
		},
	}
end

local function settings_page()
	local greeting = GREETINGS[plugin.settings.get("greeting", 1)] or GREETINGS[1]
	local signature = plugin.settings.get("signature", "")
	local size = plugin.settings.get("size", 14)
	return {
		{ type = "header", text = "Отображение" },
		{
			type = "switch",
			key = "overlay",
			text = "Плашка с открытым чатом",
			subtext = "Справа сверху, поверх окна. Нажмите на неё.",
			icon = "eye",
			default = false,
			on_change = function(on)
				if on then
					show_overlay()
				else
					hide_overlay()
				end
			end,
		},
		{
			type = "selector",
			key = "greeting",
			text = "Приветствие",
			items = GREETINGS,
			default = 1,
			icon = "chat",
			on_change = function()
				plugin.settings.refresh()
			end,
		},
		{
			type = "input",
			key = "signature",
			text = "Подпись",
			placeholder = "Например: отправлено из fuckgram",
			icon = "edit",
			on_change = function()
				plugin.settings.refresh()
			end,
		},
		{
			type = "slider",
			key = "size",
			text = "Размер текста в превью",
			min = 10,
			max = 24,
			step = 1,
			default = 14,
			on_change = function()
				plugin.settings.refresh()
			end,
		},
		{
			type = "custom",
			height = 72,
			paint = function(c)
				local w, h = c:size()
				c:rect(22, 8, w - 44, h - 16, "windowBgOver", 10)
				c:text(36, 14, greeting .. "!", {
					size = size,
					bold = true,
					color = "windowActiveTextFg",
				})
				c:text(36, 18 + size, signature ~= "" and signature or "без подписи", {
					size = 12,
					color = "windowSubTextFg",
					width = w - 72,
					height = 16,
				})
			end,
			on_click = function()
				tg.toast("Эта строка нарисована плагином")
			end,
		},
		{ type = "divider", text = "Превью выше нарисовано на холсте и меняется вместе с настройками." },
		{ type = "header", text = "О плагине" },
		{
			type = "text",
			text = "Подробнее",
			subtext = "Вложенная страница",
			icon = "info",
			page = about_page,
		},
		{
			type = "text",
			text = "Сбросить настройки",
			icon = "delete",
			red = true,
			on_click = function()
				tg.async(function()
					local answer = tg.ui.dialog{
						title = "Сбросить настройки?",
						text = "Все значения вернутся к исходным.",
						buttons = { "Сбросить", "Отмена" },
						danger = 1,
					}
					if answer == 1 then
						hide_overlay()
						plugin.settings.reset()
					end
				end)
			end,
		},
	}
end

plugin.settings.page(settings_page)

-- Пункты в разных местах клиента.
plugin.menu{
	place = "main",
	text = "Витрина API",
	icon = "plugins",
	page = settings_page,
}
plugin.menu{
	place = "settings.fuckgram",
	text = "Витрина API",
	icon = "plugins",
	page = settings_page,
}
plugin.menu{
	place = "chat",
	text = "Витрина: поздороваться",
	icon = "chat",
	on_click = function(ctx)
		tg.async(function()
			local greeting = GREETINGS[plugin.settings.get("greeting", 1)] or GREETINGS[1]
			local answer = tg.ui.dialog{
				title = "Поздороваться",
				text = "Отправить «" .. greeting .. "» в этот чат?",
				buttons = { "Отправить", "Отмена" },
			}
			if answer == 1 and ctx.peer then
				tg.messages.send(ctx.peer, "**" .. greeting .. "**", {
					parse_mode = "markdown",
				})
			end
		end)
	end,
}
plugin.menu{
	place = "send",
	text = "Отправить с подписью",
	icon = "send",
	on_click = function(ctx)
		local signature = plugin.settings.get("signature", "")
		if signature == "" then
			tg.toast("Сначала задайте подпись в настройках витрины")
		elseif ctx.peer then
			tg.messages.send(ctx.peer, "<i>" .. escape_html(signature) .. "</i>", {
				parse_mode = "html",
			})
		end
	end,
}

-- Окна может ещё не быть при старте: плашка появится, когда оно станет
-- активным.
plugin.on("app.state", function(state)
	if state == "active" and plugin.settings.get("overlay", false) then
		show_overlay()
	end
end)

function on_enable()
	if plugin.settings.get("overlay", false) then
		show_overlay()
	end
end
