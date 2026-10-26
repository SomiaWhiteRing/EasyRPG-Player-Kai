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

#ifndef EP_EMSCRIPTEN_INTERFACE_H
#define EP_EMSCRIPTEN_INTERFACE_H

#include <string>
#include <string_view>
#include <cstdint>

class Emscripten_Interface {
public:
	static bool ConsoleOpen(bool update_window);
	static void ConsoleClose();
	static bool ConsoleWrite(std::string_view text);
	static bool ConsoleColor(uint32_t options, int foreground, int background);
	static bool ConsoleFocus(bool console);
	static uint32_t ConsoleGeneration();
	static bool ReadConsoleLine(std::u16string& line);
	static bool ConfirmManiacFileOutput();
	static bool DownloadSavegame(int slot);
	static void UploadSavegame(int slot);
    static void UploadSoundfont();
    static void UploadFont();
    static void RefreshScene();
	static void TakeScreenshot(bool is_auto_screenshot = false);
	static void Reset();
	static bool ResetCanvas();
};

class Emscripten_Interface_Private {
public:
	static bool UploadSavegameStep2(int slot, int buffer_addr, int size);
    static bool UploadFontStep2(std::string name, int buffer_addr, int size);
    static bool UploadSoundfontStep2(std::string name, int buffer_addr, int size);
};

#endif
