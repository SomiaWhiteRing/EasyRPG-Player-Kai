#include "android.h"
#include "utils.h"
#if USE_SDL == 3
#include <SDL3/SDL_system.h>
#else
#include <SDL_system.h>
#endif

JNIEnv* EpAndroid::env = nullptr;
std::function<void()> EpAndroid::android_fn;
std::mutex EpAndroid::android_mutex;

namespace {
	int ConsoleCommand(const char* op, std::string_view text = {}, int options = 0, int foreground = 0, int background = 0) {
		auto* jenv = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
		if (!jenv) return 0;
		auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
		if (!activity) return 0;
		auto cls = jenv->GetObjectClass(activity);
		auto method = cls ? jenv->GetMethodID(cls, "commandManiacConsole", "(Ljava/lang/String;Ljava/lang/String;III)I") : nullptr;
		auto operation = method ? jenv->NewStringUTF(op) : nullptr;
		const auto units = Utils::DecodeUTF16(text, true);
		auto value = operation ? jenv->NewString(reinterpret_cast<const jchar*>(units.data()), static_cast<jsize>(units.size())) : nullptr;
		const auto result = value ? jenv->CallIntMethod(activity, method, operation, value, options, foreground, background) : 0;
		const bool success = !jenv->ExceptionCheck();
		if (!success) jenv->ExceptionClear();
		if (value) jenv->DeleteLocalRef(value);
		if (operation) jenv->DeleteLocalRef(operation);
		if (cls) jenv->DeleteLocalRef(cls);
		jenv->DeleteLocalRef(activity);
		return success ? result : 0;
	}
}

bool EpAndroid::ConsoleOpen(std::string_view title, bool update_window) {
	return ConsoleCommand("open", title, update_window) != 0;
}

void EpAndroid::ConsoleClose() {
	ConsoleCommand("close");
}

bool EpAndroid::ConsoleWrite(std::string_view text) {
	return ConsoleCommand("write", text) != 0;
}

bool EpAndroid::ConsoleColor(uint32_t options, int foreground, int background) {
	return ConsoleCommand("color", {}, options, foreground, background) != 0;
}

bool EpAndroid::ConsoleFocus(bool console) {
	return ConsoleCommand("focus", {}, console) != 0;
}

uint32_t EpAndroid::ConsoleGeneration() {
	return static_cast<uint32_t>(ConsoleCommand("generation"));
}

bool EpAndroid::ReadConsoleLine(std::u16string& line) {
	auto* jenv = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
	if (!jenv) return false;
	auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
	if (!activity) return false;
	auto cls = jenv->GetObjectClass(activity);
	auto method = cls ? jenv->GetMethodID(cls, "readManiacConsole", "()Ljava/lang/String;") : nullptr;
	auto text = method ? static_cast<jstring>(jenv->CallObjectMethod(activity, method)) : nullptr;
	bool success = text && !jenv->ExceptionCheck();
	if (success) {
		const auto length = jenv->GetStringLength(text);
		const auto* chars = jenv->GetStringChars(text, nullptr);
		success = chars != nullptr;
		if (chars) {
			line.assign(chars, chars + length);
			jenv->ReleaseStringChars(text, chars);
		}
	}
	if (jenv->ExceptionCheck()) jenv->ExceptionClear();
	if (text) jenv->DeleteLocalRef(text);
	if (cls) jenv->DeleteLocalRef(cls);
	jenv->DeleteLocalRef(activity);
	return success;
}
