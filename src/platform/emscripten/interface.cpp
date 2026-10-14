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

#include "interface.h"

#include <emscripten.h>
#include <emscripten/bind.h>
#include <lcf/lsd/reader.h>
#include <sstream>
#include <cstdint>
#include <cstdlib>

#include "system.h"
#include "async_handler.h"
#include "baseui.h"
#include "filefinder.h"
#include "filesystem_stream.h"
#include "player.h"
#include "scene_save.h"
#include "output.h"

EM_JS(bool, ConsoleCommandJs, (const char* op, const char* text, size_t size, int options, int foreground, int background), {
	return Module.consoleCommand({op: UTF8ToString(op),
		text: text ? new TextDecoder().decode(HEAPU8.subarray(text, text + size)) : '',
		options, foreground, background});
});

EM_JS(uint32_t, ConsoleGenerationJs, (), {
	return Module.getConsoleGeneration();
});

bool Emscripten_Interface::ConsoleOpen(bool update_window) {
	const auto title = "Console - " + Player::game_title;
	return ConsoleCommandJs("open", title.data(), title.size(), update_window, 0, 0);
}

void Emscripten_Interface::ConsoleClose() {
	ConsoleCommandJs("close", nullptr, 0, 0, 0, 0);
}

bool Emscripten_Interface::ConsoleWrite(std::string_view text) {
	return ConsoleCommandJs("write", text.data(), text.size(), 0, 0, 0);
}

bool Emscripten_Interface::ConsoleColor(uint32_t options, int foreground, int background) {
	return ConsoleCommandJs("color", nullptr, 0, options, foreground, background);
}

bool Emscripten_Interface::ConsoleFocus(bool console) {
	return ConsoleCommandJs("focus", nullptr, 0, console, 0, 0);
}

uint32_t Emscripten_Interface::ConsoleGeneration() {
	return ConsoleGenerationJs();
}

EM_ASYNC_JS(uintptr_t, ReadConsoleLineJs, (const char* title), {
	const caption = UTF8ToString(title);
	let value;
	try { value = await Module.readConsoleLine(caption); }
	catch { return 0; }
	if (typeof value !== 'string') return 0;
	const line = value + '\r\n';
	const pointer = _malloc(4 + line.length * 2);
	if (!pointer) return 0;
	HEAPU32[pointer >>> 2] = line.length;
	for (let i = 0; i < line.length; ++i) HEAPU16[((pointer + 4) >>> 1) + i] = line.charCodeAt(i);
	return pointer;
});

bool Emscripten_Interface::ReadConsoleLine(std::u16string& line) {
	const auto title = "Console - " + Player::game_title;
	const auto pointer = ReadConsoleLineJs(title.c_str());
	if (!pointer) return false;
	const auto* count = reinterpret_cast<const uint32_t*>(pointer);
	const auto* units = reinterpret_cast<const char16_t*>(pointer + 4);
	line.assign(units, units + *count);
	std::free(reinterpret_cast<void*>(pointer));
	return true;
}

EM_ASYNC_JS(bool, ConfirmFileOutputJs, (), {
	return await Module.confirmFileOutput();
});

bool Emscripten_Interface::ConfirmManiacFileOutput() {
	return ConfirmFileOutputJs();
}

void Emscripten_Interface::Reset() {
	Player::reset_flag = true;
}

bool Emscripten_Interface::DownloadSavegame(int slot) {
	auto fs = FileFinder::Save();
	std::string name = Scene_Save::GetSaveFilename(fs, slot);
	auto is = fs.OpenInputStream(name);
	if (!is) {
		return false;
	}
	auto save_buffer = Utils::ReadStream(is);
	std::string filename = std::get<1>(FileFinder::GetPathAndFilename(name));
	EM_ASM_ARGS({
		Module.api_private.download_js($0, $1, $2);
	}, save_buffer.data(), save_buffer.size(), filename.c_str());
	return true;
}

void Emscripten_Interface::UploadSavegame(int slot) {
	EM_ASM_INT({
		Module.api_private.uploadSavegame_js($0);
	}, slot);
}

void Emscripten_Interface::UploadSoundfont() {
	EM_ASM_INT({
		Module.api_private.uploadSoundfont_js($0);
	});
}

void Emscripten_Interface::UploadFont() {
	EM_ASM_INT({
	   Module.api_private.uploadFont_js($0);
   });
}

void Emscripten_Interface::RefreshScene() {
	Scene::instance->Refresh();
}

void Emscripten_Interface::TakeScreenshot(bool is_auto_screenshot) {
	static int index = 0;
	std::ostringstream os;
	Output::TakeScreenshot(os);
	std::string screenshot = os.str();
	std::string filename = Output::GetScreenshotName(is_auto_screenshot);
	if (!Player::player_config.screenshot_timestamp.Get()) {
		filename += "_" + std::to_string(index++) + ".png";
	} else {
		filename += ".png";
	}
	EM_ASM_ARGS({
		Module.api_private.download_js($0, $1, $2);
	}, screenshot.data(), screenshot.size(), filename.c_str());
}

bool Emscripten_Interface_Private::UploadSavegameStep2(int slot, int buffer_addr, int size) {
	auto fs = FileFinder::Save();
	std::string name = Scene_Save::GetSaveFilename(fs, slot);

	std::istream is(new Filesystem_Stream::InputMemoryStreamBufView(lcf::Span<uint8_t>(reinterpret_cast<uint8_t*>(buffer_addr), size)));

	if (!lcf::LSD_Reader::Load(is, "", Player::IsPatchManiac())) {
		Output::Warning("Selected file is not a valid savegame");
		return false;
	}

	{
		auto os = fs.OpenOutputStream(name);
		if (!os)
			return false;
		os.write(reinterpret_cast<char*>(buffer_addr), size);
	}

	AsyncHandler::SaveFilesystem();

	return true;
}

bool Emscripten_Interface_Private::UploadSoundfontStep2(std::string filename, int buffer_addr, int size) {
	auto fs = Game_Config::GetSoundfontFilesystem();
	if (!fs) {
		Output::Warning("Cannot access Soundfont directory");
		return false;
	}

	std::string name = std::get<1>(FileFinder::GetPathAndFilename(filename));

	// TODO: No good way to sanitize this, would require launching an entire, second fluidsynth session
	if (!EndsWith(name, ".sf2")) {
		Output::Warning("Selected file is not a valid soundfont");
		return false;
	}

	{
		auto os = fs.OpenOutputStream(name);
		if (!os)
			return false;
		os.write(reinterpret_cast<char*>(buffer_addr), size);
	}

	AsyncHandler::SaveFilesystem();

	return true;
}

bool Emscripten_Interface_Private::UploadFontStep2(std::string filename, int buffer_addr, int size) {
	auto fs = Game_Config::GetFontFilesystem();
	if (!fs) {
		Output::Warning("Cannot access Font directory");
		return false;
	}

	std::string name = std::get<1>(FileFinder::GetPathAndFilename(filename));

	Filesystem_Stream::InputStream is(new Filesystem_Stream::InputMemoryStreamBufView(lcf::Span<uint8_t>(reinterpret_cast<uint8_t*>(buffer_addr), size)), filename);
	if (!Font::CreateFtFont(std::move(is), 12, false, false)) {
		Output::Warning("Selected file is not a valid font");
		return false;
	}

	{
		auto os = fs.OpenOutputStream(name);
		if (!os)
			return false;
		os.write(reinterpret_cast<char*>(buffer_addr), size);
	}

	AsyncHandler::SaveFilesystem();

	return true;
}

bool Emscripten_Interface::ResetCanvas() {
	DisplayUi.reset();
	DisplayUi = BaseUi::CreateUi(Player::screen_width, Player::screen_height, Player::ParseCommandLine());
	return DisplayUi != nullptr;
}

// Binding code
EMSCRIPTEN_BINDINGS(player_interface) {
	emscripten::class_<Emscripten_Interface>("api")
		.class_function("requestReset", &Emscripten_Interface::Reset)
		.class_function("downloadSavegame", &Emscripten_Interface::DownloadSavegame)
		.class_function("uploadSavegame", &Emscripten_Interface::UploadSavegame)
#if defined(HAVE_FLUIDSYNTH) || defined(HAVE_FLUIDLITE)
		.class_function("uploadSoundfont", &Emscripten_Interface::UploadSoundfont)
#endif
#if defined(HAVE_FREETYPE)
		.class_function("uploadFont", &Emscripten_Interface::UploadFont)
#endif
		.class_function("refreshScene", &Emscripten_Interface::RefreshScene)
		.class_function("takeScreenshot", &Emscripten_Interface::TakeScreenshot)
		.class_function("resetCanvas", &Emscripten_Interface::ResetCanvas)
	;

	emscripten::class_<Emscripten_Interface_Private>("api_private")
		.class_function("uploadSavegameStep2", &Emscripten_Interface_Private::UploadSavegameStep2)
#if defined(HAVE_FLUIDSYNTH) || defined(HAVE_FLUIDLITE)
		.class_function("uploadSoundfontStep2", &Emscripten_Interface_Private::UploadSoundfontStep2)
#endif
#if defined(HAVE_FREETYPE)
		.class_function("uploadFontStep2", &Emscripten_Interface_Private::UploadFontStep2)
#endif
	;
}
