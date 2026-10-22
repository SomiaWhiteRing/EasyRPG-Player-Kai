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

#include <algorithm>
#include <cassert>
#include <cctype>
#include <string>

#include "scene_name.h"
#include "game_actors.h"
#include "game_system.h"
#include "input.h"
#include "pinyin_input.h"
#include "player.h"
#include "output.h"
#include "bitmap.h"
#include "font.h"
#ifdef __ANDROID__
#include "platform/android/android.h"
#elif defined(EMSCRIPTEN)
#include "platform/emscripten/interface.h"
#endif

namespace {
	constexpr int kMarginX = 32;
	constexpr int kMarginY = 8;
	constexpr int kWindowFaceWidth = 64;
	constexpr int kWindowFaceHeight = 64;
	constexpr int kWindowNameWidth = 192;
	constexpr int kWindowNameHeight = 32;
	constexpr int kWindowKeyboardWidth = 256;
	constexpr int kWindowKeyboardHeight = 160;
	constexpr int kWindowPinyinHeight = 48;
	constexpr int kKeyboardHintWidth = 224;
	constexpr int kKeyboardHintHeight = 48;
	constexpr int kWindowChoiceVisibleItems = 9;
	const char kManualInputLabel[] = "\xE4\xB8\xBB\xE5\x8A\xA8\xE8\xBE\x93\xE5\x85\xA5";

	char GetTriggeredRawNumber() {
		for (int number = 0; number <= 9; ++number) {
			if (Input::IsRawKeyTriggered(static_cast<Input::Keys::InputKey>(Input::Keys::N0 + number))
					|| Input::IsRawKeyTriggered(static_cast<Input::Keys::InputKey>(Input::Keys::KP0 + number))) {
				return static_cast<char>('0' + number);
			}
		}
		return '\0';
	}
}

Scene_Name::Scene_Name(Game_Actor& actor, int charset, bool use_default_name, std::vector<NameInputCandidate> candidates)
	: layout_index(charset), use_default_name(use_default_name), actor(actor), candidates(std::move(candidates))
{
	Scene::type = Scene::Name;
}

Scene_Name::~Scene_Name() {
	RestoreInputMask();
}

void Scene_Name::Continue(SceneType) {
	RefreshPinyinWindow();
}

void Scene_Name::Suspend(SceneType) {
	RestoreInputMask();
	if (kbd_window) {
		kbd_window->SetActive(true);
	}
}

void Scene_Name::Start() {
	CreateFaceWindow();
	CreateNameWindow();
	SetupLayouts();

	if (candidates.empty()) {
		EnterKeyboardInputMode();
	} else {
		EnterChoiceMode();
	}
}

void Scene_Name::CreateFaceWindow() {
	face_window.reset(new Window_Face(Player::menu_offset_x + kMarginX, Player::menu_offset_y + kMarginY, kWindowFaceWidth, kWindowFaceHeight));
	face_window->Set(actor);
	face_window->Refresh();
}

void Scene_Name::CreateNameWindow() {
	name_window.reset(new Window_Name(Player::menu_offset_x + kWindowFaceWidth + kMarginX, Player::menu_offset_y + kMarginY + 32, kWindowNameWidth, kWindowNameHeight));
	name_window->Set(use_default_name ? ToString(actor.GetName()) : "");
}

void Scene_Name::SetupLayouts() {
	layouts.clear();
	keyboard_done = Window_Keyboard::DONE;
	// Japanese pages
	if (Player::IsCP932()) {
		layouts.push_back(Window_Keyboard::Hiragana);
		layouts.push_back(Window_Keyboard::Katakana);
		keyboard_done = Window_Keyboard::DONE_JP;
	// Korean pages
	} else if (Player::IsCP949()) {
		layouts.push_back(Window_Keyboard::Hangul1);
		layouts.push_back(Window_Keyboard::Hangul2);
		keyboard_done = Window_Keyboard::DONE_KO;
	// Simp. Chinese pages
	} else if (Player::IsCP936()) {
		layouts.push_back(Window_Keyboard::ZhCn1);
		layouts.push_back(Window_Keyboard::ZhCn2);
		keyboard_done = Window_Keyboard::DONE_ZH_CN;
	// Trad. Chinese pages
	} else if (Player::IsBig5()) {
		layouts.push_back(Window_Keyboard::ZhTw1);
		layouts.push_back(Window_Keyboard::ZhTw2);
		keyboard_done = Window_Keyboard::DONE_ZH_TW;
	// Cyrillic page (we assume it's Russian since we have no way to detect Serbian etc.)
	} else if (Player::IsCP1251()) {
		layouts.push_back(Window_Keyboard::RuCyrl);
		keyboard_done = Window_Keyboard::DONE_RU;
	}

	// Letter and symbol pages are used everywhere
	layouts.push_back(Window_Keyboard::Letter);
	layouts.push_back(Window_Keyboard::Symbol);
	if (Player::player_config.extra_pinyin_input.Get()) {
		layouts.push_back(Window_Keyboard::Pinyin);
	}
}

void Scene_Name::CreateKeyboardWindow() {
	if (kbd_window) {
		return;
	}

	if (layout_index < 0 || layout_index >= static_cast<int>(layouts.size())) {
		layout_index = 0;
	}

	kbd_window.reset(new Window_Keyboard(
		Player::menu_offset_x + kMarginX,
		Player::menu_offset_y + kWindowFaceHeight + kMarginY,
		kWindowKeyboardWidth,
		kWindowKeyboardHeight,
		keyboard_done));

	auto next_index = layout_index + 1;
	if (next_index >= static_cast<int>(layouts.size())) {
		next_index = 0;
	}
	kbd_window->SetMode(layouts[layout_index], layouts[next_index]);

	kbd_window->Refresh();
	kbd_window->UpdateCursorRect();

	pinyin_window.reset(new Window_Pinyin(
		Player::menu_offset_x + kMarginX,
		Player::menu_offset_y + kWindowFaceHeight + kMarginY,
		kWindowKeyboardWidth,
		kWindowPinyinHeight));
	keyboard_hint_window.reset(new Window_Base(
		Player::menu_offset_x + kMarginX + 16,
		0,
		kKeyboardHintWidth,
		kKeyboardHintHeight));
	keyboard_hint_window->SetContents(Bitmap::Create(kKeyboardHintWidth - 16, kKeyboardHintHeight - 16));
	keyboard_hint_window->SetZ(Priority_Window + 1);
	keyboard_hint_window->SetActive(false);
	RefreshPinyinWindow();
}

std::vector<std::string> Scene_Name::GetChoiceLabels() const {
	std::vector<std::string> labels;
	labels.reserve(candidates.size() + 1);

	for (const auto& candidate: candidates) {
		labels.push_back(candidate.label);
	}

	labels.emplace_back(kManualInputLabel);
	return labels;
}

void Scene_Name::CreateChoiceWindow() {
	choice_window.reset(new Window_Command(GetChoiceLabels(), kWindowKeyboardWidth, kWindowChoiceVisibleItems));
	choice_window->SetX(Player::menu_offset_x + kMarginX);
	choice_window->SetY(Player::menu_offset_y + kWindowFaceHeight + kMarginY);
	choice_window->UpdateCursorRect();
}

void Scene_Name::EnterChoiceMode() {
	RestoreInputMask();
	mode = Mode::Choice;
	kbd_window.reset();
	pinyin_window.reset();
	keyboard_hint_window.reset();
	name_window->SetActive(false);
	CreateChoiceWindow();
	choice_window->SetActive(true);
}

void Scene_Name::EnterKeyboardInputMode() {
	mode = Mode::Input;
	choice_window.reset();
	name_window->SetActive(true);
	CreateKeyboardWindow();
	kbd_window->SetActive(true);
}

bool Scene_Name::IsPinyinMode() const {
	return layout_index >= 0
		&& layout_index < static_cast<int>(layouts.size())
		&& layouts[layout_index] == Window_Keyboard::Pinyin;
}

void Scene_Name::RefreshPinyinWindow() {
	if (!pinyin_window) {
		return;
	}

	pinyin_window->SetVisible(IsPinyinMode());
	pinyin_window->SetActive(IsPinyinMode());
	pinyin_window->SetInput(pinyin_query, PinyinInput::Lookup(pinyin_query));
	kbd_window->SetY(Player::menu_offset_y + kWindowFaceHeight + kMarginY
		+ (IsPinyinMode() ? kWindowPinyinHeight : 0));
	RefreshKeyboardHint();
}

void Scene_Name::RefreshKeyboardHint() {
	if (!keyboard_hint_window) {
		return;
	}
	keyboard_hint_window->SetVisible(IsPinyinMode() && keyboard_input);
	keyboard_hint_window->SetY(kbd_window->GetY() + 8);
	auto contents = keyboard_hint_window->GetContents();
	contents->Clear();
	const std::string lines[] = {system_input ? "系统输入模式" : "键盘输入模式",
		system_input ? "关闭输入器退出" : "Esc 退出  +/- 翻页"};
	int y = 0;
	for (const auto& line: lines) {
		const int x = std::max(0, (contents->GetWidth() - Text::GetSize(*Font::Default(), line).width) / 2);
		contents->TextDraw(x, y, Font::ColorDefault, line);
		y += 16;
	}
}

void Scene_Name::ResetPinyinInput() {
	pinyin_query.clear();
	RefreshPinyinWindow();
}

void Scene_Name::AppendPinyinLetter(std::string_view key) {
	if (key.empty() || pinyin_query.size() >= PinyinInput::kMaxQueryLength) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
		return;
	}

	const auto c = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
	if (!PinyinInput::IsValidInputChar(c)) {
		return;
	}
	if (c == '\'' && (pinyin_query.empty() || pinyin_query.back() == '\'')) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
		return;
	}

	pinyin_query.push_back(c);
	RefreshPinyinWindow();
}

bool Scene_Name::CommitPinyinSelection() {
	if (!pinyin_window || !pinyin_window->HasSelection()) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
		return false;
	}

	if (!name_window->Append(pinyin_window->GetSelectedCandidate())) {
		pinyin_window->SetCommitError(true);
		return false;
	}
	ResetPinyinInput();
	return true;
}

bool Scene_Name::SelectPinyinCandidateByNumber(std::string_view key) {
	if (!IsPinyinMode() || key.size() != 1 || key[0] < '1' || key[0] > '4') {
		return false;
	}

	if (!pinyin_window->SetVisibleSelection(key[0] - '1')) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
		return true;
	}
	// A rejected candidate still consumes the number key. Otherwise the caller
	// falls back to appending the digit itself when the name is almost full.
	CommitPinyinSelection();
	return true;
}

void Scene_Name::SetPhysicalKeyboardInput(bool enabled) {
	if (enabled && !input_masked) {
		previous_keymask = Input::GetMask();
		auto mask = previous_keymask;
		mask.set();
		Input::SetMask(mask);
		input_masked = true;
	}
	keyboard_input = enabled;
	wait_for_key_release = true;
	backspace_frames = 0;
	kbd_window->SetActive(!enabled);
	RefreshKeyboardHint();
}

void Scene_Name::StartTextInput() {
	bool use_system = false;
#ifdef __ANDROID__
	use_system = EpAndroid::UseSystemNameInput();
#elif defined(EMSCRIPTEN)
	use_system = Emscripten_Interface::UseSystemNameInput();
#endif
	SetPhysicalKeyboardInput(true);
	if (!use_system) return;
	system_input = true;
	Input::ResetKeys();
	bool opened = false;
#ifdef __ANDROID__
	opened = EpAndroid::BeginNameInput(name_window->Get());
#elif defined(EMSCRIPTEN)
	opened = Emscripten_Interface::BeginNameInput(name_window->Get());
#endif
	if (!opened) {
		system_input = false;
		SetPhysicalKeyboardInput(false);
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
	}
	RefreshKeyboardHint();
}

bool Scene_Name::HandleSystemTextInput() {
	if (!system_input) return false;
	std::string text;
	int result = 2;
#ifdef __ANDROID__
	result = EpAndroid::PollNameInput(text);
#elif defined(EMSCRIPTEN)
	result = Emscripten_Interface::PollNameInput(text);
#endif
	if (result == 0) return true;
	system_input = false;
	if (result == 1) {
		if (name_window->TrySet(std::move(text))) ResetPinyinInput();
		else pinyin_window->SetCommitError(true);
	}
	// A native editor may consume key-up. Do not carry a stuck game key back.
	Input::ResetKeys();
	SetPhysicalKeyboardInput(false);
	return true;
}

void Scene_Name::RestoreInputMask() {
	if (system_input) {
#ifdef __ANDROID__
		EpAndroid::CancelNameInput();
#elif defined(EMSCRIPTEN)
		Emscripten_Interface::CancelNameInput();
#endif
		system_input = false;
		Input::ResetKeys();
	}
	if (input_masked) {
		Input::SetMask(previous_keymask);
	}
	input_masked = false;
	keyboard_input = false;
	wait_for_key_release = false;
	backspace_frames = 0;
	if (keyboard_hint_window) {
		keyboard_hint_window->SetVisible(false);
	}
}

void Scene_Name::EraseInput() {
	if (!pinyin_query.empty() && IsPinyinMode()) {
		pinyin_query.pop_back();
		RefreshPinyinWindow();
	} else if (!name_window->Get().empty()) {
		name_window->Erase();
	} else {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
		return;
	}
	Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Cancel));
}

void Scene_Name::FinishNameInput() {
	if (IsPinyinMode() && !pinyin_query.empty()) {
		CommitPinyinSelection();
		return;
	}
	if (name_window->Get().empty()) {
		name_window->Set(ToString(actor.GetName()));
	} else {
		actor.SetName(name_window->Get());
		RestoreInputMask();
		Scene::Pop();
	}
}

bool Scene_Name::HandlePhysicalKeyboardInput() {
	if (!input_masked) {
		return false;
	}
#ifdef __ANDROID__
	// A detached keyboard can no longer send Esc to release the game controls.
	if (keyboard_input && EpAndroid::UseSystemNameInput()) {
		StartTextInput();
		return true;
	}
#endif

	if (keyboard_input && Input::IsRawKeyTriggered(Input::Keys::ESCAPE)) {
		SetPhysicalKeyboardInput(false);
		return true;
	}

	// Ignore the key that opened the mode, and keep Esc masked until it is
	// released. Restoring immediately would turn the same Esc into CANCEL.
	if (wait_for_key_release) {
		if (Input::GetAllRawPressed().none()) {
			wait_for_key_release = false;
			if (!keyboard_input) {
				RestoreInputMask();
			}
		}
		return true;
	}

	if (Input::IsRawKeyPressed(Input::Keys::LCTRL) || Input::IsRawKeyPressed(Input::Keys::RCTRL)
			|| Input::IsRawKeyPressed(Input::Keys::LALT) || Input::IsRawKeyPressed(Input::Keys::RALT)) {
		return true;
	}
	for (int key = Input::Keys::A; key <= Input::Keys::Z; ++key) {
		if (Input::IsRawKeyTriggered(static_cast<Input::Keys::InputKey>(key))) {
			AppendPinyinLetter(std::string(1, static_cast<char>('a' + key - Input::Keys::A)));
		}
	}
	if (Input::IsRawKeyTriggered(Input::Keys::APOSTROPH)) {
		AppendPinyinLetter("'");
	}
	backspace_frames = Input::IsRawKeyPressed(Input::Keys::BACKSPACE) ? backspace_frames + 1 : 0;
	if (backspace_frames == 1 || (backspace_frames >= 24 && backspace_frames % 4 == 0)) {
		EraseInput();
	} else if (const auto number = GetTriggeredRawNumber()) {
		const std::string key(1, number);
		if (pinyin_query.empty()) {
			name_window->Append(key);
		} else if (!SelectPinyinCandidateByNumber(key)) {
			Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
		}
	} else if (Input::IsRawKeyTriggered(Input::Keys::RETURN)) {
		FinishNameInput();
	} else if (Input::IsRawKeyTriggered(Input::Keys::SPACE)) {
		if (pinyin_query.empty()) name_window->Append(" ");
		else CommitPinyinSelection();
	} else if (Input::IsRawKeyTriggered(Input::Keys::PGUP)
			|| Input::IsRawKeyTriggered(Input::Keys::MINUS)
			|| Input::IsRawKeyTriggered(Input::Keys::KP_SUBTRACT)) {
		pinyin_window->MovePage(-1);
	} else if (Input::IsRawKeyTriggered(Input::Keys::PGDN)
			|| Input::IsRawKeyTriggered(Input::Keys::EQUALS)
			|| Input::IsRawKeyTriggered(Input::Keys::KP_ADD)) {
		pinyin_window->MovePage(1);
	} else if (Input::IsRawKeyTriggered(Input::Keys::LEFT)) {
		pinyin_window->SetSelection(pinyin_window->GetSelection() - 1);
	} else if (Input::IsRawKeyTriggered(Input::Keys::RIGHT)) {
		pinyin_window->SetSelection(pinyin_window->GetSelection() + 1);
	}

	return true;
}

void Scene_Name::vUpdate() {
	if (mode == Mode::Choice) {
		choice_window->Update();

		if (Input::IsTriggered(Input::CANCEL)) {
			Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
		} else if (Input::IsTriggered(Input::DECISION)) {
			Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Decision));
			auto index = choice_window->GetIndex();

			if (index == static_cast<int>(candidates.size())) {
				EnterKeyboardInputMode();
			} else if (index >= 0 && index < static_cast<int>(candidates.size())) {
				actor.SetName(candidates[index].value);
				Scene::Pop();
			}
		}
		return;
	}

	name_window->Update();
	if (pinyin_window) {
		pinyin_window->Update();
	}
	if (keyboard_hint_window) {
		keyboard_hint_window->Update();
	}
	if (HandleSystemTextInput() || HandlePhysicalKeyboardInput()) {
		kbd_window->Update();
		return;
	}
	kbd_window->Update();

	if (Input::IsTriggered(Input::CANCEL)) {
		EraseInput();
	} else if (Input::IsTriggered(Input::DECISION)) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Decision));
		std::string const& s = kbd_window->GetSelected();

		assert(!s.empty());

		if (s == Window_Keyboard::DONE) {
			FinishNameInput();
		} else if (s == Window_Keyboard::NEXT_PAGE) {
			++layout_index;
			if (layout_index >= static_cast<int>(layouts.size())) {
				layout_index = 0;
			}

			auto next_index = layout_index + 1;
			if (next_index >= static_cast<int>(layouts.size())) {
				next_index = 0;
			}
			kbd_window->SetMode(layouts[layout_index], layouts[next_index]);
			RefreshPinyinWindow();
		} else if (s == Window_Keyboard::KEYBOARD_INPUT) {
			StartTextInput();
		} else if (s == Window_Keyboard::ERASE) {
			EraseInput();
		} else if (s == Window_Keyboard::PREVIOUS_CANDIDATES) {
			pinyin_window->MovePage(-1);
		} else if (s == Window_Keyboard::NEXT_CANDIDATES) {
			pinyin_window->MovePage(1);
		} else if (s == Window_Keyboard::SPACE) {
			if (IsPinyinMode() && !pinyin_query.empty()) {
				CommitPinyinSelection();
				return;
			}
			name_window->Append(" ");
		} else if (SelectPinyinCandidateByNumber(s)) {
			return;
		} else if (IsPinyinMode() && s.size() == 1 && PinyinInput::IsValidInputChar(s[0])) {
			AppendPinyinLetter(s);
		} else {
			name_window->Append(s);
		}
	}

	if (IsPinyinMode() && pinyin_window && pinyin_window->HasSelection()) {
		if (Input::IsRepeated(Input::PAGE_UP)) {
			pinyin_window->MovePage(-1);
		} else if (Input::IsRepeated(Input::PAGE_DOWN)) {
			pinyin_window->MovePage(1);
		}
	}
}
