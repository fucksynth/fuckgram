/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_tl.h"

#include "logs.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QSet>

// Lua is built as C++ (see cmake/lib_lua.cmake): no extern "C" here.
#include "lua.h"
#include "lauxlib.h"

namespace Plugins::Flua::Tl {
namespace {

constexpr auto kVectorId = mtpTypeId(0x1cb5c415);
constexpr auto kBoolTrue = mtpTypeId(0x997275b5);
constexpr auto kBoolFalse = mtpTypeId(0xbc799737);
constexpr auto kMaxDepth = 64;      // objects inside objects
constexpr auto kMaxFlags = 4;       // flags:# fields per constructor
constexpr auto kMaxWrapping = 8;    // invokeWithLayer(initConnection(...))
constexpr auto kMaxWrite = 2 * 1024 * 1024 / int(sizeof(mtpPrime));
constexpr auto kMaxString = size_t((1 << 24) - 1);

// Not in api.tl, it comes from mtproto.tl: a response may be an error.
constexpr auto kExtraSchema = "rpc_error#2144ca19 error_code:int "
	"error_message:string = RpcError;\n";

enum class Kind : uchar {
	Int,
	Long,
	Double,
	String,
	Bytes,
	Int128,
	Int256,
	Bool,
	True,       // a flag without data: flags.3?true
	Flags,      // #
	Vector,     // Vector<T>: boxed, with the vector id
	BareVector, // vector<T>
	Boxed,      // a type: id + one of its constructors
	Bare,       // a constructor without its id
	Any,        // Object or a generic X: any boxed object
	Query,      // !X: any method call
};

struct TypeInfo {
	Kind kind = Kind::Any;
	int ref = -1; // Vector: element type, Boxed: boxed type, Bare: constructor
};

struct Param {
	QByteArray name;
	int type = -1;
	int slot = -1;  // flags:# itself: its slot among the flags of the object
	int flags = -1; // flags.N?T: slot of the flags it depends on
	int bit = 0;
};

struct Constructor {
	QByteArray name;
	mtpTypeId id = 0;
	std::vector<Param> params;
	bool function = false;
	int boxed = -1;  // types: the boxed type it belongs to
	int result = -1; // functions: what they return
	int query = -1;  // functions with {X:Type}: index of the !X param
};

struct BoxedInfo {
	QByteArray name;
	std::vector<int> constructors;
};

struct Error {
	QByteArray text;
};

[[noreturn]] void Throw(QByteArray text) {
	throw Error{ std::move(text) };
}

[[nodiscard]] bool IsUpper(char ch) {
	return (ch >= 'A' && ch <= 'Z');
}

[[nodiscard]] bool IsLower(char ch) {
	return (ch >= 'a' && ch <= 'z');
}

// "messages.Messages" -> 'M', "inputPeerUser" -> 'i'.
[[nodiscard]] char LastPartStart(const QByteArray &name) {
	const auto index = name.lastIndexOf('.') + 1;
	return (index < name.size()) ? name[index] : char(0);
}

[[nodiscard]] QByteArray Hex(mtpTypeId id) {
	return "0x" + QByteArray::number(id, 16);
}

class Schema final {
public:
	Schema();

	void parse(const QByteArray &text);
	void finish();

	[[nodiscard]] const Constructor *byId(mtpTypeId id) const;
	[[nodiscard]] const Constructor *byName(const QByteArray &name) const;
	[[nodiscard]] const Constructor *methodOf(
		const mtpPrime *from,
		const mtpPrime *end,
		int depth) const;

	std::vector<TypeInfo> types;
	std::vector<Constructor> constructors;
	std::vector<BoxedInfo> boxedTypes;
	QHash<mtpTypeId, int> ids;
	QHash<QByteArray, int> names;
	QHash<QByteArray, int> boxedNames;
	QHash<QByteArray, int> typeCache; // type text -> types index
	int anyType = -1;
	int queryType = -1;

private:
	int add(TypeInfo info);
	[[nodiscard]] int type(
		const QByteArray &text,
		const QSet<QByteArray> &generics);
	[[nodiscard]] int boxed(const QByteArray &name);
	void parseLine(const QByteArray &line, bool function);

	std::vector<std::pair<int, QByteArray>> _bare; // resolved in finish()

};

// Reads TL data. With a lua_State pushes what it reads, without one only
// skips it (used to look inside wrapped calls).
class Reader final {
public:
	Reader(
		const Schema &schema,
		lua_State *L,
		const mtpPrime *from,
		const mtpPrime *end)
	: _schema(schema)
	, _lua(L)
	, _from(from)
	, _end(end) {
	}

	void value(int type, int depth);

	// The params of `constructor`, its id is already read. With `stop`
	// reads only the params before that one (only without a lua_State).
	void object(const Constructor &constructor, int depth, int stop = -1);

	[[nodiscard]] const mtpPrime *position() const {
		return _from;
	}

private:
	[[nodiscard]] mtpPrime prime();
	void bytes();
	void raw(int primes);
	void vector(int element, int depth);

	const Schema &_schema;
	lua_State *_lua = nullptr;
	const mtpPrime *_from = nullptr;
	const mtpPrime *_end = nullptr;

};

// Writes a Lua value as TL data.
class Writer final {
public:
	Writer(const Schema &schema, lua_State *L, mtpBuffer &out)
	: _schema(schema)
	, _lua(L)
	, _out(out) {
	}

	void value(int type, int index, int depth);

private:
	void object(const Constructor &constructor, int index, int depth);
	[[nodiscard]] const Constructor &named(
		int index,
		int boxed,
		bool function);
	[[nodiscard]] lua_Integer integer(
		int index,
		lua_Integer min,
		lua_Integer max);
	void bytes(int index, int primes);
	void grow(int primes);
	void push(mtpPrime value);
	[[noreturn]] void fail(const QByteArray &text) const;

	const Schema &_schema;
	lua_State *_lua = nullptr;
	mtpBuffer &_out;
	std::vector<QByteArray> _path;

};

Schema::Schema() {
	anyType = add({ Kind::Any });
	queryType = add({ Kind::Query });
}

int Schema::add(TypeInfo info) {
	types.push_back(info);
	return int(types.size()) - 1;
}

int Schema::boxed(const QByteArray &name) {
	if (const auto i = boxedNames.constFind(name); i != boxedNames.cend()) {
		return *i;
	}
	boxedTypes.push_back({ .name = name });
	const auto result = int(boxedTypes.size()) - 1;
	boxedNames.insert(name, result);
	return result;
}

int Schema::type(const QByteArray &text, const QSet<QByteArray> &generics) {
	if (text.isEmpty()) {
		return -1;
	} else if (generics.contains(text) || text == "Object") {
		return anyType;
	} else if (text.startsWith('!') && generics.contains(text.mid(1))) {
		return queryType;
	} else if (const auto i = typeCache.constFind(text)
		; i != typeCache.cend()) {
		return *i;
	}
	auto info = TypeInfo();
	if (text == "#") {
		info.kind = Kind::Flags;
	} else if (text == "int") {
		info.kind = Kind::Int;
	} else if (text == "long") {
		info.kind = Kind::Long;
	} else if (text == "double") {
		info.kind = Kind::Double;
	} else if (text == "string") {
		info.kind = Kind::String;
	} else if (text == "bytes") {
		info.kind = Kind::Bytes;
	} else if (text == "int128") {
		info.kind = Kind::Int128;
	} else if (text == "int256") {
		info.kind = Kind::Int256;
	} else if (text == "Bool") {
		info.kind = Kind::Bool;
	} else if (text == "true") {
		info.kind = Kind::True;
	} else if ((text.startsWith("Vector<") || text.startsWith("vector<"))
		&& text.endsWith('>')) {
		info.kind = (text[0] == 'V') ? Kind::Vector : Kind::BareVector;
		info.ref = type(text.mid(7, text.size() - 8), generics);
		if (info.ref < 0) {
			return -1;
		}
	} else {
		const auto first = LastPartStart(text);
		if (IsUpper(first)) {
			info.kind = Kind::Boxed;
			info.ref = boxed(text);
		} else if (IsLower(first)) {
			info.kind = Kind::Bare;
		} else {
			return -1;
		}
	}
	const auto result = add(info);
	if (info.kind == Kind::Bare) {
		_bare.emplace_back(result, text);
	}
	if (generics.isEmpty()) {
		typeCache.insert(text, result);
	}
	return result;
}

void Schema::parse(const QByteArray &text) {
	auto function = false;
	for (auto line : text.split('\n')) {
		if (const auto comment = line.indexOf("//"); comment >= 0) {
			line = line.left(comment);
		}
		line = line.trimmed();
		if (line.isEmpty()) {
			continue;
		} else if (line == "---functions---") {
			function = true;
		} else if (line == "---types---") {
			function = false;
		} else {
			parseLine(line, function);
		}
	}
}

// name#1234abcd {X:Type} a:int b:flags.0?string c:Vector<long> = Result;
// Lines the codec can't use (builtins without an id, vector itself,
// %bare and [ repeat ] syntax) are skipped.
void Schema::parseLine(const QByteArray &line, bool function) {
	const auto equals = line.indexOf(" = ");
	if (equals < 0 || !line.endsWith(';')) {
		return;
	}
	const auto right = line.mid(
		equals + 3,
		line.size() - equals - 4).trimmed();
	auto parts = line.left(equals).simplified().split(' ');
	const auto &head = parts.front();
	const auto hash = head.indexOf('#');
	if (hash <= 0) {
		return;
	}
	auto ok = false;
	const auto id = mtpTypeId(head.mid(hash + 1).toUInt(&ok, 16));
	if (!ok || ids.contains(id)) {
		return;
	}
	auto result = Constructor{
		.name = head.left(hash),
		.id = id,
		.function = function,
	};
	auto generics = QSet<QByteArray>();
	auto slots = 0;
	for (auto i = 1; i < parts.size(); ++i) {
		const auto &part = parts[i];
		if (part.startsWith('{')) {
			const auto colon = part.indexOf(':');
			if (colon < 0 || !part.endsWith('}')) {
				return;
			}
			generics.insert(part.mid(1, colon - 1));
			continue;
		}
		const auto colon = part.indexOf(':');
		if (colon <= 0 || part.contains('%') || part.contains('[')) {
			return;
		}
		auto param = Param{ .name = part.left(colon) };
		auto text = part.mid(colon + 1);
		if (const auto question = text.indexOf('?'); question >= 0) {
			const auto condition = text.left(question); // flags.3
			text = text.mid(question + 1);
			const auto dot = condition.indexOf('.');
			if (dot <= 0) {
				return;
			}
			const auto flagsName = condition.left(dot);
			const auto bit = condition.mid(dot + 1).toInt(&ok);
			if (!ok || bit < 0 || bit > 31) {
				return;
			}
			for (const auto &already : result.params) {
				if (already.name == flagsName && already.slot >= 0) {
					param.flags = already.slot;
				}
			}
			if (param.flags < 0) {
				return;
			}
			param.bit = bit;
		}
		param.type = type(text, generics);
		if (param.type < 0) {
			return;
		}
		const auto kind = types[param.type].kind;
		if (kind == Kind::True && param.flags < 0) {
			return; // "true" carries no data, only as a flag
		} else if (kind == Kind::Query) {
			result.query = int(result.params.size());
		} else if (kind == Kind::Flags) {
			if (slots == kMaxFlags) {
				return;
			}
			param.slot = slots++;
		}
		result.params.push_back(std::move(param));
	}
	if (function) {
		result.result = type(right, generics);
		if (result.result < 0) {
			return;
		}
	} else {
		if (right.isEmpty()
			|| right.contains(' ')
			|| !IsUpper(LastPartStart(right))) {
			return;
		}
		result.boxed = boxed(right);
	}
	const auto index = int(constructors.size());
	if (!function) {
		boxedTypes[result.boxed].constructors.push_back(index);
	}
	ids.insert(id, index);
	if (!names.contains(result.name)) {
		names.insert(result.name, index);
	}
	constructors.push_back(std::move(result));
}

void Schema::finish() {
	for (const auto &[index, name] : base::take(_bare)) {
		const auto i = names.constFind(name);
		if (i != names.cend() && !constructors[*i].function) {
			types[index].ref = *i;
		}
	}
}

const Constructor *Schema::byId(mtpTypeId id) const {
	const auto i = ids.constFind(id);
	return (i != ids.cend()) ? &constructors[*i] : nullptr;
}

const Constructor *Schema::byName(const QByteArray &name) const {
	const auto i = names.constFind(name);
	return (i != names.cend()) ? &constructors[*i] : nullptr;
}

const Constructor *Schema::methodOf(
		const mtpPrime *from,
		const mtpPrime *end,
		int depth) const {
	if (from >= end || depth > kMaxWrapping) {
		return nullptr;
	}
	const auto constructor = byId(mtpTypeId(*from));
	if (!constructor || !constructor->function) {
		return nullptr;
	} else if (constructor->query < 0) {
		return constructor;
	}
	auto reader = Reader(*this, nullptr, from + 1, end);
	reader.object(*constructor, 0, constructor->query);
	return methodOf(reader.position(), end, depth + 1);
}

mtpPrime Reader::prime() {
	if (_from >= _end) {
		Throw("unexpected end of data");
	}
	return *_from++;
}

void Reader::bytes() {
	if (_from >= _end) {
		Throw("unexpected end of data");
	}
	const auto data = reinterpret_cast<const uchar*>(_from);
	const auto available = size_t(_end - _from) * sizeof(mtpPrime);
	auto size = size_t(data[0]);
	auto header = size_t(1);
	if (size == 254) {
		size = size_t(data[1])
			| (size_t(data[2]) << 8)
			| (size_t(data[3]) << 16);
		header = 4;
	} else if (size == 255) {
		Throw("bad string");
	}
	const auto padded = (header + size + 3) & ~size_t(3);
	if (padded > available) {
		Throw("unexpected end of data");
	}
	if (_lua) {
		lua_pushlstring(
			_lua,
			reinterpret_cast<const char*>(data + header),
			size);
	}
	_from += padded / sizeof(mtpPrime);
}

void Reader::raw(int primes) {
	if (_end - _from < primes) {
		Throw("unexpected end of data");
	}
	if (_lua) {
		lua_pushlstring(
			_lua,
			reinterpret_cast<const char*>(_from),
			size_t(primes) * sizeof(mtpPrime));
	}
	_from += primes;
}

void Reader::vector(int element, int depth) {
	const auto count = prime();
	// Every element takes at least one prime.
	if (count < 0 || count > (_end - _from)) {
		Throw("bad vector size");
	}
	if (_lua) {
		lua_createtable(_lua, int(count), 0);
	}
	for (auto i = 0; i != count; ++i) {
		value(element, depth);
		if (_lua) {
			lua_rawseti(_lua, -2, lua_Integer(i) + 1);
		}
	}
}

void Reader::value(int type, int depth) {
	if (_lua) {
		luaL_checkstack(_lua, 4, "TL");
	}
	const auto &info = _schema.types[type];
	switch (info.kind) {
	case Kind::Int: {
		const auto value = prime();
		if (_lua) {
			lua_pushinteger(_lua, lua_Integer(value));
		}
	} break;
	case Kind::Long: {
		const auto low = uint64(uint32(prime()));
		const auto high = uint64(uint32(prime()));
		if (_lua) {
			lua_pushinteger(_lua, lua_Integer(int64((high << 32) | low)));
		}
	} break;
	case Kind::Double: {
		const auto low = uint64(uint32(prime()));
		const auto high = uint64(uint32(prime()));
		const auto bits = (high << 32) | low;
		auto value = 0.;
		memcpy(&value, &bits, sizeof(value));
		if (_lua) {
			lua_pushnumber(_lua, lua_Number(value));
		}
	} break;
	case Kind::String:
	case Kind::Bytes:
		bytes();
		break;
	case Kind::Int128:
		raw(4);
		break;
	case Kind::Int256:
		raw(8);
		break;
	case Kind::Bool: {
		const auto id = mtpTypeId(prime());
		if (id != kBoolTrue && id != kBoolFalse) {
			Throw("expected Bool, got " + Hex(id));
		}
		if (_lua) {
			lua_pushboolean(_lua, (id == kBoolTrue) ? 1 : 0);
		}
	} break;
	case Kind::True:
		if (_lua) {
			lua_pushboolean(_lua, 1);
		}
		break;
	case Kind::Flags:
		Throw("flags outside of an object");
	case Kind::Vector: {
		const auto id = mtpTypeId(prime());
		if (id != kVectorId) {
			Throw("expected a vector, got " + Hex(id));
		}
		vector(info.ref, depth);
	} break;
	case Kind::BareVector:
		vector(info.ref, depth);
		break;
	case Kind::Boxed: {
		const auto id = mtpTypeId(prime());
		const auto constructor = _schema.byId(id);
		if (!constructor
			|| constructor->function
			|| constructor->boxed != info.ref) {
			Throw("unexpected constructor "
				+ Hex(id)
				+ " for "
				+ _schema.boxedTypes[info.ref].name);
		}
		object(*constructor, depth);
	} break;
	case Kind::Bare:
		if (info.ref < 0) {
			Throw("unknown bare type");
		}
		object(_schema.constructors[info.ref], depth);
		break;
	case Kind::Any: {
		const auto id = mtpTypeId(prime());
		if (id == kBoolTrue || id == kBoolFalse) {
			if (_lua) {
				lua_pushboolean(_lua, (id == kBoolTrue) ? 1 : 0);
			}
		} else if (id == kVectorId) {
			vector(_schema.anyType, depth);
		} else {
			const auto constructor = _schema.byId(id);
			if (!constructor || constructor->function) {
				Throw("unknown constructor " + Hex(id));
			}
			object(*constructor, depth);
		}
	} break;
	case Kind::Query: {
		const auto id = mtpTypeId(prime());
		const auto constructor = _schema.byId(id);
		if (!constructor || !constructor->function) {
			Throw("unknown method " + Hex(id));
		}
		object(*constructor, depth);
	} break;
	}
}

void Reader::object(const Constructor &constructor, int depth, int stop) {
	if (depth > kMaxDepth) {
		Throw("objects are nested too deep");
	}
	if (_lua) {
		luaL_checkstack(_lua, 4, "TL");
		lua_createtable(_lua, 0, int(constructor.params.size()) + 1);
		lua_pushlstring(
			_lua,
			constructor.name.constData(),
			size_t(constructor.name.size()));
		lua_setfield(_lua, -2, "_");
	}
	auto flags = std::array<uint32, kMaxFlags>();
	const auto count = int(constructor.params.size());
	for (auto i = 0; i != count; ++i) {
		if (i == stop) {
			return;
		}
		const auto &param = constructor.params[i];
		if (param.slot >= 0) {
			flags[param.slot] = uint32(prime());
			continue;
		} else if (param.flags >= 0
			&& !(flags[param.flags] & (1U << param.bit))) {
			continue;
		}
		value(param.type, depth + 1);
		if (_lua) {
			lua_setfield(_lua, -2, param.name.constData());
		}
	}
}

void Writer::fail(const QByteArray &text) const {
	auto path = QByteArray();
	for (const auto &part : _path) {
		if (!path.isEmpty() && !part.startsWith('[')) {
			path += '.';
		}
		path += part;
	}
	Throw(path.isEmpty() ? text : (path + ": " + text));
}

void Writer::grow(int primes) {
	if (_out.size() + primes > kMaxWrite) {
		fail("the object is too large");
	}
	_out.resize(_out.size() + primes);
}

void Writer::push(mtpPrime value) {
	grow(1);
	_out.back() = value;
}

lua_Integer Writer::integer(int index, lua_Integer min, lua_Integer max) {
	auto isInteger = 0;
	const auto result = lua_tointegerx(_lua, index, &isInteger);
	if (lua_type(_lua, index) != LUA_TNUMBER || !isInteger) {
		fail("expected an integer, got "
			+ QByteArray(luaL_typename(_lua, index)));
	} else if (result < min || result > max) {
		fail("integer out of range");
	}
	return result;
}

void Writer::bytes(int index, int primes) {
	if (lua_type(_lua, index) != LUA_TSTRING) {
		fail("expected a string, got "
			+ QByteArray(luaL_typename(_lua, index)));
	}
	auto size = size_t(0);
	const auto data = lua_tolstring(_lua, index, &size);
	const auto start = _out.size();
	if (primes > 0) { // int128 / int256
		if (size != size_t(primes) * sizeof(mtpPrime)) {
			fail("expected "
				+ QByteArray::number(primes * int(sizeof(mtpPrime)))
				+ " bytes");
		}
		grow(primes);
		memcpy(_out.data() + start, data, size);
		return;
	} else if (size > kMaxString) {
		fail("the string is too long");
	}
	const auto header = (size < 254) ? size_t(1) : size_t(4);
	const auto total = header + size;
	const auto padded = (total + 3) & ~size_t(3);
	grow(int(padded / sizeof(mtpPrime)));
	const auto bytes = reinterpret_cast<uchar*>(_out.data() + start);
	if (header == 1) {
		bytes[0] = uchar(size);
	} else {
		bytes[0] = 254;
		bytes[1] = uchar(size & 0xFF);
		bytes[2] = uchar((size >> 8) & 0xFF);
		bytes[3] = uchar((size >> 16) & 0xFF);
	}
	memcpy(bytes + header, data, size);
	memset(bytes + total, 0, padded - total);
}

const Constructor &Writer::named(int index, int boxed, bool function) {
	if (lua_type(_lua, index) != LUA_TTABLE) {
		fail("expected a table, got "
			+ QByteArray(luaL_typename(_lua, index)));
	}
	lua_pushliteral(_lua, "_");
	lua_rawget(_lua, index);
	auto name = QByteArray();
	auto constructor = (const Constructor*)nullptr;
	if (lua_type(_lua, -1) == LUA_TSTRING) {
		auto size = size_t(0);
		const auto data = lua_tolstring(_lua, -1, &size);
		name = QByteArray(data, int(size));
		constructor = _schema.byName(name);
	} else if (lua_isnil(_lua, -1)
		&& boxed >= 0
		&& _schema.boxedTypes[boxed].constructors.size() == 1) {
		// A type with one constructor: "_" may be left out.
		constructor = &_schema.constructors[
			_schema.boxedTypes[boxed].constructors.front()];
	}
	lua_pop(_lua, 1);
	if (!constructor) {
		fail(name.isEmpty()
			? QByteArray("the constructor name in \"_\" is missing")
			: ("unknown constructor " + name));
	} else if (constructor->function != function) {
		fail(constructor->name
			+ (function ? " is not a method" : " is a method"));
	} else if (boxed >= 0 && constructor->boxed != boxed) {
		fail(constructor->name
			+ " is not a "
			+ _schema.boxedTypes[boxed].name);
	}
	return *constructor;
}

void Writer::value(int type, int index, int depth) {
	if (depth > kMaxDepth) {
		fail("objects are nested too deep");
	}
	luaL_checkstack(_lua, 4, "TL");
	index = lua_absindex(_lua, index);
	const auto &info = _schema.types[type];
	switch (info.kind) {
	case Kind::Int:
		push(mtpPrime(uint32(integer(index, -2147483648LL, 4294967295LL))));
		break;
	case Kind::Long: {
		const auto value = uint64(
			integer(index, LUA_MININTEGER, LUA_MAXINTEGER));
		push(mtpPrime(uint32(value & 0xFFFFFFFFULL)));
		push(mtpPrime(uint32(value >> 32)));
	} break;
	case Kind::Double: {
		if (lua_type(_lua, index) != LUA_TNUMBER) {
			fail("expected a number, got "
				+ QByteArray(luaL_typename(_lua, index)));
		}
		const auto value = double(lua_tonumber(_lua, index));
		auto bits = uint64();
		memcpy(&bits, &value, sizeof(bits));
		push(mtpPrime(uint32(bits & 0xFFFFFFFFULL)));
		push(mtpPrime(uint32(bits >> 32)));
	} break;
	case Kind::String:
	case Kind::Bytes:
		bytes(index, 0);
		break;
	case Kind::Int128:
		bytes(index, 4);
		break;
	case Kind::Int256:
		bytes(index, 8);
		break;
	case Kind::Bool:
		if (lua_type(_lua, index) != LUA_TBOOLEAN) {
			fail("expected a boolean, got "
				+ QByteArray(luaL_typename(_lua, index)));
		}
		push(mtpPrime(lua_toboolean(_lua, index) ? kBoolTrue : kBoolFalse));
		break;
	case Kind::True:
		break; // the flag bit is the whole value
	case Kind::Flags:
		fail("flags are computed from the fields");
	case Kind::Vector:
	case Kind::BareVector: {
		if (lua_type(_lua, index) != LUA_TTABLE) {
			fail("expected an array, got "
				+ QByteArray(luaL_typename(_lua, index)));
		}
		const auto count = lua_rawlen(_lua, index);
		if (count > size_t(kMaxWrite)) {
			fail("the array is too long");
		}
		if (info.kind == Kind::Vector) {
			push(mtpPrime(kVectorId));
		}
		push(mtpPrime(int32(count)));
		for (auto i = lua_Integer(1); i <= lua_Integer(count); ++i) {
			_path.push_back('[' + QByteArray::number(i) + ']');
			lua_rawgeti(_lua, index, i);
			value(info.ref, -1, depth);
			lua_pop(_lua, 1);
			_path.pop_back();
		}
	} break;
	case Kind::Boxed: {
		const auto &constructor = named(index, info.ref, false);
		push(mtpPrime(constructor.id));
		object(constructor, index, depth);
	} break;
	case Kind::Bare:
		if (info.ref < 0) {
			fail("unknown bare type");
		} else if (lua_type(_lua, index) != LUA_TTABLE) {
			fail("expected a table, got "
				+ QByteArray(luaL_typename(_lua, index)));
		}
		object(_schema.constructors[info.ref], index, depth);
		break;
	case Kind::Any: {
		auto vector = false;
		if (lua_type(_lua, index) == LUA_TTABLE) {
			lua_pushliteral(_lua, "_");
			vector = (lua_rawget(_lua, index) == LUA_TNIL);
			lua_pop(_lua, 1);
		}
		if (lua_type(_lua, index) == LUA_TBOOLEAN) {
			push(mtpPrime(lua_toboolean(_lua, index)
				? kBoolTrue
				: kBoolFalse));
		} else if (vector) {
			// An array read as Object: a vector of objects, as the Reader
			// gives it.
			const auto count = lua_rawlen(_lua, index);
			if (count > size_t(kMaxWrite)) {
				fail("the array is too long");
			}
			push(mtpPrime(kVectorId));
			push(mtpPrime(int32(count)));
			for (auto i = lua_Integer(1); i <= lua_Integer(count); ++i) {
				_path.push_back('[' + QByteArray::number(i) + ']');
				lua_rawgeti(_lua, index, i);
				value(_schema.anyType, -1, depth + 1);
				lua_pop(_lua, 1);
				_path.pop_back();
			}
		} else {
			const auto &constructor = named(index, -1, false);
			push(mtpPrime(constructor.id));
			object(constructor, index, depth);
		}
	} break;
	case Kind::Query: {
		const auto &constructor = named(index, -1, true);
		push(mtpPrime(constructor.id));
		object(constructor, index, depth);
	} break;
	}
}

void Writer::object(const Constructor &constructor, int index, int depth) {
	if (depth > kMaxDepth) {
		fail("objects are nested too deep");
	}
	luaL_checkstack(_lua, 4, "TL");
	const auto known = [&](const QByteArray &key) {
		if (key == "_") {
			return true;
		}
		for (const auto &param : constructor.params) {
			if (param.name == key) {
				return true;
			}
		}
		return false;
	};
	// A misspelled field would be dropped silently: refuse it instead.
	lua_pushnil(_lua);
	while (lua_next(_lua, index)) {
		lua_pop(_lua, 1);
		auto size = size_t(0);
		const auto data = (lua_type(_lua, -1) == LUA_TSTRING)
			? lua_tolstring(_lua, -1, &size)
			: nullptr;
		if (!data) {
			fail(constructor.name + " has only named fields");
		}
		const auto key = QByteArray(data, int(size));
		if (!known(key)) {
			fail(constructor.name + " has no field " + key);
		}
	}

	const auto field = [&](const Param &param) {
		lua_pushlstring(
			_lua,
			param.name.constData(),
			size_t(param.name.size()));
		lua_rawget(_lua, index);
	};
	auto flags = std::array<uint32, kMaxFlags>();
	for (const auto &param : constructor.params) {
		if (param.flags < 0) {
			continue;
		}
		field(param);
		const auto present = (_schema.types[param.type].kind == Kind::True)
			? (lua_toboolean(_lua, -1) != 0)
			: !lua_isnil(_lua, -1);
		lua_pop(_lua, 1);
		if (present) {
			flags[param.flags] |= (1U << param.bit);
		}
	}
	const auto count = int(constructor.params.size());
	for (auto i = 0; i != count; ++i) {
		const auto &param = constructor.params[i];
		const auto kind = _schema.types[param.type].kind;
		if (param.slot >= 0) {
			push(mtpPrime(flags[param.slot]));
			continue;
		} else if (param.flags >= 0
			&& (kind == Kind::True
				|| !(flags[param.flags] & (1U << param.bit)))) {
			continue;
		}
		_path.push_back(param.name);
		field(param);
		if (lua_isnil(_lua, -1)) {
			fail("missing field");
		}
		value(param.type, -1, depth + 1);
		lua_pop(_lua, 1);
		_path.pop_back();
	}
}

[[nodiscard]] const Schema *Loaded() {
	static const auto result = []() -> std::unique_ptr<Schema> {
		auto file = QFile(u":/plugins/api.tl"_q);
		if (!file.open(QIODevice::ReadOnly)) {
			LOG(("Plugins Error: could not open the TL schema."));
			return nullptr;
		}
		auto schema = std::make_unique<Schema>();
		schema->parse(file.readAll());
		schema->parse(QByteArray(kExtraSchema));
		schema->finish();
		LOG(("Plugins: TL schema, %1 constructors and methods."
			).arg(schema->constructors.size()));
		return schema;
	}();
	return result.get();
}

} // namespace

bool Available() {
	return Loaded() != nullptr;
}

Type Query() {
	const auto schema = Loaded();
	return schema ? Type{ schema->queryType } : Type();
}

Type Object() {
	const auto schema = Loaded();
	return schema ? Type{ schema->anyType } : Type();
}

Type Boxed(const char *name) {
	const auto schema = Loaded();
	if (!schema) {
		return {};
	}
	const auto i = schema->typeCache.constFind(QByteArray(name));
	if (i == schema->typeCache.cend()
		|| schema->types[*i].kind != Kind::Boxed) {
		return {};
	}
	return { *i };
}

mtpTypeId MethodOf(const mtpBuffer &call) {
	const auto schema = Loaded();
	if (!schema) {
		return 0;
	}
	try {
		const auto from = call.constData();
		if (const auto method = schema->methodOf(from, from + call.size(), 0)) {
			return method->id;
		}
	} catch (const Error &) {
	}
	return 0;
}

Type ResultOf(mtpTypeId method) {
	const auto schema = Loaded();
	if (!schema) {
		return {};
	}
	const auto constructor = schema->byId(method);
	return (constructor && constructor->function)
		? Type{ constructor->result }
		: Type{ schema->anyType };
}

mtpTypeId Id(const QByteArray &name) {
	const auto schema = Loaded();
	const auto constructor = schema ? schema->byName(name) : nullptr;
	return constructor ? constructor->id : 0;
}

QByteArray Name(mtpTypeId id) {
	const auto schema = Loaded();
	const auto constructor = schema ? schema->byId(id) : nullptr;
	return constructor ? constructor->name : QByteArray();
}

bool IsMethod(mtpTypeId id) {
	const auto schema = Loaded();
	const auto constructor = schema ? schema->byId(id) : nullptr;
	return constructor && constructor->function;
}

void Push(lua_State *L, Type type, const mtpBuffer &data) {
	const auto schema = Loaded();
	if (!schema || !type.valid()) {
		luaL_error(L, "the TL schema is not available");
		return;
	}
	auto error = QByteArray();
	try {
		const auto from = data.constData();
		auto reader = Reader(*schema, L, from, from + data.size());
		reader.value(type.index, 0);
		return;
	} catch (const Error &e) {
		error = e.text;
	}
	luaL_error(L, "TL: %s", error.constData());
}

mtpBuffer Read(lua_State *L, int index, Type type) {
	const auto schema = Loaded();
	if (!schema || !type.valid()) {
		luaL_error(L, "the TL schema is not available");
		return {};
	}
	index = lua_absindex(L, index);
	auto result = mtpBuffer();
	auto error = QByteArray();
	try {
		auto writer = Writer(*schema, L, result);
		writer.value(type.index, index, 0);
		return result;
	} catch (const Error &e) {
		error = e.text;
	}
	luaL_error(L, "TL: %s", error.constData());
	return {};
}

} // namespace Plugins::Flua::Tl
