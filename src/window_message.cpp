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
#include <cctype>
#include <cstring>
#include <sstream>
#include <iterator>

#include "compiler.h"
#include "utils.h"
#include "window_message.h"
#include "baseui.h"
#include "game_actors.h"
#include "game_map.h"
#include "game_message.h"
#include "game_interpreter.h"
#include "game_party.h"
#include "game_system.h"
#include "game_variables.h"
#include "input.h"
#include "output.h"
#include "player.h"
#include "util_macro.h"
#include "game_battle.h"
#include "bitmap.h"
#include "font.h"
#include "cache.h"
#include "text.h"
#include "message_history.h"

// FIXME: Off by 1 bug in window base class
constexpr int message_animation_frames = 7;
constexpr int gold_window_width = 88;
constexpr int gold_window_height = 32;

//#define EP_DEBUG_MESSAGE
//#define EP_DEBUG_MESSAGE_TEXT

namespace {
#if defined(EP_DEBUG_MESSAGE) || defined(EP_DEBUG_MESSAGE_TEXT)
static int frame_offset = 0;

void DebugLogResetFrameCounter() {
	frame_offset = Main_Data::game_system->GetFrameCounter();
}
#else
void DebugLogResetFrameCounter() { }
#endif

#ifdef EP_DEBUG_MESSAGE
template <typename... Args>
void DebugLog(const char* fmt, Args&&... args) {
	int frames = Main_Data::game_system->GetFrameCounter() - frame_offset;
	Output::Debug(fmt, frames, std::forward<Args>(args)...);
}
#else

template <typename... Args>
void DebugLog(const char*, Args&&...) { }
#endif

#ifdef EP_DEBUG_MESSAGE_TEXT
template <typename... Args>
void DebugLogText(const char* fmt, Args&&... args) {
	int frames = Main_Data::game_system->GetFrameCounter() - frame_offset;
	Output::Debug(fmt, frames, std::forward<Args>(args)...);
}
#else

template <typename... Args>
void DebugLogText(const char*, Args&&...) { }
#endif

/**
 * Cleans message text by removing control characters.
 * Used for message history to store plain text.
 */
std::string CleanMessageText(const std::string& raw_text) {
	std::string result;
	result.reserve(raw_text.size());

	for (size_t i = 0; i < raw_text.size(); ++i) {
		if (raw_text[i] == '\\' && i + 1 < raw_text.size()) {
			char next = raw_text[i + 1];
			if (next == 'c' || next == 's') {
				// Skip color/speed control codes \c[n] \s[n]
				i += 2; // Skip \c or \s
				if (i < raw_text.size() && raw_text[i] == '[') {
					i++; // Skip [
					while (i < raw_text.size() && raw_text[i] != ']') i++;
				}
				continue;
			} else if (next == 'f') {
				// Page separator, replace with newline
				result += '\n';
				i++;
				continue;
			} else if (next == '$' || next == '>' || next == '<' || next == '^' || next == '.' || next == '|' || next == '!') {
				// Skip special control codes
				i++;
				continue;
			}
		}
		result += raw_text[i];
	}
	return result;
}
} //namespace

namespace {
Point ManiacColorCell(const Bitmap& system, const std::array<int32_t, 2>& colors) {
	uint32_t sx, sy;
	if (colors[1] < 0) {
		const auto columns = std::max(1, system.GetWidth() / 16);
		const auto index = std::max(0, colors[0]);
		sx = static_cast<uint32_t>(index % columns) << 4;
		sy = static_cast<uint32_t>(index / columns) << 4;
	} else {
		sx = static_cast<uint32_t>(colors[0]) << 4;
		if (sx & 0x80000000) sx = 0;
		sy = static_cast<uint32_t>(colors[1]) << 4;
	}
	sx += 2;
	sy += 50;
	// Native validates the two 14-pixel color axes independently, after 32-bit scaling.
	if (sx + 14 > static_cast<uint32_t>(system.GetWidth())) sx = 2;
	if (sy + 14 > static_cast<uint32_t>(system.GetHeight())) sy = 50;
	return {static_cast<int>(sx) - 2, static_cast<int>(sy) - 2};
}
}

// C4428 is nonsense
#ifdef _MSC_VER
#pragma warning (disable : 4428)
#endif

Window_Message::Window_Message(int ix, int iy, int iwidth, int iheight) :
	Window_Selectable(ix, iy, iwidth, iheight),
	number_input_window(new Window_NumberInput(0, 0)),
	gold_window(new Window_Gold(Player::screen_width - Player::menu_offset_x - gold_window_width, Player::menu_offset_y, gold_window_width, gold_window_height)),
	pending_message(Game_Message::CommandCodeInserter)
{
	SetContents(Bitmap::Create(width - 16, height - 16));

	// 2k3 transparent message boxes
	bool msg_transparent = Player::IsRPG2k3()
		// if the flag is set ..
		&& (lcf::Data::battlecommands.transparency == lcf::rpg::BattleCommands::Transparency_transparent)
		// if we're not in battle, or if we are in battle but not using mode A
		&& (!Game_Battle::IsBattleRunning() || lcf::Data::battlecommands.battle_type != lcf::rpg::BattleCommands::BattleType_traditional)
		// RPG_RT < 1.11 bug, map messages were not transparent if the battle type was mode A.
		&& (Player::IsRPG2k3E() || lcf::Data::battlecommands.battle_type != lcf::rpg::BattleCommands::BattleType_traditional);
	if (msg_transparent) {
		SetBackOpacity(160);
	}
	gold_window->SetBackOpacity(GetBackOpacity());

	SetVisible(false);
	// Above other windows
	SetZ(Priority_Window + 100);

	active = true;
	SetIndex(-1);
	text_color = Font::ColorDefault;

	number_input_window->SetVisible(false);

	gold_window->SetVisible(false);

	// The 250921 face and animation start are system state restored before
	// the map creates its message window. Keep them when recreating that window.
	if (!Player::IsPatchManiac()) {
		Main_Data::game_system->ClearMessageFace();
	}
	Game_Message::SetWindow(this);
}

Window_Message::~Window_Message() {
	if (Game_Message::GetWindow() == this) {
		Game_Message::SetWindow(nullptr);
	}
}

void Window_Message::StartMessageProcessing(PendingMessage pm) {
	++maniac_message_generation;
	maniac_hook_commit = {};
	maniac_hook_invocation.reset();
	maniac_hook_resume_text = false;
	aop = {};
	text.clear();
	pending_message = std::move(pm);

	if (!IsVisible()) {
		DebugLogResetFrameCounter();
	}
	DebugLog("{}: MSG START");

	if (!pending_message.IsActive()) {
		return;
	}

	const auto& lines = pending_message.GetLines();

	int num_lines = 0;
	auto append = [&](const std::string& line) {
		bool force_page_break = (!line.empty() && line.back() == '\f');

		text.append(line, 0, line.size() - force_page_break);
		if (line.empty() || text.back() != '\n') {
			text.push_back('\n');
		}
		++num_lines;

		if (num_lines == GetMaxLinesPerPage() || force_page_break) {
			text.push_back('\f');
			num_lines = 0;
		}
	};

	if (pending_message.IsWordWrapped()) {
		for (const std::string& line : lines) {
			/* TODO: don't take commands like \> \< into account when word-wrapping */
			Game_Message::WordWrap(
					line,
					width - 24,
					[&](std::string_view wrapped_line) {
						append(std::string(wrapped_line));
					}
			);
		}
	} else {
		for (const std::string& line : lines) {
			append(line);
		}
	}

	if (text.empty() || text.back() != '\f') {
		text.push_back('\f');
	}

	item_max = min(4, pending_message.GetNumChoices());

	text_index = text.data();

	DebugLog("{}: MSG TEXT \n{}", text);

	auto open_frames = (!IsVisible() && !Game_Battle::IsBattleRunning()) ? message_animation_frames : 0;
	SetOpenAnimation(open_frames);
	DebugLog("{}: MSG START OPEN {}", open_frames);

	InsertNewPage();

	if (Player::player_config.extra_message_history.Get()) {
		CaptureMessageForHistory();
	}
}

void Window_Message::OnFinishPage() {
	DebugLog("{}: FINISH PAGE");

	if (pending_message.GetNumChoices() > 0) {
		StartChoiceProcessing();
	} else if (pending_message.HasNumberInput()) {
		StartNumberInputProcessing();
	} else if (!kill_page) {
		DebugLog("{}: SET PAUSE");
		SetPause(true);
	}

	line_count = 0;
	kill_page = false;
	line_char_counter = 0;
}

void Window_Message::StartChoiceProcessing() {
	SetIndex(0);
}

void Window_Message::StartNumberInputProcessing() {
	number_input_window->SetMaxDigits(pending_message.GetNumberInputDigits());
	if (IsFaceEnabled() && !Main_Data::game_system->IsMessageFaceRightPosition()) {
		number_input_window->SetX(Player::message_box_offset_x + LeftMargin + Main_Data::game_system->GetMessageFaceWidth() + RightFaceMargin);
	} else {
		number_input_window->SetX(x);
	}
	number_input_window->SetY(y + contents_y - 2);
	number_input_window->SetActive(true);
	if (IsVisible() && !IsOpeningOrClosing()) {
		number_input_window->SetVisible(true);
	}
	number_input_window->Update();
}

void Window_Message::ShowGoldWindow() {
	if (Game_Battle::IsBattleRunning()) {
		return;
	}
	if (!gold_window->IsVisible()) {
		gold_window->SetY(y == 0 ? Player::screen_height - Player::menu_offset_y - 32 : Player::menu_offset_y);
		gold_window->SetOpenAnimation(message_animation_frames);
	} else if (gold_window->IsClosing()) {
		gold_window->SetOpenAnimation(0);
	}
	gold_window->Refresh();
}

void Window_Message::InsertNewPage(bool resume_text) {
	DebugLog("{}: MSG NEW PAGE");
	// Cancel pending face requests for async
	// Otherwise they render on the wrong page
	face_request_ids.clear();
	maniac_face.reset();
	maniac_faceset.reset();
	shape_ret.clear();

	contents->Clear();
	SetIndex(-1);
	SetPause(false);
	number_input_window->SetActive(false);
	number_input_window->SetVisible(false);
	kill_page = false;
	line_count = 0;
	text_color = Font::ColorDefault;
	maniac_text_color_row = Player::IsPatchManiac() ? 0 : -1;
	speed = 1;
	kill_page = false;
	instant_speed = false;
	prev_char_printable = false;
	prev_char_waited = true;

	if (GetFont()) {
		page_font = GetFont();
	} else {
		page_font = Font::Default();
	}

	if (Player::IsPatchManiac() && Game_Battle::IsBattleRunning()) {
		const auto display_offset = Main_Data::game_system->GetBattleDisplayOffset();
		x = display_offset.x;
		y = display_offset.y;
	} else {
		// Position the message box vertically
		// Game_Message::GetRealPosition() specify top/middle/bottom
		if (Game_Message::GetRealPosition() == 0) {
			y = Player::menu_offset_y;
		}
		else if (Game_Message::GetRealPosition() == 1) {
			y = static_cast<int>((Player::screen_height - MESSAGE_BOX_HEIGHT) / 2.f);
		}
		else if (Game_Message::GetRealPosition() == 2) {
			y = Player::screen_height - Player::menu_offset_y - MESSAGE_BOX_HEIGHT ;
		}
	}
	maniac_face_x = x + (Main_Data::game_system->IsMessageFaceRightPosition()
		? std::min(width, Player::screen_width) - Main_Data::game_system->GetMessageFaceWidth() - RightFaceMargin
		: 8 + LeftMargin);
	maniac_face_y = y + 8 + TopMargin;
	if (Player::IsPatchManiac()) {
		std::array<int32_t, 9> packet = {2, x, y, width, height, maniac_face_x, maniac_face_y,
			Main_Data::game_system->GetMessageFaceIndex(), IsOpening() ? 1 : 0};
		const auto face_name = ToString(Main_Data::game_system->GetMessageFaceName());
		RunManiacHook(packet.data(), packet.size(), &face_name, [this, transition = packet[8]](bool hooked) {
			if (hooked) {
				SetX(Game_Message::ReadManiacHookResult(maniac_hook_caller, 1, x));
				SetY(Game_Message::ReadManiacHookResult(maniac_hook_caller, 2, y));
				SetWidth(Utils::Clamp(Game_Message::ReadManiacHookResult(maniac_hook_caller, 3, width), 1, 2000));
				SetHeight(Utils::Clamp(Game_Message::ReadManiacHookResult(maniac_hook_caller, 4, height), 1, 2000));
				// Native fallback uses the window coordinates after their readback.
				maniac_face_x = Game_Message::ReadManiacHookResult(maniac_hook_caller, 5, x);
				maniac_face_y = Game_Message::ReadManiacHookResult(maniac_hook_caller, 6, y);
				const bool animate = Game_Message::ReadManiacHookResult(maniac_hook_caller, 8, transition) != 0;
				if (contents->GetWidth() != std::max(0, width - 16) || contents->GetHeight() != std::max(0, height - 16)) {
					SetContents(Bitmap::Create(std::max(0, width - 16), std::max(0, height - 16)));
				}
				SetOpenAnimation(animate ? message_animation_frames : 0);
			}
			CompleteNewPage();
		}, resume_text);
		return;
	}
	CompleteNewPage();
}

void Window_Message::CompleteNewPage() {
	maniac_glyphs = Player::IsPatchManiac() ? Bitmap::Create(width, height) : BitmapRef{};

	if (Main_Data::game_system->IsMessageTransparent()) {
		SetOpacity(0);
		gold_window->SetBackOpacity(0);
	} else {
		SetOpacity(255);
		gold_window->SetBackOpacity(GetBackOpacity());
	}

	if (IsFaceEnabled()) {
		if (!Main_Data::game_system->IsMessageFaceRightPosition()) {
			contents_x = LeftMargin + Main_Data::game_system->GetMessageFaceWidth() + RightFaceMargin;
		} else {
			contents_x = 0;
		}
		if (Player::IsPatchManiac()) {
			ReloadManiacFace();
		} else {
			DrawFace(Main_Data::game_system->GetMessageFaceName(), Main_Data::game_system->GetMessageFaceIndex(),
				Main_Data::game_system->IsMessageFaceRightPosition() ? 248 : LeftMargin, TopMargin,
				Main_Data::game_system->IsMessageFaceFlipped());
		}
	} else {
		contents_x = 0;
	}

	if (pending_message.GetChoiceStartLine() == 0 && pending_message.HasChoices()) {
		contents_x += 12;
	}

	contents_y = 2;

	if (pending_message.GetNumberInputStartLine() == 0 && pending_message.HasNumberInput()) {
		// If there is an input window on the first line
		StartNumberInputProcessing();
	}
	line_char_counter = 0;

	if (pending_message.ShowGoldWindow()) {
		ShowGoldWindow();
	} else {
		// If first character is gold, the gold window appears immediately and animates open with the main window.
		auto tret = Utils::TextNext(text_index, (text.data() + text.size()), Player::escape_char);
		if (tret && tret.is_escape && tret.ch == '$') {
			ShowGoldWindow();
		}
	}
}

void Window_Message::ReloadManiacFace() {
	face_request_ids.clear();
	maniac_face.reset();
	maniac_faceset.reset();
	if (Main_Data::game_system->GetMessageFaceName().empty()) return;
	auto* request = AsyncHandler::RequestFile("FaceSet", Main_Data::game_system->GetMessageFaceName());
	request->SetGraphicFile(true);
	face_request_ids.push_back(request->Bind(&Window_Message::OnManiacFaceReady, this));
	request->Start();
}

void Window_Message::ProcessManiacFace(const char* end) {
	std::array<int32_t, 2> numbers;
	std::optional<std::string> name;
	text_index = Game_Message::ParseManiacFace(text_index, end, maniac_hook_caller, numbers, name);
	const bool at_start = contents_x <= 0 && line_count <= 0;
	if (at_start || IsFaceEnabled()) {
		if (name) {
			if (!name->empty() && name->front() == '*') {
				if (!at_start) return;
				Main_Data::game_system->SetMessageFaceName("");
			} else {
				Main_Data::game_system->SetMessageFaceName(*name);
			}
		}
		// Native fallback is checked against the visibility state before the new flag is written.
		if (IsFaceEnabled() && (maniac_face_x == INT_MAX || maniac_face_y == INT_MAX)) {
			maniac_face_x = x + 8 + (Main_Data::game_system->IsMessageFaceRightPosition()
				? std::min(width, Player::screen_width) - Main_Data::game_system->GetMessageFaceWidth() - RightFaceMargin - 8 : LeftMargin);
			maniac_face_y = y + 8 + TopMargin;
		}
		Main_Data::game_system->SetMessageFaceIndex(numbers[0]);
		pending_message.SetEnableFace(static_cast<uint8_t>(numbers[1]) != 0);
	}
	if (Player::IsPatchManiac()) {
		Main_Data::game_system->SetManiacFaceAnimationFlags(0);
	}
	Main_Data::game_system->ResetManiacFaceAnimation();
	ReloadManiacFace();
}

void Window_Message::OnManiacFaceReady(FileRequestResult* result) {
	maniac_faceset = Cache::Faceset(result->file);
	UpdateManiacFaceBitmap();
}

void Window_Message::UpdateManiacFaceBitmap() {
	if (!maniac_faceset) return;
	maniac_face = maniac_faceset;
}

void Window_Message::DrawContents(Bitmap& dst) {
	if (maniac_face && IsFaceEnabled() && !IsOpeningOrClosing() && contents_opacity > 0) {
		dst.FlipBlit(maniac_face_x, maniac_face_y, *maniac_face, GetMessageFaceRect(*maniac_face),
			Main_Data::game_system->IsMessageFaceFlipped(), false, contents_opacity);
	}
	if (maniac_glyphs && !IsOpeningOrClosing() && contents_opacity > 0
		&& x >= -width && x <= dst.GetWidth() && y >= -height && y <= dst.GetHeight()) {
		dst.Blit(x, y, *maniac_glyphs, maniac_glyphs->GetRect(), contents_opacity);
	}
	Window::DrawContents(dst);
}

void Window_Message::InsertNewLine() {
	DebugLog("{}: MSG NEW LINE");
	if (IsFaceEnabled() && !Main_Data::game_system->IsMessageFaceRightPosition()) {
		contents_x = LeftMargin + Main_Data::game_system->GetMessageFaceWidth() + RightFaceMargin;
	} else {
		contents_x = 0;
	}

	contents_y += 16;
	++line_count;

	if (pending_message.HasChoices() && line_count >= pending_message.GetChoiceStartLine()) {
		unsigned choice_index = line_count - pending_message.GetChoiceStartLine();
		if (pending_message.GetChoiceResetColor()) {
			// Check for disabled choices
			if (!pending_message.IsChoiceEnabled(choice_index)) {
				text_color = Font::ColorDisabled;
			} else {
				text_color = Font::ColorDefault;
			}
			maniac_text_color_row = -1;
		}

		contents_x += 12;
	}
	line_char_counter = 0;
	prev_char_printable = false;
	prev_char_waited = true;
}

void Window_Message::CaptureMessageForHistory() {
	// Don't capture empty messages
	if (pending_message.GetLines().empty()) {
		return;
	}

	MessageHistoryEntry entry;

	// Capture text content (combine all lines)
	const auto& lines = pending_message.GetLines();
	std::string raw_text;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i > 0) {
			raw_text += "\n";
		}
		raw_text += lines[i];
	}

	// Process text to remove timing control characters and handle content display characters
	entry.text = Game_Message::ProcessTextForHistory(raw_text);

	// Capture face information
	if (IsFaceEnabled()) {
		entry.face_name = std::string(Main_Data::game_system->GetMessageFaceName());
		entry.face_index = Main_Data::game_system->GetMessageFaceIndex();
		entry.face_flipped = Main_Data::game_system->IsMessageFaceFlipped();
		entry.face_right_position = Main_Data::game_system->IsMessageFaceRightPosition();
	}

	// Capture window display attributes
	entry.window_position = Main_Data::game_system->GetMessagePosition();
	entry.transparent = Main_Data::game_system->IsMessageTransparent();
	entry.position_fixed = Main_Data::game_system->IsMessagePositionFixed();

	// Capture System tile information (CRITICAL for re-rendering!)
	entry.system_name = std::string(Main_Data::game_system->GetSystemName());
	entry.message_stretch = static_cast<int>(Main_Data::game_system->GetMessageStretch());

	// Capture choices if present (keep raw text for re-rendering)
	if (pending_message.HasChoices()) {
		int choice_start = pending_message.GetChoiceStartLine();
		int num_choices = pending_message.GetNumChoices();
		for (int i = 0; i < num_choices; ++i) {
			entry.choices.push_back(lines[choice_start + i]);
		}
		// Note: selected_choice will be set to -1 by default
		// In Phase 3, we can add logic to track the actual selection
	}

	// Capture timestamp
	entry.timestamp = Main_Data::game_system->GetFrameCounter();

	// Add to history
	Game_Message::GetMessageHistory().AddEntry(std::move(entry));
}

void Window_Message::FinishMessageProcessing(bool run_hook) {
	++maniac_message_generation;
	maniac_hook_commit = {};
	maniac_hook_invocation.reset();
	maniac_hook_resume_text = false;
	aop = {};
	DebugLog("{}: FINISH MSG");
	auto close_frames = Game_Battle::IsBattleRunning() ? 0 : message_animation_frames;
	if (run_hook && IsVisible() && !IsClosing() && Player::IsPatchManiac()) {
		std::array<int32_t, 8> packet = {4, x, y, width, height, maniac_face_x, maniac_face_y, 1};
		RunManiacHook(packet.data(), packet.size(), nullptr, [this, close_frames](bool hooked) {
			CompleteMessageClose(hooked && Game_Message::ReadManiacHookResult(maniac_hook_caller, 7, 1) == 0 ? 0 : close_frames);
		}, false);
		return;
	}
	CompleteMessageClose(run_hook ? close_frames : 0);
}

void Window_Message::CompleteMessageClose(int close_frames) {
	face_request_ids.clear();
	maniac_face.reset();
	maniac_glyphs.reset();

	text.clear();
	text_index = text.data();

	SetPause(false);
	kill_page = false;
	line_char_counter = 0;
	SetIndex(-1);

	pending_message = PendingMessage(Game_Message::CommandCodeInserter);

	if (number_input_window->IsVisible()) {
		number_input_window->SetActive(false);
		number_input_window->SetVisible(false);
	}

	SetCloseAnimation(close_frames);
	close_started_this_frame = true;
	DebugLog("{}: MSG START CLOSE {}", close_frames);

	// RPG_RT updates window open/close at the end of the main loop.
	// To simulate this timing, we run base class Update() again once
	// to animate the closing by 1 frame.
	Window::Update();

	if (gold_window->IsVisible()) {
		gold_window->SetCloseAnimation(close_frames);
		gold_window->Update();
	}
}

void Window_Message::ResetWindow() {

}

void Window_Message::Update() {
	aop = {};
	if (maniac_hook_invocation) {
		ResumeManiacMessageHook();
		return;
	}
	if (Player::IsPatchManiac()) {
		Main_Data::game_system->UpdateManiacFaceAnimation();
		UpdateManiacFaceBitmap();
	}
	if (IsOpening()) { DebugLog("{}: MSG OPENING"); }
	if (IsClosing()) { DebugLog("{}: MSG CLOSING"); }

	close_started_this_frame = false;
	close_finished_this_frame = false;

	const bool was_closing = IsClosing();

	Window_Selectable::Update();
	number_input_window->Update();
	gold_window->Update();

	if (was_closing && !IsClosing()) {
		close_finished_this_frame = true;
	}

	if (!IsVisible()) {
		return;
	}

	maniac_rshift_forced = Player::IsPatchManiac() && Player::maniac_rshift_message
		&& DisplayUi && DisplayUi->IsRightShiftPressed();
	if (maniac_rshift_forced) instant_speed = true;

	if (IsOpeningOrClosing()) {
		return;
	}

	if (wait_count > 0) {
		DebugLog("{}: MSG WAIT {}", wait_count);
		--wait_count;
		return;
	}

	if (GetPause()) {
		DebugLog("{}: MSG PAUSE");
		WaitForInput();
		if (aop.IsActive()) return;

		if (GetPause()) {
			return;
		}
	}

	if (GetIndex() >= 0) {
		DebugLog("{}: MSG CHOICE");
		InputChoice();
		if (aop.IsActive()) return;
		if (GetIndex() >= 0) {
			return;
		}
	}

	if (number_input_window->GetActive()) {
		DebugLog("{}: MSG NUMBER");
		InputNumber();
		if (aop.IsActive()) return;
		if (number_input_window->GetActive()) {
			return;
		}
	}

	DebugLog("{}: MSG UPD");
	UpdateMessage();
}

void Window_Message::UpdateMessage() {
	// Message Box Show Message rendering loop
	bool instant_speed_forced = false;

	if (Player::IsPatchManiac() ? maniac_rshift_forced : Player::debug_flag && Input::IsPressed(Input::SHIFT)) {
		instant_speed = true;
		instant_speed_forced = true;
	}

	auto system = Cache::SystemOrBlack();

	while (true) {
		const auto* end = text.data() + text.size();

		if (wait_count > 0) {
			DebugLog("{}: MSG WAIT LOOP {}", wait_count);
			--wait_count;
			break;
		}

		if (!shape_ret.empty()) {
			if (!DrawGlyph(*page_font, *system, shape_ret[0])) {
				continue;
			}

			shape_ret.erase(shape_ret.begin());
			continue;
		}

		if (GetPause() || GetIndex() >= 0 || number_input_window->GetActive()) {
			break;
		}

		if (text_index == end) {
			FinishMessageProcessing();
			break;
		}

		auto tret = Utils::TextNext(text_index, end, Player::escape_char);
		auto text_prev = text_index;
		text_index = tret.next;

		if (EP_UNLIKELY(!tret)) {
			continue;
		}

		const auto ch = tret.ch;
		if (tret.is_exfont) {
			if (!DrawGlyph(*page_font, *system, ch, true, std::string_view(text_prev, text_index - text_prev))) {
				text_index = text_prev;
			}
			if (aop.IsActive()) return;
			continue;
		}
		if (Player::IsPatchManiac() && !tret.is_escape && ch == '$') {
			text_prev = text_index - 1;
			const auto* icon_end = Game_Message::ManiacIconEnd(text_prev, end);
			const size_t begin_offset = text_prev - text.data();
			auto units = Utils::DecodeUTF16(std::string_view(text_prev,
				std::min<size_t>(icon_end - text_prev, 512)), true);
			const bool split_pair = units.size() > 127 && units[126] >= 0xd800 && units[126] <= 0xdbff
				&& units[127] >= 0xdc00 && units[127] <= 0xdfff;
			const auto token = Utils::EncodeUTF(units.substr(0, 127), true);
			if (split_pair && static_cast<uint8_t>(text_prev[token.size() - 3]) >= 0xf0) {
				// A retry must keep the same 127-unit cut and leave the low surrogate in the source.
				text.replace(begin_offset + token.size() - 3, 4,
					Utils::EncodeUTF(std::u16string(1, units[126]), true)
					+ Utils::EncodeUTF(std::u16string(1, units[127]), true));
				text_prev = text.data() + begin_offset;
				end = text.data() + text.size();
			}
			text_index = text_prev + token.size();
			const bool parameter = token.size() > 1 && token[1] != '$';
			if (!DrawGlyph(*page_font, *system, '$', parameter,
				token, parameter)) text_index = text_prev;
			if (aop.IsActive()) return;
			continue;
		}

		if (ch == '\f') {
			if (text_index != end) {
				InsertNewPage(true);
				SetWait(1);
				if (aop.IsActive()) return;
			}
			continue;
		}

		if (ch == '\n') {
			int wait_frames = 0;
			bool end_page = (*text_index == '\f');

			if (!instant_speed) {
				if (!prev_char_printable) {
					wait_frames += 1 + end_page;
				}
			} else if (end_page) {
				// When the page ends and speed is instant, RPG_RT always waits 2 frames.
				wait_frames += 2;
			}

			InsertNewLine();

			if (end_page) {
				OnFinishPage();
			}
			SetWait(wait_frames);

			if (instant_speed && !instant_speed_forced) {
				// instant_speed stops at the end of the line
				// unless it was triggered by the shift key.
				instant_speed = false;
			}
			continue;
		}

		if (Utils::IsControlCharacter(ch)) {
			// control characters not handled
			continue;
		}

		if (tret.is_escape && ch != Player::escape_char) {
			// Special message codes
			switch (ch) {
			case 'e':
			case 'E':
				if (Player::IsPatchManiac()) {
					std::array<int32_t, 8> packet = {1, x, y, maniac_face_x, maniac_face_y,
						line_count, contents_x, contents_y};
					text_index = Game_Message::ParseManiacUserHook(text_index, end, maniac_hook_caller);
					RunManiacHook(packet.data(), packet.size(), nullptr, [this, packet](bool hooked) {
						SetX(hooked ? Game_Message::ReadManiacHookResult(maniac_hook_caller, 1, packet[1]) : packet[1]);
						SetY(hooked ? Game_Message::ReadManiacHookResult(maniac_hook_caller, 2, packet[2]) : packet[2]);
						maniac_face_x = hooked ? Game_Message::ReadManiacHookResult(maniac_hook_caller, 3, packet[3]) : packet[3];
						maniac_face_y = hooked ? Game_Message::ReadManiacHookResult(maniac_hook_caller, 4, packet[4]) : packet[4];
						SetWaitForNonPrintable(0);
					}, true);
					if (aop.GetType() != AsyncOp::eNone) return;
				} else {
					SetWaitForNonPrintable(speed);
				}
				break;
			case 'f':
			case 'F':
				if (Player::IsPatchManiac()) {
					ProcessManiacFace(end);
				} else {
					SetWaitForNonPrintable(speed);
				}
				break;
			case 'c':
			case 'C':
				{
					// Color
					if (Player::IsPatchManiac()) {
						std::array<int32_t, 2> colors;
						text_index = Game_Message::ParseManiacNumbers(text_index, end, maniac_hook_caller, colors);
						text_color = colors[0];
						maniac_text_color_row = colors[1];
						SetWaitForNonPrintable(0);
						break;
					}
					auto pres = Game_Message::ParseColor(text_index, end, Player::escape_char, true);
					auto value = pres.value;
					text_index = pres.next;
					DebugLogText("{}: MSG Color \\c[{}]", value);
					SetWaitForNonPrintable(0);
					text_color = value > 19 ? 0 : value;
				}
				break;
			case 's':
			case 'S':
				{
					// Speed modifier
					const bool maniac = Player::IsPatchManiac();
					const auto pres = maniac
						? Game_Message::ParseManiacNumber(text_index, end, maniac_hook_caller)
						: Game_Message::ParseSpeed(text_index, end, Player::escape_char, true);
					text_index = pres.next;
					DebugLogText("{}: MSG Speed \\s[{}]", pres.value);
					if (!maniac) SetWaitForNonPrintable(0);
					speed = Utils::Clamp(pres.value, 1, 20);
				}
				break;
			case 'w':
			case 'W':
				if (Player::IsPatchManiac()) {
					const auto pres = Game_Message::ParseManiacNumber(text_index, end, maniac_hook_caller);
					text_index = pres.next;
					if (!instant_speed) {
						// Native control-code tail also applies the speed floor, then returns without decrementing.
						SetWait(std::max(speed, pres.value) - 1);
						prev_char_waited = true;
						prev_char_printable = false;
						line_char_counter &= ~1;
						return;
					}
				} else {
					SetWaitForNonPrintable(speed);
				}
				break;
			case '_':
				// Insert half size space
				contents_x += Text::GetSize(*page_font, " ").width / 2;
				DebugLogText("{}: MSG HalfWait \\_");
				SetWaitForCharacter(1);
				break;
			case '$':
				// Show Gold Window
				ShowGoldWindow();
				DebugLogText("{}: MSG Gold \\$");
				SetWaitForNonPrintable(speed);
				break;
			case '!':
				// Text pause
				DebugLogText("{}: MSG Pause \\!");
				SetWaitForNonPrintable(0);
				SetPause(true);
				break;
			case '^':
				// Force message close
				// The close happens at the end of the message, not where
				// the ^ is encountered
				DebugLogText("{}: MSG Kill Page \\^");
				kill_page = true;
				SetWaitForNonPrintable(speed);
				break;
			case '>':
				// Instant speed start
				DebugLogText("{}: MSG Instant Speed Start \\>");
				SetWaitForNonPrintable(0);
				instant_speed = true;
				break;
			case '<':
				// Instant speed stop - also cancels shift key and forces a delay.
				instant_speed = false;
				instant_speed_forced = false;
				DebugLogText("{}: MSG Instant Speed Stop \\<");
				SetWaitForNonPrintable(speed);
				break;
			case '.':
				// 1/4 second sleep
				// Despite documentation saying 1/4 second, RPG_RT waits for 16 frames.
				// RPG_RT also has a bug(??) where speeds >= 17 slow this down by 1 more frame per speed.
				SetWaitForNonPrintable(16 + Utils::Clamp(speed - 16, 0, 4));
				DebugLogText("{}: MSG Quick Sleep \\.");
				break;
			case '|':
				// Second sleep
				// Despite documentation saying 1 second, RPG_RT waits for 61 frames.
				SetWaitForNonPrintable(61);
				DebugLogText("{}: MSG Sleep \\|");
				break;
			default:
				// Unknown characters will not display anything but do wait.
				SetWaitForNonPrintable(speed);
				break;
			}
			continue;
		}

		// RPG_RT consumes and invokes the hook per source character. Joining glyphs would merge callbacks.
		if (page_font->CanShape() && !Player::IsPatchManiac()) {
			assert(shape_ret.empty());

			auto text_index_shape = text_index;
			std::u32string text32;
			text32 += ch;

			while (true) {
				tret = Utils::TextNext(text_index_shape, end, Player::escape_char);

				if (EP_UNLIKELY(!tret)) {
					break;
				}

				auto text_prev_shape = text_index_shape;
				text_index_shape = tret.next;
				auto chs = tret.ch;

				if (text_index_shape == end || tret.is_exfont || tret.is_escape || Utils::IsControlCharacter(chs)) {
					text_index = text_prev_shape;
					break;
				}

				text32 += tret.ch;
			}

			shape_ret = page_font->Shape(text32);
			continue;
		} else {
			if (!DrawGlyph(*page_font, *system, ch, false, std::string_view(text_prev, text_index - text_prev))) {
				text_index = text_prev;
				continue;
			}
			if (aop.IsActive()) return;
		}
	}
}

void Window_Message::RunManiacHook(const int32_t* packet, size_t count, const std::string* source,
	std::function<void(bool)> commit, bool resume_text) {
	const auto generation = maniac_message_generation;
	std::unique_ptr<Game_Interpreter> invocation;
	AsyncOp operation;
	const bool hooked = Game_Message::RunManiacSystemHook(maniac_hook_caller, packet, count, source, invocation, operation);
	// A replacement message owns its own consumer. Do not overwrite it with
	// a callback belonging to the message which invoked this hook.
	if (generation != maniac_message_generation) return;
	aop = operation;
	if (invocation) {
		maniac_hook_invocation = std::move(invocation);
		maniac_hook_commit = std::move(commit);
		maniac_hook_resume_text = resume_text;
		// The enclosing ShowMessage is consumed, unlike the hook's yielding
		// command. Its caller must not rewind and create the message again.
		aop = AsyncOp::MakeYield();
	} else if (aop.GetType() != AsyncOp::eLoad && aop.GetType() != AsyncOp::eToTitle
		&& aop.GetType() != AsyncOp::eExitGame) {
		commit(hooked);
	}
}

AsyncOp Window_Message::ResumeManiacMessageHook() {
	if (!maniac_hook_invocation) return {};
	const auto generation = maniac_message_generation;
	// Own the executing slice locally; message replacement can cancel the
	// member without destroying an interpreter while Update is on its stack.
	auto invocation = std::move(maniac_hook_invocation);
	const auto operation = Game_Interpreter::ResumeManiacMessageHook(invocation);
	if (generation != maniac_message_generation) return aop;
	aop = operation;
	if (invocation) {
		maniac_hook_invocation = std::move(invocation);
		aop = AsyncOp::MakeYield();
		return aop;
	}
	auto commit = std::move(maniac_hook_commit);
	maniac_hook_commit = {};
	const bool resume_text = maniac_hook_resume_text;
	maniac_hook_resume_text = false;
	if (aop.GetType() == AsyncOp::eLoad || aop.GetType() == AsyncOp::eToTitle || aop.GetType() == AsyncOp::eExitGame) return aop;
	if (commit) commit(true);
	if (!aop.IsActive() && resume_text) UpdateMessage();
	return aop;
}

bool Window_Message::DrawGlyph(Font& font, const Bitmap& system, char32_t glyph, bool is_exfont, std::string_view token, bool maniac_icon) {
	if (is_exfont) {
		DebugLogText("{}: MSG DrawGlyph Exfont {}", static_cast<uint32_t>(glyph));
	} else {
		if (glyph < 128) {
			DebugLogText("{}: MSG DrawGlyph ASCII {}", static_cast<char>(glyph));
		} else {
			DebugLogText("{}: MSG DrawGlyph UTF32 {#:X}", static_cast<uint32_t>(glyph));
		}
	}

	// RPG_RT compatible for half-width (6) and full-width (12)
	// generalizes the algo for even bigger glyphs
	// FIXME: When using Freetype this can cause slow rendering speeds due to dynamic width
	auto get_width = [](int w) {
		return (w > 0) ? (w - 1) / 6 + 1 : 0;
	};

	// Wide characters cause an extra wait if the last printed character did not wait.
	if (prev_char_printable && !prev_char_waited) {
		auto& wide_font = is_exfont ? *Font::exfont : font;
		auto rect = wide_font.GetSize(glyph);
		auto width = get_width(rect.width);
		if (width >= 2) {
			prev_char_waited = true;
			++line_char_counter;
			SetWait(1);
			return false;
		}
	}

	if (!Player::IsPatchManiac()) {
		const auto rect = Text::Draw(*contents, contents_x, contents_y, font, system, text_color, glyph, is_exfont);
		contents_x += rect.x;
		SetWaitForCharacter(get_width(rect.x));
		return true;
	}
	std::array<int32_t, 9> packet = {8, x, y, maniac_face_x, maniac_face_y,
		contents_x + border_x, contents_y + border_y, text_color, maniac_text_color_row};
	const std::string source(token);
	RunManiacHook(packet.data(), packet.size(), &source,
		[this, packet, font = page_font, system = Cache::SystemOrBlack(), glyph, is_exfont, source, maniac_icon](bool hooked) {
			const Point position = hooked ? Point{
				Utils::Clamp(Game_Message::ReadManiacHookResult(maniac_hook_caller, 5, packet[5]), 0, width),
				Utils::Clamp(Game_Message::ReadManiacHookResult(maniac_hook_caller, 6, packet[6]), 0, height)} : Point{packet[5], packet[6]};
			const std::array<int32_t, 2> colors = hooked ? std::array<int32_t, 2>{
				Game_Message::ReadManiacHookResult(maniac_hook_caller, 7, packet[7]),
				Game_Message::ReadManiacHookResult(maniac_hook_caller, 8, packet[8])} : std::array<int32_t, 2>{packet[7], packet[8]};
			RenderManiacGlyph(*font, *system, glyph, is_exfont, source, maniac_icon, position, colors);
		}, true);
	return true;
}

void Window_Message::RenderManiacGlyph(Font& font, const Bitmap& system, char32_t glyph, bool is_exfont,
	std::string_view token, bool maniac_icon, Point position, const std::array<int32_t, 2>& colors) {
	const auto cell = ManiacColorCell(system, colors);
	Point rect;
	const auto size = font.GetCurrentStyle().size;
	if (static_cast<int64_t>(position.x) + size + 1 >= width
		|| static_cast<int64_t>(position.y) + size + 1 >= height) {
		rect.x = font.GetCurrentStyle().letter_spacing;
	} else if (maniac_icon) {
		std::array<int32_t, 2> numbers;
		Game_Message::ParseManiacNumbers(token.data() + 1, token.data() + token.size(), maniac_hook_caller, numbers);
		const auto image = Cache::Exfont();
		Rect source{0, 0, 12, 12};
		if (numbers[1] < 0) {
			const int columns = image->GetWidth() / 12;
			const int index = std::max(0, numbers[0]);
			if (columns > 0) {
				source.x = index % columns * 12;
				const uint32_t y_bits = static_cast<uint32_t>(index / columns) * 12u;
				std::memcpy(&source.y, &y_bits, sizeof(y_bits));
			}
			else source.y = -1;
		} else {
			// Native coordinates multiply in 32 bits before clamping the signed source X.
			const uint32_t x_bits = static_cast<uint32_t>(numbers[0]) * 12u;
			const uint32_t y_bits = static_cast<uint32_t>(numbers[1]) * 12u;
			std::memcpy(&source.x, &x_bits, sizeof(x_bits));
			std::memcpy(&source.y, &y_bits, sizeof(y_bits));
			source.x = std::max(0, source.x);
		}
		if (font.IsStyleApplied()) {
			auto style_guard = Font::exfont->ApplyStyle(font.GetCurrentStyle());
			rect = Font::exfont->RenderExfontCell(*maniac_glyphs, position.x, position.y, system, source, &cell);
		} else {
			rect = Font::exfont->RenderExfontCell(*maniac_glyphs, position.x, position.y, system, source, &cell);
		}
	} else {
		rect = Text::Draw(*maniac_glyphs, position.x, position.y, font, system, 0, glyph, is_exfont, &cell);
	}

	int glyph_width = rect.x;
	contents_x += glyph_width;
	int width = glyph_width > 0 ? (glyph_width - 1) / 6 + 1 : 0;
	SetWaitForCharacter(width);
}

bool Window_Message::DrawGlyph(Font& font, const Bitmap& system, const Font::ShapeRet& shape) {
	// RPG_RT compatible for half-width (6) and full-width (12)
	// generalizes the algo for even bigger glyphs
	// FIXME: This can cause slow rendering speeds for complex shapes
	auto get_width = [](int w) {
		return (w > 0) ? (w - 1) / 6 + 1 : 0;
	};

	DebugLogText("{}: MSG DrawGlyph Shape {}, {}", static_cast<uint32_t>(shape.code), get_width(shape.advance.x));

	// Wide characters cause an extra wait if the last printed character did not wait.
	if (prev_char_printable && !prev_char_waited) {
		auto width = get_width(shape.advance.x);
		if (width >= 2) {
			prev_char_waited = true;
			++line_char_counter;
			SetWait(1);
			return false;
		}
	}

	auto rect = font.Render(*contents, contents_x, contents_y, system, text_color, shape);

	int glyph_width = rect.x;
	contents_x += glyph_width;
	int width = get_width(glyph_width);
	SetWaitForCharacter(width);

	return true;
}

void Window_Message::IncrementLineCharCounter(int width) {
	// For speed 1, RPG_RT prints 2 half width chars every frame. This
	// resets anytime we print a full width character or another
	// character with a different speed.
	// To emulate this, we increment by 2 and clear the low bit anytime
	// we're not a speed 1 half width char.
	if (width == 1 && speed <= 1) {
		line_char_counter++;
	} else {
		line_char_counter = (line_char_counter & ~1) + 2;
	}
}

void Window_Message::UpdateCursorRect() {
	if (index >= 0) {
		int x_pos = 2;
		int y_pos = (pending_message.GetChoiceStartLine() + index) * 16;
		int width = contents->GetWidth() - 4;

		if (IsFaceEnabled()) {
			if (!Main_Data::game_system->IsMessageFaceRightPosition()) {
				x_pos += LeftMargin + Main_Data::game_system->GetMessageFaceWidth() + RightFaceMargin;
			}
			width = width - LeftMargin - Main_Data::game_system->GetMessageFaceWidth() - RightFaceMargin;
		}

		SetCursorRect(Rect(x_pos, y_pos, width, 16));
	} else {
		SetCursorRect(Rect());
	}
}

void Window_Message::WaitForInput() {
	const bool mouse_decision = Player::IsPatchManiac()
		? Main_Data::game_system->IsManiacMessageMouseEnabled()
			&& (Input::IsTriggered(Input::MOUSE_LEFT) || Input::IsTriggered(Input::MOUSE_RIGHT))
		: Player::player_config.extra_mouse_support.Get()
			&& (Input::IsTriggered(Input::MOUSE_LEFT) || Input::IsTriggered(Input::SCROLL_DOWN));

	if (Input::IsTriggered(Input::DECISION) ||
			mouse_decision ||
			Input::IsTriggered(Input::CANCEL)) {
		SetPause(false);
	}
}

void Window_Message::InputChoice() {
	int choice_result = -1;
	const bool maniac = Player::IsPatchManiac();
	const bool mouse_enabled = maniac && Main_Data::game_system->IsManiacMessageMouseEnabled();
	bool decision = Input::IsTriggered(Input::DECISION);
	const bool cancel = Input::IsTriggered(Input::CANCEL)
		|| (mouse_enabled && Input::IsTriggered(Input::MOUSE_RIGHT));

	if (maniac && !decision && mouse_enabled && Input::IsTriggered(Input::MOUSE_LEFT)) {
		const auto point = Input::GetMousePosition();
		const auto cursor = GetCursorRect();
		// Native hits include four pixels outside either horizontal cursor edge.
		const int left = x + 8 + cursor.x - 4;
		const int right = left + cursor.width + 8;
		for (int candidate = 0; candidate < item_max; ++candidate) {
			const int top = y + 8 + cursor.y + (candidate - index) * 16;
			if (point.x < left || point.x > right || point.y < top || point.y > top + 16) continue;
			if (candidate == index) decision = true;
			else {
				SetIndex(candidate);
				Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Cursor));
			}
			break;
		}
	} else if (!maniac) {
		decision = !cancel && (decision
			|| (Player::player_config.extra_mouse_support.Get() && Input::IsTriggered(Input::MOUSE_LEFT)));
	}

	if (decision) {
		if (!pending_message.IsChoiceEnabled(index)) {
			Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Buzzer));
			return;
		}

		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Decision));
		choice_result = index;
	} else if (cancel && pending_message.GetChoiceCancelType() > 0) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Cancel));
		choice_result = pending_message.GetChoiceCancelType() - 1;
	}

	if (choice_result >= 0) {
		auto& continuation = pending_message.GetChoiceContinuation();
		if (continuation) {
			aop = continuation(choice_result);
		}
		// This disables choices
		index = -1;
	}
}

void Window_Message::InputNumber() {
	number_input_window->SetVisible(true);

	if (Input::IsTriggered(Input::DECISION) ||
	    (!Player::IsPatchManiac() && Player::player_config.extra_mouse_support.Get() && Input::IsTriggered(Input::MOUSE_LEFT))) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_Decision));
		Main_Data::game_variables->Set(pending_message.GetNumberInputVariable(), number_input_window->GetNumber());
		if (Player::IsPatchManiac()) Game_Map::RefreshAfterManiacChange();
		else Game_Map::SetNeedRefresh(true);
		number_input_window->SetNumber(0);
		number_input_window->SetActive(false);
	}
}

void Window_Message::SetWaitForNonPrintable(int frames) {
	if (!instant_speed) {
		if (speed <= 1) {
			frames += (line_char_counter & 1);
		}
		SetWait(frames);
	}
	prev_char_waited = (instant_speed || frames > 0);
	prev_char_printable = false;
	// Non printables only contribute to character count after the first printable..
	if (line_char_counter > 0) {
		IncrementLineCharCounter(1);
	}
}

void Window_Message::SetWaitForCharacter(int width) {
	int frames = 0;
	if (!instant_speed && width > 0) {
		bool is_last_for_page;
		if (!shape_ret.empty()) {
			is_last_for_page = (shape_ret.size() == 1) && (
				(text.data() + text.size() - text_index) <= 1 || (*text_index == '\n' && *(text_index + 1) == '\f'));
		} else {
			is_last_for_page = (text.data() + text.size() - text_index) <= 1 || (*text_index == '\n' && *(text_index + 1) == '\f');
		}

		if (is_last_for_page) {
			// RPG_RT always waits 2 frames for last character on the page.
			// FIXME: Exfonts / wide last on page?
			DebugLogText("{}: is_last_for_page");
			frames = 2;
		} else {
			if (speed > 1) {
				frames = speed * width / 2 + 1;
			} else {
				frames = width / 2;
				if (width & 1) {
					bool is_last_for_line;
					if (!shape_ret.empty()) {
						is_last_for_line = shape_ret.size() == 1 && (*text_index == '\n');
					} else {
						is_last_for_line = (*text_index == '\n');
					}
					if (is_last_for_line) {
						DebugLogText("{}: is_last_for_line");
					}

					// RPG_RT waits for every even character. Also always waits
					// for the last character.
					frames += (line_char_counter & 1) || is_last_for_line;
				}
			}
		}
	}
	prev_char_waited = (instant_speed || frames > 0);
	prev_char_printable = true;
	SetWait(frames);
	IncrementLineCharCounter(width);
}

void Window_Message::SetWait(int frames) {
	assert(speed >= 1 && speed <= 20);
	DebugLogText("{}: MSG SetWait {}", frames);
	wait_count = frames;
}

bool Window_Message::IsFaceEnabled() const {
	return pending_message.IsFaceEnabled() && !Main_Data::game_system->GetMessageFaceName().empty();
}

