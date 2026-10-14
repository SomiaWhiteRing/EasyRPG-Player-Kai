/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

// Headers
#include <cstdlib>
#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <fmt/color.h>
#include <fmt/ostream.h>
#ifdef EMSCRIPTEN
#  include <emscripten.h>
#  include "platform/emscripten/interface.h"
#endif

#include "output.h"
#include "graphics.h"
#include "filefinder.h"
#include "input.h"
#include "options.h"
#include "player.h"
#include "bitmap.h"
#include "message_overlay.h"
#include "font.h"
#include "baseui.h"
#include "utils.h"
#ifdef _WIN32
#  include <windows.h>
#elif defined(__ANDROID__)
#  include "platform/android/android.h"
#endif

// fmt 7 has renamed the namespace
#if FMT_VERSION < 70000
#  define FMT_COLOR_TYPE fmt::internal::color_type
#else
#  define FMT_COLOR_TYPE fmt::detail::color_type
#endif

using namespace std::chrono_literals;

namespace {
	constexpr const char* const log_prefix[] = {
		"Error", "Warning", "Info", "Debug"
	};
	LogLevel log_level = LogLevel::Debug;

	std::ostream& output_time() {
		std::time_t t = std::time(nullptr);
		return Game_Config::GetLogFileOutput() << Utils::FormatDate(std::localtime(&t), "[%Y-%m-%d %H:%M:%S] ");
	}

	bool ignore_pause = false;
	bool colored_log = true;
	uint32_t maniac_output_count = 0;
	uint32_t maniac_output_time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		Game_Clock::now().time_since_epoch()).count()) - 1001u;
	uint32_t ManiacOutputTime() {
		return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			Game_Clock::now().time_since_epoch()).count());
	}
#ifdef _WIN32
	bool maniac_console_owned = false;
#elif defined(EMSCRIPTEN) || defined(__ANDROID__)
	std::u16string maniac_console_input;
	size_t maniac_console_input_position = 0;
	uint32_t maniac_console_generation = 0;
#endif

	// pair of repeat count + message
	struct {
		int repeat = 0;
		std::string msg;
		LogLevel lvl = {};
	} last_message;

	void LogCallback(LogLevel lvl, std::string const& msg, LogCallbackUserData /* userdata */) {
		// terminal output
		std::string prefix = Output::LogLevelToString(lvl) + ":";

		// only support colors with a "recent" fmt 6
	#if FMT_VERSION >= 60000
		if (colored_log) {
			FMT_COLOR_TYPE log_color =
				(lvl == LogLevel::Error) ? fmt::terminal_color::red :
				(lvl == LogLevel::Warning) ? fmt::terminal_color::yellow :
				(lvl == LogLevel::Debug) ? fmt::terminal_color::white :
				fmt::terminal_color::bright_white;

	#  if FMT_VERSION < 90000
			// format using temporary strings
			fmt::print(std::cerr, "{} {}\n",
				fmt::format(fmt::fg(log_color) | fmt::emphasis::bold, prefix),
				fmt::format(fmt::fg(log_color), msg));
	#  else
			// fmt 9 has styled arguments
			fmt::print(std::cerr, "{} {}\n",
				fmt::styled(prefix, fmt::fg(log_color) | fmt::emphasis::bold),
				fmt::styled(msg, fmt::fg(log_color)));
	#  endif
			return;
		}
	#endif
		std::cerr << prefix << " " << msg << std::endl;
	}

	LogCallbackFn log_cb = LogCallback;
	LogCallbackUserData log_cb_udata = nullptr;

	bool ShouldHideManiacLog(std::string const& msg) {
		return Player::player_config.extra_hide_maniac_logs.Get()
			&& msg.size() >= 6
			&& msg.substr(0, 6) == "Maniac";
	}
}

std::string Output::LogLevelToString(LogLevel lvl) {
	return log_prefix[static_cast<int>(lvl)];
}

LogLevel Output::GetLogLevel() {
	return log_level;
}

void Output::SetLogLevel(LogLevel lvl) {
	log_level = lvl;
}

void Output::SetTermColor(bool colored) {
	colored_log = colored;
}

void Output::IgnorePause(bool const val) {
	ignore_pause = val;
}

void Output::SetLogCallback(LogCallbackFn fn, LogCallbackUserData userdata) {
	if (!fn) {
		log_cb = LogCallback;
		log_cb_udata = nullptr;
	} else {
		log_cb = fn;
		log_cb_udata = userdata;
	}
}

bool Output::ManiacConsoleOpen(bool update_window) {
#ifdef _WIN32
	// JS hosts only ensure ownership; event commands also refresh the window.
	if (!update_window && maniac_console_owned) return true;
	const auto game = DisplayUi ? static_cast<HWND>(DisplayUi->GetNativeWindowHandle()) : nullptr;
	const bool game_active = game && GetForegroundWindow() == game;
	if (!maniac_console_owned || !GetConsoleWindow()) maniac_console_owned = AllocConsole() != 0;
	if (!maniac_console_owned) return false;
	const auto console = GetConsoleWindow();
	const auto title = Utils::DecodeUTF16("Console - " + Player::game_title, true);
	SetConsoleTitleW(reinterpret_cast<const wchar_t*>(title.c_str()));
	RECT work = {}, game_rect = {};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	GetWindowRect(game, &game_rect);
	int left, right;
	if (game_rect.left - work.left > work.right - game_rect.right) {
		left = static_cast<SHORT>(work.left);
		right = static_cast<SHORT>(game_rect.left);
	} else {
		left = static_cast<SHORT>(game_rect.right);
		right = static_cast<SHORT>(work.right);
	}
	int top = static_cast<SHORT>(work.top + 100);
	int bottom = static_cast<SHORT>(work.bottom - 100);
	if (left < work.left || right > work.right || top < work.top || bottom > work.bottom
			|| right - left < 100 || bottom - top < 100) {
		left = static_cast<SHORT>(work.left);
		right = static_cast<SHORT>(work.left + 640);
		top = static_cast<SHORT>(work.top);
		bottom = static_cast<SHORT>(work.top + 480);
	}
	SetWindowPos(console, game, left, top, right - left, bottom - top, SWP_SHOWWINDOW);
	if (game_active) SetForegroundWindow(game);
#elif defined(EMSCRIPTEN)
	return Emscripten_Interface::ConsoleOpen(update_window);
#elif defined(__ANDROID__)
	return EpAndroid::ConsoleOpen("Console - " + Player::game_title, update_window);
#else
	(void)update_window;
#endif
	return true;
}

bool Output::IsManiacFileOutputAvailable() {
	return ManiacOutputTime() - maniac_output_time > 1000u
		|| static_cast<int32_t>(maniac_output_count + 1u) < 4;
}

void Output::BeginManiacFileOutput() {
	if (!Player::IsPatchManiac()) return;
	const auto now = ManiacOutputTime();
	++maniac_output_count;
	if (now - maniac_output_time > 1000u) {
		maniac_output_count = 0;
		maniac_output_time = now;
	} else if (static_cast<int32_t>(maniac_output_count) > 3) {
		if (!DisplayUi || !DisplayUi->ConfirmManiacFileOutput()) {
#ifdef EMSCRIPTEN
			emscripten_cancel_main_loop();
			EM_ASM({
				Module.stopping = Module.stopped = true;
				Module.syncSaves().then(() => postMessage({type: 'stopped'})).catch(() => {});
			});
			emscripten_exit_with_live_runtime();
#else
			std::exit(EXIT_SUCCESS);
#endif
		}
		maniac_output_count = 0;
		maniac_output_time = now;
	}
}

void Output::ManiacConsoleClose() {
#ifdef _WIN32
	if (maniac_console_owned) FreeConsole();
	maniac_console_owned = false;
#elif defined(EMSCRIPTEN) || defined(__ANDROID__)
#ifdef EMSCRIPTEN
	Emscripten_Interface::ConsoleClose();
#else
	EpAndroid::ConsoleClose();
#endif
	maniac_console_input.clear();
	maniac_console_input_position = 0;
#endif
}

bool Output::ManiacConsoleWrite(std::string_view text) {
#if !defined(_WIN32) && !defined(EMSCRIPTEN) && !defined(__ANDROID__)
	if (!ManiacConsoleOpen()) return false;
#endif
#ifdef _WIN32
	const auto units = Utils::DecodeUTF16(text, true);
	DWORD written = 0;
	return WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), units.data(),
		static_cast<DWORD>(units.size()), &written, nullptr) != 0;
#elif defined(EMSCRIPTEN)
	return Emscripten_Interface::ConsoleWrite(text);
#elif defined(__ANDROID__)
	return EpAndroid::ConsoleWrite(text);
#else
	std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
	std::cout.flush();
	return static_cast<bool>(std::cout);
#endif
}

bool Output::ManiacConsoleRead(int capacity, std::string& text) {
#if !defined(_WIN32) && !defined(EMSCRIPTEN) && !defined(__ANDROID__)
	if (!ManiacConsoleOpen()) return false;
#endif
	if (capacity < 1) capacity = 1024;
	std::u16string units;
#ifdef _WIN32
	units.resize(capacity);
	DWORD count = 0;
	if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), units.data(),
		static_cast<DWORD>(units.size()), &count, nullptr)) return false;
	units.resize(count);
#elif defined(EMSCRIPTEN) || defined(__ANDROID__)
#ifdef EMSCRIPTEN
	const auto generation = Emscripten_Interface::ConsoleGeneration();
#else
	const auto generation = EpAndroid::ConsoleGeneration();
#endif
	if (generation != maniac_console_generation) {
		maniac_console_input.clear();
		maniac_console_input_position = 0;
		maniac_console_generation = generation;
	}
	if (maniac_console_input_position == maniac_console_input.size()) {
#ifdef EMSCRIPTEN
		if (!Emscripten_Interface::ReadConsoleLine(maniac_console_input)) return false;
#else
		if (!EpAndroid::ReadConsoleLine(maniac_console_input)) return false;
#endif
		maniac_console_input_position = 0;
	}
	const auto count = std::min(static_cast<size_t>(capacity),
		maniac_console_input.size() - maniac_console_input_position);
	units = maniac_console_input.substr(maniac_console_input_position, count);
	maniac_console_input_position += count;
#else
	(void)text;
	return false;
#endif
	// Native input removes its final two UTF-16 units, even for a limited read.
	units.resize(units.size() >= 2 ? units.size() - 2 : 0);
	const auto nul = units.find(u'\0');
	if (nul != std::u16string::npos) units.resize(nul);
	text = Utils::EncodeUTF(units, true);
	return true;
}

bool Output::ManiacConsoleColor(uint32_t options, int foreground, int background) {
#if !defined(_WIN32) && !defined(EMSCRIPTEN) && !defined(__ANDROID__)
	if (!ManiacConsoleOpen()) return false;
#endif
#ifdef _WIN32
	const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO info = {};
	if (!GetConsoleScreenBufferInfo(handle, &info)) return false;
	WORD attributes = info.wAttributes;
	if (options & 8) attributes = (attributes & ~0x0f) | (foreground & 0x0f);
	if (options & 16) attributes = (attributes & ~0xf0) | ((background & 0x0f) << 4);
	return SetConsoleTextAttribute(handle, attributes) != 0;
#elif defined(EMSCRIPTEN)
	return Emscripten_Interface::ConsoleColor(options, foreground, background);
#elif defined(__ANDROID__)
	return EpAndroid::ConsoleColor(options, foreground, background);
#else
	(void)options; (void)foreground; (void)background;
	return false;
#endif
}

bool Output::ManiacConsoleFocus(bool console) {
#if !defined(_WIN32) && !defined(EMSCRIPTEN) && !defined(__ANDROID__)
	if (!ManiacConsoleOpen()) return false;
#endif
#ifdef _WIN32
	const auto game = DisplayUi ? static_cast<HWND>(DisplayUi->GetNativeWindowHandle()) : nullptr;
	const auto terminal = GetConsoleWindow();
	const auto active = GetActiveWindow();
	if ((active == terminal && !console) || (active == game && console)) {
		const auto target = console ? terminal : game;
		return target && BringWindowToTop(target);
	}
	return true;
#elif defined(EMSCRIPTEN)
	return Emscripten_Interface::ConsoleFocus(console);
#elif defined(__ANDROID__)
	return EpAndroid::ConsoleFocus(console);
#else
	(void)console;
	return false;
#endif
}

static void WriteLog(LogLevel lvl, std::string const& msg, Color const& c = Color()) {
// skip writing log file
#ifndef EMSCRIPTEN
	std::string prefix = Output::LogLevelToString(lvl) + ": ";

	// Every new message is written once to the file.
	// When it is repeated increment a counter until a different message appears,
	// then write the buffered message with the counter.
	if (msg == last_message.msg) {
		last_message.repeat++;
	} else {
		if (last_message.repeat > 0) {
			output_time() << Output::LogLevelToString(last_message.lvl) << ": " << last_message.msg << " [" << last_message.repeat + 1 << "x]" << std::endl;
		}
		output_time() << prefix << msg << '\n';

		last_message.repeat = 0;
		last_message.msg = msg;
		last_message.lvl = lvl;
	}
#endif

	// output to custom logger or terminal
	log_cb(lvl, msg, log_cb_udata);

	// output to overlay
	if (lvl != LogLevel::Debug && lvl != LogLevel::Error) {
		Graphics::GetMessageOverlay().AddMessage(msg, c);
	}
}

static void HandleErrorOutput(const std::string& err) {
	// Drawing directly on the screen because message_overlay is not visible
	// when faded out
	BitmapRef surface = DisplayUi->GetDisplaySurface();
	surface->FillRect(surface->GetRect(), Color(255, 0, 0, 128));

	std::string error = err;
	if (!ignore_pause) error += "\nPress [ENTER] key to exit...";

	Text::Draw(*surface, 11, 11, *Font::DefaultBitmapFont(), Color(0, 0, 0, 255), error);
	Text::Draw(*surface, 10, 10, *Font::DefaultBitmapFont(), Color(255, 255, 255, 255), error);
	DisplayUi->UpdateDisplay();

	if (ignore_pause) { return; }

	Input::ResetKeys();
	while (!Input::IsAnyPressed()) {
#if !defined(USE_LIBRETRO)
		Game_Clock::SleepFor(1ms);
#endif
		if (!DisplayUi->ProcessEvents() || Player::exit_flag) {
			break;
		}

		Input::Update();
	}
}

void Output::Quit() {
	Game_Config::CloseLogFile();
}

bool Output::TakeScreenshot(bool is_auto_screenshot) {
#ifdef EMSCRIPTEN
	Emscripten_Interface::TakeScreenshot(is_auto_screenshot);
	return true;
#else
	auto fs = FileFinder::Save();
	auto p = GetNextScreenshotFileName(fs, is_auto_screenshot);
	return TakeScreenshot(p);
#endif
}

bool Output::TakeScreenshot(std::string_view file) {
	auto ret = FileFinder::Save().OpenOutputStream(file, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);

	if (ret) {
		Output::Debug("Saving Screenshot {}", file);
		return Output::TakeScreenshot(ret);
	}
	return false;
}

bool Output::TakeScreenshot(std::ostream& os) {
	int scale = Player::player_config.screenshot_scale.Get();
	if (scale > 1) {
		auto& disp = DisplayUi->GetDisplaySurface();
		auto scaled_disp = Bitmap::Create(disp->GetWidth() * scale, disp->GetHeight() * scale, false);
		scaled_disp->ZoomOpacityBlit(0, 0, 0, 0, *disp, disp->GetRect(), scale, scale, Opacity::Opaque());
		return scaled_disp->WritePNG(os);
	} else {
		return DisplayUi->GetDisplaySurface()->WritePNG(os);
	}
}

std::string Output::GetScreenshotName(bool is_auto_screenshot) {
	std::string prefix = is_auto_screenshot ? "auto" : "screenshot";

	if (Player::player_config.screenshot_timestamp.Get()) {
		std::time_t t = std::time(nullptr);
		std::tm* tm = std::localtime(&t);
		prefix += "_" + Utils::FormatDate(tm, Utils::DateFormat_YYYYMMDD_HHMMSS);
	}

	return prefix;
}

std::string Output::GetNextScreenshotFileName(FilesystemView fs, bool is_auto_screenshot) {
	std::string output_path;
	int index = 0;
	std::string name = GetScreenshotName(is_auto_screenshot);

	if (Player::player_config.screenshot_timestamp.Get()) {
		output_path = name + ".png";
		if (!fs.Exists(output_path)) {
			return output_path;
		}
		index++;
	}
	name += "_";

	do {
		output_path = name + std::to_string(index++) + ".png";
	} while (fs.Exists(output_path));
	return output_path;
}

void Output::ToggleLog() {
	static bool show_log = true;
	Graphics::GetMessageOverlay().SetShowAll(show_log);
	show_log = !show_log;
}

void Output::ErrorStr(std::string const& err) {
	WriteLog(LogLevel::Error, err);
	std::string error = "Error:\n" + err + "\n\nEasyRPG Player will close now.";

	static bool recursive_call = false;
	if (!recursive_call && DisplayUi) {
		recursive_call = true;
		// Try platform handler first, then global one
		if (!DisplayUi->HandleErrorOutput(error)) {
			HandleErrorOutput(error);
		}
		DisplayUi.reset();
	} else {
		// Fallback to Console if the display is not ready yet
		std::cout << error << std::endl;
#if defined (PLAYER_NINTENDO) || defined(__vita__)
		// stdin is non-blocking
		Game_Clock::SleepFor(5s);
#elif defined (EMSCRIPTEN)
		// Don't show JavaScript Window.prompt from stdin call
		std::cout << " Process finished.";
#else
		std::cout << " Press any key..." << std::endl;
		std::cin.get();
#endif
	}

	Player::exit_code = EXIT_FAILURE;

	// FIXME: No idea how to indicate error from core in libretro
#ifdef EMSCRIPTEN
	// Keep the Worker alive for IDBFS, but never run the failed game again.
	emscripten_cancel_main_loop();
	EM_ASM({
		Module.stopping = Module.stopped = true;
		Module.syncSaves().then(() => postMessage({type: 'stopped'})).catch(() => {});
	});
	emscripten_exit_with_live_runtime();
#else
	exit(Player::exit_code);
#endif
}

void Output::WarningStr(std::string const& warn) {
	if (log_level < LogLevel::Warning) {
		return;
	}
	if (ShouldHideManiacLog(warn)) {
		return;
	}
	WriteLog(LogLevel::Warning, warn, Color(255, 255, 0, 255));
}

void Output::InfoStr(std::string const& msg) {
	if (log_level < LogLevel::Info) {
		return;
	}
	if (ShouldHideManiacLog(msg)) {
		return;
	}
	WriteLog(LogLevel::Info, msg, Color(255, 255, 255, 255));
}

void Output::DebugStr(std::string const& msg) {
	if (log_level < LogLevel::Debug) {
		return;
	}
	if (ShouldHideManiacLog(msg)) {
		return;
	}
	WriteLog(LogLevel::Debug, msg, Color(128, 128, 128, 255));
}
