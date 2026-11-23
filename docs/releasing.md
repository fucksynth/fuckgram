# Releasing fuckgram

Users update to the newest **stable release** in
[Releases](https://github.com/fucksynth/fuckgram/releases): the client
checks it once a day, downloads its Windows zip, checks it by the SHA-256
GitHub keeps for every uploaded file, and offers "Update fuckgram" under
the chat list. Nothing reaches users until a release is published by hand.

## Steps

1. Push to `release`. CI builds Release and stamps the build with its
   commit (`Telegram/SourceFiles/fuckgram/fuckgram_build.h`): only such
   builds update themselves, Debug builds of `dev` never do.
2. Download the artifact `fuckgram-win-x64-Release-<sha>` of that run and
   check the exe yourself.
3. Pack the exe into a zip whose name has `win` and `x64`, e.g.
   `fuckgram-v7.2.9-win-x64-portable.zip`. The exe may be at the top or in
   a folder.
4. Draft a new release: a new tag (e.g. `7.2.10`) **on the commit the exe
   was built from** (the run's commit; by default GitHub takes the head of
   the chosen branch, which is the same if nothing was pushed since),
   a title, notes, the zip.
5. Publish it. "Set as a pre-release" keeps it from users: a test build
   for those who download it by hand.

## How the client decides

- The newest release is the one `releases/latest` of the GitHub API gives:
  not a draft, not a pre-release.
- No update if this program came from that release, if its commit is the
  release tag's commit, or if its commit is newer than the release.
- The client remembers the release it updated to, so a tag on a later
  commit than the exe does not make it download the same release again.
- A release without a `win`/`x64` zip, or with a file that does not match
  its size and SHA-256, is not installed.
