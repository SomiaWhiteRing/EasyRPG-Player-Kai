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

#ifndef EP_SCENE_NAME_H
#define EP_SCENE_NAME_H

#include <bitset>
#include "keys.h"
#include "name_input_candidate.h"
#include "scene.h"
#include "window_command.h"
#include "window_name.h"
#include "window_face.h"
#include "window_keyboard.h"
#include "window_pinyin.h"

/**
 * Scene_Item class.
 */
class Scene_Name : public Scene {

public:
	/**
	 * Constructor.
	 */
	Scene_Name(Game_Actor& actor, int charset, bool use_default_name, std::vector<NameInputCandidate> candidates = {});
	~Scene_Name() override;

	void Start() override;
	void Continue(SceneType prev_scene) override;
	void Suspend(SceneType next_scene) override;
	void vUpdate() override;

protected:
	std::vector<Window_Keyboard::Mode> layouts;
	int layout_index = 0;
	bool use_default_name = false;

private:
	enum class Mode {
		Choice,
		Input,
	};

	void CreateFaceWindow();
	void CreateNameWindow();
	void SetupLayouts();
	void CreateKeyboardWindow();
	void CreateChoiceWindow();
	void EnterChoiceMode();
	void EnterKeyboardInputMode();
	std::vector<std::string> GetChoiceLabels() const;
	bool IsPinyinMode() const;
	void RefreshPinyinWindow();
	void RefreshKeyboardHint();
	void ResetPinyinInput();
	void AppendPinyinLetter(std::string_view key);
	bool CommitPinyinSelection();
	bool SelectPinyinCandidateByNumber(std::string_view key);
	void SetPhysicalKeyboardInput(bool enabled);
	void StartTextInput();
	bool HandleSystemTextInput();
	void RestoreInputMask();
	bool HandlePhysicalKeyboardInput();
	void EraseInput();
	void FinishNameInput();

	Game_Actor& actor;
	Mode mode = Mode::Input;
	const char* keyboard_done = Window_Keyboard::DONE;
	std::vector<NameInputCandidate> candidates;
	std::string pinyin_query;
	std::bitset<Input::Keys::KEYS_COUNT> previous_keymask;
	bool keyboard_input = false;
	bool system_input = false;
	bool input_masked = false;
	bool wait_for_key_release = false;
	int backspace_frames = 0;

	std::unique_ptr<Window_Keyboard> kbd_window;
	std::unique_ptr<Window_Pinyin> pinyin_window;
	std::unique_ptr<Window_Base> keyboard_hint_window;
	std::unique_ptr<Window_Command> choice_window;
	std::unique_ptr<Window_Name> name_window;
	std::unique_ptr<Window_Face> face_window;
};

#endif
