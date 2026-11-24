# Security

## Reporting a vulnerability

Please do not open a public issue. Report it privately through
[GitHub security advisories](https://github.com/fucksynth/fuckgram/security/advisories/new)
of this repository: what is affected, how to reproduce it, the version.
You get an answer within a few days; a fix goes out as a new release and the
advisory is published after it.

The plugin runtime (`Telegram/SourceFiles/plugins/`), the catalog and the
self-updater (`Telegram/SourceFiles/fuckgram/`) are the parts written here;
issues of Telegram Desktop itself also go to
[its developers](https://github.com/telegramdesktop/tdesktop/security), and of
AyuGram features to [AyuGram](https://github.com/AyuGram/AyuGramDesktop).

## What protects users

- **Updates.** The client installs a release only when its archive matches
  the SHA-256 GitHub keeps for it and the Ed25519 signature `<archive>.sig`
  next to it. The signing key is kept offline, not on GitHub: a release made
  by someone who gets into the account does not install.
- **The plugin catalog.** The official repository publishes its list signed
  (`index.signed.json`); a package from a verified source installs as trusted
  only when its SHA-256 is in a signed list. Files from chats and own channels
  need "Unknown sources", off by default.
- **Plugins.** Every package is analysed before install: the permissions it
  uses must be exactly those in its manifest. Each plugin runs in its own Lua
  state with time and memory limits; payments, sessions, the password,
  account changes and logins to sites always ask the user first. A plugin
  with the `mtproto` permission can still make other requests of the
  account: install those only from authors you trust.
