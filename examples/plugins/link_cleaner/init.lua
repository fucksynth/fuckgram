-- Чистые ссылки: перед отправкой убирает из ссылок параметры, по которым
-- сайты и соцсети следят, откуда пришёл человек. Остальные параметры
-- (номер страницы, поиск, время в видео) остаются как были.

-- Параметры, которые убираются на любом сайте.
local TRACKERS = {
	fbclid = true, gclid = true, dclid = true, gbraid = true, wbraid = true,
	yclid = true, ysclid = true, msclkid = true, twclid = true, ttclid = true,
	igshid = true, igsh = true, mc_cid = true, mc_eid = true,
	_hsenc = true, _hsmi = true, mkt_tok = true, vero_id = true,
	oly_enc_id = true, oly_anon_id = true, rb_clickid = true, s_cid = true,
	ref_src = true, ref_url = true, spm = true, scm = true,
	share_source = true, share_medium = true, share_plat = true,
	share_session_id = true, share_tag = true, share_from = true,
	from_spmid = true, unique_k = true,
}

-- Начала имён: utm_source, utm_medium, pk_campaign...
local PREFIXES = { "utm_", "pk_", "mtm_", "hsa_", "vk_", "_openstat" }

-- Параметры, которые следят только на своём сайте.
local BY_SITE = {
	["youtube.com"] = { si = true, feature = true, pp = true },
	["youtu.be"] = { si = true, feature = true },
	["spotify.com"] = { si = true, context = true },
	["instagram.com"] = { igsh = true },
	["twitter.com"] = { s = true, t = true },
	["x.com"] = { s = true, t = true },
	["tiktok.com"] = { _r = true, _t = true, is_from_webapp = true, sender_device = true },
	["aliexpress.com"] = { spm = true, aff_platform = true, sk = true, aff_trace_key = true },
	["aliexpress.ru"] = { spm = true, aff_platform = true, sk = true, aff_trace_key = true },
}

local function siteRules(host)
	for site, rules in pairs(BY_SITE) do
		if host == site or host:sub(-#site - 1) == "." .. site then
			return rules
		end
	end
	return nil
end

local function tracked(key, rules)
	if TRACKERS[key] then
		return true
	end
	for _, prefix in ipairs(PREFIXES) do
		if key:sub(1, #prefix) == prefix then
			return true
		end
	end
	return rules ~= nil and rules[key] == true
end

-- Ссылка без меток и сколько их было.
local function clean(url)
	local base, query, fragment = url:match("^([^?#]*)%?([^#]*)(.*)$")
	if not base then
		return url, 0
	end
	local host = (base:match("^%a+://([^/:]+)") or ""):lower()
	local rules = siteRules(host)
	local kept, removed = {}, 0
	for pair in query:gmatch("[^&]+") do
		local key = (pair:match("^([^=]*)") or ""):lower()
		if tracked(key, rules) then
			removed = removed + 1
		else
			kept[#kept + 1] = pair
		end
	end
	if removed == 0 then
		return url, 0
	end
	local result = base
	if #kept > 0 then
		result = result .. "?" .. table.concat(kept, "&")
	end
	return result .. fragment, removed
end

plugin.on("message.send", function(draft)
	local total = 0
	local text = draft.text:gsub("https?://[^%s<>\"]+", function(url)
		-- Точка или запятая в конце — это конец предложения, не ссылки.
		local body, tail = url:match("^(.-)([%.,;:!%?]*)$")
		local cleaned, removed = clean(body)
		total = total + removed
		return cleaned .. tail
	end)
	if total == 0 then
		return nil
	end
	tg.toast(total == 1
		and "Чистые ссылки: убрана метка слежки"
		or ("Чистые ссылки: убрано меток слежки — " .. total))
	return text
end)
