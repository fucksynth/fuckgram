-- Автоответчик: «/away текст» — включить (без текста — прежний текст),
-- «/back» — выключить. То же из главного меню. Отвечает только людям в
-- личных сообщениях, каждому не чаще раза в REPEAT_SECONDS, и молчит в
-- чате, куда вы сами писали за последние QUIET_SECONDS.

local REPEAT_SECONDS = 30 * 60
local QUIET_SECONDS = 10 * 60
local DEFAULT_TEXT = "Сейчас не на связи, отвечу, как только смогу."

local repliedAt = {} -- chat id -> когда ответили
local wroteAt = {} -- chat id -> когда вы сами писали

local function enabled()
	return plugin.settings.get("enabled", false)
end

local function turnOn(text)
	if text and text ~= "" then
		plugin.settings.set("text", text)
	end
	plugin.settings.set("enabled", true)
	plugin.settings.set("since", os.time())
	repliedAt = {}
	tg.toast("Автоответчик включён: «"
		.. plugin.settings.get("text", DEFAULT_TEXT) .. "»")
end

local function turnOff()
	plugin.settings.set("enabled", false)
	tg.toast("Автоответчик выключен")
end

plugin.on("message.send", function(draft)
	local text = draft.text
	local away = text:match("^/away%s*(.*)$")
	if away and (away == "" or text:match("^/away%s")) then
		turnOn(away)
		return false
	elseif text == "/back" then
		turnOff()
		return false
	end
	wroteAt[draft.peer] = os.time()
	return nil
end)

plugin.on("message.new", function(msg)
	if not enabled() or msg.out or msg.peer <= 0 or msg.from ~= msg.peer then
		return
	end
	local now = os.time()
	-- Старые сообщения, догруженные после переподключения, не в счёт.
	if msg.date < plugin.settings.get("since", now) or now - msg.date > 300 then
		return
	end
	if (repliedAt[msg.peer] or 0) > now - REPEAT_SECONDS
		or (wroteAt[msg.peer] or 0) > now - QUIET_SECONDS then
		return
	end
	local peer = tg.peers.get(msg.peer)
	if not peer or peer.type ~= "user" or peer.is_self then
		return
	end
	repliedAt[msg.peer] = now
	tg.messages.send(msg.peer, plugin.settings.get("text", DEFAULT_TEXT), {
		reply_to = msg.id,
	})
end)

plugin.menu("main", "Автоответчик", function()
	tg.async(function()
		if enabled() then
			if tg.ui.confirm{
				title = "Автоответчик",
				text = "Сейчас включён: «"
					.. plugin.settings.get("text", DEFAULT_TEXT)
					.. "». Выключить?",
				ok = "Выключить",
				cancel = "Оставить",
			} then
				turnOff()
			end
			return
		end
		local text = tg.ui.input{
			title = "Автоответчик",
			text = "Что отвечать в личных сообщениях, пока вас нет?",
			value = plugin.settings.get("text", DEFAULT_TEXT),
			ok = "Включить",
		}
		if text and text ~= "" then
			turnOn(text)
		end
	end)
end)
