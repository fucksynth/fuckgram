/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

namespace Fuckgram {

// Updates of the client itself come from the newest stable release in
// the Releases of fucksynth/fuckgram: the one the authors publish by hand
// (pre-releases and drafts are not offered). Its Windows x64 zip is
// downloaded, checked by the SHA-256 GitHub keeps for it, and the program
// from it is put next to the running one; the user restarts into it from
// the button under the chat list or the settings.

enum class UpdateState {
	Disabled, // not a release build (Debug, local) or not Windows
	Idle,
	Checking,
	Latest,
	Downloading,
	Ready, // downloaded and checked, waits for a restart
	Failed,
};

struct UpdateStatus {
	UpdateState state = UpdateState::Idle;
	QString current; // the release this program came from, if known
	QString commit; // the commit of this program
	QString available; // the tag of the newest release
	int64 received = 0;
	int64 total = 0;
	QString error;
	QString page; // the release on GitHub
};

// At the start of the client: removes what the last update left and
// plans the checks.
void StartUpdater();

// Checks now; `manual` shows the errors and the result.
void CheckForUpdates(bool manual = true);

// Puts the downloaded program in place and restarts.
void InstallUpdate();

[[nodiscard]] UpdateStatus CurrentUpdateStatus();
[[nodiscard]] rpl::producer<UpdateStatus> UpdateStatusValue();

[[nodiscard]] bool AutoUpdates();
void SetAutoUpdates(bool enabled);

// Short state for the settings: "Version 7.2.10", "Downloading 40%".
[[nodiscard]] QString UpdateStatusText(const UpdateStatus &status);

// English or Russian by the interface language.
[[nodiscard]] QString UpdateText(const char *english, const char *russian);

} // namespace Fuckgram
