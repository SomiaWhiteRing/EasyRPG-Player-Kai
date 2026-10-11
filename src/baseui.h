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

#ifndef EP_BASEUI_H
#define EP_BASEUI_H

// Headers
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <bitset>

#include "system.h"
#include "color.h"
#include "font.h"
#include "point.h"
#include "rect.h"
#include "keys.h"
#include "game_config.h"
#include "game_clock.h"
#include "input.h"

#ifdef SUPPORT_AUDIO
struct AudioInterface;
#endif

/**
 * BaseUi base abstract class.
 */
class BaseUi {
public:
	/**
	 * Virtual Destructor.
	 */
	virtual ~BaseUi() {}

	/**
	 * Creates a new BaseUi.
	 *
	 * @param width display client width.
	 * @param height display client height.
	 * @param cfg config options
	 */
	static std::shared_ptr<BaseUi> CreateUi(long width, long height, const Game_Config& cfg);

	/**
	 * Toggles fullscreen.
	 */
	virtual void ToggleFullscreen() {};

	/**
	 * Toggles zoom.
	 */
	virtual void ToggleZoom() {};

	/** Applies Maniac's explicit window zoom; unsupported hosts return false. */
	virtual bool SetManiacWindowZoom(int zoom) { return false; }
	int GetManiacWindowZoom() const { return maniac_window_zoom; }
	virtual bool SetManiacWindowInterpolation(bool enabled) { return false; }
	bool GetManiacWindowInterpolation() const { return maniac_window_interpolation; }
	void SetManiacFullscreenUnevenPixels(bool enabled) { maniac_fullscreen_uneven_pixels = enabled; }
	int GetManiacFullscreenUnevenPixels() const { return maniac_fullscreen_uneven_pixels; }
	virtual bool SetManiacWindowFit(bool enabled) { return false; }
	bool GetManiacWindowFit() const { return maniac_window_fit; }
	virtual bool SetManiacScreenMode(bool fullscreen) { return false; }
	bool GetManiacScreenMode() const { return maniac_screen_mode; }
	void SetManiacFullscreenLayoutOption(int type, bool value) {
		if (type == 5) maniac_fullscreen_layout = value;
		else maniac_fullscreen_stretch = value;
	}
	bool GetManiacFullscreenLayoutOption(int type) const {
		return type == 5 ? maniac_fullscreen_layout : maniac_fullscreen_stretch;
	}
	void LoadManiacVideoDefaults(int fullscreen_override = -1);
	void SaveManiacVideoDefaults();
	void SetManiacSaveVideoDefaults(bool enabled) { maniac_save_video_defaults = enabled; }
	bool GetManiacSaveVideoDefaults() const { return maniac_save_video_defaults; }

	/**
	 * Processes events queue.
	 *
	 * @return When false requests an immediate Player shutdown
	 */
	virtual bool ProcessEvents() = 0;

	/**
	 * Cleans video buffer.
	 */
	void CleanDisplay();

	/**
	 * Updates video buffer.
	 */
	virtual void UpdateDisplay() = 0;

	/**
	 * Gets a copy of the display surface.
	 *
	 * @return bitmap a copy of the display surface.
	 */
	BitmapRef CaptureScreen();

	/**
	 * Sets display title.
	 *
	 * @param title title string.
	 */
	virtual void SetTitle(const std::string & /* title */) {};

	/**
	 * Sets if the cursor should be shown.
	 *
	 * @param flag cursor visibility flag.
	 * @return previous state.
	 */
	virtual bool ShowCursor(bool /* flag */) { return true; };

	/**
	 * Outputs the error message in a custom way depending on platform
	 *
	 * @param message message string.
	 * @return wether error has been handled
	 */
	virtual bool HandleErrorOutput(const std::string & /* message */) { return false; }

	/**
	 * Gets if fullscreen mode is active.
	 *
	 * @return whether fullscreen mode is active.
	 */
	bool IsFullscreen() const;

#ifdef SUPPORT_AUDIO
	/**
	 * Returns audio instance.
	 *
	 * @return audio implementation.
	 */
	virtual AudioInterface& GetAudio() = 0;
#endif

	/** @return dimensions of the window */
	virtual Rect GetWindowMetrics() const;

	/** @return rectangle where the game image is presented inside the window */
	virtual Rect GetPresentationRect() const;

	/**
	 * Gets client width size.
	 *
	 * @return client width size.
	 */
	long GetWidth() const;

	/**
	 * Gets client height size.
	 *
	 * @return client height size.
	 */
	long GetHeight() const;

	/**
	 * Gets whether mouse is hovering the display.
	 *
	 * @return whether mouse is hovering the display.
	 */
	bool GetMouseFocus() const;

	/**
	 * @return mouse position.
	 */
	const Point& GetMousePosition() const;

	/** Windows client dimensions for commands that use native cursor coordinates. */
	bool GetMouseClientSize(Point& size) const;
	/** Read the system cursor in native window client coordinates when available. */
	bool GetMouseClientPosition(Point& position) const;

	/** Attempt to move the system cursor from client coordinates. */
	bool SetMouseClientPosition(Point position) const;

	/**
	 * @return Axis of the analog controller inputs
	 */
	const Input::AnalogInput& GetAnalogInput() const;

	/**
	 * @return Information about touch input
	 */
	std::array<Input::TouchInput, 5>& GetTouchInput();

	BitmapRef const& GetDisplaySurface() const;
	BitmapRef& GetDisplaySurface();

	/**
	 * Requests a resolution change of the framebuffer.
	 *
	 * @param new_width new width
	 * @param new_height new height
	 * @return Whether the resolution change was successful
	 */
	bool ChangeDisplaySurfaceResolution(int new_width, int new_height);

	typedef std::bitset<Input::Keys::KEYS_COUNT> KeyStatus;

	/**
	 * Gets vector with the all keys pressed states.
	 *
	 * @returns vector with the all keys pressed states.
	 */
	KeyStatus& GetKeyStates();

	/** @return true if the display manages the framerate */
	bool IsFrameRateSynchronized() const;

	/** @return true if we should render the fps counter to the screen */
	bool RenderFps() const;

	/** @return true if we should render the fps counter to the title bar */
	bool ShowFpsOnTitle() const;

	/** Toggle between showing FPS or not showing FPS */
	void ToggleShowFps();

	/** Whether we should show fps */
	void SetShowFps(ConfigEnum::ShowFps fps);

	/**
	 * Set whether the program pauses the execution when the program focus is lost.
	 * @param value
	 */
	void SetPauseWhenFocusLost(bool value);
	bool ShouldPauseWhenFocusLost() const;
	virtual void RefreshFocusPolicy() {}
	virtual bool IsFocusPaused() const { return false; }
	virtual bool HasInputFocus() const { return true; }
	virtual bool IsJoystickAvailable() const { return false; }
	virtual bool ConfirmManiacFileOutput() { return false; }
	bool IsRightShiftPressed() const;

	/**
	 * @return the minimum amount of time each physical frame should take.
	 * If the UI manages time (i.e.) vsync, will return a 0 duration.
	 */
	Game_Clock::duration GetFrameLimit() const;

	/**
	 * Sets the frame limit.
	 * Note that this uses int instead of Game_Clock to make the invocation easier.
	 *
	 * @param fps_limit new fps limit
	 */
	void SetFrameLimit(int fps_limit);

	/** Sets the scaling mode of the window */
	virtual void SetScalingMode(ConfigEnum::ScalingMode) {};

	/**
	 * Sets the game resolution settings.
	 * Not to be confused with WinW/WinH setting from the ini.
	 * This is for configuring a resolution that is effective for all games.
	 *
	 * @param resolution new resolution
	 */
	void SetGameResolution(ConfigEnum::GameResolution resolution);

	/**
	 * Opens the specified URL through the operating system.
	 * Opens a file browser when file:// is provided.
	 *
	 * @param url URL to open
	 * @return true when successful
	 */
	virtual bool OpenURL(std::string_view path) { (void)path; return false; }

	/** @return native window handle when available, otherwise nullptr */
	virtual void* GetNativeWindowHandle() const { return nullptr; }

	/** Toggles "stretch to screen width" on or off */
	virtual void ToggleStretch() {};

	/** Turns vsync on or off */
	virtual void ToggleVsync() {};

	/** Turns a touch ui on or off. */
	virtual void ToggleTouchUi() {};

	/**
	 * @return current video options.
	 */
	Game_ConfigVideo GetConfig() const;

protected:
	/**
	 * Protected Constructor. Use CreateUi instead.
	 */
	explicit BaseUi(const Game_Config& cfg);

	void SetFrameRateSynchronized(bool value);
	void SetIsFullscreen(bool value);
	virtual void vGetConfig(Game_ConfigVideo& cfg) const = 0;
	virtual bool vChangeDisplaySurfaceResolution(int new_width, int new_height);

	Game_ConfigVideo vcfg;

	/**
	 * Display mode data struct.
	 */
	struct DisplayMode {
		int zoom = 0;
		int width = 0;
		int height = 0;
		uint32_t flags = 0;
		uint8_t bpp = 0;
		bool effective = false;
		bool vsync = false;
	};

	/** Current display mode. */
	DisplayMode current_display_mode;

	KeyStatus keys;

	/** Surface used for zoom. */
	BitmapRef main_surface;

	/** Mouse position on screen relative to the window. */
	Point mouse_pos;

	/** Axis of game controllers */
	Input::AnalogInput analog_input;

	/** Touch inputs for up to five finger */
	std::array<Input::TouchInput, 5> touch_input;

	/** State of the 5 fingers (true touched, false not) */
	std::array<bool, 5> finger_input;

	/** Color for display background. */
	Color back_color = Color{ 0, 0, 0, 255 };

	/** Mouse hovering the window flag. */
	bool mouse_focus = false;

	/** The amount of time each frame should take, based on fps limit */
	Game_Clock::duration frame_limit = Game_Clock::GetTargetGameTimeStep();

	/** Cursor visibility flag. */
	bool cursor_visible = false;

	/** Ui manages frame rate externally */
	bool external_frame_rate = false;

	/** Used by the F2 toggle: Remembers which configuration (ON or Overlay) was used */
	ConfigEnum::ShowFps original_fps_show_state = ConfigEnum::ShowFps::OFF;

	/** Raw Maniac cursor scaling starts at zero (clamped to one) before an option request. */
	int maniac_window_zoom = 0;
	bool maniac_window_interpolation = false;
	bool maniac_window_interpolation_requested = false;
	uint8_t maniac_fullscreen_uneven_pixels = 0;
	bool maniac_window_fit = true;
	bool maniac_window_fit_requested = false;
	bool maniac_screen_mode = false;
	bool maniac_screen_mode_requested = false;
	bool maniac_fullscreen_layout = false;
	bool maniac_fullscreen_stretch = false;
	bool maniac_active_fullscreen_layout = false;
	bool maniac_active_fullscreen_stretch = false;
	bool maniac_save_video_defaults = true;
	bool maniac_video_defaults_loaded = false;
	std::array<uint8_t, 16> maniac_video_defaults{};
	void ApplyManiacScreenMode(bool fullscreen) {
		maniac_screen_mode = fullscreen;
		maniac_screen_mode_requested = true;
		maniac_active_fullscreen_layout = maniac_fullscreen_layout;
		maniac_active_fullscreen_stretch = maniac_fullscreen_stretch;
	}
	Rect ManiacFullscreenRect(int width, int height) const {
		int w = width, h = height;
		if (!maniac_active_fullscreen_layout) {
			const int scale = std::max(1, static_cast<int>(std::min(width / GetWidth(), height / GetHeight())));
			w = GetWidth() * scale;
			h = GetHeight() * scale;
		} else if (!maniac_active_fullscreen_stretch) {
			if (int64_t(width) * GetHeight() > int64_t(height) * GetWidth())
				w = static_cast<int>(int64_t(height) * GetWidth() / GetHeight());
			else h = static_cast<int>(int64_t(width) * GetHeight() / GetWidth());
		}
		return {(width - w) / 2, (height - h) / 2, w, h};
	}
	int ManiacWindowZoomForBounds(int requested, int width, int height) const {
		const int64_t numerator = maniac_window_fit ? 2 : 3;
		const int limit_x = static_cast<int>(width * numerator / (2 * GetWidth()));
		const int limit_y = static_cast<int>(height * numerator / (2 * GetHeight()));
		return std::max(1, std::min({requested, limit_x, limit_y}));
	}
	bool UseManiacWindowInterpolation() const {
#ifdef __ANDROID__
		// An explicit zoom uses a window-sized game viewport inside the Activity.
		return maniac_window_interpolation_requested && !maniac_screen_mode
			&& (maniac_window_zoom > 0 || maniac_window_fit_requested || maniac_screen_mode_requested);
#else
		return maniac_window_interpolation_requested && !IsFullscreen();
#endif
	}
};

/** Global DisplayUi variable. */
extern std::shared_ptr<BaseUi> DisplayUi;

inline Rect BaseUi::GetWindowMetrics() const {
	return {-1, -1, -1, -1};
}

inline Rect BaseUi::GetPresentationRect() const {
	return {0, 0, static_cast<int>(GetWidth()), static_cast<int>(GetHeight())};
}

inline bool BaseUi::IsFrameRateSynchronized() const {
	return external_frame_rate;
}

inline void BaseUi::SetFrameRateSynchronized(bool value) {
	external_frame_rate = value;
}

inline bool BaseUi::IsFullscreen() const {
	return vcfg.fullscreen.Get();
}

inline void BaseUi::SetIsFullscreen(bool fs) {
	vcfg.fullscreen.Set(fs);
}

inline BaseUi::KeyStatus& BaseUi::GetKeyStates() {
	return keys;
}

inline BitmapRef const& BaseUi::GetDisplaySurface() const {
	return main_surface;
}

inline BitmapRef& BaseUi::GetDisplaySurface() {
	return main_surface;
}

inline bool BaseUi::vChangeDisplaySurfaceResolution(int new_width, int new_height) {
	(void)new_width;
	(void)new_height;
	return false;
}

inline long BaseUi::GetWidth() const {
	return current_display_mode.width;
}

inline long BaseUi::GetHeight() const {
	return current_display_mode.height;
}

inline bool BaseUi::GetMouseFocus() const {
	return mouse_focus;
}

inline const Point& BaseUi::GetMousePosition() const {
	return mouse_pos;
}

inline const Input::AnalogInput& BaseUi::GetAnalogInput() const {
	return analog_input;
}

inline std::array<Input::TouchInput, 5>& BaseUi::GetTouchInput() {
	return touch_input;
}

inline bool BaseUi::RenderFps() const {
	return vcfg.fps.Get() == ConfigEnum::ShowFps::Overlay || (IsFullscreen() && vcfg.fps.Get() == ConfigEnum::ShowFps::ON);
}

inline bool BaseUi::ShowFpsOnTitle() const {
	return vcfg.fps.Get() == ConfigEnum::ShowFps::ON;
}

inline void BaseUi::ToggleShowFps() {
	if (vcfg.fps.Get() != ConfigEnum::ShowFps::OFF) {
		original_fps_show_state = vcfg.fps.Get();
		vcfg.fps.Set(ConfigEnum::ShowFps::OFF);
	} else {
		if (original_fps_show_state == ConfigEnum::ShowFps::OFF) {
			vcfg.fps.Set(ConfigEnum::ShowFps::ON);
		} else {
			vcfg.fps.Set(original_fps_show_state);
		}
	}
}

inline void BaseUi::SetShowFps(ConfigEnum::ShowFps fps) {
	vcfg.fps.Set(fps);
	original_fps_show_state = fps;
}

inline void BaseUi::SetPauseWhenFocusLost(bool value) {
	vcfg.pause_when_focus_lost.Set(value);
}

inline Game_Clock::duration BaseUi::GetFrameLimit() const {
	return IsFrameRateSynchronized() ? Game_Clock::duration(0) : frame_limit;
}

inline void BaseUi::SetFrameLimit(int fps_limit) {
	vcfg.fps_limit.Set(fps_limit);

	frame_limit = (fps_limit == 0 ? Game_Clock::duration(0) : Game_Clock::TimeStepFromFps(fps_limit));
}

#endif
