/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

// The commit a release build is made of and its time (unixtime): CI
// writes them before a Release build of the "release" branch. Builds
// without a commit (Debug, local) do not update themselves.
#define FUCKGRAM_COMMIT ""
#define FUCKGRAM_COMMIT_TIME 0
