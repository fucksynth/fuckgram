-- Manual test of the plugin API. Type "/t" in any chat (Saved Messages is
-- best): the command is not sent, the plugin shows the list of checks.
-- "Test: ..." items are in the message, chat, profile and main menus; the
-- settings page runs the checks that need no chat and turns the hooks on
-- and off. "/t lang" (or "Localization" on the page) checks plugin.t and
-- tg.lang().

local count = { new = 0, screen = 0, updates = 0, full_user = 0, reactions = 0 }
local last_incoming = {}
local last_screen = "—"

local function show(title, text)
	tg.ui.alert(text, title)
end

local function describe(peer)
	if not peer then
		return plugin.t("peer.unknown")
	end
	return plugin.t("peer.describe", {
		name = tostring(peer.name),
		type = tostring(peer.type),
		id = tostring(peer.id),
	}) .. (peer.username and (" @" .. peer.username) or "")
end

-- plugin.t and tg.lang(). The test texts (keys "test.*") are the same in
-- every locale file, so the results do not depend on the language, except
-- the plural forms: they follow the rules of the language of the file the
-- text came from (ru.json for "ru" and "ru-*", en.json for the rest).
local function check_localization()
	local lines = {}
	local failed, total = 0, 0
	local function check(label, got, want)
		total = total + 1
		if got == want then
			lines[#lines + 1] = plugin.t("lang.ok", label)
		else
			failed = failed + 1
			lines[#lines + 1] = plugin.t("lang.fail", {
				name = label,
				got = tostring(got),
				want = tostring(want),
			})
		end
	end

	local lang = tg.lang()
	check(plugin.t("lang.check.lang", tostring(lang)),
		type(lang) == "string" and lang:match("^[a-z][a-z0-9%-]*$") ~= nil,
		true)
	local base = (type(lang) == "string" and lang:match("^[^-]+")) or "en"
	local ru = (base == "ru")
	local file = ru and "ru" or "en"
	check(plugin.t("lang.check.file", file), plugin.t("lang.file"), file)

	check(plugin.t("lang.check.positional"),
		plugin.t("test.positional", "a", 7), "a|7|a|{3}")
	check(plugin.t("lang.check.tostring"),
		plugin.t("test.positional", true, 2.5), "true|2.5|true|{3}")
	check(plugin.t("lang.check.no_args"),
		plugin.t("test.positional"), "{1}|{2}|{1}|{3}")
	check(plugin.t("lang.check.named"),
		plugin.t("test.named", { name = "x", count = 2 }), "x|2|{missing}|{1}")

	local forms = {
		{ 0, ru and "many" or "other" },
		{ 1, "one" },
		{ 2, ru and "few" or "other" },
		{ 5, ru and "many" or "other" },
		{ 11, ru and "many" or "other" },
		{ 21, ru and "one" or "other" },
		{ 22, ru and "few" or "other" },
		{ 112, ru and "many" or "other" },
		{ 1.5, "other" },
	}
	for _, case in ipairs(forms) do
		local n, form = case[1], case[2]
		check(plugin.t("lang.check.plural", tostring(n)),
			plugin.t("test.plural", n), form .. ":" .. tostring(n))
	end
	check(plugin.t("lang.check.plural_named"),
		plugin.t("test.plural_named", { count = 3, name = "x" }),
		(ru and "few" or "other") .. ":3:x")
	-- "few" and "other" are missing: "many" is next after "other".
	check(plugin.t("lang.check.missing_form"),
		plugin.t("test.plural_partial", 2), "many:2")
	check(plugin.t("lang.check.missing_form"),
		plugin.t("test.plural_partial", 1), "one:1")
	-- Only "few": the first form there is.
	check(plugin.t("lang.check.single_form"),
		plugin.t("test.plural_single", 1), "few:1")
	-- A key that no file has comes back as it is.
	check(plugin.t("lang.check.fallback"),
		plugin.t("test.no_such_key"), "test.no_such_key")
	check(plugin.t("lang.check.fallback"),
		plugin.t("test.no_such_key", 1), "test.no_such_key")

	table.insert(lines, 1, failed == 0
		and plugin.t("lang.passed", total)
		or plugin.t("lang.failed", { failed = failed, total = total }))
	table.insert(lines, 2, "")
	show(plugin.t("lang.title"), table.concat(lines, "\n"))
end

local function help()
	show(plugin.t("help.title"), plugin.t("help.text"))
end

local function summary()
	show(plugin.t("summary.title"), plugin.t("summary.text", {
		new = count.new,
		screen = count.screen,
		last = last_screen,
		updates = count.updates,
		full_user = count.full_user,
		reactions = count.reactions,
	}))
end

local commands = {}

commands[""] = help

commands["lang"] = check_localization

commands["me"] = function(draft)
	show(plugin.t("me.title"), plugin.t("me.text", {
		me = describe(tg.me()),
		chat = describe(tg.peers.get(draft.peer)),
		open = tostring(tg.chats.current()),
	}))
end

commands["send"] = function(draft)
	tg.messages.send(draft.peer, plugin.t("send.normal"))
	tg.messages.send(draft.peer, plugin.t("send.silent"), { silent = true })
	tg.messages.send(draft.peer, plugin.t("send.scheduled"), {
		schedule = os.time() + 60,
	})
	tg.toast(plugin.t("send.done"))
end

commands["ask"] = function(draft)
	tg.async(function()
		local yes = tg.ui.confirm{
			title = plugin.t("ask.title"),
			text = plugin.t("ask.confirm"),
			ok = plugin.t("ask.yes"),
			cancel = plugin.t("ask.no"),
		}
		tg.toast(yes and plugin.t("ask.answer_yes") or plugin.t("ask.answer_no"))
		local text = tg.ui.input{
			title = plugin.t("ask.title"),
			text = plugin.t("ask.input"),
			placeholder = plugin.t("ask.placeholder"),
		}
		if text and text ~= "" then
			tg.messages.send(draft.peer, plugin.t("ask.sent", text))
		else
			tg.toast(plugin.t("ask.cancelled"))
		end
	end)
end

commands["ask2"] = function()
	tg.ui.confirm(plugin.t("ask2.text"), function(yes)
		tg.toast(plugin.t("ask2.result", tostring(yes)))
	end)
end

commands["copy"] = function()
	tg.ui.copy(plugin.t("copy.text", os.date("%H:%M:%S")))
	tg.toast(plugin.t("copy.done"))
end

commands["timer"] = function()
	tg.toast(plugin.t("timer.start"))
	tg.timer.after(3000, function()
		local n = 3
		local id
		id = tg.timer.every(1000, function()
			if n == 0 then
				tg.timer.cancel(id)
				tg.toast(plugin.t("timer.stopped"))
				return
			end
			tg.toast(plugin.t("timer.tick", n))
			n = n - 1
		end)
	end)
end

commands["files"] = function()
	local written = plugin.files.write("test.txt", plugin.t("files.sample", os.time()))
	local data = plugin.files.read("test.txt")
	local list = table.concat(plugin.files.list(), ", ")
	local removed = plugin.files.remove("test.txt")
	local after = plugin.files.read("test.txt")
	show(plugin.t("files.title"), plugin.t("files.result", {
		written = tostring(written),
		data = tostring(data),
		list = list,
		removed = tostring(removed),
		after = tostring(after),
		starts = tostring(plugin.settings.get("starts", 0)),
	}))
end

commands["json"] = function()
	local text = tg.json.encode({ a = 1, list = { 1, 2, 3 }, s = plugin.t("json.sample") })
	local back = tg.json.decode(text)
	local bad, err = tg.json.decode("{oops")
	show(plugin.t("json.title"), plugin.t("json.result", {
		json = text,
		a = tostring(back.a),
		third = tostring(back.list[3]),
		s = tostring(back.s),
		bad = tostring(bad),
		err = tostring(err),
	}))
end

commands["invoke"] = function()
	tg.async(function()
		local lines = {}
		local function failure(method, err)
			return plugin.t("invoke.error", { method = method, error = err.type })
		end
		local users, err = tg.invoke{
			_ = "users.getUsers",
			id = { { _ = "inputUserSelf" } },
		}
		if users then
			lines[#lines + 1] = plugin.t("invoke.users",
				tostring(users[1] and users[1].first_name))
		else
			lines[#lines + 1] = failure("users.getUsers", err)
		end
		local dc, err2 = tg.invoke{ _ = "help.getNearestDc" }
		if dc then
			lines[#lines + 1] = plugin.t("invoke.dc", {
				dc = dc.this_dc,
				country = dc.country,
			})
		else
			lines[#lines + 1] = failure("help.getNearestDc", err2)
		end
		local _, bad = tg.invoke{
			_ = "users.getFullUser",
			id = { _ = "inputUserEmpty" },
		}
		lines[#lines + 1] = plugin.t("invoke.expected",
			bad and (bad.code .. " " .. bad.type) or plugin.t("invoke.none"))
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
			wrong = plugin.t("invoke.no_error")
		end
		lines[#lines + 1] = plugin.t("invoke.wrong", wrong)
		show(plugin.t("invoke.title"), table.concat(lines, "\n"))
	end)
	tg.invoke({ _ = "help.getConfig" }, function(config, err)
		if config then
			tg.toast(plugin.t("invoke.callback", #config.dc_options))
		else
			tg.toast(plugin.t("invoke.callback_error", err.type))
		end
	end)
end

-- Reading Telegram Passport data is harmless, but it is on the list of
-- sensitive requests: the client asks before sending it.
commands["secure"] = function()
	tg.async(function()
		local values, err = tg.invoke{ _ = "account.getAllSecureValues" }
		if values then
			show(plugin.t("secure.title"), plugin.t("secure.allowed", #values))
		else
			show(plugin.t("secure.title"), plugin.t("secure.denied", err.type))
		end
	end)
end

commands["get"] = function(draft)
	local id = last_incoming[draft.peer]
	if not id then
		show(plugin.t("get.title"), plugin.t("get.none"))
		return
	end
	local message = tg.messages.get(draft.peer, id)
	if message then
		show(plugin.t("get.title"), plugin.t("get.found", {
			id = message.id,
			from = message.from,
			text = message.text,
		}))
	else
		show(plugin.t("get.title"), plugin.t("get.not_found"))
	end
end

commands["compose"] = function()
	tg.timer.after(300, function()
		tg.compose.set(plugin.t("compose.text"))
	end)
end

commands["refresh"] = function()
	tg.ui.refresh()
	tg.toast(plugin.t("refresh.done"))
end

commands["error"] = function()
	tg.toast(plugin.t("error.toast"))
	tg.timer.after(10, function()
		error(plugin.t("error.message"))
	end)
end

commands["loop"] = function()
	tg.toast(plugin.t("loop.toast"))
	tg.timer.after(10, function()
		while true do
		end
	end)
end

commands["mem"] = function()
	tg.toast(plugin.t("mem.toast"))
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
		tg.toast(plugin.t("unknown_command", name))
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
	if plugin.settings.get("display", true)
		and shown.place == "message"
		and shown.text
		and shown.text:find("[fg]", 1, true) then
		return (shown.text:gsub("%[fg%]", "✅"))
	end
	return nil
end)

plugin.on("profile.rows", function(peer)
	return { { label = plugin.t("profile.label"), value = plugin.t("profile.value", peer) } }
end)

plugin.on("url.open", function(link)
	if plugin.settings.get("links", true) and link.url:find("fgtest", 1, true) then
		tg.toast(plugin.t("link.taken", link.url))
		return true
	end
	return nil
end)

plugin.on("action", function(action)
	if action.type ~= "reaction" or action.remove
		or not plugin.settings.get("reactions", true) then
		return nil
	end
	count.reactions = count.reactions + 1
	if action.emoji == "👎" then
		tg.toast(plugin.t("reaction.cancelled"))
		return false
	end
	tg.toast(plugin.t("reaction.shown", tostring(action.emoji or action.custom_emoji_id)))
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

plugin.settings.page(function()
	return {
		{ type = "header", text = plugin.t("settings.checks") },
		{
			type = "text",
			text = plugin.t("settings.lang"),
			subtext = plugin.t("settings.lang.about", tg.lang()),
			icon = "translate",
			on_click = check_localization,
		},
		{
			type = "text",
			text = plugin.t("json.title"),
			subtext = plugin.t("settings.json.about"),
			icon = "code",
			on_click = commands["json"],
		},
		{
			type = "text",
			text = plugin.t("settings.files"),
			subtext = plugin.t("settings.files.about"),
			icon = "storage",
			on_click = commands["files"],
		},
		{
			type = "text",
			text = plugin.t("settings.timer"),
			subtext = plugin.t("settings.timer.about"),
			icon = "timer",
			on_click = commands["timer"],
		},
		{
			type = "text",
			text = plugin.t("summary.title"),
			subtext = plugin.t("settings.summary.about"),
			icon = "stats",
			on_click = summary,
		},
		{
			type = "text",
			text = plugin.t("help.title"),
			subtext = plugin.t("settings.help.about"),
			icon = "info",
			on_click = help,
		},
		{ type = "header", text = plugin.t("settings.hooks") },
		{
			type = "switch",
			key = "display",
			text = plugin.t("settings.display"),
			subtext = plugin.t("settings.display.about"),
			icon = "eye",
			default = true,
			on_change = function()
				tg.ui.refresh()
			end,
		},
		{
			type = "switch",
			key = "reactions",
			text = plugin.t("settings.reactions"),
			subtext = plugin.t("settings.reactions.about"),
			icon = "reactions",
			default = true,
		},
		{
			type = "switch",
			key = "links",
			text = plugin.t("settings.links"),
			subtext = plugin.t("settings.links.about"),
			icon = "link",
			default = true,
		},
		{ type = "divider", text = plugin.t("settings.menus") },
	}
end)

plugin.menu{
	place = "message",
	text = plugin.t("menu.reply"),
	icon = "reply",
	on_click = function(ctx)
		if ctx.message then
			tg.messages.send(ctx.peer, plugin.t("reply.text"), { reply_to = ctx.message.id })
		end
	end,
}

plugin.menu{
	place = "message",
	text = plugin.t("menu.edit"),
	icon = "edit",
	on_click = function(ctx)
		local message = ctx.message
		if not message then
			return
		end
		tg.async(function()
			local text = tg.ui.input{
				title = plugin.t("edit.title"),
				value = message.text,
				ok = plugin.t("edit.save"),
			}
			if text then
				local queued = tg.messages.edit(ctx.peer, message.id, text)
				tg.toast(queued and plugin.t("edit.sent") or plugin.t("edit.denied"))
			end
		end)
	end,
}

plugin.menu{
	place = "message",
	text = plugin.t("menu.delete"),
	icon = "delete",
	on_click = function(ctx)
		local message = ctx.message
		if not message then
			return
		end
		tg.async(function()
			local yes = tg.ui.confirm{
				title = plugin.t("delete.title"),
				text = plugin.t("delete.text"),
				ok = plugin.t("delete.ok"),
			}
			if yes then
				tg.toast(plugin.t("delete.done", tg.messages.delete(ctx.peer, message.id)))
			end
		end)
	end,
}

plugin.menu{
	place = "chat",
	text = plugin.t("menu.chat"),
	icon = "chat",
	on_click = function(ctx)
		show(plugin.t("chat.title"), plugin.t("chat.text", {
			chat = describe(tg.peers.get(ctx.peer)),
			field = tostring(tg.compose.get()),
		}))
	end,
}

plugin.menu{
	place = "profile",
	text = plugin.t("menu.profile"),
	icon = "user",
	on_click = function(ctx)
		show(plugin.t("profile.title"), describe(tg.peers.get(ctx.peer)))
	end,
}

plugin.menu{
	place = "main",
	text = plugin.t("menu.summary"),
	icon = "stats",
	on_click = summary,
}

function on_enable()
	local starts = plugin.settings.get("starts", 0) + 1
	plugin.settings.set("starts", starts)
	tg.log("selftest: started", starts, "times")
end

function on_disable()
	tg.log("selftest: stopping")
end
