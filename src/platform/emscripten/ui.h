#ifndef EP_WEB_UI_H
#define EP_WEB_UI_H

#include "baseui.h"
#include "platform/emscripten/audio.h"

// The browser owns the window; mixing runs independently in the audio Worker.
class WebUi final : public BaseUi {
public:
	WebUi(int width, int height, const Game_Config& cfg);
	bool ProcessEvents() override;
	void RefreshFocusPolicy() override;
	bool IsFocusPaused() const override;
	void SetWindowFocus(bool value) { window_focused = value; }
	bool HasInputFocus() const override { return window_focused; }
	bool IsJoystickAvailable() const override;
	bool ConfirmManiacFileOutput() override;
	void UpdateDisplay() override;
	bool vChangeDisplaySurfaceResolution(int width, int height) override;
	void vGetConfig(Game_ConfigVideo& cfg) const override;
	void ToggleFullscreen() override;
	bool SetManiacWindowZoom(int zoom) override;
	bool SetManiacWindowInterpolation(bool enabled) override;
	bool SetManiacWindowFit(bool enabled) override;
	bool SetManiacScreenMode(bool fullscreen) override;
	bool ShowCursor(bool visible) override;
	bool OpenURL(std::string_view url) override;
	AudioInterface& GetAudio() override { return audio; }
	void Mouse(int x, int y, bool focus) { mouse_pos = {x, y}; mouse_focus = focus; }
	void Fullscreen(bool value) { SetIsFullscreen(value); }
	void Gamepad(float x, float y, float rx, float ry, float lt, float rt) {
		analog_input.primary = {x, y}; analog_input.secondary = {rx, ry};
		analog_input.trigger_left = lt; analog_input.trigger_right = rt;
	}
private:
	WebAudio audio;
	bool window_focused = true;
};
#endif
