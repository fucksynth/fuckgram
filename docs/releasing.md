# Releasing fuckgram

Users update to the newest **stable release** in
[Releases](https://github.com/fucksynth/fuckgram/releases): the client
checks it once a day, downloads its Windows zip, checks it by the SHA-256
GitHub keeps for every uploaded file and by the release's Ed25519
signature, offers "Update fuckgram" under the
chat list and shows the release notes after the restart ("What's new").
Nothing reaches users until a release is published by hand.

## A full version

1. Actions → Windows → **Run workflow** on `release`, with
   `release_version` set, e.g. `7.2.10`.
2. CI builds Release, stamps it with its commit
   (`Telegram/SourceFiles/fuckgram/fuckgram_build.h`) and makes a **draft**
   release: tag `7.2.10` on that commit, title "fuckgram Desktop 7.2.10",
   `fuckgram-v7.2.10-win-x64-portable.zip` and the notes: the file
   `docs/releases/7.2.10.md` when there is one (commit it before step 1),
   else up to 40 latest commits since the last release. A draft of the same
   version made before is replaced.
3. Download the zip from the draft and check it.
4. **Sign it** on the releaser's machine, where the key is (never on
   GitHub): `GH_TOKEN=... python tools/release-sign/sign_release.py 7.2.10
   <release key .pem>` downloads the draft's zip, signs it with Ed25519 and
   uploads `<zip>.sig` to the draft. Clients install only an archive with a
   matching signature, so a release made by someone who gets into the
   account does not install. The public half of the key is
   `kReleasePublicKey` in `Telegram/SourceFiles/fuckgram/fuckgram_updater.cpp`.
5. Edit the notes (they are the "What's new" window in the client: headings,
   `**bold**`, lists and links are shown) and **Publish**. "Set as a
   pre-release" keeps it from users.

Pushes to `release` build Release too (stamped, so they update themselves
later) but make no drafts; Debug builds of `dev` never update.

## By hand

A release made without the workflow works as well: a zip whose name has
`win` and `x64` with the exe (at the top or in a folder) of a stamped
Release build named `fuckgram.exe`, its `<zip>.sig` made by `sign_release.py`,
a tag on the commit the exe was built from.

## How the client decides

- The newest release is the one `releases/latest` of the GitHub API gives:
  not a draft, not a pre-release.
- No update if this program came from that release, if its commit is the
  release tag's commit, or if its commit is newer than the release.
- The client remembers the release it updated to, so a tag on a later
  commit than the exe does not make it download the same release again.
- A release without a `win`/`x64` zip, or with a file that does not match
  its size and SHA-256, is not installed.
