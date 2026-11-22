-- Example fuckgram plugin.
-- Copy the "hello" folder into <fuckgram data>/plugins/ and restart.
-- Settings → Plugins shows whether it runs and why not.

local shrug = require("source.shrug")

function on_enable()
	local starts = plugin.settings.get("starts", 0) + 1
	plugin.settings.set("starts", starts)
	tg.log("hello started", starts, "times")
end

-- Outgoing text: return false to cancel, a string to replace, nil to keep.
plugin.on("message.send", function(draft)
	if draft.text == "/nope" then
		return false
	end
	return shrug.apply(draft.text)
end)

plugin.menu("message", "Show message id", function(ctx)
	if ctx.message then
		tg.toast("Message " .. ctx.message.id .. " in chat " .. ctx.peer)
	end
end)

plugin.menu("main", "Hello plugin", function()
	tg.toast("Hello! Started " .. plugin.settings.get("starts", 0) .. " times.")
end)
