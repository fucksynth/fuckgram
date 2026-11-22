-- "/weather Moscow" is not sent: the plugin asks wttr.in and sends the
-- answer instead. Shows tg.async + tg.http.request: inside tg.async the
-- request reads like a normal call, the client keeps running meanwhile.

local function urlencode(text)
	return (text:gsub("[^%w%-_.~]", function(c)
		return string.format("%%%02X", c:byte())
	end))
end

plugin.on("message.send", function(draft)
	local city = draft.text:match("^/weather%s+(.+)$")
	if not city then
		return nil
	end
	tg.async(function()
		local response = tg.http.request{
			url = "https://wttr.in/" .. urlencode(city) .. "?format=3",
			headers = { ["User-Agent"] = "curl" },
		}
		if response.status == 200 then
			tg.messages.send(draft.peer, response.body)
		else
			tg.log("weather failed:", response.status, response.error)
		end
	end)
	return false -- do not send the command itself
end)
