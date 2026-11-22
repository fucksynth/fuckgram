# Plugins

Status: **in development.** The Lua runtime (API version 1) is in the
client: it checks every package by analysing its code, loads plugins, runs
them in a sandbox and connects them to the hooks, including raw MTProto
requests, responses and updates decoded with the TL schema. The `.flua`
catalog is not done yet.

## Writing a plugin (API 1)

A plugin is a folder or a `.flua` archive (zip) with `manifest.json` at its
root. Put it into `plugins/` next to `tdata` (portable) or in the fuckgram data
folder, then restart. Settings → Plugins lists what is loaded and the last
error of each plugin. Examples: [`examples/plugins/hello`](../examples/plugins/hello) (send filter,
menus, settings), [`examples/plugins/weather`](../examples/plugins/weather)
(`tg.async` + HTTP) and [`examples/plugins/raw`](../examples/plugins/raw)
(`tg.invoke`, MTProto filters).

Pack a folder with `python tools/flua_pack.py <folder>`: it checks the
manifest and writes `<id>.flua`. The code check below runs in the client.

### Code check

Before a package is loaded or installed the client compiles every `.lua` file
(without running it) and follows where `tg` and `plugin` go. The package is
refused when:

- the code uses something the manifest does not declare, or the manifest
  declares a permission the code never uses;
- an API name, event name or menu place is unknown or not a constant
  string (`plugin.on(name, ...)` with a computed `name`);
- an API table is indexed dynamically (`tg[x]`), stored in a table, global
  or upvalue, passed to a function, returned, or walked with `pairs`;
- `require` gets a computed module name;
- the code touches `load`, `loadfile`, `dofile`, `rawget`, `rawset`, `debug`
  and similar.

Local aliases are fine: `local send = tg.messages.send` or
`local m = tg.messages; m.send(peer, text)`. API functions (not tables) can be
passed around, for example `pcall(tg.toast, text)`, except `plugin.on`,
`plugin.menu` and `require`, which the check must see called directly.

To install a packed plugin, send the `.flua` file to any chat (Saved
Messages works) and open it: the client shows what the plugin will be able to
do and installs it on confirmation. Settings → Plugins turns plugins on and
off and opens the plugins folder.

```json
{
    "id": "example.hello",
    "name": "Hello",
    "version": "1.0",
    "api": { "min": 1, "max": 1 },
    "entry": "init.lua",
    "permissions": ["messages.send", "ui", "storage"]
}
```

| Lua | Permission | What it does |
|---|---|---|
| `plugin.on("message.new", fn(msg))` | `messages.read` | new incoming message |
| `plugin.on("message.send", fn(draft))` | `messages.send` | outgoing text: return `false` to cancel, a string to replace, `nil` to keep |
| `plugin.on("app.state", fn(state))` | — | `started`, `active`, `inactive`, `stopping` |
| `plugin.on("url.open", fn(link))` | `links` | `link.url`, `link.local` (tg://); return `true` to handle it instead of the client |
| `plugin.on("action", fn(action))` | `actions` | `action.type`: `reply`, `reaction`, `call`, `transcribe`, `notification`, `player.next`, `player.previous`, `player.stop`, `play`, `file.open`, `select`, `message.menu`, `header.click`; return `false` to cancel |
| `plugin.on("display", fn(shown))` | `display` | `shown.place`: `message`, `chat.list`, `chat.title`, `chat.status`, `copy`; return a string to show (or copy) instead |
| `plugin.on("screen", fn(screen))` | `chats.read` | `screen.screen`: `chat.list`, `chat`, `profile`; `screen.peer` |
| `plugin.on("profile.rows", fn(peer))` | `ui` | return `{ { label = ..., value = ... }, ... }` to add rows to a profile |
| `plugin.menu(place, text, fn(ctx))` | `ui` | menu item; `place` is `message`, `main`, `chat` or `profile`; `ctx.peer`, `ctx.message` |
| `plugin.settings.get(key, default)`, `.set(key, value)` | `storage` | small values kept between runs |
| `tg.toast(text)` | `ui` | short notice in the window |
| `tg.messages.send(peer, text)` | `messages.send` | send a text message |
| `tg.compose.get()`, `tg.compose.set(text)` | `compose` | the text in the current message field |
| `tg.ui.refresh()` | `display` | re-apply `display` handlers to what is already shown |
| `tg.async(fn, ...)` | — | run `fn` as a coroutine; inside it `tg.http.request` waits for the reply without blocking the client |
| `tg.http.request{ url, method, headers, body }` | `net:<host>` | HTTPS only, to declared hosts (`net:api.example.com`, `net:*.example.com`); returns `{ status, body, headers, error }`; no automatic redirects; pass a callback as the 2nd argument instead of using `tg.async` |
| `tg.timer.after(ms, fn)`, `tg.timer.every(ms, fn)`, `tg.timer.cancel(id)` | — | one-shot and repeating timers (repeating ≥ 100 ms, up to 64) |
| `tg.json.encode(value)`, `tg.json.decode(text)` | — | JSON ↔ Lua tables; `decode` returns `nil, error` on bad input |
| `tg.invoke(call)` | `mtproto` | any API method: `local r, err = tg.invoke{ _ = "help.getNearestDc" }` inside `tg.async`, or pass a callback `fn(result, err)` as the 2nd argument; `err` is `{ code, type, description }` |
| `plugin.on("mtproto.request", fn(call), methods)` | `mtproto` | every request the client sends; return `false` to cancel it (the caller gets `PLUGIN_CANCELLED`), a table to send instead, `nil` to keep |
| `plugin.on("mtproto.response", fn(result, method), methods)` | `mtproto` | every answer (also `rpc_error`) before the client sees it; return a value to replace it |
| `plugin.on("mtproto.updates", fn(updates))` | `mtproto` | raw updates container; return `false` to drop it, a table to replace it |
| `tg.log(...)`, `print(...)` | — | write to the client log |
| `require("source.module")` | — | load `source/module.lua` or `lib/module.lua` from the package |
| global `on_enable()`, `on_disable()` | — | called after the entry file ran and before the plugin stops |

`msg` is `{ id, peer, from, text, out, date }`. Chat ids are written as in the
Bot API: users `> 0`, basic groups `< 0`, channels and supergroups
`-100…`.

### Raw MTProto

`mtproto` is full access to the account, the install window says so in
capitals. Objects are Lua tables with the constructor name in `_`, exactly as
in the [schema](../Telegram/SourceFiles/mtproto/scheme/api.tl) of the client's
layer:

```lua
tg.async(function()
    local history, err = tg.invoke{
        _ = "messages.getHistory",
        peer = { _ = "inputPeerSelf" },
        offset_id = 0, offset_date = 0, add_offset = 0,
        limit = 10, max_id = 0, min_id = 0, hash = 0,
    }
    if history then
        tg.log(#history.messages, "messages")
    else
        tg.log("failed:", err.type)
    end
end)

-- Never tell anyone we are typing.
plugin.on("mtproto.request", function(call)
    return false
end, { "messages.setTyping" })
```

- `int` and `long` are integers, `double` a number, `string` and `bytes`
  strings, `int128` / `int256` strings of 16 / 32 bytes, `Bool` a boolean,
  `Vector<T>` an array.
- Optional fields are `nil` when absent; `flags.N?true` fields are `true` or
  `nil`. `flags` themselves are computed when a table is written and are not
  shown when one is read.
- `_` may be left out for a type with one constructor. A missing required
  field, an unknown field or a constructor of the wrong type is an error with
  its path: `peer.user_id: expected an integer, got string`.
- The method list of `mtproto.request` / `mtproto.response` is checked
  against the schema and keeps the other calls out of Lua: without it every
  request or response is decoded, which costs time on big answers. Methods
  are matched through wrappers: `invokeWithTakeout(messages.getHistory)` is
  `messages.getHistory`.
- Filters of plugins never see `tg.invoke` calls of plugins, so a filter can't
  loop on its own requests. Up to 32 calls of one plugin wait at a time.

The sandbox has `string` (no `dump`), `table`, `math`, `utf8`, `coroutine` and
`os.time` / `os.clock` / `os.date` / `os.difftime`. There is no `io`, no
`load` / `loadfile` / `dofile`, no `debug`, no `rawget` / `rawset`, and only
text Lua is accepted, never bytecode. A call that runs too long is stopped
(50 ms for the send filter, 200 ms for events, 1 s for menu clicks, 2 s at
start), a plugin is limited to 64 MB, and after 5 errors it is stopped.

## Design

- **Lua 5.4**, built as C++ so Lua errors unwind C++ frames safely.
- **One `lua_State` per plugin.** No shared globals; unloading a plugin is
  `lua_close`. Memory is capped by a custom allocator, CPU time by
  `lua_sethook`.
- **No native code.** `lib/` in a package may contain only `.lua` files;
  `package.cpath` and `package.loadlib` are removed. Heavy lifting (JSON,
  SQLite, hashing, regular expressions, HTTP) comes from modules of the core,
  each behind a permission.
- **Permissions are checked by static analysis on import.** A plugin lists
  what it needs in `manifest.json`; the analyser walks all its files and
  rejects the plugin if it uses an API it did not declare or uses a
  construct that cannot be analysed (`load`, `_ENV`, dynamic indexing of API
  tables, dynamic event names or `require`). The runtime checks permissions
  again on every call.
- **Main thread only.** Long operations (network, MTProto requests, timers)
  run in C++ and resume the plugin's coroutine on the main thread
  (`await`), so the UI never waits for a plugin.

## Package format: `.flua`

A zip-like archive, read in memory and never unpacked to disk:

```
cool_plugin.flua
├── manifest.json   id, name, version, api range, permissions, file hashes, signature
├── init.lua        entry point: on_load / on_enable / on_disable / on_update
├── source/         plugin code
├── lib/            Lua libraries (no native code)
└── resources/      icons, texts, initial database (read-only)
```

Writable data (settings, database copies) lives in a separate per-plugin data
folder, so updating a package does not lose it.

Catalog flow: the author uploads plain source → analysis and review →
obfuscation on the server → the catalog signs the manifest with the verified
permissions and file hashes. The client trusts that signature and does not
re-analyse obfuscated code. Packages without a catalog signature are analysed
locally and must not be obfuscated.

## Hook layer

Everything between the client and the runtime goes through
[`plugins_hooks.h`](../Telegram/SourceFiles/plugins/plugins_hooks.h):

- typed requests instead of JSON;
- a `Plugins::Sink` interface that the runtime implements;
- one bit per hook: an unused hook costs a single test;
- a re-entrance guard, `BypassScope` for the runtime's own sends, and a log
  warning for handlers slower than 50 ms.

| Group | Hook | What a plugin can do |
|---|---|---|
| Sending | `send_message` | cancel or rewrite outgoing text |
| | `send_media` | cancel outgoing files |
| Events | `new_message` | see new messages |
| | `app_state`, `app_stop` | started, window active / inactive, stopping |
| | `screen_chat`, `screen_profile` | chat list, chat or profile shown |
| Links | `open_url`, `open_local_url` | handle external and `tg://` links |
| Actions | `client_event_*` | cancel reply, reaction, outgoing call, transcription, notification, music player next / previous / close, playing a voice or round message, opening a file (this is how `.flua` packages sent in a chat get installed), selecting a message, opening the message menu, clicking the chat header |
| Menus | `peer_menu_*` | items in the chat and profile menus |
| | `message_menu_*` | items in the message context menu |
| | `main_menu` | items in the main (drawer) menu |
| Input | `compose_field_*` | read and replace the text being typed |
| Server | `outgoing_request` | cancel or replace any outgoing MTProto request (by TL constructor); a cancelled request fails with `PLUGIN_CANCELLED` |
| | `incoming_response` | replace the server reply to a request (also an error) |
| | `incoming_updates` | drop or replace a raw updates container |
| | `incoming_update` | drop a single parsed update before the client applies it |
| Profile | `profile_rows` | add rows to the profile info block |
| Settings | `settings_plugins` | the "Plugins" entry that opens the runtime's manager |
| Display | `display_*` | replace shown text: message, chat list name, chat title and status |
| | `copy_text_*` | replace the text that "Copy text" puts to the clipboard |

After a plugin changes its display rules the runtime calls
`Plugins::RefreshDisplayText()`: messages of loaded chats are re-laid out
at once, chat names and headers on their next repaint.

## Events without hooks

The runtime lives in the client process, so for plain notifications it can
subscribe to the client's own event streams (for example `Data::Session`
item added / removed / edited) without new hooks. Hooks are added only where
a plugin must cancel, change or add something.

## Adding a hook

1. Add the hook to `Plugins::Hook`, a method to `Plugins::Sink` and a
   dispatcher function in `plugins_hooks.{h,cpp}`.
2. Call it from client code and mark the place with
   `// PLUGIN-HOOK: <name>`.
3. Add `<name>` to [`tools/plugin_hooks.txt`](../tools/plugin_hooks.txt).
4. Run `python tools/check_plugin_hooks.py`. Run it again after every merge
   of a new AyuGram / Telegram Desktop version: it fails if a hook got lost.
