-- Раскладка: «ghbdtn» -> «привет», «руддщ» -> «hello». Направление плагин
-- выбирает сам: каких букв в тексте больше, те и неправильные.

local EN = "`qwertyuiop[]asdfghjkl;'zxcvbnm,./~QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?@#$^&"
local RU = "ёйцукенгшщзхъфывапролджэячсмитьбю.ЁЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ,\"№;:?"

local function chars(text)
	local result = {}
	for _, c in utf8.codes(text) do
		result[#result + 1] = utf8.char(c)
	end
	return result
end

local EN_TO_RU, RU_TO_EN = {}, {}
do
	local en, ru = chars(EN), chars(RU)
	for i = 1, #en do
		EN_TO_RU[en[i]] = ru[i]
		-- Точка, запятая и кавычки есть в обеих раскладках: для обратного
		-- пути берём первое соответствие, так «ю.» -> «./».
		if RU_TO_EN[ru[i]] == nil then
			RU_TO_EN[ru[i]] = en[i]
		end
	end
end

local function convert(text)
	if not utf8.len(text) then
		return nil
	end
	local latin, cyrillic = 0, 0
	for _, c in utf8.codes(text) do
		if (c >= 65 and c <= 90) or (c >= 97 and c <= 122) then
			latin = latin + 1
		elseif (c >= 0x410 and c <= 0x44F) or c == 0x401 or c == 0x451 then
			cyrillic = cyrillic + 1
		end
	end
	if latin == 0 and cyrillic == 0 then
		return nil
	end
	local map = (latin >= cyrillic) and EN_TO_RU or RU_TO_EN
	local result = {}
	for _, char in ipairs(chars(text)) do
		result[#result + 1] = map[char] or char
	end
	return table.concat(result)
end

-- Ссылки и @имена не трогаем: их обычно вставляют, а не набирают.
local function convertKeepingLinks(text)
	local pieces, last = {}, 1
	for start, word, finish in text:gmatch("()(%S+)()") do
		if word:match("^https?://") or word:match("^@[%w_]+$") then
			pieces[#pieces + 1] = convert(text:sub(last, start - 1)) or text:sub(last, start - 1)
			pieces[#pieces + 1] = word
			last = finish
		end
	end
	pieces[#pieces + 1] = convert(text:sub(last)) or text:sub(last)
	return table.concat(pieces)
end

plugin.menu("chat", "Исправить раскладку в поле ввода", function()
	local text = tg.compose.get()
	if not text or text == "" then
		tg.toast("Раскладка: поле ввода пустое")
		return
	end
	tg.compose.set(convertKeepingLinks(text))
end)

plugin.menu("message", "Перевести раскладку", function(ctx)
	local text = ctx.message and ctx.message.text
	if not text or text == "" then
		tg.toast("Раскладка: в сообщении нет текста")
		return
	end
	local fixed = convertKeepingLinks(text)
	tg.ui.copy(fixed)
	tg.ui.alert(fixed, "В другой раскладке (скопировано)")
end)

-- «/fix ghbdtn» уходит как «привет».
plugin.on("message.send", function(draft)
	local text = draft.text:match("^/fix%s+(.+)$")
	if not text then
		return nil
	end
	return convertKeepingLinks(text)
end)
