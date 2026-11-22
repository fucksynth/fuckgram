# Plugins

Status: **in development.** The Lua runtime (API version 1) is in the
client: it loads plugins, runs them in a sandbox and connects a first set of
events and functions to the hooks. Static permission analysis on import,
async calls and the `.flua` catalog are not done yet.

## Writing a plugin (API 1)

A plugin is a folder or a `.flua` archive (zip) with `manifest.json` at its
root. Put it into `plugins/` next to `tdata` (portable) or in the fuckgram data
folder, then restart. Settings → Plugins lists what is loaded and the last
error of each plugin. A full example: [`examples/plugins/hello`](../examples/plugins/hello).

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
| `plugin.menu(place, text, fn(ctx))` | `ui` | menu item; `place` is `message`, `main`, `chat` or `profile`; `ctx.peer`, `ctx.message` |
| `plugin.settings.get(key, default)`, `.set(key, value)` | `storage` | small values kept between runs |
| `tg.toast(text)` | `ui` | short notice in the window |
| `tg.messages.send(peer, text)` | `messages.send` | send a text message |
| `tg.log(...)`, `print(...)` | — | write to the client log |
| `require("source.module")` | — | load `source/module.lua` or `lib/module.lua` from the package |
| global `on_enable()`, `on_disable()` | — | called after the entry file ran and before the plugin stops |

`msg` is `{ id, peer, from, text, out, date }`. Chat ids are written as in the
Bot API: users `> 0`, basic groups `< 0`, channels and supergroups
`-100…`.

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
