# fuckgram

<img src="Telegram/Resources/art/icon256.png" width="96" align="right" alt="fuckgram icon">

[ English | [Русский](README-RU.md) ]

A Telegram Desktop client with plugins on Lua. Based on
[AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) 7.2.9, which is
based on [Telegram Desktop](https://github.com/telegramdesktop/tdesktop).

[![Windows](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml/badge.svg?branch=release)](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)

Channel: [@fuckgramdesktop](https://t.me/fuckgramdesktop) ·
Plugins: [@fuckgramplugins](https://t.me/fuckgramplugins)

> **Status.** Windows builds work. `release` is the main branch and builds
> the Release client, `dev` is where development goes and builds Debug.
> There are no GitHub releases yet; Linux and macOS are not built yet.

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
- **43 hooks into the client:** new and sent messages, menus of messages,
  chats, profiles and the main menu, text shown in chats and the chat list,
  links, replies, reactions, calls, the player, profile rows, screens, and
  raw MTProto requests, responses and updates.
- **API:** send, edit and delete messages; chats and users; the message
  field; dialogs, notices and the clipboard; settings and files of the
  plugin; timers, JSON, HTTPS to declared hosts, coroutines; any API method
  through `tg.invoke` with a TL codec of the client's layer.
- **Safety:** payments, Stars, gifts, logins, sessions, the password,
  Telegram Passport and account deletion called by a plugin wait for the
  user's "Allow". A crash while plugins start turns safe mode on.
- **Plugin manager** (Settings → Plugins): cards with icons, versions and
  permissions, on/off, journal, share, delete; plugin engine settings with a
  developer mode that reloads plugin folders on save.
- **Install** by opening a `.flua` file in any chat, or from the **plugin
  catalog**: verified channels and your own ones, with categories, search
  and updates.

### Also

- 12 app icons (Settings → fuckgram → Appearance → App icon).

## Plugins

Ready to use, in [examples/plugins](examples/plugins):

| Plugin | What it does |
|---|---|
| Link cleaner | strips trackers (`utm_*`, `fbclid`, `si`…) from links you send |
| Calculator | `2+2*2=` is sent as `2+2*2 = 6` |
| Currency | `/cur 100 usd rub` sends the amount at today's rate |
| Layout | fixes text typed in the wrong keyboard layout |
| Auto-reply | `/away text` answers private chats while you are away |
| hello, weather, raw, selftest | examples of the API |

A plugin is a folder with `manifest.json` and `init.lua`:

```lua
plugin.on("message.send", function(draft)
    if draft.text == "/shrug" then
        return "¯\\_(ツ)_/¯"
    end
end)
```

Pack it with `python tools/flua_pack.py <folder>` and post the `.flua` in a
channel. The API, permissions and the catalog are described in
[docs/plugins.md](docs/plugins.md); types for editors are in
[docs/sdk/fuckgram.lua](docs/sdk/fuckgram.lua).

## Downloads

Builds are in [GitHub Actions](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)
(sign in to GitHub to download): the latest successful run of `release` has
the Release `fuckgram.exe`, runs of `dev` have Debug builds. Artifacts are
kept for 2 days. Put the exe in a folder of its own: it keeps its data next
to itself.

## Building

- [Windows](docs/building-win.md)
- [Linux](docs/building-linux.md)
- [macOS](docs/building-mac.md)

You need your own [API credentials](docs/api_credentials.md).

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
