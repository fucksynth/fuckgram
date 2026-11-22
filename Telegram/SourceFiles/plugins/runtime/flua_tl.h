/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#pragma once

#include "mtproto/core_types.h"

struct lua_State;

// Generic TL codec for plugins: raw MTProto objects <-> Lua values, driven
// by api.tl itself (built into the client as :/plugins/api.tl), so every
// method and type of the current layer works without generated bindings.
//
// In Lua an object is a table with its constructor name in "_":
//   { _ = "messages.sendMessage", peer = { _ = "inputPeerSelf" },
//     message = "hi", random_id = 42 }
// int / long -> integer, double -> number, string / bytes -> string,
// int128 / int256 -> string of 16 / 32 bytes, Bool -> boolean,
// Vector<T> -> array. Optional fields are nil when absent, "true" flags
// are true or nil. flags fields are computed when writing and are not
// shown when reading.
namespace Plugins::Flua::Tl {

// What a value is read or written as.
struct Type {
	int index = -1;

	[[nodiscard]] bool valid() const {
		return index >= 0;
	}
};

// False if api.tl could not be loaded: every function below then fails.
[[nodiscard]] bool Available();

[[nodiscard]] Type Query();  // a method call: { _ = "messages.getHistory" ... }
[[nodiscard]] Type Object(); // any boxed object, also Bool and vectors
[[nodiscard]] Type Boxed(const char *name); // e.g. "Updates", invalid if unknown

// The innermost method of a call, looking through invokeWithLayer-like
// wrappers: messages.getHistory for invokeWithTakeout(getHistory).
// 0 if not known.
[[nodiscard]] mtpTypeId MethodOf(const mtpBuffer &call);

// What a method returns, Object() if the method is not known.
[[nodiscard]] Type ResultOf(mtpTypeId method);

// Constructor / method by name and back, 0 / empty if not known.
[[nodiscard]] mtpTypeId Id(const QByteArray &name);
[[nodiscard]] QByteArray Name(mtpTypeId id);
[[nodiscard]] bool IsMethod(mtpTypeId id);

inline constexpr auto kRpcError = mtpTypeId(0x2144ca19);

// Only inside a protected Lua call: both raise Lua errors on bad input.
// Push leaves one value on the stack.
void Push(lua_State *L, Type type, const mtpBuffer &data);
[[nodiscard]] mtpBuffer Read(lua_State *L, int index, Type type);

} // namespace Plugins::Flua::Tl
