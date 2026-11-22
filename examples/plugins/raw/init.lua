-- Raw MTProto from Lua: objects are tables with the constructor name in "_",
-- as in api.tl of the client's layer.

-- Others never see "typing...": the request is not sent at all.
plugin.on("mtproto.request", function(call)
	return false
end, { "messages.setTyping" })

-- "/dc" is not sent: the plugin asks Telegram and sends the answer instead.
plugin.on("message.send", function(draft)
	if draft.text ~= "/dc" then
		return nil
	end
	tg.async(function()
		local dc, err = tg.invoke{ _ = "help.getNearestDc" }
		if dc then
			tg.messages.send(draft.peer, string.format(
				"Country: %s, my data center: %d, nearest: %d",
				dc.country, dc.this_dc, dc.nearest_dc))
		else
			tg.log("help.getNearestDc failed:", err.type)
		end
	end)
	return false
end)
