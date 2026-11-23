-- Калькулятор: «2+2*2=» или «/calc 2+2*2» отправляется как «2+2*2 = 6».
-- Выражение разбирает свой парсер: load() в плагинах нет, и это хорошо —
-- в сообщении не выполнится чужой код.

local FUNCTIONS = {
	sqrt = math.sqrt, abs = math.abs, floor = math.floor, ceil = math.ceil,
	sin = math.sin, cos = math.cos, tan = math.tan, exp = math.exp,
	ln = math.log,
	log = function(x) return math.log(x, 10) end,
	round = function(x) return math.floor(x + 0.5) end,
}
local CONSTANTS = { pi = math.pi, e = math.exp(1) }

local function tokenize(text)
	text = text:gsub("×", "*"):gsub("·", "*"):gsub("÷", "/"):gsub("−", "-")
	text = text:gsub("(%d),(%d)", "%1.%2")
	local tokens, i = {}, 1
	while i <= #text do
		local c = text:sub(i, i)
		if c:match("%s") then
			i = i + 1
		else
			local number = text:match("^%d+%.?%d*", i) or text:match("^%.%d+", i)
			local word = not number and text:match("^%a+", i)
			if number then
				-- Дробные числа: целые в Lua переполняются молча.
				tokens[#tokens + 1] = { kind = "number", value = tonumber(number) * 1.0 }
				i = i + #number
			elseif word then
				tokens[#tokens + 1] = { kind = "word", value = word:lower() }
				i = i + #word
			elseif c:match("[%+%-%*/%%%^%(%)]") then
				tokens[#tokens + 1] = { kind = c }
				i = i + 1
			else
				return nil
			end
		end
	end
	return tokens
end

-- Рекурсивный спуск: сложение < умножение < унарный минус < степень.
local function parse(tokens)
	local position = 1
	local function peek() return tokens[position] end
	local function take() position = position + 1; return tokens[position - 1] end
	local function accept(kind)
		if peek() and peek().kind == kind then
			return take()
		end
	end

	local expression
	local function primary()
		local token = take()
		if not token then
			error("unexpected end")
		elseif token.kind == "number" then
			return token.value
		elseif token.kind == "(" then
			local value = expression()
			if not accept(")") then error("no )") end
			return value
		elseif token.kind == "word" then
			if FUNCTIONS[token.value] then
				if not accept("(") then error("no (") end
				local value = expression()
				if not accept(")") then error("no )") end
				return FUNCTIONS[token.value](value)
			elseif CONSTANTS[token.value] then
				return CONSTANTS[token.value]
			end
		end
		error("bad token")
	end
	local unary
	local function power()
		local base = primary()
		if accept("^") then
			return base ^ unary()
		end
		return base
	end
	unary = function()
		if accept("-") then return -unary() end
		if accept("+") then return unary() end
		return power()
	end
	-- Второе значение: слагаемое — проценты («1000-10%» = 900).
	local function term()
		local value = unary()
		local percent = false
		while true do
			if accept("*") then
				value = value * unary()
				percent = false
			elseif accept("/") then
				value = value / unary()
				percent = false
			elseif accept("%") then
				-- «15%» — проценты, «7 % 3» — остаток от деления.
				local next = peek()
				if not next or next.kind == ")" or next.kind == "+"
					or next.kind == "-" then
					value = value / 100
					percent = true
				else
					value = math.fmod(value, unary())
					percent = false
				end
			else
				return value, percent
			end
		end
	end
	expression = function()
		local value = term()
		while true do
			local sign = accept("+") and 1 or (accept("-") and -1)
			if not sign then
				return value
			end
			local part, percent = term()
			if percent then
				part = value * part
			end
			value = value + sign * part
		end
	end

	local value = expression()
	if position <= #tokens then
		error("extra tokens")
	end
	return value
end

local function evaluate(text)
	local tokens = tokenize(text)
	if not tokens or #tokens == 0 then
		return nil
	end
	local ok, value = pcall(parse, tokens)
	if not ok or type(value) ~= "number" or value ~= value
		or value == math.huge or value == -math.huge then
		return nil
	end
	return value
end

local function format(value, comma)
	local result
	local integer = math.tointeger(value)
	if integer and math.abs(value) < 1e15 then
		result = tostring(integer)
	else
		result = string.format("%.10g", value)
	end
	return comma and result:gsub("%.", ",") or result
end

local function hasOperation(text)
	return text:find("[%+%*/%%%^×÷·]") or text:find(".%-") or text:find("%a")
end

plugin.on("message.send", function(draft)
	local text = draft.text
	local command = text:match("^/calc%s+(.+)$")
	local expression = command or text:match("^(.-)%s*=%s*$")
	if not expression or expression == "" then
		return nil
	end
	-- «Привет =» и подобное не трогаем: только числа и действия.
	if not command and (not hasOperation(expression)
		or not expression:match("^[%w%s%.,%+%-%*/%%%^%(%)×÷·−]+$")) then
		return nil
	end
	local value = evaluate(expression)
	if not value then
		if command then
			tg.toast("Калькулятор: не понял выражение")
			return false
		end
		return nil
	end
	local comma = expression:find("%d,%d") ~= nil
	return expression .. " = " .. format(value, comma)
end)
