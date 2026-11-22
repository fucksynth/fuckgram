/*
This file is part of fuckgram, a Telegram Desktop client based on AyuGram.

For license and copyright information please follow this link:
https://github.com/fucksynth/fuckgram/blob/main/LICENSE
*/
#include "plugins/runtime/flua_analyzer.h"

#include "plugins/runtime/flua_package.h"

#include <deque>
#include <set>
#include <string>

// Lua internals (Proto, opcodes): Lua is built as C++, see lib_lua.cmake.
#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lopcodes.h"
#include "ldebug.h"

namespace Plugins::Flua {
namespace {

constexpr auto kMaxSteps = 2'000'000;

enum class Kind : unsigned char {
	Unknown, // a value unrelated to the API
	String,  // a constant string (for event names and require)
	Api,     // an API object, `path` says which
	Mixed,   // maybe an API object, depends on the control flow
	Env,     // the global environment (_ENV)
};

struct Value {
	Kind kind = Kind::Unknown;
	std::string text; // String: the string; Api: the path

	friend inline bool operator==(const Value&, const Value&) = default;
};

[[nodiscard]] Value Join(const Value &a, const Value &b) {
	if (a == b) {
		return a;
	}
	const auto api = [](const Value &value) {
		return (value.kind == Kind::Api)
			|| (value.kind == Kind::Mixed)
			|| (value.kind == Kind::Env);
	};
	return (api(a) || api(b)) ? Value{ Kind::Mixed } : Value();
}

enum class ApiType {
	Table,
	Function,
	Field,
};

struct ApiEntry {
	const char *path;
	ApiType type;
	const char *permission; // needed to touch it, nullptr for none
};

const ApiEntry kApi[] = {
	{ "tg", ApiType::Table, nullptr },
	{ "tg.messages", ApiType::Table, nullptr },
	{ "tg.compose", ApiType::Table, nullptr },
	{ "tg.ui", ApiType::Table, nullptr },
	{ "plugin", ApiType::Table, nullptr },
	{ "plugin.settings", ApiType::Table, nullptr },
	{ "tg.log", ApiType::Function, nullptr },
	{ "tg.toast", ApiType::Function, kPermUi },
	{ "tg.messages.send", ApiType::Function, kPermMessagesSend },
	{ "tg.compose.get", ApiType::Function, kPermCompose },
	{ "tg.compose.set", ApiType::Function, kPermCompose },
	{ "tg.ui.refresh", ApiType::Function, kPermDisplay },
	{ "plugin.on", ApiType::Function, nullptr },
	{ "plugin.menu", ApiType::Function, kPermUi },
	{ "plugin.settings.get", ApiType::Function, kPermStorage },
	{ "plugin.settings.set", ApiType::Function, kPermStorage },
	{ "plugin.id", ApiType::Field, nullptr },
	{ "plugin.name", ApiType::Field, nullptr },
	{ "plugin.version", ApiType::Field, nullptr },
	{ "require", ApiType::Function, nullptr },
};

struct EventEntry {
	const char *name;
	const char *permission;
};

const EventEntry kEvents[] = {
	{ "message.new", kPermMessagesRead },
	{ "message.send", kPermMessagesSend },
	{ "app.state", nullptr },
	{ "url.open", kPermLinks },
	{ "action", kPermActions },
	{ "display", kPermDisplay },
	{ "screen", kPermChatsRead },
	{ "profile.rows", kPermUi },
};

const char *kMenuPlaces[] = { "message", "main", "chat", "profile" };

// Not available in the sandbox, so using them only hides intent.
const char *kForbidden[] = {
	"load",
	"loadfile",
	"loadstring",
	"dofile",
	"rawget",
	"rawset",
	"collectgarbage",
	"debug",
	"package",
	"io",
	"module",
	"setfenv",
	"getfenv",
};

[[nodiscard]] const ApiEntry *FindApi(const std::string &path) {
	for (const auto &entry : kApi) {
		if (path == entry.path) {
			return &entry;
		}
	}
	return nullptr;
}

[[nodiscard]] bool Special(const std::string &path) {
	// Calls that the analysis must see directly to read their arguments.
	return (path == "plugin.on")
		|| (path == "plugin.menu")
		|| (path == "require");
}

[[nodiscard]] std::optional<std::string> ConstString(
		const Proto *p,
		int index) {
	if (index < 0 || index >= p->sizek) {
		return std::nullopt;
	}
	const TValue *value = &p->k[index];
	if (!ttisstring(value)) {
		return std::nullopt;
	}
	const TString *string = tsvalue(value);
	return std::string(getstr(string), tsslen(string));
}

struct State {
	std::vector<Value> registers;
	int multiBase = -1; // first register of an open (variable) result list

	friend inline bool operator==(const State&, const State&) = default;
};

[[nodiscard]] State Join(const State &a, const State &b) {
	auto result = a;
	for (auto i = 0; i != int(result.registers.size()); ++i) {
		result.registers[i] = Join(a.registers[i], b.registers[i]);
	}
	result.multiBase = (a.multiBase == b.multiBase) ? a.multiBase : -1;
	return result;
}

class Analyzer final {
public:
	explicit Analyzer(QString file) : _file(std::move(file)) {
	}

	void analyze(const Proto *p, std::vector<Value> upvalues);

	[[nodiscard]] const QString &error() const {
		return _error;
	}
	[[nodiscard]] const std::set<std::string> &permissions() const {
		return _permissions;
	}

private:
	void fail(const Proto *p, int pc, const QString &what);
	void use(const char *permission);
	[[nodiscard]] Value global(
		const std::optional<std::string> &name,
		const Proto *p,
		int pc);
	[[nodiscard]] Value child(
		const Value &base,
		const std::optional<std::string> &key,
		const Proto *p,
		int pc);
	void escape(const Value &value, const Proto *p, int pc);
	void call(
		const Value &callee,
		const State &state,
		int a,
		int last,
		const Proto *p,
		int pc);

	const QString _file;
	QString _error;
	std::set<std::string> _permissions;
	int _steps = 0;

};

void Analyzer::fail(const Proto *p, int pc, const QString &what) {
	if (!_error.isEmpty()) {
		return;
	}
	const auto line = (p && pc >= 0) ? luaG_getfuncline(p, pc) : -1;
	_error = _file
		+ (line > 0 ? (u':' + QString::number(line)) : QString())
		+ u": "_q
		+ what;
}

void Analyzer::use(const char *permission) {
	if (permission) {
		_permissions.emplace(permission);
	}
}

Value Analyzer::global(
		const std::optional<std::string> &name,
		const Proto *p,
		int pc) {
	if (!name) {
		return {};
	}
	if (*name == "tg" || *name == "plugin" || *name == "require") {
		return { Kind::Api, *name };
	} else if (*name == "_G" || *name == "_ENV") {
		return { Kind::Api, "_G" };
	}
	for (const auto forbidden : kForbidden) {
		if (*name == forbidden) {
			fail(p, pc, u"'"_q
				+ QString::fromStdString(*name)
				+ u"' is not available to plugins"_q);
			return {};
		}
	}
	return {};
}

Value Analyzer::child(
		const Value &base,
		const std::optional<std::string> &key,
		const Proto *p,
		int pc) {
	switch (base.kind) {
	case Kind::Env:
		return global(key, p, pc);
	case Kind::Mixed:
		fail(p, pc, u"can't tell which object is indexed here"_q);
		return {};
	case Kind::Api:
		break;
	default:
		return {};
	}
	if (!key) {
		fail(p, pc, u"API objects can be indexed only by a constant name"_q);
		return {};
	} else if (base.text == "_G") {
		return global(key, p, pc);
	}
	const auto path = base.text + '.' + *key;
	const auto entry = FindApi(path);
	if (!entry) {
		fail(p, pc, u"unknown API: "_q + QString::fromStdString(path));
		return {};
	}
	use(entry->permission);
	return { Kind::Api, path };
}

void Analyzer::escape(const Value &value, const Proto *p, int pc) {
	switch (value.kind) {
	case Kind::Env:
	case Kind::Mixed:
		fail(p, pc, u"an API object leaves the reach of the analysis"_q);
		return;
	case Kind::Api: {
		const auto entry = FindApi(value.text);
		if (value.text == "_G"
			|| (entry && entry->type == ApiType::Table)
			|| Special(value.text)) {
			fail(p, pc, u"'"_q
				+ QString::fromStdString(value.text)
				+ u"' can't be stored, passed or returned"_q);
		}
	} return;
	default:
		return;
	}
}

void Analyzer::call(
		const Value &callee,
		const State &state,
		int a,
		int last,
		const Proto *p,
		int pc) {
	const auto &r = state.registers;
	const auto arg = [&](int index) -> Value {
		const auto at = a + index;
		return (at <= last && at < int(r.size())) ? r[at] : Value();
	};
	auto first = 1;
	if (callee.kind == Kind::Mixed) {
		fail(p, pc, u"can't tell which function is called here"_q);
		return;
	} else if (callee.kind == Kind::Api && callee.text == "plugin.on") {
		const auto name = arg(1);
		if (name.kind != Kind::String) {
			fail(p, pc, u"plugin.on needs a constant event name"_q);
			return;
		}
		auto known = false;
		for (const auto &event : kEvents) {
			if (name.text == event.name) {
				use(event.permission);
				known = true;
			}
		}
		if (!known) {
			fail(p, pc, u"unknown event: "_q
				+ QString::fromStdString(name.text));
			return;
		}
		first = 2;
	} else if (callee.kind == Kind::Api && callee.text == "plugin.menu") {
		const auto place = arg(1);
		auto known = false;
		for (const auto name : kMenuPlaces) {
			known |= (place.kind == Kind::String && place.text == name);
		}
		if (!known) {
			fail(p, pc, u"plugin.menu needs a constant place: "
				"message, main, chat or profile"_q);
			return;
		}
		first = 2;
	} else if (callee.kind == Kind::Api && callee.text == "require") {
		if (arg(1).kind != Kind::String) {
			fail(p, pc, u"require needs a constant module name"_q);
			return;
		}
		first = 2;
	}
	for (auto i = a + first; i <= last && i < int(r.size()); ++i) {
		escape(r[i], p, pc);
	}
}

void Analyzer::analyze(const Proto *p, std::vector<Value> upvalues) {
	const auto size = p->sizecode;
	if (size <= 0) {
		return;
	}
	const auto count = std::max(int(p->maxstacksize), 1);
	const auto up = [&](int index) -> Value {
		return (index >= 0 && index < int(upvalues.size()))
			? upvalues[index]
			: Value();
	};

	auto in = std::vector<std::optional<State>>(size);
	in[0] = State{ .registers = std::vector<Value>(count) };
	auto work = std::deque<int>{ 0 };
	auto closures = std::map<int, std::vector<Value>>();

	const auto flow = [&](int target, const State &state) {
		if (target < 0 || target >= size) {
			return;
		}
		auto &slot = in[target];
		if (!slot) {
			slot = state;
		} else {
			auto joined = Join(*slot, state);
			if (joined == *slot) {
				return;
			}
			slot = std::move(joined);
		}
		work.push_back(target);
	};

	while (!work.empty() && _error.isEmpty()) {
		if (++_steps > kMaxSteps) {
			fail(p, 0, u"the code is too complex to check"_q);
			return;
		}
		const auto pc = work.front();
		work.pop_front();
		auto state = *in[pc];
		auto &r = state.registers;
		const auto reg = [&](int index) -> Value {
			return (index >= 0 && index < count) ? r[index] : Value();
		};
		const auto set = [&](int index, Value value) {
			if (index >= 0 && index < count) {
				r[index] = std::move(value);
			}
		};
		const auto clear = [&](int from, int to) {
			for (auto i = std::max(from, 0); i <= to && i < count; ++i) {
				r[i] = Value();
			}
		};
		const auto api = [](const Value &value) {
			return (value.kind == Kind::Api)
				|| (value.kind == Kind::Mixed)
				|| (value.kind == Kind::Env);
		};

		const auto i = p->code[pc];
		const auto op = GET_OPCODE(i);
		const auto a = GETARG_A(i);
		auto multiBase = -1;
		auto successors = std::vector<int>{ pc + 1 };

		switch (op) {
		case OP_MOVE:
			set(a, reg(GETARG_B(i)));
			break;
		case OP_LOADK: {
			const auto string = ConstString(p, GETARG_Bx(i));
			set(a, string ? Value{ Kind::String, *string } : Value());
		} break;
		case OP_LOADKX: {
			const auto string = (pc + 1 < size)
				? ConstString(p, GETARG_Ax(p->code[pc + 1]))
				: std::nullopt;
			set(a, string ? Value{ Kind::String, *string } : Value());
			successors = { pc + 2 };
		} break;
		case OP_GETUPVAL: {
			const auto value = up(GETARG_B(i));
			set(a, (value.kind == Kind::Env)
				? Value{ Kind::Api, "_G" }
				: value);
		} break;
		case OP_GETTABUP:
			set(a, child(
				up(GETARG_B(i)),
				ConstString(p, GETARG_C(i)),
				p,
				pc));
			break;
		case OP_GETFIELD:
			set(a, child(
				reg(GETARG_B(i)),
				ConstString(p, GETARG_C(i)),
				p,
				pc));
			break;
		case OP_GETTABLE:
		case OP_GETI:
			if (api(reg(GETARG_B(i)))) {
				fail(p, pc, u"API objects can be indexed only by a "
					"constant name"_q);
			}
			set(a, Value());
			break;
		case OP_SELF: {
			const auto base = reg(GETARG_B(i));
			auto method = Value();
			if (GETARG_k(i)) {
				method = child(base, ConstString(p, GETARG_C(i)), p, pc);
			} else if (api(base)) {
				fail(p, pc, u"API objects can be indexed only by a "
					"constant name"_q);
			}
			set(a + 1, base);
			set(a, method);
		} break;
		case OP_SETTABUP: {
			const auto target = up(a);
			const auto key = ConstString(p, GETARG_B(i));
			if (target.kind == Kind::Env
				&& key
				&& (*key == "tg"
					|| *key == "plugin"
					|| *key == "require"
					|| *key == "_G"
					|| *key == "_ENV"
					|| *key == "print")) {
				fail(p, pc, u"'"_q
					+ QString::fromStdString(*key)
					+ u"' can't be replaced"_q);
			} else if (target.kind == Kind::Api
				|| target.kind == Kind::Mixed) {
				fail(p, pc, u"API objects can't be changed"_q);
			}
			if (!GETARG_k(i)) {
				escape(reg(GETARG_C(i)), p, pc);
			}
		} break;
		case OP_SETFIELD:
		case OP_SETI:
			if (api(reg(a))) {
				fail(p, pc, u"API objects can't be changed"_q);
			}
			if (!GETARG_k(i)) {
				escape(reg(GETARG_C(i)), p, pc);
			}
			break;
		case OP_SETTABLE:
			if (api(reg(a))) {
				fail(p, pc, u"API objects can't be changed"_q);
			}
			escape(reg(GETARG_B(i)), p, pc);
			if (!GETARG_k(i)) {
				escape(reg(GETARG_C(i)), p, pc);
			}
			break;
		case OP_SETUPVAL:
			escape(reg(a), p, pc);
			break;
		case OP_SETLIST: {
			const auto b = GETARG_B(i);
			const auto last = b
				? (a + b)
				: ((state.multiBase > a) ? (state.multiBase - 1) : a);
			for (auto k = a + 1; k <= last; ++k) {
				escape(reg(k), p, pc);
			}
			if (GETARG_k(i)) {
				successors = { pc + 2 };
			}
		} break;
		case OP_NEWTABLE:
			set(a, Value());
			successors = { pc + 2 };
			break;
		case OP_CALL:
		case OP_TAILCALL: {
			const auto b = GETARG_B(i);
			const auto last = b
				? (a + b - 1)
				: ((state.multiBase > a) ? (state.multiBase - 1) : a);
			call(reg(a), state, a, last, p, pc);
			clear(a, count - 1);
			if (op == OP_CALL && GETARG_C(i) == 0) {
				multiBase = a;
			}
		} break;
		case OP_VARARG: {
			const auto c = GETARG_C(i);
			clear(a, c ? (a + c - 2) : (count - 1));
			if (!c) {
				multiBase = a;
			}
		} break;
		case OP_RETURN: {
			const auto b = GETARG_B(i);
			const auto last = b
				? (a + b - 2)
				: ((state.multiBase > a) ? (state.multiBase - 1) : (a - 1));
			for (auto k = a; k <= last; ++k) {
				escape(reg(k), p, pc);
			}
			successors.clear();
		} break;
		case OP_RETURN1:
			escape(reg(a), p, pc);
			successors.clear();
			break;
		case OP_RETURN0:
			successors.clear();
			break;
		case OP_CLOSURE: {
			const auto index = GETARG_Bx(i);
			if (index >= 0 && index < p->sizep) {
				const Proto *nested = p->p[index];
				auto captured = std::vector<Value>(nested->sizeupvalues);
				for (auto u = 0; u != nested->sizeupvalues; ++u) {
					const auto &description = nested->upvalues[u];
					captured[u] = description.instack
						? reg(description.idx)
						: up(description.idx);
				}
				auto &slot = closures[index];
				if (slot.empty()) {
					slot = std::move(captured);
				} else {
					for (auto u = 0; u != int(slot.size()); ++u) {
						slot[u] = Join(slot[u], captured[u]);
					}
				}
			}
			set(a, Value());
		} break;
		case OP_TESTSET:
			set(a, reg(GETARG_B(i)));
			successors = { pc + 1, pc + 2 };
			break;
		case OP_EQ:
		case OP_LT:
		case OP_LE:
		case OP_EQK:
		case OP_EQI:
		case OP_LTI:
		case OP_LEI:
		case OP_GTI:
		case OP_GEI:
		case OP_TEST:
			successors = { pc + 1, pc + 2 };
			break;
		case OP_LFALSESKIP:
			set(a, Value());
			successors = { pc + 2 };
			break;
		case OP_LOADNIL:
			clear(a, a + GETARG_B(i));
			break;
		case OP_JMP:
			successors = { pc + 1 + GETARG_sJ(i) };
			break;
		case OP_FORPREP:
			clear(a, a + 3);
			successors = { pc + 1, pc + GETARG_Bx(i) + 2 };
			break;
		case OP_FORLOOP:
			clear(a, a + 3);
			successors = { pc + 1, pc + 1 - GETARG_Bx(i) };
			break;
		case OP_TFORPREP:
			successors = { pc + 1 + GETARG_Bx(i) };
			break;
		case OP_TFORCALL:
			// Calls R[A](R[A+1], R[A+2]): a generic for over an API table
			// (`for k, v in next, tg`) lets it be walked dynamically.
			escape(reg(a + 1), p, pc);
			escape(reg(a + 2), p, pc);
			if (reg(a).kind == Kind::Mixed) {
				fail(p, pc, u"can't tell which function is called here"_q);
			}
			clear(a + 4, a + 3 + GETARG_C(i));
			break;
		case OP_TFORLOOP:
			set(a + 2, reg(a + 4));
			successors = { pc + 1, pc + 1 - GETARG_Bx(i) };
			break;
		default:
			if (testAMode(op)) {
				set(a, Value());
			}
			break;
		}
		state.multiBase = multiBase;
		for (const auto target : successors) {
			flow(target, state);
		}
	}
	if (!_error.isEmpty()) {
		return;
	}
	for (const auto &[index, captured] : closures) {
		analyze(p->p[index], captured);
		if (!_error.isEmpty()) {
			return;
		}
	}
}

} // namespace

QString AnalyzePackage(const Package &package) {
	const auto L = luaL_newstate();
	if (!L) {
		return u"could not start the code check"_q;
	}
	const auto guard = gsl::finally([&] { lua_close(L); });

	auto used = std::set<std::string>();
	for (const auto &[path, bytes] : package.files) {
		if (!path.endsWith(u".lua"_q, Qt::CaseInsensitive)) {
			continue;
		}
		const auto chunk = QString(u"@"_q + path).toUtf8();
		if (luaL_loadbufferx(
				L,
				bytes.constData(),
				size_t(bytes.size()),
				chunk.constData(),
				"t") != LUA_OK) {
			const auto message = lua_tostring(L, -1);
			auto result = QString::fromUtf8(message ? message : "");
			lua_pop(L, 1);
			return result.isEmpty() ? (path + u": syntax error"_q) : result;
		}
		// A loaded chunk is a Lua closure whose only upvalue is _ENV.
		const auto closure = static_cast<const LClosure*>(
			lua_topointer(L, -1));
		auto analyzer = Analyzer(path);
		analyzer.analyze(closure->p, { Value{ Kind::Env } });
		lua_pop(L, 1);
		if (!analyzer.error().isEmpty()) {
			return analyzer.error();
		}
		used.insert(
			analyzer.permissions().begin(),
			analyzer.permissions().end());
	}

	const auto &declared = package.manifest.permissions;
	for (const auto &permission : used) {
		const auto name = QString::fromStdString(permission);
		if (std::find(begin(declared), end(declared), name) == end(declared)) {
			return u"the code uses '"_q
				+ name
				+ u"' but manifest.json does not declare it"_q;
		}
	}
	for (const auto &permission : declared) {
		if (!used.contains(permission.toStdString())) {
			return u"manifest.json declares '"_q
				+ permission
				+ u"' but the code never uses it"_q;
		}
	}
	return QString();
}

} // namespace Plugins::Flua
