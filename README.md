# fuckgram

<img src="Telegram/Resources/art/icon256.png" width="96" align="right" alt="fuckgram icon">

[ English | [Русский](README-RU.md) ]

A Telegram Desktop client with plugins on Lua. Based on
[Telegram Desktop](https://github.com/telegramdesktop/tdesktop) 7.4.6 with
everything from [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop).

[![Windows](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml/badge.svg?branch=release)](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)
[![Release](https://img.shields.io/github/v/release/fucksynth/fuckgram?label=release)](https://github.com/fucksynth/fuckgram/releases/latest)

Channel: [@fuckgramdesktop](https://t.me/fuckgramdesktop) ·
Plugins: [@fuckgramplugins](https://t.me/fuckgramplugins) ·
[Official plugins](https://github.com/fucksynth/fuckgram-plugins)

> **Status.** Windows 10 and newer, x64 (Qt 6). The current version is
> [7.4.6](https://github.com/fucksynth/fuckgram/releases/latest); it updates
> itself. Linux and macOS are not built yet.

## Download

Get the `win-x64` zip from
[Releases](https://github.com/fucksynth/fuckgram/releases/latest) and put
`fuckgram.exe` in a folder of its own: it keeps its data next to itself.

From then on fuckgram updates itself to every stable release. It checks
once a day, downloads the new version in the background, checks its
SHA-256 and shows "Update fuckgram" under the chat list; after the restart
it shows what is new. fuckgram Preferences → Updates has the state, the
check button and the switch. Pre-releases are not offered.

## Features

Everything AyuGram Desktop has: ghost mode, message history and
anti-recall, streamer mode, translator, fonts and appearance, local Telegram
Premium features. Plus:

### Plugins on Lua 5.4

- **Sandbox.** Every plugin runs in its own Lua state: no files, processes,
  `load`, `debug` or bytecode. Time limits per call (50 ms for the send
  filter, 200 ms for events, 1 s for menu clicks), 64 MB of memory, and a
  plugin that fails 5 times is stopped.
- **Code check before install.** The manifest lists permissions; the code
  is analysed when a plugin is imported, and a plugin that uses more — or
  less — than it declares is not installed.
- **45 hooks into the client:** new and sent messages; menus of messages,
  chats, profiles, the send button and the main menu; any section of the
  settings; text shown in chats and the chat list; links, replies,
  reactions, calls, the player, profile rows, screens; raw MTProto
  requests, responses and updates.
- **Interface:** a settings page for every plugin (switches, choices,
  inputs, sliders, nested pages), menu items with icons anywhere, dialogs
  and toasts, and its own drawing: a canvas for rows and floating panels
  over the window.
- **API:** send, edit and delete messages (Markdown and HTML); chats and
  users; the message field; the music player; settings and files of the
  plugin; timers, JSON, HTTPS to declared hosts, coroutines; any API method
  through `tg.invoke` with a TL codec of the client's layer.
- **Translations:** a plugin speaks the language of the interface:
  `locales/<language>.json` with `plugin.t()`, plural forms included, and
  translated names and descriptions in the manifest.
- **Safety:** payments, Stars, gifts, logins, sessions, the password,
  Telegram Passport and account deletion called by a plugin wait for the
  user's "Allow". A crash while plugins start turns safe mode on.
- **Plugin manager** (Settings → Plugins): cards with icons, versions and
  permissions, on/off, settings, journal, share, delete; a developer mode
  that reloads plugin folders on save.
- **Plugin catalog:** the verified channel and the official repository,
  your own channels, categories, search and updates. Or open a `.flua`
  file in any chat.

### Also

- 12 app icons (fuckgram Preferences → Appearance → App icon).

## Plugins

The official ones live in
[fucksynth/fuckgram-plugins](https://github.com/fucksynth/fuckgram-plugins)
and in [@fuckgramplugins](https://t.me/fuckgramplugins); the catalog in the
client shows them.

| Plugin | What it does |
|---|---|
| Material Player | the music player as a Material 3 card over the window |
| Pixel Cat | a pixel cat of 8 breeds walks around the window; pet it, carry it with the mouse |
| Link Cleaner | strips trackers (`utm_*`, `fbclid`, `si`…) from links you send |
| Calculator | `2+2*2=` is sent as `2+2*2 = 6` |
| Exchange Rates | `/cur 100 usd rub` sends the amount at today's rate |
| Keyboard Layout | fixes text typed in the wrong keyboard layout |
| Auto-Reply | answers private chats while you are away |
| API Showcase | every kind of page, menu and drawing a plugin can make |

Examples for authors (hello, weather, raw, selftest) are in
[examples/plugins](examples/plugins).

## Writing a plugin

A plugin is a folder with `manifest.json`, `init.lua` and, for
translations, `locales/`:

```lua
plugin.on("message.send", function(draft)
    if draft.text == "/shrug" then
        return "¯\\_(ツ)_/¯"
    end
end)

plugin.menu{
    place = "main",
    text = plugin.t("menu.hello"),
    icon = "plugins",
    on_click = function() tg.toast(plugin.t("hello")) end,
}
```

Pack it with `python tools/flua_pack.py <folder>`. The API, permissions,
pages, translations and the catalog are described in
[docs/plugins.md](docs/plugins.md); types for editors are in
[docs/sdk/fuckgram.lua](docs/sdk/fuckgram.lua).

## Building

- [Windows](docs/building-win.md)
- [Linux](docs/building-linux.md)
- [macOS](docs/building-mac.md)
- [How a release is made](docs/releasing.md)

You need your own [API credentials](docs/api_credentials.md). Branches:
`release` is the main one and builds the Release client, `dev` is where
development goes and builds Debug.

## License

GPL-3.0, see [LICENSE](LICENSE). This is a fork; the original copyright
notices of Telegram Desktop and AyuGram Desktop are kept in their files.

## Credits

- [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) by Radolyn Labs,
  the base of this client
- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
- [exteraGram](https://github.com/exteraSquad/exteraGram) and
  [exDesktop](https://github.com/metawka/exDesktop), whose plugin hooks and
  catalog showed the way
- Clients credited by AyuGram: [Kotatogram](https://github.com/kotatogram/kotatogram-desktop),
  [64Gram](https://github.com/TDesktop-x64/tdesktop),
  [Forkgram](https://github.com/forkgram/tdesktop)
- Libraries: [Lua](https://www.lua.org), [JSON for Modern C++](https://github.com/nlohmann/json),
  [SQLite](https://github.com/sqlite/sqlite), [sqlite_orm](https://github.com/fnc12/sqlite_orm)
