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
#include "baseui.h"
#include "bitmap.h"
#include "player.h"
#include "main_data.h"
#include "game_system.h"
#include "async_handler.h"
#include "filesystem_stream.h"
#include "output.h"

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(PLAYER_WEB)
#  include "platform/emscripten/ui.h"
#elif USE_SDL==3
#  include "platform/sdl/sdl3_ui.h"
#elif USE_SDL==2
#  include "platform/sdl/sdl2_ui.h"
#elif USE_SDL==1
#  include "platform/sdl/sdl_ui.h"
#elif USE_LIBRETRO
#  include "platform/libretro/ui.h"
#elif defined(__3DS__)
#  include "platform/3ds/ui.h"
#elif defined(__vita__)
#  include "platform/psvita/ui.h"
#elif defined(__SWITCH__)
#  include "platform/switch/ui.h"
#endif

std::shared_ptr<BaseUi> DisplayUi;

bool BaseUi::GetMouseClientSize(Point& size) const {
#ifdef _WIN32
	const auto hwnd = static_cast<HWND>(GetNativeWindowHandle());
	RECT client = {};
	if (hwnd && GetClientRect(hwnd, &client)) {
		size = {client.right - client.left, client.bottom - client.top};
		return true;
	}
#endif
	return false;
}

bool BaseUi::GetMouseClientPosition(Point& position) const {
#ifdef _WIN32
	const auto hwnd = static_cast<HWND>(GetNativeWindowHandle());
	POINT point = {};
	if (hwnd && GetCursorPos(&point) && ScreenToClient(hwnd, &point)) {
		position = {point.x, point.y};
		return true;
	}
#endif
	return false;
}

bool BaseUi::SetMouseClientPosition(Point position) const {
#ifdef _WIN32
	const auto hwnd = static_cast<HWND>(GetNativeWindowHandle());
	if (hwnd) {
		POINT point = {position.x, position.y};
		// Match the native command: attempt both calls and ignore OS failure.
		ClientToScreen(hwnd, &point);
		SetCursorPos(point.x, point.y);
		return true;
	}
#endif
	return false;
}

bool BaseUi::IsRightShiftPressed() const {
	if (!HasInputFocus()) return false;
#ifdef _WIN32
	return (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
#else
	return Input::IsRawKeyPressed(Input::Keys::RSHIFT);
#endif
}

bool BaseUi::ShouldPauseWhenFocusLost() const {
	if (Player::IsPatchManiac() && Main_Data::game_system)
		return !Main_Data::game_system->IsManiacRunWhenInactive();
	auto cfg = vcfg;
	vGetConfig(cfg);
	return cfg.pause_when_focus_lost.Get();
}

std::shared_ptr<BaseUi> BaseUi::CreateUi(long width, long height, const Game_Config& cfg) {
#if defined(PLAYER_WEB)
	return std::make_shared<WebUi>(width, height, cfg);
#elif USE_SDL==3
	return std::make_shared<Sdl3Ui>(width, height, cfg);
#elif USE_SDL==2
	return std::make_shared<Sdl2Ui>(width, height, cfg);
#elif USE_SDL==1
	return std::make_shared<SdlUi>(width, height, cfg);
#elif defined(PLAYER_UI)
	return std::make_shared<PLAYER_UI>(width, height, cfg);
#else
#  error cannot create UI
#endif
}

BaseUi::BaseUi(const Game_Config& cfg)
{
	keys.reset();

	vcfg = cfg.video;

	auto fps_limit = vcfg.fps_limit.Get();
	frame_limit = (fps_limit == 0 ? Game_Clock::duration(0) : Game_Clock::TimeStepFromFps(fps_limit));
}

void BaseUi::LoadManiacVideoDefaults(int fullscreen_override) {
	if (maniac_video_defaults_loaded) return;
	maniac_video_defaults_loaded = true;
	maniac_video_defaults[4] = 2;
#ifdef _WIN32
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\KADOKAWA\\rpg2003", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
		std::array<uint8_t, 16> saved{};
		DWORD size = saved.size(), type = 0;
		if (RegQueryValueExW(key, L"VideoSettings", nullptr, &type, saved.data(), &size) == ERROR_SUCCESS
			&& type == REG_BINARY && size == saved.size())
			maniac_video_defaults = saved;
		RegCloseKey(key);
	}
#else
	const auto fs = Game_Config::GetGlobalConfigFilesystem();
	if (fs) {
		auto input = fs.OpenInputStream("ManiacVideoSettings.bin", std::ios_base::in | std::ios_base::binary);
		std::array<uint8_t, 16> saved{};
		if (input && input.read(reinterpret_cast<char*>(saved.data()), saved.size()))
			maniac_video_defaults = saved;
	}
#endif
	const auto& saved = maniac_video_defaults;
	const uint32_t zoom = uint32_t(saved[4]) | (uint32_t(saved[5]) << 8)
		| (uint32_t(saved[6]) << 16) | (uint32_t(saved[7]) << 24);
	SetManiacWindowZoom(std::clamp(static_cast<int32_t>(zoom), 1, 4));
	SetManiacWindowFit(saved[2] == 0);
	SetManiacWindowInterpolation(saved[8] != 0);
	// The fixed runtime exposes this byte through option4 without a draw hook.
	// Preserve an imported byte until the command setter normalizes it.
	maniac_fullscreen_uneven_pixels = saved[12];
	SetManiacFullscreenLayoutOption(5, saved[10] != 0);
	SetManiacFullscreenLayoutOption(6, saved[11] != 0);
	bool fullscreen = saved[1] != 0;
	if (fullscreen_override >= 0) fullscreen = fullscreen_override != 0;
	SetManiacScreenMode(fullscreen);
#ifdef _WIN32
	const auto shown = ConfigEnum::ShowFps::ON;
#else
	const auto shown = ConfigEnum::ShowFps::Overlay;
#endif
	SetShowFps(saved[13] != 0 ? shown : ConfigEnum::ShowFps::OFF);
}

void BaseUi::SaveManiacVideoDefaults() {
	if (!maniac_video_defaults_loaded || !maniac_save_video_defaults) return;
	auto current = maniac_video_defaults;
	current[1] = maniac_screen_mode;
	current[2] = !maniac_window_fit;
	const uint32_t zoom = static_cast<uint32_t>(maniac_window_zoom);
	for (int i = 0; i < 4; ++i) current[4 + i] = (zoom >> (i * 8)) & 255;
	current[8] = maniac_window_interpolation;
	current[10] = maniac_fullscreen_layout;
	current[11] = maniac_fullscreen_stretch;
	current[12] = maniac_fullscreen_uneven_pixels;
	current[13] = RenderFps() || ShowFpsOnTitle();
	if (current == maniac_video_defaults) return;
	current[0] = 1;
#ifdef _WIN32
	HKEY key;
	bool saved = false;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\KADOKAWA\\rpg2003", 0, nullptr, 0,
		KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
		saved = RegSetValueExW(key, L"VideoSettings", 0, REG_BINARY, current.data(), current.size()) == ERROR_SUCCESS;
		RegCloseKey(key);
	}
	if (!saved) Output::Warning("Maniac video defaults could not be saved");
#else
	const auto fs = Game_Config::GetGlobalConfigFilesystem();
	if (fs) {
		auto output = fs.OpenOutputStream("ManiacVideoSettings.bin", std::ios_base::out | std::ios_base::binary);
		if (output) {
			output.write(reinterpret_cast<const char*>(current.data()), current.size());
			output.Close();
			AsyncHandler::SaveFilesystem();
		} else Output::Warning("Maniac video defaults could not be saved");
	}
#endif
	// The native writer records the comparison baseline even when its OS write fails.
	maniac_video_defaults = current;
}

BitmapRef BaseUi::CaptureScreen() {
	BitmapRef capture = Bitmap::Create(main_surface->width(), main_surface->height(), false);
	capture->BlitFast(0, 0, *main_surface, main_surface->GetRect(), Opacity::Opaque());
	return capture;
}

void BaseUi::CleanDisplay() {
	main_surface->Clear();
}

void BaseUi::SetGameResolution(ConfigEnum::GameResolution resolution) {
	vcfg.game_resolution.Set(resolution);
}

Game_ConfigVideo BaseUi::GetConfig() const {
	Game_ConfigVideo cfg = vcfg;

	cfg.Hide();

	vGetConfig(cfg);

	Rect metrics = GetWindowMetrics();
	cfg.window_x.Set(metrics.x);
	cfg.window_y.Set(metrics.y);
	cfg.window_width.Set(metrics.width);
	cfg.window_height.Set(metrics.height);

	if (!cfg.fullscreen.IsOptionVisible()) {
		cfg.fps.RemoveFromValidSet(ConfigEnum::ShowFps::Overlay);
	}

	if (cfg.vsync.IsOptionVisible()
			&& cfg.vsync.Get()) {
		cfg.fps_limit.SetLocked(true);
		cfg.fps_limit.SetDescription("This option requires V-Sync to be disabled");
	}

	if (cfg.fullscreen.IsOptionVisible()
			&& cfg.fullscreen.Get()) {
		cfg.window_zoom.SetLocked(true);
		cfg.window_zoom.SetDescription("This option requires to be in windowed mode");
	}

	if (Player::has_custom_resolution) {
		cfg.game_resolution.SetLocked(true);
		cfg.game_resolution.SetDescription("This game uses a custom resolution");
	}

	return cfg;
}

bool BaseUi::ChangeDisplaySurfaceResolution(int new_width, int new_height) {
	if (new_width == current_display_mode.width && new_height == current_display_mode.height) {
		return true;
	}

	return vChangeDisplaySurfaceResolution(new_width, new_height);
}
