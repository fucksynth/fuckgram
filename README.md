# fuckgram

<img src="Telegram/Resources/art/icon256.png" width="96" align="right" alt="fuckgram icon">

[ English | [Русский](README-RU.md) ]

A Telegram Desktop client with a plugin core on Lua. Based on
[AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop), which is based
on [Telegram Desktop](https://github.com/telegramdesktop/tdesktop).

[![Windows](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml/badge.svg?branch=dev)](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)

> **Early development.** There are no releases yet. Test builds for Windows
> are produced by GitHub Actions on every push to `dev`.

## Features

Everything AyuGram Desktop has:

- Ghost mode (flexible)
- Message history and anti-recall
- Streamer mode
- Translator
- Font and appearance customization
- Local Telegram Premium features

Plus, in progress:

- **Plugins on Lua 5.4.** Every plugin runs in its own Lua state with memory
  and time limits and no access to the file system, processes or native code.
- **Permissions checked before install.** A plugin declares what it needs in
  its manifest; the code is analysed on import and a plugin that uses more
  than it declares is rejected.
- **`.flua` packages**: manifest, code and resources in one signed file.
- **A small, stable hook surface.** Client code talks to plugins only through
  `Telegram/SourceFiles/plugins/plugins_hooks.h`, so updating to a new
  AyuGram / Telegram Desktop version stays cheap.

See [docs/plugins.md](docs/plugins.md) for the design and the list of hooks.

## Downloads

No releases yet. To try a test build, open the latest successful
[Windows run](https://github.com/fucksynth/fuckgram/actions/workflows/win.yml)
and download the artifact.

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
  [exDesktop](https://github.com/metawka/exDesktop), whose plugin hooks showed
  where to attach ours
- Clients credited by AyuGram: [Kotatogram](https://github.com/kotatogram/kotatogram-desktop),
  [64Gram](https://github.com/TDesktop-x64/tdesktop),
  [Forkgram](https://github.com/forkgram/tdesktop)
- Libraries: [Lua](https://www.lua.org), [JSON for Modern C++](https://github.com/nlohmann/json),
  [SQLite](https://github.com/sqlite/sqlite), [sqlite_orm](https://github.com/fnc12/sqlite_orm)
