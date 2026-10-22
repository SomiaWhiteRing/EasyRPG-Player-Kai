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
	// JNI environments belong to the calling thread.
	struct NameInputActivity {
		JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
		jobject activity = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
		jclass cls = activity ? env->GetObjectClass(activity) : nullptr;
		~NameInputActivity() {
			if (!env) return;
			if (env->ExceptionCheck()) env->ExceptionClear();
			if (cls) env->DeleteLocalRef(cls);
			if (activity) env->DeleteLocalRef(activity);
		}
	};

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

bool EpAndroid::UseSystemNameInput() {
	NameInputActivity target;
	if (!target.cls) return true;
	auto method = target.env->GetMethodID(target.cls, "hasPhysicalTextKeyboard", "()Z");
	return !method || !target.env->CallBooleanMethod(target.activity, method);
}

bool EpAndroid::BeginNameInput(std::string_view text) {
	NameInputActivity target;
	if (!target.cls) return false;
	auto method = target.env->GetMethodID(target.cls, "beginNameInput", "(Ljava/lang/String;)Z");
	if (!method) return false;
	const auto units = Utils::DecodeUTF16(text, true);
	auto value = target.env->NewString(reinterpret_cast<const jchar*>(units.data()), static_cast<jsize>(units.size()));
	if (!value) return false;
	const auto result = target.env->CallBooleanMethod(target.activity, method, value);
	target.env->DeleteLocalRef(value);
	return result && !target.env->ExceptionCheck();
}

int EpAndroid::PollNameInput(std::string& text) {
	NameInputActivity target;
	if (!target.cls) return 2;
	auto method = target.env->GetMethodID(target.cls, "pollNameInput", "()[Ljava/lang/String;");
	if (!method) return 2;
	auto result = static_cast<jobjectArray>(target.env->CallObjectMethod(target.activity, method));
	if (target.env->ExceptionCheck()) return 2;
	if (!result) return 0;
	int status = 2;
	if (target.env->GetArrayLength(result) == 1) {
		auto value = static_cast<jstring>(target.env->GetObjectArrayElement(result, 0));
		const auto* chars = value ? target.env->GetStringChars(value, nullptr) : nullptr;
		if (chars) {
			std::u16string units(chars, chars + target.env->GetStringLength(value));
			text = Utils::EncodeUTF(units);
			target.env->ReleaseStringChars(value, chars);
			status = 1;
		}
		if (value) target.env->DeleteLocalRef(value);
	}
	target.env->DeleteLocalRef(result);
	return status;
}

void EpAndroid::CancelNameInput() {
	NameInputActivity target;
	if (!target.cls) return;
	auto method = target.env->GetMethodID(target.cls, "cancelNameInput", "()V");
	if (method) target.env->CallVoidMethod(target.activity, method);
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
