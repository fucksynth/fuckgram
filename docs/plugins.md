# Plugins

Status: **in development.** The Lua runtime (API version 1) is in the
client: it checks every package by analysing its code, loads plugins, runs
them in a sandbox and connects them to the hooks, including raw MTProto
requests, responses and updates decoded with the TL schema. The catalog
installs and updates `.flua` packages from Telegram channels and the
official GitHub repository.

## Writing a plugin (API 1)

A plugin is a folder or a `.flua` archive (zip) with `manifest.json` at its
root. Put it into `plugins/` next to `tdata` (portable) or in the fuckgram data
folder, then restart. Settings → Plugins shows a card for each plugin: its
icon, version, author, description, permissions and last error, a switch,
and buttons to send the `.flua` to a chat, read its journal (`tg.log`,
`print`, errors), open a development folder or delete an installed plugin.
Examples: [`examples/plugins/hello`](../examples/plugins/hello) (send filter,
menus, settings), [`examples/plugins/weather`](../examples/plugins/weather)
(`tg.async` + HTTP), [`examples/plugins/raw`](../examples/plugins/raw)
(`tg.invoke`, MTProto filters) and
[`examples/plugins/selftest`](../examples/plugins/selftest) (a manual test of
the whole API: type `/t` in any chat).

Pack a folder with `python tools/flua_pack.py <folder>`: it checks the
manifest and writes `<id>.flua`. The code check below runs in the client.
An unpacked plugin folder in `plugins/` is a development folder: with the
developer mode on (Settings → Plugins → Plugin engine, on by default), when a
file in it is saved, added or removed the client reloads the plugin within a
second and shows a toast with the result or the error, no restart needed.
Types for the editor (lua-language-server) are in
[`docs/sdk/fuckgram.lua`](sdk/fuckgram.lua): add `docs/sdk` to
`workspace.library` for completion and checks of the whole API.

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
do and installs it on confirmation, turned on or off. Settings → Plugins
turns plugins on and off and opens the plugins folder.

```json
{
    "id": "example.hello",
    "name": "Hello",
    "version": "1.0",
    "author": "you",
    "description": "What it does, in a sentence or two.",
    "icon": "resources/icon.png",
    "api": { "min": 1, "max": 1 },
    "entry": "init.lua",
    "categories": ["tools"],
    "permissions": ["messages.send", "ui", "storage"]
}
```

`icon` is optional: a square PNG or JPEG in the package, up to 1024×1024
(without it the card shows the first letter of the name).

`categories` is optional too: the plugin catalog sorts plugins by them.
Known ones are `appearance`, `chats`, `media`, `privacy`, `tools`,
`integrations`, `fun` and `libraries`;
other names are skipped (`tools/flua_pack.py` warns about them).

### Plugin catalog

Settings → Plugins → Plugin catalog lists plugins posted in Telegram
channels. To publish a plugin, post its `.flua` file in a public channel:
the catalog reads the newest 500 documents of the channel, downloads every
`.flua` among them into `plugins_data/catalog/` and checks it like any
package (manifest and the code check), so a broken file never installs.
When the same `id` is posted again, the newest post wins; a newer `version`
than the installed one shows the plugin under "Updates".

- **Verified sources** are built into the client: the channel
  [@fuckgramplugins](https://t.me/fuckgramplugins) and the repository
  [fucksynth/fuckgram-plugins](https://github.com/fucksynth/fuckgram-plugins).
  Their plugins install with "Unknown sources" off and the install box
  says "verified source".
- **The repository** is read from its `main` branch: `index.json` lists
  the packages, newest first, as
  `{ "plugins": [ { "file": "dist/x.flua", "size": 123, "sha256": "...", "date": 1700000000 } ] }`.
  A file whose size or SHA-256 does not match is not shown; downloaded
  files are kept by their hash, so an unchanged plugin is not downloaded
  again.
- **Your channels** are added by username or `t.me` link and kept in
  `plugins_data/catalog.json`. Their plugins install only with "Unknown
  sources" on, like `.flua` files from chats.

### Plugin engine

Settings → Plugins → Plugin engine:

- **Developer mode** — development folders reload on save.
- **Compact view** — cards without descriptions and buttons.
- **Unknown sources** — off: `.flua` files from chats and from your own
  catalog channels are not installed (verified channels still are).
- **Code check** — always on.
- **Confirm sensitive actions** (on by default) — `tg.invoke` of payments,
  Stars, gifts (`payments.*` except reading), paid reactions, `auth.*`,
  sessions, the password, the phone, Telegram Passport, account deletion and
  ownership transfer waits for the user's "Allow"; refused calls end with
  `CLIENT_PLUGIN_DENIED`. `mtproto.request` filters may cancel such
  requests but not change them.
- **Safe mode** — plugins do not run. It turns on by itself when the client
  closes while plugins start (the first 10 seconds).

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
| `plugin.menu(place, text, fn(ctx))`, `plugin.menu{...}` | `ui` | menu item or settings row, see [Pages, menus and drawing](#pages-menus-and-drawing); returns an id for `plugin.menu_remove(id)` |
| `plugin.settings.page(builder)` | `ui` | the plugin's settings page (a gear on its card), see below |
| `plugin.settings.get(key, default)`, `.set(key, value)` | `storage` | small values kept between runs |
| `plugin.resource(path)` | — | bytes of `resources/<path>` from the package, `nil` if there is none; read-only |
| `plugin.files.read(name)`, `.write(name, bytes)`, `.remove(name)`, `.list()` | `storage` | the plugin's own files in `plugins_data/<id>/files/` (names of `A-Z a-z 0-9 _ . -`, up to 16 MB each and 64 MB in all); copy a database from `resources/` here to change it |
| `tg.toast(text)`, `tg.toast{ text, title, duration }` | `ui` | short notice in the window |
| `tg.messages.send(peer, text, options)` | `messages.send` | send a text message; `options`: `reply_to` (message id), `silent`, `schedule` (unixtime), `parse_mode` (`markdown` or `html`) |
| `tg.messages.send_file(peer, { bytes, name, caption, photo, silent })` | `messages.send` | send bytes as a file or a photo, up to 16 MB |
| `tg.messages.edit(peer, id, text, options)` | `messages.manage` | edit a message you may edit; `options.parse_mode` as for send |
| `tg.messages.delete(peer, ids, revoke)` | `messages.manage` | delete up to 100 messages; `revoke` deletes for everyone where allowed; returns how many |
| `tg.messages.get(peer, id)` | `messages.read` | a loaded message or `nil` |
| `tg.peers.get(id)` | `chats.read` | `{ id, type, name, username, is_self, premium, verified }` of a known chat or user, `type` is `user`, `bot`, `group`, `supergroup` or `channel` |
| `tg.me()` | `chats.read` | the same for your account |
| `tg.chats.current()` | `chats.read` | id of the chat open in the active window |
| `tg.compose.get()`, `tg.compose.set(text)` | `compose` | the text in the current message field |
| `tg.ui.refresh()` | `display` | re-apply `display` handlers to what is already shown |
| `tg.ui.alert(text, title)` | `ui` | a message box |
| `tg.ui.confirm(text or { title, text, ok, cancel })` | `ui` | `true` / `false`; waits inside `tg.async` or takes a callback |
| `tg.ui.input{ title, text, value, placeholder, ok, cancel }` | `ui` | the entered text or `nil`; waits inside `tg.async` or takes a callback |
| `tg.ui.copy(text)` | `ui` | put text into the clipboard |
| `tg.ui.dialog{...}`, `tg.ui.overlay{...}`, `tg.ui.image(bytes)`, `tg.ui.open(target)` | `ui` | see [Pages, menus and drawing](#pages-menus-and-drawing) |
| `tg.ui.current()` | `chats.read` | the chat open in the active window and its title |
| `tg.async(fn, ...)` | — | run `fn` as a coroutine; inside it `tg.http.request` waits for the reply without blocking the client |
| `tg.http.request{ url, method, headers, body }` | `net:<host>` | HTTPS only, to declared hosts (`net:api.example.com`, `net:*.example.com`); returns `{ status, body, headers, error }`; no automatic redirects; pass a callback as the 2nd argument instead of using `tg.async` |
| `tg.timer.after(ms, fn)`, `tg.timer.every(ms, fn)`, `tg.timer.cancel(id)` | — | one-shot and repeating timers (repeating ≥ 100 ms, up to 64) |
| `tg.json.encode(value)`, `tg.json.decode(text)` | — | JSON ↔ Lua tables; `decode` returns `nil, error` on bad input |
| `tg.invoke(call)` | `mtproto` | any API method: `local r, err = tg.invoke{ _ = "help.getNearestDc" }` inside `tg.async`, or pass a callback `fn(result, err)` as the 2nd argument; `err` is `{ code, type, description }`; a request dropped with its account (logout) ends with `CLIENT_PLUGIN_ABORTED` |
| `plugin.on("mtproto.request", fn(call), methods)` | `mtproto` | every request the client sends; return `false` to cancel it (the caller gets the error `CLIENT_PLUGIN_CANCELLED`), a table to send instead, `nil` to keep |
| `plugin.on("mtproto.response", fn(result, method), methods)` | `mtproto` | every answer (also `rpc_error`) before the client sees it; return a value to replace it |
| `plugin.on("mtproto.updates", fn(updates))` | `mtproto` | raw updates container; return `false` to drop it, a table to replace it |
| `tg.log(...)`, `print(...)` | — | write to the client log and the plugin journal |
| `tg.dump(value)` | — | readable text of a value, tables with their fields |
| `require("source.module")` | — | load `source/module.lua` or `lib/module.lua` from the package |
| global `on_enable()`, `on_disable()` | — | called after the entry file ran and before the plugin stops |

`msg` is `{ id, peer, from, text, out, date }`. Chat ids are written as in the
Bot API: users `> 0`, basic groups `< 0`, channels and supergroups
`-100…`.

### Pages, menus and drawing

See `examples/plugins/showcase`: it uses everything below.

#### Settings page and other pages

`plugin.settings.page(builder)` gives the plugin a settings page: a gear
appears on its card in Settings → Plugins. `builder` returns the rows; it
runs every time the page opens and after `plugin.settings.refresh()`.

```lua
plugin.settings.page(function()
    return {
        { type = "header", text = "Display" },
        { type = "switch", key = "compact", text = "Compact", subtext = "…",
          icon = "eye", default = false, on_change = function(on) end },
        { type = "selector", key = "mode", text = "Mode",
          items = { "One", "Two" }, default = 1 },          -- 1-based
        { type = "input", key = "name", text = "Name", placeholder = "…" },
        { type = "slider", key = "size", text = "Size", min = 10, max = 24,
          step = 1, default = 14 },
        { type = "custom", height = 64, paint = function(c) end,
          on_click = function() end },                       -- canvas row
        { type = "divider", text = "A caption under a block" },
        { type = "text", text = "More", subtext = "…", icon = "info",
          page = function() return { title = "More", ... } end },  -- sub page
        { type = "text", text = "Reset", red = true, on_click = function() end },
    }
end)
```

- Rows with a `key` (switch, selector, input, slider) keep their value in the
  plugin settings: `plugin.settings.get(key)` reads it (needs `storage`),
  `on_change(value)` gets the new one.
- `text` rows take `on_click` or `page` (a nested page; nesting is allowed);
  `accent` and `red` color the title. A page may set `title`.
- `icon` is one of the [icon names](#icons).
- `plugin.settings.refresh()` builds open pages of the plugin again;
  `plugin.settings.all()` returns every setting, `plugin.settings.reset()`
  clears them (both need `storage`).

#### Menus and rows anywhere

`plugin.menu{ place, text, icon, on_click, page }` returns an id for
`plugin.menu_remove(id)`. `on_click(ctx)` runs on a click, `page` opens a
page instead. The short form `plugin.menu(place, text, fn)` still works.

| `place` | Where |
|---|---|
| `message` | message context menu; `ctx.peer`, `ctx.message` |
| `chat` | the ⋮ menu of a chat (in the "Plugins" submenu); `ctx.peer` |
| `profile` | the ⋮ menu of a profile; `ctx.peer` |
| `main` | the main menu (the left drawer) |
| `send` | the menu of the send button (right click); `ctx.peer` |
| `settings.<section>` | a row at the end of a settings section |

Settings sections: `main`, `account`, `notifications`, `privacy`, `chats`,
`folders`, `advanced`, `calls`, `sessions`, `blocked`, `shortcuts`,
`fuckgram`, `fuckgram.ghost`, `fuckgram.filters`, `fuckgram.general`,
`fuckgram.appearance`, `fuckgram.chats`, `fuckgram.other`, `plugins`.

#### Canvas and overlays

`tg.ui.overlay{ x, y, width, height, corner, paint, on_click }` shows a
widget over the window, `corner` is `top-left` (default), `top-right`,
`bottom-left` or `bottom-right` and `x`, `y` count from it. Without
`on_click` clicks go through it. It returns `{ id, update(), move{...},
close() }`: call `o:update()` when what it shows changes.

With `draggable = true` the user carries the overlay with the mouse: once
a press moves past the drag distance the client moves it after the cursor
by itself (smoothly, without calls per frame), and a press that does not
move is still a click. `on_drag_start(x, y)`, `on_drag(x, y)` on every move
and `on_drop(x, y)` get its top-left in the window; `o:move{...}` waits
until the drop, and a dropped overlay counts `x`, `y` from the top-left
corner. Older clients ignore these fields: the overlay just stays put.

```lua
local cat = tg.ui.overlay{
    x = 100, y = 100, width = 64, height = 64,
    paint = paint_cat,
    on_click = pet,
    draggable = true,
    on_drag_start = function() carried = true end,
    on_drop = function(x, y) carried = false; home = { x = x, y = y } end,
}
```

`paint(c)` gets a canvas — for overlays and `custom` rows — that works only
inside that call. Sizes are pixels at 100 % interface scale, the client
scales them.

| Canvas | |
|---|---|
| `c:size()` | width, height |
| `c:fill(color)` | the whole area |
| `c:rect(x, y, w, h, color, radius)` | a filled (rounded) rectangle |
| `c:stroke(x, y, w, h, color, width, radius)` | its outline |
| `c:circle(x, y, r, color)`, `c:line(x1, y1, x2, y2, color, width)` | |
| `c:text(x, y, text, { size, bold, italic, color, align, width, height })` | `width` wraps, returns the width and height taken |
| `c:measure(text, options)` | width, height |
| `c:image(id, x, y, w, h, radius)` | a picture from `tg.ui.image` |
| `c:polygon({ x1, y1, x2, y2, ... }, color)` | a filled polygon: icons, arrows |
| `c:opacity(value)` | for what is drawn next in this call |

Colors are `"#rrggbb"`, `"#aarrggbb"`, `{ r, g, b, a }` or a theme color:
`windowBg`, `windowBgOver`, `windowBgRipple`, `windowFg`, `windowSubTextFg`,
`windowBoldFg`, `windowActiveTextFg`, `windowShadowFg`, `activeButtonBg`,
`activeButtonFg`, `lightButtonFg`, `attentionButtonFg`, `boxBg`,
`boxTextFgGood`, `menuBg`, `menuIconFg`, `shadowFg`, `placeholderFg`,
`dialogsBg`, `sideBarBg`, `titleBg`, `historyComposeAreaBg`, `msgInBg`,
`msgOutBg`, `historyTextInFg`, `historyTextOutFg`,
`historyPeer1UserpicBg` … `historyPeer8UserpicBg`. `tg.ui.color(name)`
gives its current value as `#aarrggbb`.

`tg.ui.image(bytes)` decodes PNG, JPEG, WebP or GIF bytes (from
`plugin.resource`, `plugin.files.read` or an HTTP reply) and returns
`id, width, height`; `tg.ui.image_free(id)` drops it. Up to 64 images,
larger than 2048 px are scaled down.

#### Dialogs, notices, the window

| Lua | Permission | |
|---|---|---|
| `tg.ui.dialog{ title, text, items, buttons, danger }` | `ui` | `items` is a list to choose from, `buttons` up to 3, `danger` makes one red; returns the number chosen, `nil` if closed. Waits inside `tg.async` or takes a callback |
| `tg.toast{ text, title, duration }` | `ui` | `duration` in ms; `tg.toast(text)` still works |
| `tg.ui.current()` | `chats.read` | `{ screen = "chat" or "chats", peer, title, window }` |
| `tg.ui.open(target)` | `ui` | opens a chat id, `"@username"` or a link |
| `tg.dump(value)` | — | readable text of a value, tables with their fields |
| `tg.ui.window_size()` | `ui` | width and height of the window content, at 100 % scale |
| `tg.now()` | — | milliseconds of a steady clock, for animations |

#### The music player

`player` lets a plugin see what the client's player plays and press its
buttons (`examples/plugins/material_player` draws its own player with it).

| Lua | |
|---|---|
| `tg.player.state()` | `{ title, performer, playing, position, duration, voice, has_next, has_previous, peer, message }` of the music, or of a voice message if no music plays; `nil` when nothing plays. `position` and `duration` are seconds |
| `tg.player.toggle()`, `.play()`, `.pause()`, `.stop()` | |
| `tg.player.next()`, `.previous()` | |
| `tg.player.seek(fraction)` | `0` is the start, `1` the end |

#### Formatting and files

`tg.messages.send(peer, text, { parse_mode = "markdown" })` and
`tg.messages.edit(peer, id, text, { parse_mode = "html" })` format the
text. Markdown is the client's own: `**bold**`, `__italic__`, `~~strike~~`,
`||spoiler||`, `` `code` ``, fenced blocks, links. HTML takes `<b>`, `<i>`,
`<u>`, `<s>`, `<code>`, `<pre>`, `<a href>`, `<tg-spoiler>`,
`<blockquote>` and `&amp;`-style entities.

`tg.messages.send_file(peer, { bytes, name, caption, photo, silent })`
sends bytes as a file (`photo = true` compresses a picture), up to 16 MB;
needs `messages.send`.

#### Icons

`settings`, `palette`, `customize`, `font`, `chat`, `chats`, `reply`,
`forward`, `edit`, `draw`, `copy`, `delete`, `pin`, `unpin`, `select`,
`send`, `schedule`, `silent`, `mute`, `unmute`, `link`, `share`,
`download`, `folder`, `file`, `photo`, `gif`, `sticker`, `emoji`,
`reactions`, `search`, `translate`, `info`, `faq`, `user`, `users`,
`add_user`, `admin`, `channel`, `bot`, `plugins`, `lock`, `key`, `shield`,
`privacy`, `block`, `eye`, `eye_off`, `ghost`, `bell`, `sound`,
`sound_off`, `phone`, `video`, `star`, `fave`, `like`, `premium`, `gift`,
`shop`, `stats`, `timer`, `hourglass`, `archive`, `saved`, `night`,
`network`, `storage`, `devices`, `experimental`, `power`, `restore`,
`refresh`, `add`, `remove`, `cancel`, `expand`, `collapse`, `new_window`,
`qr`, `location`, `ip`, `username`, `topics`, `poll`, `article`, `spoiler`,
`tag`, `payment`, `code`.

### Translations

A plugin speaks the interface language of the client. `name` and
`description` in `manifest.json` are the default (English) texts; others
go to `localized`:

```json
{
    "name": "Pixel cat",
    "description": "A pixel cat walks around the window.",
    "localized": {
        "ru": { "name": "Пиксельный котик", "description": "По окну гуляет пиксельный котик." }
    }
}
```

The plugin manager, the catalog and the install box show the texts of the
interface language, and `plugin.name` is the translated name.

Texts of the code live in `locales/<language>.json` of the package, one
flat object per language:

```json
{
    "menu.call": "Call the cat",
    "toast.hello": "Hello, {1}!",
    "toast.named": "{name} is here",
    "messages": { "one": "{1} message", "other": "{1} messages" }
}
```

```lua
plugin.t("menu.call")                  -- "Позвать котика"
plugin.t("toast.hello", "Ann")         -- {1}, {2}... are the arguments
plugin.t("toast.named", { name = "Tom" })
plugin.t("messages", 5)                -- "5 сообщений"
tg.lang()                              -- "ru", "en", "pt-br"
```

- A text is looked up in the interface language (`pt-br`), then its base
  (`pt`), then `en`; a missing key returns the key itself.
- A plural value is an object of `one`, `few`, `many`, `other`. The count
  is the first argument when it is a number, or the `count` field of a
  table; the form follows the rules of the file's language (Russian,
  Ukrainian, Belarusian, Polish, Czech, Slovak, and "one"/"other" for the
  rest). A missing form falls back to `other`, then `many`.
- The language can change while the plugin runs: settings pages are built
  every time they open, so take texts in the page function; menu texts
  are taken once.
- `tools/flua_pack.py` checks the locale files and warns when languages
  have different keys.

### exteraGram SDK and fuckgram

What a plugin for exteraGram (Android) uses and where it is here:

| exteraGram | fuckgram |
|---|---|
| `__id__`, `__name__`, `__version__`, `__author__`, `__description__`, `__icon__`, `__category__` | `manifest.json`: `id`, `name`, `version`, `author`, `description`, `icon`, `categories` |
| `on_plugin_load` / `on_plugin_unload` / `on_app_event` | `on_enable()` / `on_disable()` / `plugin.on("app.state")` |
| `get_setting` / `set_setting` / `export_settings` / `import_settings` | `plugin.settings.get` / `.set` / `.all` / `.reset` |
| `create_settings()`: `Header`, `Divider`, `Switch`, `Selector`, `Input`, `Text`, `EditText`, `Custom`, `create_sub_fragment` | `plugin.settings.page()`: `header`, `divider`, `switch`, `selector`, `input`, `text`, `slider`, `custom` (canvas), `page` |
| `add_menu_item(MenuItemData)`: message, drawer, chat action, profile action menus; `icon`; `remove_menu_item` | `plugin.menu{...}`: `message`, `main`, `chat`, `profile`, plus `send` and `settings.<section>`; `icon`; `plugin.menu_remove` |
| `pre_request_hook` / `post_request_hook` / `on_updates_hook` | `plugin.on("mtproto.request" / "mtproto.response" / "mtproto.updates")` |
| `on_send_message_hook` | `plugin.on("message.send")` |
| `send_request(TLObject)` | `tg.invoke{ _ = "method", ... }` |
| `send_text` / `send_message(parse_mode)` / `edit_message` | `tg.messages.send` / `.edit` with `parse_mode` |
| `send_photo` / `send_document` / `send_video` / `send_audio` | `tg.messages.send_file` |
| `BulletinHelper.show_*` | `tg.toast` |
| `AlertDialogBuilder` (title, message, items, buttons, red button) | `tg.ui.dialog`, `tg.ui.confirm`, `tg.ui.input`, `tg.ui.alert` |
| `copy_to_clipboard`, `run_on_ui_thread(delay)` | `tg.ui.copy`, `tg.timer.after` |
| `parse_text` (HTML, Markdown) | `parse_mode` of send / edit |
| `file_utils` (plugin folders, read / write) | `plugin.files.*`, `plugin.resource` (only the plugin's own folder) |
| Intents (deep link handlers) | `plugin.on("url.open")` |
| `FilesController` (open a file type) | `plugin.on("action")` with `file.open` |
| `NotificationCenterDelegate` events | `plugin.on("message.new" / "screen" / "action" / ...)` |
| Custom Android views, `SimpleSettingFactory` | the canvas: `custom` rows and `tg.ui.overlay` |
| `get_media_controller()` (the player) | `tg.player` |
| Plugin repositories (channels, Git index) | the plugin catalog (channels) |
| Dev server | development folders that reload on save |
| `logger` | `tg.log`, `print`, `tg.dump`, the plugin journal |
| Xposed method hooks, reflection | not offered: plugins see the client only through the hooks and the API, which is what the code check is for |

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
`load` / `loadfile` / `dofile`, no `debug`, no `rawget` / `rawset`, no `__gc`
metamethods (finalizers can't be time-limited), and only text Lua is accepted,
never bytecode. A call that runs too long is stopped
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
├── locales/        texts: en.json, ru.json... (see "Translations")
└── resources/      icons, texts, initial database (read-only)
```

Writable data (settings, database copies) lives in a separate per-plugin data
folder, so updating a package does not lose it.

Catalog trust today: "verified source" means the package comes from the
@fuckgramplugins channel or the official repository
(`fucksynth/fuckgram-plugins`); from the repository the client also checks
the file's size and SHA-256 against `index.json`. There are no signatures:
every package, from any source, is analysed locally on install like a file
the user opens, so packages must not be obfuscated.

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
| Server | `outgoing_request` | cancel or replace any outgoing MTProto request (by TL constructor); a cancelled request fails with `CLIENT_PLUGIN_CANCELLED` |
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
