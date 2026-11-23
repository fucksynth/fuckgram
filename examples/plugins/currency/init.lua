-- Курсы валют: «/cur 100 usd rub» не уходит, вместо него плагин спрашивает
-- курс у open.er-api.com (бесплатно, без ключа) и отправляет ответ.
-- Курсы одной валюты держатся час, чтобы не спрашивать на каждое сообщение.

local CACHE_SECONDS = 3600

local ALIASES = {
	["$"] = "USD", ["доллар"] = "USD", ["доллара"] = "USD", ["долларов"] = "USD",
	["бакс"] = "USD", ["баксов"] = "USD",
	["€"] = "EUR", ["евро"] = "EUR",
	["₽"] = "RUB", ["руб"] = "RUB", ["рубль"] = "RUB", ["рубля"] = "RUB",
	["рублей"] = "RUB", ["р"] = "RUB",
	["¥"] = "CNY", ["юань"] = "CNY", ["юаня"] = "CNY", ["юаней"] = "CNY",
	["£"] = "GBP", ["фунт"] = "GBP", ["фунтов"] = "GBP",
	["₸"] = "KZT", ["тенге"] = "KZT",
	["₴"] = "UAH", ["гривна"] = "UAH", ["гривен"] = "UAH",
	["byn"] = "BYN", ["белруб"] = "BYN",
	["лира"] = "TRY", ["лир"] = "TRY",
	["иена"] = "JPY", ["иен"] = "JPY", ["йена"] = "JPY",
	["вон"] = "KRW",
}

local cache = {} -- base code -> { rates = {...}, time = os.time(), updated = unixtime }

-- string.lower знает только латиницу: «Доллар» -> «доллар» делаем сами.
local function lower(text)
	if not utf8.len(text) then
		return text:lower()
	end
	local result = {}
	for _, c in utf8.codes(text) do
		if c >= 0x410 and c <= 0x42F then
			c = c + 0x20
		elseif c == 0x401 then
			c = 0x451
		elseif c >= 65 and c <= 90 then
			c = c + 32
		end
		result[#result + 1] = utf8.char(c)
	end
	return table.concat(result)
end

local function code(word)
	if not word then
		return nil
	end
	local alias = ALIASES[word] or ALIASES[lower(word)]
	if alias then
		return alias
	end
	if word:match("^%a%a%a$") then
		return word:upper()
	end
	return nil
end

-- «1234567.891» -> «1 234 567,89»
local function money(value)
	local fixed = string.format("%.2f", value)
	local whole, fraction = fixed:match("^(-?%d+)%.(%d+)$")
	local sign = ""
	if whole:sub(1, 1) == "-" then
		sign, whole = "-", whole:sub(2)
	end
	local grouped = whole:reverse():gsub("(%d%d%d)", "%1 "):reverse()
	grouped = grouped:gsub("^ ", "")
	return sign .. grouped .. "," .. fraction
end

-- «/cur 100 usd rub», «/cur usd», «/cur 50 € в ₽», «/cur 100$»
local function parse(text)
	local rest = text:match("^/cur%s+(.*)$") or ((text == "/cur") and "")
	if not rest then
		return nil
	end
	rest = lower(rest):gsub(",", "."):gsub("%s+в%s+", " "):gsub("%s+to%s+", " ")
	local words = {}
	for word in rest:gmatch("%S+") do
		-- «100$» -> «100», «$»
		local number, sign = word:match("^([%d%.]+)(%D+)$")
		if number then
			words[#words + 1] = number
			words[#words + 1] = sign
		else
			words[#words + 1] = word
		end
	end
	local amount = tonumber(words[1])
	local first = amount and 2 or 1
	local from = code(words[first])
	local to = code(words[first + 1])
	if not from then
		return false
	end
	if not to then
		to = (from == "RUB") and "USD" or "RUB"
	end
	return { amount = amount or 1, from = from, to = to }
end

local function rates(base, done)
	local cached = cache[base]
	if cached and os.time() - cached.time < CACHE_SECONDS then
		done(cached)
		return
	end
	local response = tg.http.request{
		url = "https://open.er-api.com/v6/latest/" .. base,
	}
	if response.status ~= 200 then
		done(nil, "сервис курсов не ответил (" .. tostring(response.status or response.error) .. ")")
		return
	end
	local data = tg.json.decode(response.body or "")
	if not data or data.result ~= "success" or type(data.rates) ~= "table" then
		done(nil, data and data["error-type"] == "unsupported-code"
			and ("нет такой валюты: " .. base)
			or "непонятный ответ сервиса курсов")
		return
	end
	cached = {
		rates = data.rates,
		time = os.time(),
		updated = tonumber(data.time_last_update_unix),
	}
	cache[base] = cached
	done(cached)
end

plugin.on("message.send", function(draft)
	local request = parse(draft.text)
	if request == nil then
		return nil
	elseif request == false then
		tg.toast("Курсы валют: напишите, например, «/cur 100 usd rub»")
		return false
	end
	tg.async(function()
		rates(request.from, function(result, problem)
			if not result then
				tg.toast("Курсы валют: " .. problem)
				return
			end
			local rate = result.rates[request.to]
			if not rate then
				tg.toast("Курсы валют: нет такой валюты: " .. request.to)
				return
			end
			local text = money(request.amount):gsub(",00$", "")
				.. " " .. request.from .. " = "
				.. money(request.amount * rate) .. " " .. request.to
			if result.updated then
				text = text .. "\nкурс на " .. os.date("!%d.%m.%Y", result.updated)
			end
			tg.messages.send(draft.peer, text)
		end)
	end)
	return false -- сама команда не уходит
end)
