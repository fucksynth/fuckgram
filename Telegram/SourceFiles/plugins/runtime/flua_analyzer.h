/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

namespace Plugins::Flua {

struct Package;

// Static check of a package before it is loaded or installed.
//
// Every .lua file is compiled (not run) by the real Lua parser and its
// bytecode is followed with a data-flow pass that tracks where the `tg`
// and `plugin` API objects go. The package is rejected if:
// - it uses an API it did not declare in manifest.json, or declares a
//   permission it never uses;
// - it uses an unknown API name, an unknown event or a menu place that
//   is not a constant string;
// - it hides API access from the analysis: dynamic indexing of API
//   tables (tg[x]), storing them in tables, globals or upvalues, passing
//   them to functions, returning them, or calling require with a
//   computed name;
// - it touches load, loadfile, dofile, rawget, rawset, debug and the like.
//
// The runtime still checks permissions on every call; this check only
// refuses packages that do not say honestly what they do.
//
// Returns an empty string if the package passes, otherwise the reason.
[[nodiscard]] QString AnalyzePackage(const Package &package);

} // namespace Plugins::Flua
