/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

namespace Fuckgram {

// Updates of the client itself: CI publishes every Release build of the
// "release" branch as a GitHub release "build-N" with the program in
// fuckgram-win-x64.zip and its size and SHA-256 in update.json. The
// client checks the newest release once a day, downloads it, checks the
// archive and puts the new program next to the running one; the user
// restarts into it from the button under the chat list or the settings.

enum class UpdateState {
	Disabled, // a build without a number: Debug or local, or not Windows
	Idle,
	Checking,
	Latest,
	Downloading,
	Ready, // downloaded and checked, waits for a restart
	Failed,
};

struct UpdateStatus {
	UpdateState state = UpdateState::Idle;
	int current = 0; // the build of this program
	int available = 0; // the newest build found
	int64 received = 0;
	int64 total = 0;
	QString error;
	QString page; // the release on GitHub
};

[[nodiscard]] int CurrentBuild();

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

// Short state for the settings: "Build 120", "Downloading 40%".
[[nodiscard]] QString UpdateStatusText(const UpdateStatus &status);

// English or Russian by the interface language.
[[nodiscard]] QString UpdateText(const char *english, const char *russian);

} // namespace Fuckgram
