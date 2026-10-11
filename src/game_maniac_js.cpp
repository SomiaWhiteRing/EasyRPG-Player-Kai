#include "game_maniac_js.h"
#include "game_interpreter_shared.h"
#include "game_maniac_self_variables.h"
#include "game_switches.h"
#include "game_variables.h"
#include "game_strings.h"
#include "game_pictures.h"
#include "filefinder.h"
#include "filesystem_stream.h"
#include "utils.h"
#include "main_data.h"
#include "output.h"
#include "player.h"
#include <quickjs.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>

namespace {
int32_t Offset(int32_t id, uint32_t offset) {
	const uint32_t bits = id < 0 ? static_cast<uint32_t>(id) - offset : static_cast<uint32_t>(id) + offset;
	int32_t result;
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

class Runtime {
public:
	Runtime() : runtime(JS_NewRuntime()), context(runtime ? JS_NewContext(runtime) : nullptr) {
		if (!context) Output::Error("Maniac JavaScript: Could not create the runtime");
		JS_SetContextOpaque(context, this);
		JS_SetMaxStackSize(runtime, 256 * 1024);
		JS_SetModuleLoaderFunc(runtime, nullptr, LoadModule, nullptr);
		auto global = JS_GetGlobalObject(context);
		const char* getters[] = {"getv", "gets", "gett"};
		const char* setters[] = {"setv", "sets", "sett"};
		for (int type = 0; type < 3; ++type) {
			JS_SetPropertyStr(context, global, getters[type],
				JS_NewCFunctionMagic(context, Get, getters[type], 2, JS_CFUNC_generic_magic, type));
			JS_SetPropertyStr(context, global, setters[type],
				JS_NewCFunctionMagic(context, Set, setters[type], 2, JS_CFUNC_generic_magic, type));
		}
		JS_SetPropertyStr(context, global, "include", JS_NewCFunction(context, Include, "include", 1));
		JSClassDef picture_definition = {};
		picture_definition.class_name = "Rm2k3_Picture";
		JS_NewClassID(runtime, &picture_class);
		if (JS_NewClass(runtime, picture_class, &picture_definition) < 0)
			Output::Error("Maniac JavaScript: Could not create the picture class");
		auto prototype = JS_NewObject(context);
		const auto id_key = JS_NewAtom(context, "id");
		JS_DefinePropertyGetSet(context, prototype, id_key,
			JS_NewCFunctionMagic(context, PictureProperty, "get id", 0, JS_CFUNC_generic_magic, 0),
			JS_UNDEFINED, JS_PROP_CONFIGURABLE);
		JS_FreeAtom(context, id_key);
		const auto file_key = JS_NewAtom(context, "file");
		JS_DefinePropertyGetSet(context, prototype, file_key,
			JS_NewCFunctionMagic(context, PictureProperty, "get file", 0, JS_CFUNC_generic_magic, 1),
			JS_NewCFunctionMagic(context, PictureProperty, "set file", 1, JS_CFUNC_generic_magic, 2),
			JS_PROP_CONFIGURABLE);
		JS_FreeAtom(context, file_key);
		const char* methods[] = {"scale", "show", "move", "erase"};
		for (int operation = 0; operation < 4; ++operation) {
			JS_DefinePropertyValueStr(context, prototype, methods[operation],
				JS_NewCFunctionMagic(context, PictureMethod, methods[operation], operation == 3 ? 0 : 2,
					JS_CFUNC_generic_magic, operation), JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
		}
		JS_SetClassProto(context, picture_class, prototype);
		auto rm2k3 = JS_NewObject(context);
		JS_SetPropertyStr(context, rm2k3, "getPicture", JS_NewCFunction(context, GetPicture, "getPicture", 1));
		JS_SetPropertyStr(context, global, "rm2k3", rm2k3);
		auto console = JS_NewObject(context);
		JS_SetPropertyStr(context, console, "log", JS_NewCFunction(context, ConsoleLog, "log", 1));
		JS_SetPropertyStr(context, console, "input", JS_NewCFunction(context, ConsoleInput, "input", 1));
		JS_SetPropertyStr(context, global, "console", console);
		JS_FreeValue(context, global);

		// Use the fixed patch's bootstrap; the shared parser accepts arrow method bodies.
		static constexpr char bootstrap[] =
			"const s = new Proxy({}, { get(a, b) => gets(b), set(a, b, c) => sets(b, c), });"
			"const v = new Proxy({}, { get(a, b) => getv(b), set(a, b, c) => setv(b, c), });"
			"const t = new Proxy({}, { get(a, b) => gett(b), set(a, b, c) => sett(b, c), });"
			"const selfs = new Proxy({}, { get(a, b) => gets(-b), set(a, b, c) => sets(-b, c), });"
			"const selfv = new Proxy({}, { get(a, b) => getv(-b), set(a, b, c) => setv(-b, c), });"
			"const selft = new Proxy({}, { get(a, b) => gett(-b), set(a, b, c) => sett(-b, c), });";
		auto initialized = JS_Eval(context, bootstrap, sizeof(bootstrap) - 1, "",
			JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
		if (JS_IsException(initialized)) Output::Error("Maniac JavaScript: Could not initialize variable proxies");
		JS_FreeValue(context, initialized);
	}
	~Runtime() {
		JS_FreeContext(context);
		JS_FreeRuntime(runtime);
	}

	// A proxy can wrap another proxy. Preserve native array recognition without
	// consulting a script-replaceable Array.isArray property.
	bool IsArray(JSValueConst value) {
		JSValue target = JS_DupValue(context, value);
		while (JS_IsProxy(target)) {
			auto next = JS_GetProxyTarget(context, target);
			JS_FreeValue(context, target);
			target = next;
			if (JS_IsException(target)) break;
		}
		const bool array = JS_IsArray(target);
		JS_FreeValue(context, target);
		return array;
	}

	void Execute(std::string_view source, Span<const int32_t> parameters,
		const Game_BaseInterpreterContext& binding) {
		caller = &binding;
		source = source.substr(0, source.find('\0'));
		auto result = JS_Eval(context, source.data(), source.size(), "",
			JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
		if (JS_IsException(result)) {
			auto exception = JS_GetException(context);
			const char* message = JS_ToCString(context, exception);
			Output::Warning("Maniac JavaScript: {}", message ? message : "Exception");
			if (message) JS_FreeCString(context, message);
			JS_FreeValue(context, exception);
			result = JS_UNDEFINED;
		}
		const int32_t count = parameters.size() > 4 ? parameters[4] : 0;
		const bool array = count > 0 && IsArray(result);
		for (int32_t i = 0; i < count; ++i) {
			const size_t index = 5 + static_cast<size_t>(i) * 2;
			const int type = index < parameters.size() ? parameters[index] : 0;
			int32_t id = index + 1 < parameters.size() ? parameters[index + 1] : 0;
			auto value = array ? JS_GetPropertyUint32(context, result, static_cast<uint32_t>(i))
				: JS_DupValue(context, result);
			// Native fetches the array element before validating or resolving its target.
			if (type < 0 || type > 5) {
				JS_FreeValue(context, value);
				continue;
			}
			if (type & 1) id = binding.ResolveVariableIndirectAddress(id);
			// Return target ordering is switch, number, string; host magic is number, switch, string.
			Write(type < 2 ? 1 : type < 4 ? 0 : 2, id, value);
			JS_FreeValue(context, value);
		}
		JS_FreeValue(context, result);
		caller = nullptr;
	}

private:
	bool Text(JSValueConst value, std::string& text) {
		size_t length = 0;
		const char* data = JS_ToCStringLen(context, &length, value);
		if (!data) return false;
		text.assign(data, length);
		JS_FreeCString(context, data);
		const auto nul = text.find('\0');
		if (nul != std::string::npos) text.resize(nul);
		return true;
	}
	static JSValue ConsoleLog(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		if (!Output::ManiacConsoleOpen(false)) return JS_UNDEFINED;
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		for (int i = 0; i < argc; ++i) {
			std::string text;
			if (!self.Text(argv[i], text)) return JS_EXCEPTION;
			if (i) text.insert(text.begin(), ' ');
			text += "\r\n";
			Output::ManiacConsoleWrite(text);
		}
		return JS_UNDEFINED;
	}
	static JSValue ConsoleInput(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		if (!Output::ManiacConsoleOpen(false)) return JS_UNDEFINED;
		int32_t capacity = 0;
		if (JS_ToInt32(context, &capacity, argc > 0 ? argv[0] : JS_UNDEFINED) < 0) return JS_EXCEPTION;
		std::string text;
		if (!Output::ManiacConsoleRead(std::max<int32_t>(1024, capacity), text)) {
			Output::Warning("Maniac JavaScript: Console input unavailable");
			return JS_UNDEFINED;
		}
		return JS_NewStringLen(context, text.data(), text.size());
	}
	static JSValue Include(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		std::string filename;
		if (!self.Text(argc > 0 ? argv[0] : JS_UNDEFINED, filename)) return JS_EXCEPTION;
		auto stream = FileFinder::Game().OpenInputStream(filename);
		if (!stream) return JS_UNDEFINED;
		const auto bytes = Utils::ReadStream(stream);
		std::string source(bytes.begin(), bytes.end());
		const auto nul = source.find('\0');
		if (nul != std::string::npos) source.resize(nul);
		// Include runs on every call in the same strict global and active caller.
		return JS_Eval(context, source.c_str(), source.size(), "",
			JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
	}
	static JSModuleDef* LoadModule(JSContext* context, const char* name, void*) {
		auto stream = FileFinder::Game().OpenInputStream(name);
		if (!stream) {
			JS_ThrowReferenceError(context, "could not load module filename '%s'", name);
			return nullptr;
		}
		const auto bytes = Utils::ReadStream(stream);
		const std::string source(bytes.begin(), bytes.end());
		auto compiled = JS_Eval(context, source.c_str(), source.size(), name,
			JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
		if (JS_IsException(compiled)) return nullptr;
		auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
		JS_FreeValue(context, compiled);
		return module;
	}
	static JSValue GetPicture(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		int32_t id = 0;
		if (argc > 0 && JS_ToInt32(context, &id, argv[0]) < 0) return JS_EXCEPTION;
		if (!Main_Data::game_pictures || id <= 0 || id > Main_Data::game_pictures->GetMaxPictureId())
			return JS_ThrowRangeError(context, "Invalid Picture Access [%d]", id);
		Main_Data::game_pictures->GetPicture(id);
		auto picture = JS_NewObjectClass(context, self.picture_class);
		if (!JS_IsException(picture)) JS_SetOpaque(picture, reinterpret_cast<void*>(static_cast<intptr_t>(id)));
		return picture;
	}
	static int PictureId(JSContext* context, JSValueConst object) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		const auto id = static_cast<int>(reinterpret_cast<intptr_t>(JS_GetOpaque2(context, object, self.picture_class)));
		if (!id) return 0;
		if (!Main_Data::game_pictures || id > Main_Data::game_pictures->GetMaxPictureId()) {
			JS_ThrowRangeError(context, "Invalid Picture Access [%d]", id);
			return 0;
		}
		return id;
	}
	static JSValue PictureProperty(JSContext* context, JSValueConst object, int argc, JSValueConst* argv, int property) {
		const int id = PictureId(context, object);
		if (!id) return JS_EXCEPTION;
		if (property == 0) return JS_NewInt32(context, id);
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		if (property == 1) {
			const auto& name = Main_Data::game_pictures->GetPicture(id).data.name;
			return JS_NewStringLen(context, name.data(), name.size());
		}
		std::string name;
		if (!self.Text(argc > 0 ? argv[0] : JS_UNDEFINED, name)) return JS_EXCEPTION;
		Main_Data::game_pictures->ChangeFile(id, std::move(name));
		return JS_UNDEFINED;
	}
	static JSValue PictureMethod(JSContext* context, JSValueConst object, int argc, JSValueConst* argv, int operation) {
		const int id = PictureId(context, object);
		if (!id) return JS_EXCEPTION;
		if (operation == 3) {
			Main_Data::game_pictures->Erase(id);
			return JS_UNDEFINED;
		}
		auto& picture = Main_Data::game_pictures->GetPicture(id);
		auto& data = picture.data;
		for (int axis = 0; axis < 2; ++axis) {
			double value = 1.0;
			const bool number = argc > axis && JS_IsNumber(argv[axis]);
			if (number && JS_ToFloat64(context, &value, argv[axis]) < 0) return JS_EXCEPTION;
			if (operation == 0) {
				value *= 100;
				const int32_t percent = std::isfinite(value) && value >= INT32_MIN && value < 2147483648.0
					? static_cast<int32_t>(value) : INT32_MIN;
				(axis == 0 ? data.finish_magnify : data.maniac_finish_magnify_height) = percent;
			} else if (number) {
				(axis == 0 ? data.finish_x : data.finish_y) = value;
				if (operation == 1) {
					(axis == 0 ? data.start_x : data.start_y) = value;
					(axis == 0 ? data.current_x : data.current_y) = value;
				}
			}
		}
		if (operation == 2) data.time_left = std::max(1, data.time_left);
		picture.needs_update = true;
		return JS_UNDEFINED;
	}
	Game_ManiacSelfVariables::Bank* Bank() {
		if (!caller || caller->GetState().stack.empty()) return nullptr;
		if (!Main_Data::game_maniac_self_variables) {
			Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		}
		const auto& frame = caller->GetFrame();
		return Main_Data::game_maniac_self_variables->Ensure(frame.easyrpg_self_scope,
			frame.easyrpg_self_map_id, frame.easyrpg_self_event_id);
	}
	JSValue Read(int type, int32_t id, bool bulk) {
		if (type == 0) return JS_NewInt32(context,
			bulk && id >= 0 ? Main_Data::game_variables->GetManiacRawSlot(id) : caller->ReadVariable(id));
		if (type == 1) return JS_NewBool(context,
			bulk && id >= 0 ? Main_Data::game_switches->GetManiacRawSlot(id) != 0 : caller->ReadSwitch(id) != 0);
		auto value = bulk && id >= 0 ? Main_Data::game_strings->GetManiacRawSlot(id) : caller->ReadString(id);
		value = value.substr(0, value.find('\0'));
		return JS_NewStringLen(context, value.data(), value.size());
	}
	bool Write(int type, int32_t id, JSValueConst value) {
		int32_t number = 0;
		std::string text;
		bool string_present = true;
		if (type == 2) {
			string_present = Text(value, text);
			if (!string_present) text.clear();
			if (Player::game_config.maniac_version.Get() == 250921 && text.empty()) string_present = false;
		} else if (JS_ToInt32(context, &number, value) < 0) number = 0;
		// Both native setters and command returns ignore conversion failure:
		// write zero or a null string and continue to the next destination.
		if (id >= 0) {
			if (type == 0) {
				if (id < INT32_MAX) Main_Data::game_variables->GetManiacWritable(id + 1);
				if (auto* destination = Main_Data::game_variables->GetManiacWritable(id)) *destination = number;
			} else if (type == 1) Main_Data::game_switches->SetManiacRaw(id, number != 0);
			else Main_Data::game_strings->SetManiacRawSlot(id, text, string_present);
			return true;
		}
		auto* bank = Bank();
		if (!bank) return true;
		const size_t slot = static_cast<size_t>(-static_cast<int64_t>(id));
		if (type == 0) {
			bank->numbers.resize(std::max(bank->numbers.size(), slot + 2));
			bank->numbers[slot] = number;
		} else if (type == 1) {
			bank->switches.resize(std::max(bank->switches.size(), slot + 1));
			bank->switches[slot] = number != 0;
		} else {
			bank->strings.resize(std::max(bank->strings.size(), slot + 1));
			bank->strings[slot] = lcf::DBString(std::move(text));
			Game_ManiacSelfVariables::SetStringPresence(bank, slot, string_present);
		}
		return true;
	}
	static JSValue Get(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int type) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		if (!self.caller) return JS_ThrowInternalError(context, "No active Maniac caller");
		int32_t id = 0, count = 1;
		if (argc > 0 && self.IsArray(argv[0])) {
			auto first = JS_GetPropertyUint32(context, argv[0], 0);
			auto last = JS_GetPropertyUint32(context, argv[0], 1);
			int32_t end = 0;
			const bool ok = JS_ToInt32(context, &id, first) == 0 && JS_ToInt32(context, &end, last) == 0;
			JS_FreeValue(context, first);
			JS_FreeValue(context, last);
			if (!ok) return JS_EXCEPTION;
			const uint32_t distance = static_cast<uint32_t>(std::abs(static_cast<int64_t>(end))
				- std::abs(static_cast<int64_t>(id)) + 1);
			std::memcpy(&count, &distance, sizeof(count));
		} else {
			if (argc > 0 && JS_ToInt32(context, &id, argv[0]) < 0) return JS_EXCEPTION;
			if (argc > 1 && JS_IsNumber(argv[1]) && JS_ToInt32(context, &count, argv[1]) < 0) return JS_EXCEPTION;
		}
		if (count == 1) return self.Read(type, id, false);
		auto result = JS_NewArray(context);
		for (int32_t i = 0; i < count; ++i) {
			if (JS_SetPropertyUint32(context, result, static_cast<uint32_t>(i),
				self.Read(type, Offset(id, static_cast<uint32_t>(i)), true)) < 0) {
				JS_FreeValue(context, result);
				return JS_EXCEPTION;
			}
		}
		return result;
	}
	static JSValue Set(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int type) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		if (!self.caller) return JS_ThrowInternalError(context, "No active Maniac caller");
		int32_t id = 0;
		if (argc > 0 && JS_ToInt32(context, &id, argv[0]) < 0) return JS_EXCEPTION;
		const auto input = argc > 1 ? argv[1] : JS_UNDEFINED;
		if (!self.IsArray(input)) return self.Write(type, id, input) ? JS_TRUE : JS_EXCEPTION;
		auto length = JS_GetPropertyStr(context, input, "length");
		uint32_t count = 0;
		const bool ok = JS_ToUint32(context, &count, length) == 0;
		JS_FreeValue(context, length);
		if (!ok) return JS_EXCEPTION;
		for (uint32_t i = 0; i < count; ++i) {
			auto value = JS_GetPropertyUint32(context, input, i);
			const bool written = self.Write(type, Offset(id, i), value);
			JS_FreeValue(context, value);
			if (!written) return JS_EXCEPTION;
		}
		return JS_TRUE;
	}
	JSRuntime* runtime;
	JSContext* context;
	JSClassID picture_class = 0;
	const Game_BaseInterpreterContext* caller = nullptr;
};

std::unique_ptr<Runtime> runtime;
}

void Game_ManiacJs::Execute(std::string_view source, Span<const int32_t> parameters,
	const Game_BaseInterpreterContext& caller) {
	if (!runtime) runtime = std::make_unique<Runtime>();
	runtime->Execute(source, parameters, caller);
}

void Game_ManiacJs::Reset() {
	runtime.reset();
}
