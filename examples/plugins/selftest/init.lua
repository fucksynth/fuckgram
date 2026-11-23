-- Manual test of the plugin API. Type "/t" in any chat (Saved Messages is
-- best): the command is not sent, the plugin shows the list of checks.
-- Menu items "Тест: ..." are in the message, chat, profile and main menus.

local count = { new = 0, screen = 0, updates = 0, full_user = 0, reactions = 0 }
local last_incoming = {}
local last_screen = "—"

local function show(title, text)
	tg.ui.alert(text, title)
end

local function describe(peer)
	if not peer then
		return "нет данных"
	end
	return string.format("%s (%s, id %s)%s",
		tostring(peer.name),
		tostring(peer.type),
		tostring(peer.id),
		peer.username and (" @" .. peer.username) or "")
end

local help = [[
/t me — кто я, этот чат, открытый чат
/t send — обычное, без звука и отложенное на минуту
/t ask — окна «Да/Нет» и ввода текста (tg.async)
/t ask2 — окно с колбэком вместо tg.async
/t copy — текст в буфер обмена
/t timer — таймер через 3 с и отсчёт 3-2-1
/t files — файлы и настройки плагина
/t json — tg.json туда и обратно
/t invoke — сырые запросы MTProto и ошибки
/t secure — важный запрос: клиент должен спросить разрешение
/t get — последнее входящее в этом чате
/t compose — плагин вставит текст в поле ввода
/t refresh — заново применить подмену [fg]
/t error, /t loop, /t mem — защита: ошибка, вечный цикл, 64 МБ

Ещё: напишите «[fg] привет» — в чате [fg] покажется как ✅.
Ссылка https://example.com/fgtest не откроется — её заберёт плагин.
Реакция 👎 отменяется плагином, остальные показываются уведомлением.
Пункты «Тест: …» — в меню сообщения, чата (⋮), профиля и в главном меню.]]

local commands = {}

commands[""] = function()
	show("Команды теста", help)
end

commands["me"] = function(draft)
	show("Кто я", "Аккаунт: " .. describe(tg.me())
		.. "\nЭтот чат: " .. describe(tg.peers.get(draft.peer))
		.. "\nОткрытый чат: " .. tostring(tg.chats.current()))
end

commands["send"] = function(draft)
	tg.messages.send(draft.peer, "Тест: обычное сообщение")
	tg.messages.send(draft.peer, "Тест: без звука", { silent = true })
	tg.messages.send(draft.peer, "Тест: отложенное на минуту", {
		schedule = os.time() + 60,
	})
	tg.toast("Отправлено два сообщения, третье — в отложенных")
end

commands["ask"] = function(draft)
	tg.async(function()
		local yes = tg.ui.confirm{
			title = "Тест",
			text = "Нажмите «Да» или «Нет».",
			ok = "Да",
			cancel = "Нет",
		}
		tg.toast(yes and "Ответ: да" or "Ответ: нет")
		local text = tg.ui.input{
			title = "Тест",
			text = "Введите текст — он уйдёт сообщением.",
			placeholder = "текст",
		}
		if text and text ~= "" then
			tg.messages.send(draft.peer, "Вы ввели: " .. text)
		else
			tg.toast("Ввод отменён")
		end
	end)
end

commands["ask2"] = function()
	tg.ui.confirm("Окно с колбэком: ответьте как угодно.", function(yes)
		tg.toast("Колбэк получил: " .. tostring(yes))
	end)
end

commands["copy"] = function()
	tg.ui.copy("fuckgram: текст из плагина, " .. os.date("%H:%M:%S"))
	tg.toast("Скопировано — вставьте Ctrl+V в поле ввода")
end

commands["timer"] = function()
	tg.toast("Через 3 секунды начнётся отсчёт")
	tg.timer.after(3000, function()
		local n = 3
		local id
		id = tg.timer.every(1000, function()
			if n == 0 then
				tg.timer.cancel(id)
				tg.toast("Таймер остановлен")
				return
			end
			tg.toast("Отсчёт: " .. n)
			n = n - 1
		end)
	end)
end

commands["files"] = function()
	local written = plugin.files.write("test.txt", "привет " .. os.time())
	local data = plugin.files.read("test.txt")
	local list = table.concat(plugin.files.list(), ", ")
	local removed = plugin.files.remove("test.txt")
	local after = plugin.files.read("test.txt")
	show("Файлы", "записан: " .. tostring(written)
		.. "\nпрочитан: " .. tostring(data)
		.. "\nсписок: " .. list
		.. "\nудалён: " .. tostring(removed)
		.. "\nпосле удаления: " .. tostring(after)
		.. "\nзапусков плагина: " .. tostring(plugin.settings.get("starts", 0)))
end

commands["json"] = function()
	local text = tg.json.encode({ a = 1, list = { 1, 2, 3 }, s = "строка" })
	local back = tg.json.decode(text)
	local bad, err = tg.json.decode("{oops")
	show("JSON", text
		.. "\nобратно: a = " .. tostring(back.a)
		.. ", list[3] = " .. tostring(back.list[3])
		.. ", s = " .. tostring(back.s)
		.. "\nбитый JSON: " .. tostring(bad) .. ", " .. tostring(err))
end

commands["invoke"] = function()
	tg.async(function()
		local lines = {}
		local users, err = tg.invoke{
			_ = "users.getUsers",
			id = { { _ = "inputUserSelf" } },
		}
		if users then
			lines[#lines + 1] = "users.getUsers: "
				.. tostring(users[1] and users[1].first_name)
		else
			lines[#lines + 1] = "users.getUsers: ошибка " .. err.type
		end
		local dc, err2 = tg.invoke{ _ = "help.getNearestDc" }
		if dc then
			lines[#lines + 1] = "help.getNearestDc: DC " .. dc.this_dc
				.. ", страна " .. dc.country
		else
			lines[#lines + 1] = "help.getNearestDc: ошибка " .. err2.type
		end
		local _, bad = tg.invoke{
			_ = "users.getFullUser",
			id = { _ = "inputUserEmpty" },
		}
		lines[#lines + 1] = "ожидаемая ошибка сервера: "
			.. (bad and (bad.code .. " " .. bad.type) or "не пришла")
		local ok, a, b = pcall(tg.invoke, {
			_ = "help.getNearestDc",
			wrong_field = 1,
		})
		local wrong
		if not ok then
			wrong = tostring(a)
		elseif b then
			wrong = b.type .. " " .. tostring(b.description)
		else
			wrong = "ошибки нет — так быть не должно"
		end
		lines[#lines + 1] = "лишнее поле: " .. wrong
		show("tg.invoke", table.concat(lines, "\n"))
	end)
	tg.invoke({ _ = "help.getConfig" }, function(config, err)
		if config then
			tg.toast("Колбэк: help.getConfig, адресов DC: "
				.. #config.dc_options)
		else
			tg.toast("Колбэк: ошибка " .. err.type)
		end
	end)
end

-- Reading Telegram Passport data is harmless, but it is on the list of
-- sensitive requests: the client asks before sending it.
commands["secure"] = function()
	tg.async(function()
		local values, err = tg.invoke{ _ = "account.getAllSecureValues" }
		if values then
			show("Важный запрос", "Разрешено. Документов Telegram Passport: "
				.. #values)
		else
			show("Важный запрос", "Ответ: " .. err.type
				.. "\n(CLIENT_PLUGIN_DENIED — вы нажали «Запретить»)")
		end
	end)
end

commands["get"] = function(draft)
	local id = last_incoming[draft.peer]
	if not id then
		show("tg.messages.get", "В этом чате не было входящих после запуска "
			.. "плагина. Попросите кого-нибудь написать сюда и повторите.")
		return
	end
	local message = tg.messages.get(draft.peer, id)
	if message then
		show("tg.messages.get", "id " .. message.id .. " от " .. message.from
			.. ":\n" .. message.text)
	else
		show("tg.messages.get", "не найдено")
	end
end

commands["compose"] = function()
	tg.timer.after(300, function()
		tg.compose.set("Этот текст вставил плагин")
	end)
end

commands["refresh"] = function()
	tg.ui.refresh()
	tg.toast("Подмена [fg] применена заново")
end

commands["error"] = function()
	tg.toast("Ошибка в плагине — клиент работает дальше, запись в логе")
	tg.timer.after(10, function()
		error("проверочная ошибка")
	end)
end

commands["loop"] = function()
	tg.toast("Вечный цикл — клиент не должен зависнуть")
	tg.timer.after(10, function()
		while true do
		end
	end)
end

commands["mem"] = function()
	tg.toast("Пробуем занять больше 64 МБ")
	tg.timer.after(10, function()
		local t = {}
		for i = 1, 200 do
			t[i] = string.rep("x", 1024 * 1024) .. i
		end
	end)
end

plugin.on("message.send", function(draft)
	local name
	if draft.text == "/t" then
		name = ""
	else
		name = draft.text:match("^/t%s+(%S+)%s*$")
	end
	if not name then
		return nil
	end
	local command = commands[name]
	if command then
		command(draft)
	else
		tg.toast("Нет команды «" .. name .. "», отправьте /t для списка")
	end
	return false
end)

plugin.on("message.new", function(message)
	count.new = count.new + 1
	if not message.out then
		last_incoming[message.peer] = message.id
	end
end)

plugin.on("screen", function(screen)
	count.screen = count.screen + 1
	last_screen = screen.screen
		.. (screen.peer and (" " .. screen.peer) or "")
end)

plugin.on("display", function(shown)
	if shown.place == "message"
		and shown.text
		and shown.text:find("[fg]", 1, true) then
		return (shown.text:gsub("%[fg%]", "✅"))
	end
	return nil
end)

plugin.on("profile.rows", function(peer)
	return { { label = "Тест fuckgram", value = "id " .. peer } }
end)

plugin.on("url.open", function(link)
	if link.url:find("fgtest", 1, true) then
		tg.toast("Ссылку перехватил плагин: " .. link.url)
		return true
	end
	return nil
end)

plugin.on("action", function(action)
	if action.type ~= "reaction" or action.remove then
		return nil
	end
	count.reactions = count.reactions + 1
	if action.emoji == "👎" then
		tg.toast("Плагин отменил реакцию 👎")
		return false
	end
	tg.toast("Реакция: " .. tostring(action.emoji or action.custom_emoji_id))
	return nil
end)

plugin.on("mtproto.response", function()
	count.full_user = count.full_user + 1
	return nil
end, { "users.getFullUser" })

plugin.on("mtproto.updates", function()
	count.updates = count.updates + 1
	return nil
end)

plugin.on("app.state", function(state)
	tg.log("selftest: app.state", state)
end)

plugin.menu("message", "Тест: ответить на это", function(ctx)
	if ctx.message then
		tg.messages.send(ctx.peer, "Тест: ответ", { reply_to = ctx.message.id })
	end
end)

plugin.menu("message", "Тест: изменить", function(ctx)
	local message = ctx.message
	if not message then
		return
	end
	tg.async(function()
		local text = tg.ui.input{
			title = "Изменить сообщение",
			value = message.text,
			ok = "Сохранить",
		}
		if text then
			local queued = tg.messages.edit(ctx.peer, message.id, text)
			tg.toast(queued and "Изменение отправлено"
				or "Это сообщение нельзя изменить")
		end
	end)
end)

plugin.menu("message", "Тест: удалить у себя", function(ctx)
	local message = ctx.message
	if not message then
		return
	end
	tg.async(function()
		local yes = tg.ui.confirm{
			title = "Удалить?",
			text = "Сообщение удалится только у вас.",
			ok = "Удалить",
		}
		if yes then
			tg.toast("Удаляется сообщений: "
				.. tg.messages.delete(ctx.peer, message.id))
		end
	end)
end)

plugin.menu("chat", "Тест: этот чат", function(ctx)
	show("Чат", describe(tg.peers.get(ctx.peer))
		.. "\nВ поле ввода: «" .. tostring(tg.compose.get()) .. "»")
end)

plugin.menu("profile", "Тест: этот профиль", function(ctx)
	show("Профиль", describe(tg.peers.get(ctx.peer)))
end)

plugin.menu("main", "Тест: сводка", function()
	show("Сводка теста", "новых сообщений: " .. count.new
		.. "\nсмен экрана: " .. count.screen .. " (сейчас " .. last_screen .. ")"
		.. "\nпакетов обновлений MTProto: " .. count.updates
		.. "\nответов users.getFullUser: " .. count.full_user
		.. "\nреакций: " .. count.reactions)
end)

function on_enable()
	local starts = plugin.settings.get("starts", 0) + 1
	plugin.settings.set("starts", starts)
	tg.log("selftest: started", starts, "times")
end

function on_disable()
	tg.log("selftest: stopping")
end
