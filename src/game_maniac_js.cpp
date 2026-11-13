#include "game_maniac_js.h"
#include "bitmap.h"
#include "cache.h"
#include "font.h"
#include "game_interpreter_shared.h"
#include "game_maniac_self_variables.h"
#include "game_netplay.h"
#include "game_switches.h"
#include "game_variables.h"
#include "game_strings.h"
#include "game_pictures.h"
#include "filefinder.h"
#include "filesystem_stream.h"
#include "input.h"
#include "utils.h"
#include "main_data.h"
#include "output.h"
#include "player.h"
#include "text.h"
#include "transition.h"
#include <quickjs.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#ifdef PLAYER_WEB
#include <emscripten.h>
#endif

namespace {
int32_t Offset(int32_t id, uint32_t offset) {
	const uint32_t bits = id < 0 ? static_cast<uint32_t>(id) - offset : static_cast<uint32_t>(id) + offset;
	int32_t result;
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

class Runtime {
	struct PictureHandle { std::weak_ptr<Game_Pictures::Picture> picture; };
	static void FinalizePicture(JSRuntime* runtime, JSValue object) {
		auto* self = static_cast<Runtime*>(JS_GetRuntimeOpaque(runtime));
		if (self) delete static_cast<PictureHandle*>(JS_GetOpaque(object, self->picture_class));
	}
public:
	Runtime() : runtime(JS_NewRuntime()), context(runtime ? JS_NewContext(runtime) : nullptr) {
		if (!context) Output::Error("Maniac JavaScript: Could not create the runtime");
		JS_SetContextOpaque(context, this);
		JS_SetRuntimeOpaque(runtime, this);
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
		picture_definition.finalizer = FinalizePicture;
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
		JS_SetPropertyStr(context, rm2k3, "setNextNumberInput",
			JS_NewCFunction(context, SetNextNumberInput, "setNextNumberInput", 1));
		if (Player::game_config.netplay_enabled) {
			JS_SetPropertyStr(context, rm2k3, "setNetplayOverlay",
				JS_NewCFunction(context, SetNetplayOverlay, "setNetplayOverlay", 1));
			JS_SetPropertyStr(context, rm2k3, "netplayTime", JS_NewCFunction(context, NetplayTime, "netplayTime", 0));
			JS_SetPropertyStr(context, rm2k3, "netplayFade", JS_NewCFunction(context, NetplayFade, "netplayFade", 2));
			JS_SetPropertyStr(context, rm2k3, "netplayButtons", JS_NewCFunction(context, NetplayButtons, "netplayButtons", 1));
			JS_SetPropertyStr(context, rm2k3, "netplayWaitingForInput", JS_NewCFunction(context, NetplayWaitingForInput, "netplayWaitingForInput", 0));
			JS_SetPropertyStr(context, rm2k3, "netplayContinuePicture",
				JS_NewCFunction(context, NetplayContinuePicture, "netplayContinuePicture", 4));
			JS_SetPropertyStr(context, rm2k3, "setNetplayProjection",
				JS_NewCFunction(context, SetNetplayProjection, "setNetplayProjection", 1));
			JS_SetPropertyStr(context, rm2k3, "netplaySample", JS_NewCFunction(context, NetplaySample, "netplaySample", 2));
			JS_SetPropertyStr(context, rm2k3, "netplayUpcoming", JS_NewCFunction(context, NetplayUpcoming, "netplayUpcoming", 2));
			JS_SetPropertyStr(context, rm2k3, "netplayPosition", JS_NewCFunction(context, NetplayPosition, "netplayPosition", 2));
			JS_SetPropertyStr(context, rm2k3, "netplayProject", JS_NewCFunction(context, NetplayProject, "netplayProject", 7));
			canvas = JS_NewObject(context);
			JS_SetPropertyStr(context, canvas, "fill", JS_NewCFunction(context, FillOverlay, "fill", 5));
			JS_SetPropertyStr(context, canvas, "textWidth", JS_NewCFunction(context, OverlayTextWidth, "textWidth", 1));
			JS_SetPropertyStr(context, canvas, "text", JS_NewCFunction(context, DrawOverlayText, "text", 5));
			JS_SetPropertyStr(context, canvas, "picture", JS_NewCFunction(context, DrawOverlayPicture, "picture", 11));
#ifdef PLAYER_WEB
			JS_SetPropertyStr(context, canvas, "avatar", JS_NewCFunction(context, DrawOverlayAvatar, "avatar", 5));
			JS_SetPropertyStr(context, canvas, "peerLatency", JS_NewCFunction(context, OverlayPeerLatency, "peerLatency", 2));
			JS_SetPropertyStr(context, rm2k3, "setInputFrameDriver",
				JS_NewCFunction(context, SetInputFrameDriver, "setInputFrameDriver", 1));
			JS_SetPropertyStr(context, rm2k3, "inputFramePrecomputing",
				JS_NewCFunction(context, InputFramePrecomputing, "inputFramePrecomputing", 0));
			JS_SetPropertyStr(context, rm2k3, "inputFramePrecompute",
				JS_NewCFunction(context, InputFramePrecompute, "inputFramePrecompute", 1));
			JS_SetPropertyStr(context, rm2k3, "inputFramePrecomputeData",
				JS_NewCFunction(context, InputFramePrecomputeData, "inputFramePrecomputeData", 0));
			JS_SetPropertyStr(context, rm2k3, "requestHost", JS_NewCFunction(context, RequestHost, "requestHost", 2));
			JS_SetPropertyStr(context, rm2k3, "pollHost", JS_NewCFunction(context, PollHost, "pollHost", 1));
#endif
			JS_SetPropertyStr(context, canvas, "capture", JS_NewCFunctionMagic(context, OverlayFrame, "capture", 0,
				JS_CFUNC_generic_magic, 0));
			JS_SetPropertyStr(context, canvas, "restore", JS_NewCFunctionMagic(context, OverlayFrame, "restore", 0,
				JS_CFUNC_generic_magic, 1));
		}
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
		JS_FreeValue(context, overlay);
		JS_FreeValue(context, projection);
		JS_FreeValue(context, input_frame_driver);
		JS_FreeValue(context, canvas);
		JS_FreeContext(context);
		JS_FreeRuntime(runtime);
	}

	int TakeNumberInputInitialValue() { return std::exchange(number_input_initial_value, 0); }
	bool HasInputFrameDriver() const { return !JS_IsUndefined(input_frame_driver); }
	Game_ManiacJs::InputFramePlan ReadInputFramePlan(int next, bool stopped, bool precompute) {
		Game_ManiacJs::InputFramePlan plan;
		if (!HasInputFrameDriver()) { plan.close = true; return plan; }
		JSValue args[] = {JS_NewInt32(context, next), JS_NewBool(context, stopped), JS_TRUE, JS_NewBool(context, precompute)};
		auto result = JS_Call(context, input_frame_driver, JS_UNDEFINED, 4, args);
		if (JS_IsException(result)) {
			ReportException();
			plan.close = true;
		} else if (JS_IsObject(result)) {
			const auto flag = [&](const char* name) {
				auto value = JS_GetPropertyStr(context, result, name);
				const bool enabled = JS_ToBool(context, value) > 0;
				JS_FreeValue(context, value);
				return enabled;
			};
			plan.restart = flag("restart");
			plan.silent = flag("silent");
			plan.close = flag("close");
			plan.precompute = flag("precompute");
			auto seek = JS_GetPropertyStr(context, result, "seek");
			if (JS_IsNumber(seek)) {
				int32_t index;
				if (JS_ToInt32(context, &index, seek) == 0 && index >= 0) plan.seek = index;
			}
			JS_FreeValue(context, seek);
			auto transfer = JS_GetPropertyStr(context, result, "transfer");
			if (IsArray(transfer)) {
				for (uint32_t i = 0; i < plan.transfer.size(); ++i) {
					auto value = JS_GetPropertyUint32(context, transfer, i);
					int32_t n = 0;
					JS_ToInt32(context, &n, value);
					plan.transfer[i] = n;
					JS_FreeValue(context, value);
				}
			}
			JS_FreeValue(context, transfer);
			auto frames = JS_GetPropertyStr(context, result, "frames");
			if (IsArray(frames)) {
				auto length = JS_GetPropertyStr(context, frames, "length");
				int32_t count = 0;
				JS_ToInt32(context, &count, length);
				JS_FreeValue(context, length);
				for (int i = 0; i < std::min(count, 120); ++i) {
					auto value = JS_GetPropertyUint32(context, frames, i);
					int32_t bits = 0;
					const bool valid = JS_IsNumber(value) && JS_ToInt32(context, &bits, value) == 0 &&
						bits >= 0 && bits <= 0xffff;
					JS_FreeValue(context, value);
					if (!valid) break;
					plan.inputs.push_back({bits & 255, bits >> 8});
				}
			}
			JS_FreeValue(context, frames);
		}
		JS_FreeValue(context, result);
		return plan;
	}
	void DrawNetplayOverlay(Bitmap& destination, bool above_windows) {
		if (JS_IsUndefined(overlay) || (above_windows && !overlay_after_windows)) return;
		overlay_target = &destination;
		JSValue args[] = {
			canvas, JS_NewInt32(context, destination.GetWidth()), JS_NewInt32(context, destination.GetHeight()),
			JS_NewFloat64(context, NetplayTimeMs()), JS_NewBool(context, above_windows)
		};
		auto result = JS_Call(context, overlay, JS_UNDEFINED, overlay_after_windows ? 5 : 4, args);
		overlay_target = nullptr;
		JS_FreeValue(context, args[3]);
		if (JS_IsException(result)) {
			ReportException();
			JS_FreeValue(context, overlay);
			overlay = JS_UNDEFINED;
		}
		JS_FreeValue(context, result);
	}

	void RunNetplayProjection(int seat, bool stopping) {
		if (JS_IsUndefined(projection)) return;
		JSValue args[] = {JS_NewInt32(context, seat), JS_NewBool(context, stopping)};
		auto result = JS_Call(context, projection, JS_UNDEFINED, 2, args);
		if (JS_IsException(result)) {
			ReportException();
			JS_FreeValue(context, projection);
			projection = JS_UNDEFINED;
		}
		JS_FreeValue(context, result);
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
			ReportException();
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
	static JSValue SetNextNumberInput(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		int32_t value = 0;
		if (argc > 0 && JS_ToInt32(context, &value, argv[0]) < 0) return JS_EXCEPTION;
		static_cast<Runtime*>(JS_GetContextOpaque(context))->number_input_initial_value = value;
		return JS_UNDEFINED;
	}
	static double NetplayTimeMs() {
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		return std::chrono::duration<double, std::milli>(now).count();
	}

	static JSValue NetplayTime(JSContext* context, JSValueConst, int, JSValueConst*) {
		return JS_NewFloat64(context, NetplayTimeMs());
	}

	static JSValue NetplayWaitingForInput(JSContext* context, JSValueConst, int, JSValueConst*) {
		return JS_NewBool(context, Game_Netplay::WaitingForInput());
	}

	static JSValue NetplayButtons(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		const bool repeat = argc > 0 && JS_ToBool(context, argv[0]);
		auto button = repeat ? Input::IsRepeated : Input::IsTriggered;
		constexpr Input::InputButton menu[] = {
			Input::DOWN, Input::LEFT, Input::RIGHT, Input::UP, Input::DECISION, Input::CANCEL, Input::SHIFT
		};
		int keys = 0;
		for (int i = 0; i < 7; ++i) {
			if (button(menu[i])) keys |= 1 << (i + 1);
		}
		for (int i = 0; i < 10; ++i) {
			if (button(static_cast<Input::InputButton>(Input::N0 + i))) keys |= 1 << (10 + i);
		}
		if (!repeat && Input::IsRawKeyTriggered(Input::Keys::ESCAPE)) keys |= 1 << 8;
		return JS_NewInt32(context, keys);
	}

	static JSValue NetplayFade(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& transition = Transition::instance();
		if (!argc) {
			return JS_NewInt32(context, transition.IsActive() ? 1 : transition.IsErasedNotActive() ? 2 : 0);
		}
		int32_t frames;
		if (JS_ToInt32(context, &frames, argv[0]) < 0) return JS_EXCEPTION;
		if (frames <= 0 || !Scene::instance) {
			return JS_ThrowRangeError(context, "Expected a positive fade duration");
		}
		if (argc > 1 && JS_ToBool(context, argv[1])) {
			transition.InitShow(Transition::TransitionFadeIn, Scene::instance.get(), frames);
		} else {
			transition.InitErase(Transition::TransitionFadeOut, Scene::instance.get(), frames);
		}
		return JS_UNDEFINED;
	}

	static JSValue NetplayContinuePicture(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		using Action = Game_Netplay::PictureContinuation;
		int32_t id;
		if (argc < 1) return JS_ThrowTypeError(context, "Expected a picture slot");
		if (JS_ToInt32(context, &id, argv[0]) < 0) return JS_EXCEPTION;
		if (argc == 1) {
			Game_Netplay::ContinuePicture(id, Action::Capture);
		} else if (argc == 2 && JS_IsNull(argv[1])) {
			Game_Netplay::ContinuePicture(id, Action::Remove);
		} else if (argc == 4) {
			int32_t x, y, angle;
			if (JS_ToInt32(context, &x, argv[1]) < 0 || JS_ToInt32(context, &y, argv[2]) < 0 ||
					JS_ToInt32(context, &angle, argv[3]) < 0) return JS_EXCEPTION;
			Game_Netplay::ContinuePicture(id, Action::Move, x, y, angle);
		} else {
			return JS_ThrowTypeError(context, "Expected a picture slot, null or position and rotation");
		}
		return JS_UNDEFINED;
	}

	static JSValue SetNetplayProjection(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		const auto callback = argc > 0 ? argv[0] : JS_UNDEFINED;
		const bool clear = JS_IsNull(callback) || JS_IsUndefined(callback);
		if (!clear && !JS_IsFunction(context, callback)) {
			return JS_ThrowTypeError(context, "Expected a netplay projection function");
		}
		JS_FreeValue(context, self.projection);
		self.projection = clear ? JS_UNDEFINED : JS_DupValue(context, callback);
		return JS_UNDEFINED;
	}

	static JSValue NetplayPair(JSContext* context, std::pair<int, int> pair) {
		auto result = JS_NewArray(context);
		JS_SetPropertyUint32(context, result, 0, JS_NewInt32(context, pair.first));
		JS_SetPropertyUint32(context, result, 1, JS_NewInt32(context, pair.second));
		return result;
	}

	static JSValue NetplaySample(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		int32_t id;
		if (argc < 1) return JS_UNDEFINED;
		if (JS_ToInt32(context, &id, argv[0]) < 0) return JS_EXCEPTION;
		return JS_NewInt32(context, Game_Netplay::ProjectionSample(id, argc > 1 && JS_ToBool(context, argv[1])));
	}

	static JSValue NetplayUpcoming(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		int32_t id, related;
		if (argc < 2) return JS_UNDEFINED;
		if (JS_ToInt32(context, &id, argv[0]) < 0 || JS_ToInt32(context, &related, argv[1]) < 0)
			return JS_EXCEPTION;
		return NetplayPair(context, Game_Netplay::UpcomingSample(id, related));
	}

	static JSValue NetplayPosition(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		int32_t id, source;
		if (argc < 2) return JS_UNDEFINED;
		if (JS_ToInt32(context, &id, argv[0]) < 0 || JS_ToInt32(context, &source, argv[1]) < 0)
			return JS_EXCEPTION;
		const auto position = Game_Netplay::ProjectionPosition(id, source);
		return position ? NetplayPair(context, *position) : JS_NULL;
	}

	static JSValue NetplayProject(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		int32_t values[7]{};
		if (argc < 5) return JS_UNDEFINED;
		for (int i = 0; i < std::min(argc, 7); ++i) {
			if (JS_ToInt32(context, &values[i], argv[i]) < 0) return JS_EXCEPTION;
		}
		Game_Netplay::ProjectPictures(values[0], values[1], values[2], values[3], values[4], values[5], values[6]);
		return JS_UNDEFINED;
	}

	void ReportException() {
		auto exception = JS_GetException(context);
		const char* message = JS_ToCString(context, exception);
		Output::Warning("Maniac JavaScript: {}", message ? message : "Exception");
		if (message) JS_FreeCString(context, message);
		JS_FreeValue(context, exception);
	}

	static JSValue SetNetplayOverlay(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		const auto callback = argc > 0 ? argv[0] : JS_UNDEFINED;
		const bool clear = JS_IsNull(callback) || JS_IsUndefined(callback);
		if (!clear && !JS_IsFunction(context, callback)) {
			return JS_ThrowTypeError(context, "Expected a netplay overlay function");
		}
		JS_FreeValue(context, self.overlay);
		self.overlay = clear ? JS_UNDEFINED : JS_DupValue(context, callback);
		self.overlay_after_windows = !clear && argc > 1 && JS_IsBool(argv[1]) && JS_ToBool(context, argv[1]);
		if (clear) {
			self.overlay_frame.reset();
#ifdef PLAYER_WEB
			self.overlay_avatars.clear();
#endif
		}
		return JS_UNDEFINED;
	}

	template <size_t N>
	static bool ReadOverlayNumbers(JSContext* context, const JSValueConst* args, int32_t (&values)[N]) {
		for (size_t i = 0; i < N; ++i) {
			if (JS_ToInt32(context, &values[i], args[i]) < 0) return false;
		}
		return true;
	}

	static Color OverlayColor(uint32_t rgba) {
		return Color(rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255);
	}

	static JSValue FillOverlay(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto* target = static_cast<Runtime*>(JS_GetContextOpaque(context))->overlay_target;
		if (!target || argc < 5) return JS_ThrowTypeError(context, "Expected overlay fill bounds and color");
		int32_t args[5];
		if (!ReadOverlayNumbers(context, argv, args)) return JS_EXCEPTION;
		target->FillRect(Rect(args[0], args[1], args[2], args[3]), OverlayColor(args[4]));
		return JS_UNDEFINED;
	}

	static JSValue OverlayTextWidth(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		const char* value = JS_ToCString(context, argc > 0 ? argv[0] : JS_UNDEFINED);
		if (!value) return JS_EXCEPTION;
		const auto size = Text::GetSize(*Font::Default(), value);
		JS_FreeCString(context, value);
		return JS_NewInt32(context, size.width);
	}

	static JSValue DrawOverlayText(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto* target = static_cast<Runtime*>(JS_GetContextOpaque(context))->overlay_target;
		if (!target || argc < 5) return JS_ThrowTypeError(context, "Expected overlay text, position, color and scale");
		int32_t args[4];
		if (!ReadOverlayNumbers(context, argv + 1, args)) return JS_EXCEPTION;
		const char* value = JS_ToCString(context, argv[0]);
		if (!value) return JS_EXCEPTION;
		const auto font = Font::Default();
		const auto color = OverlayColor(args[2]);
		if (args[3] == 2) {
			const auto size = Text::GetSize(*font, value);
			if (size.width > 0 && size.height > 0) {
				auto label = Bitmap::Create(size.width, size.height, true);
				Text::Draw(*label, 0, 0, *font, color, value);
				target->StretchBlit(Rect(args[0], args[1], size.width * 2, size.height * 2),
					*label, label->GetRect(), Opacity::Opaque());
			}
		} else {
			Text::Draw(*target, args[0], args[1], *font, color, value);
		}
		JS_FreeCString(context, value);
		return JS_UNDEFINED;
	}

	static JSValue DrawOverlayPicture(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto* target = static_cast<Runtime*>(JS_GetContextOpaque(context))->overlay_target;
		if (!target || argc < 11) return JS_ThrowTypeError(context, "Expected overlay picture, bounds, opacity and flip");
		int32_t args[10];
		if (!ReadOverlayNumbers(context, argv + 1, args)) return JS_EXCEPTION;
		const char* file = JS_ToCString(context, argv[0]);
		if (!file) return JS_EXCEPTION;
		auto picture = Cache::Picture(file, true);
		JS_FreeCString(context, file);
		if (!picture) return JS_UNDEFINED;
		const Rect source = args[2] > 0 && args[3] > 0
			? Rect(args[0], args[1], args[2], args[3]) : picture->GetRect();
		const Rect destination(args[4], args[5], args[6], args[7]);
		const Opacity opacity(std::clamp(args[8], 0, 255));
		if (args[9]) {
			auto flipped = Bitmap::Create(source.width, source.height, true);
			flipped->FlipBlit(0, 0, *picture, source, true, false, Opacity::Opaque());
			target->StretchBlit(destination, *flipped, flipped->GetRect(), opacity);
		} else {
			target->StretchBlit(destination, *picture, source, opacity);
		}
		return JS_UNDEFINED;
	}

#ifdef PLAYER_WEB
	static JSValue InputFramePrecomputing(JSContext* context, JSValueConst, int, JSValueConst*) {
		return JS_NewBool(context, EM_ASM_INT({ return Number(!!Module.replayCheckpoint); }));
	}

	static JSValue InputFramePrecompute(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		if (!argc || JS_IsNull(argv[0])) {
			EM_ASM({ Module.closeReplayPrecompute?.(); });
			return JS_UNDEFINED;
		}
		auto value = JS_JSONStringify(context, argv[0], JS_UNDEFINED, JS_UNDEFINED);
		if (JS_IsException(value)) return value;
		auto text = JS_ToCString(context, value);
		JS_FreeValue(context, value);
		if (!text) return JS_EXCEPTION;
		EM_ASM({ Module.startReplayPrecompute?.(UTF8ToString($0)); }, text);
		JS_FreeCString(context, text);
		return JS_UNDEFINED;
	}

	static JSValue InputFramePrecomputeData(JSContext* context, JSValueConst, int, JSValueConst*) {
		const auto raw = reinterpret_cast<char*>(EM_ASM_PTR({
			const text = Module.replayInput;
			if (!text) return 0;
			Module.replayInput = undefined;
			const size = lengthBytesUTF8(text) + 1;
			const ptr = _malloc(size);
			if (ptr) stringToUTF8(text, ptr, size);
			return ptr;
		}));
		if (!raw) return JS_UNDEFINED;
		auto value = JS_ParseJSON(context, raw, std::strlen(raw), "replay");
		std::free(raw);
		return value;
	}

	static JSValue SetInputFrameDriver(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		if (!argc || (!JS_IsNull(argv[0]) && !JS_IsFunction(context, argv[0])))
			return JS_ThrowTypeError(context, "Expected a frame driver callback or null");
		JS_FreeValue(context, self.input_frame_driver);
		self.input_frame_driver = JS_IsNull(argv[0]) ? JS_UNDEFINED : JS_DupValue(context, argv[0]);
		return JS_UNDEFINED;
	}

	static JSValue RequestHost(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		std::string service, payload;
		if (argc < 2 || !self.Text(argv[0], service)) return JS_EXCEPTION;
		auto json = JS_JSONStringify(context, argv[1], JS_UNDEFINED, JS_UNDEFINED);
		if (JS_IsException(json)) return json;
		const bool valid = self.Text(json, payload);
		JS_FreeValue(context, json);
		if (!valid) return JS_EXCEPTION;
		return JS_NewInt32(context, EM_ASM_INT({
			return Module.gameServices?.request(UTF8ToString($0), UTF8ToString($1)) ?? 0;
		}, service.c_str(), payload.c_str()));
	}

	static JSValue PollHost(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		int32_t request;
		if (argc < 1 || JS_ToInt32(context, &request, argv[0]) < 0) return JS_EXCEPTION;
		auto* text = reinterpret_cast<char*>(EM_ASM_PTR({
			const json = Module.gameServices?.take($0);
			if (json === undefined) return 0;
			const length = lengthBytesUTF8(json) + 1;
			const ptr = _malloc(length);
			if (ptr) stringToUTF8(json, ptr, length);
			return ptr;
		}, request));
		if (!text) return JS_UNDEFINED;
		auto result = JS_ParseJSON(context, text, std::strlen(text), "host");
		std::free(text);
		return result;
	}

	static JSValue OverlayPeerLatency(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		const bool poll = argc > 0 && JS_ToBool(context, argv[0]);
		const bool reset = argc > 1 && JS_ToBool(context, argv[1]);
		return JS_NewInt32(context, EM_ASM_INT({
			return Module.netplay?.peerLatency?.($0, $1) ?? -1;
		}, poll, reset));
	}

	static JSValue DrawOverlayAvatar(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		auto* target = self.overlay_target;
		if (!target || argc < 5) return JS_ThrowTypeError(context, "Expected overlay avatar and bounds");
		int32_t args[4];
		if (!ReadOverlayNumbers(context, argv + 1, args)) return JS_EXCEPTION;
		std::string path;
		if (!self.Text(argv[0], path)) return JS_EXCEPTION;
		auto& avatar = self.overlay_avatars[path];
		if (!avatar) {
			constexpr int size = 48;
			std::array<uint8_t, size * size * 4> pixels;
			if (!EM_ASM_INT({
				const path = UTF8ToString($0);
				return Module.netplay?.avatar?.(path, $1, $2) || Module.gameServices?.avatar(path, $1, $2) || 0;
			}, path.c_str(), pixels.data(), size)) return JS_UNDEFINED;
			avatar = Bitmap::Create(size, size, true);
			for (int y = 0; y < size; ++y) {
				for (int x = 0; x < size; ++x) {
					const auto* pixel = pixels.data() + (y * size + x) * 4;
					avatar->SetManiacColorAt(x, y, Color(pixel[0], pixel[1], pixel[2], pixel[3]));
				}
			}
		}
		target->StretchBlit(Rect(args[0], args[1], args[2], args[3]), *avatar, avatar->GetRect(), Opacity::Opaque());
		return JS_UNDEFINED;
	}
#endif

	static JSValue OverlayFrame(JSContext* context, JSValueConst, int, JSValueConst*, int mode) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		auto* target = self.overlay_target;
		if (!target) return JS_ThrowTypeError(context, "Expected an active netplay overlay");
		if (mode == 0) {
			self.overlay_frame = Bitmap::Create(target->GetWidth(), target->GetHeight(), false);
			self.overlay_frame->Blit(0, 0, *target, target->GetRect(), Opacity::Opaque());
		} else if (self.overlay_frame) {
			target->Blit(0, 0, *self.overlay_frame, self.overlay_frame->GetRect(), Opacity::Opaque());
		}
		return JS_UNDEFINED;
	}

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
		auto holder = Main_Data::game_pictures->GetPictureRef(id);
		auto picture = JS_NewObjectClass(context, self.picture_class);
		if (!JS_IsException(picture)) {
			auto* handle = new (std::nothrow) PictureHandle{holder};
			if (!handle) { JS_FreeValue(context, picture); return JS_ThrowOutOfMemory(context); }
			JS_SetOpaque(picture, handle);
		}
		return picture;
	}
	static std::shared_ptr<Game_Pictures::Picture> Picture(JSContext* context, JSValueConst object) {
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		auto* handle = static_cast<PictureHandle*>(JS_GetOpaque2(context, object, self.picture_class));
		if (!handle) return {};
		auto picture = handle->picture.lock();
		if (!picture) JS_ThrowReferenceError(context, "Picture object has been removed");
		return picture;
	}
	static JSValue PictureProperty(JSContext* context, JSValueConst object, int argc, JSValueConst* argv, int property) {
		auto picture = Picture(context, object);
		if (!picture) return JS_EXCEPTION;
		if (property == 0) return JS_NewInt32(context, picture->data.ID);
		auto& self = *static_cast<Runtime*>(JS_GetContextOpaque(context));
		if (property == 1) {
			const auto& name = picture->data.name;
			return JS_NewStringLen(context, name.data(), name.size());
		}
		std::string name;
		if (!self.Text(argc > 0 ? argv[0] : JS_UNDEFINED, name)) return JS_EXCEPTION;
		Main_Data::game_pictures->ChangeFile(*picture, std::move(name));
		return JS_UNDEFINED;
	}
	static JSValue PictureMethod(JSContext* context, JSValueConst object, int argc, JSValueConst* argv, int operation) {
		auto holder = Picture(context, object);
		if (!holder) return JS_EXCEPTION;
		if (operation == 3) {
			holder->Erase();
			return JS_UNDEFINED;
		}
		auto& picture = *holder;
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
			if (Player::IsPatchManiac() && text.empty()) string_present = false;
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
	int32_t number_input_initial_value = 0;
	const Game_BaseInterpreterContext* caller = nullptr;
	JSValue overlay = JS_UNDEFINED;
	bool overlay_after_windows = false;
	JSValue projection = JS_UNDEFINED;
	JSValue input_frame_driver = JS_UNDEFINED;
	JSValue canvas = JS_UNDEFINED;
	Bitmap* overlay_target = nullptr;
	BitmapRef overlay_frame;
#ifdef PLAYER_WEB
	std::unordered_map<std::string, BitmapRef> overlay_avatars;
#endif
};

std::unique_ptr<Runtime> runtime;
}

void Game_ManiacJs::Execute(std::string_view source, Span<const int32_t> parameters,
	const Game_BaseInterpreterContext& caller) {
	if (!runtime) runtime = std::make_unique<Runtime>();
	runtime->Execute(source, parameters, caller);
}

void Game_ManiacJs::DrawNetplayOverlay(Bitmap& destination, bool above_windows) {
	if (runtime) runtime->DrawNetplayOverlay(destination, above_windows);
}

int Game_ManiacJs::TakeNumberInputInitialValue() {
	return runtime ? runtime->TakeNumberInputInitialValue() : 0;
}

void Game_ManiacJs::RunNetplayProjection(int seat, bool stopping) {
	if (runtime) runtime->RunNetplayProjection(seat, stopping);
}

bool Game_ManiacJs::HasInputFrameDriver() {
	return runtime && runtime->HasInputFrameDriver();
}

Game_ManiacJs::InputFramePlan Game_ManiacJs::ReadInputFramePlan(int next, bool stopped, bool precompute) {
	if (runtime) return runtime->ReadInputFramePlan(next, stopped, precompute);
	InputFramePlan plan;
	plan.close = true;
	return plan;
}

void Game_ManiacJs::Reset() {
	runtime.reset();
}
