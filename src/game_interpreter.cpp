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
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <new>
#include <regex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <cassert>
#include "game_interpreter.h"
#include "game_maniac_self_variables.h"
#include "game_maniac_js.h"
#include "async_handler.h"
#include "cache.h"
#include "audio.h"
#include "game_dynrpg.h"
#include "filefinder.h"
#include "game_destiny.h"
#include "game_map.h"
#include "map_data.h"
#include "game_event.h"
#include "game_enemyparty.h"
#include "game_ineluki.h"
#include "game_player.h"
#include "game_targets.h"
#include "game_switches.h"
#include "game_strings.h"
#include "game_variables.h"
#include "game_party.h"
#include "game_actors.h"
#include "game_system.h"
#include "window_message.h"
#include "game_message.h"
#include "game_pictures.h"
#include "game_screen.h"
#include "game_interpreter_control_variables.h"
#include "game_windows.h"
#include "maniac_text_picture.h"
#include "maniac_picture_renderer.h"
#include "graphics.h"
#include "json_helper.h"
#include "maniac_patch.h"
#include "maniac_original_save.h"
#include "spriteset_map.h"
#include "sprite_character.h"
#include "scene_gameover.h"
#include "scene_map.h"
#include "scene_save.h"
#include "scene_settings.h"
#include "scene.h"
#include "game_clock.h"
#include "tilemap_layer.h"
#include "input.h"
#include "main_data.h"
#include "output.h"
#include "player.h"
#include "util_macro.h"
#include <lcf/reader_util.h>
#include <lcf/lsd/reader.h>
#include <lcf/reader_lcf.h>
#include <lcf/writer_lcf.h>
#include "game_battle.h"
#include "utils.h"
#include "transition.h"
#include "baseui.h"
#include "algo.h"
#include "rand.h"

using namespace Game_Interpreter_Shared;

namespace {
	double ManiacFixedPictureAngle(int numerator, int denominator) {
		const double angle = static_cast<double>(numerator) / (denominator ? denominator : 1);
		return std::fmod(angle, 360.0) + (angle < 0.0 ? 360.0 : 0.0);
	}
}

enum BranchSubcommand {
	eOptionBranchElse = 1
};

Game_Interpreter::Game_Interpreter(bool _main_flag) {
	main_flag = _main_flag;

	Clear();
}

Game_Interpreter::~Game_Interpreter() {
	if (maniac_text_picture_pending) maniac_text_picture_pending->Cancel();
}

// Clear.
void Game_Interpreter::Clear() {
	if (maniac_text_picture_pending) maniac_text_picture_pending->Cancel();
	maniac_text_picture_pending.reset();
	maniac_generated_command_pending.reset();
	_state = {};
	_keyinput = {};
	_async_op = {};
	maniac_dispatch = ManiacDispatch::Return;
}

// Is interpreter running.
bool Game_Interpreter::IsRunning() const {
	return !_state.stack.empty();
}

// Setup.
void Game_Interpreter::NormalizeSerializedManiacText(std::vector<lcf::rpg::EventCommand>& commands) {
	if (!Player::IsPatchManiac()) return;
	for (auto& command : commands) {
		const std::string_view text = command.string;
		const auto end = text.find('\0');
		if (end != std::string_view::npos) command.string = lcf::DBString(text.substr(0, end));
	}
}

void Game_Interpreter::Push(
	std::vector<lcf::rpg::EventCommand> _list,
	int event_id,
	bool started_by_decision_key,
	int event_page_id
) {
	const bool maniac = Player::IsPatchManiac();
	if (_list.empty() && !maniac) {
		return;
	}

	const int limit = maniac ? 8192 : call_stack_limit;
	if (_state.stack.size() > static_cast<size_t>(limit)) {
		Output::Error("Call Event limit ({}) has been exceeded", limit);
	}

	lcf::rpg::SaveEventExecFrame frame;
	frame.ID = _state.stack.size() + 1;
	frame.commands = std::move(_list);
	NormalizeSerializedManiacText(frame.commands);
	frame.current_command = 0;
	frame.triggered_by_decision_key = started_by_decision_key;
	frame.event_id = event_id;
	frame.maniac_event_id = event_id;
	frame.maniac_event_page_id = event_page_id;
	if (event_id > 0 && Main_Data::game_player) {
		frame.easyrpg_self_scope = 2;
		frame.easyrpg_self_map_id = Main_Data::game_player->GetMapId();
		frame.easyrpg_self_event_id = event_id;
	}

	if (_state.stack.empty() && main_flag && !Game_Battle::IsBattleRunning()) {
		Main_Data::game_system->ClearMessageFace();
		Main_Data::game_player->SetMenuCalling(false);
		Main_Data::game_player->SetEncounterCalling(false);
	}

	_state.stack.push_back(std::move(frame));
}


void Game_Interpreter::KeyInputState::fromSave(const lcf::rpg::SaveEventExecState& save) {
	*this = {};

	// Maniac Patch aware check functions for parameters that handle
	// keyboard and mouse through a bitmask
	bool is_maniac = Player::IsPatchManiac();
	auto check_key = [&](auto& var) {
		if (is_maniac) {
			return (var & 1) != 0;
		} else {
			return var != 0;
		}
	};
	auto check_mouse = [&](auto& var) {
		return (var & 2) != 0;
	};

	wait = save.keyinput_wait;
	// Maniac uses a full BER integer; only ordinary RPG_RT truncates to a byte.
	variable = is_maniac ? save.keyinput_variable : static_cast<uint8_t>(save.keyinput_variable);

	if (save.keyinput_all_directions) {
		keys[Keys::eDown] = true;
		keys[Keys::eLeft] = true;
		keys[Keys::eRight] = true;
		keys[Keys::eUp] = true;
	} else {
		if (Player::IsRPG2k3()) {
			keys[Keys::eDown] = check_key(save.keyinput_2k3down);
			keys[Keys::eLeft] = check_key(save.keyinput_2k3left);
			keys[Keys::eRight] = check_key(save.keyinput_2k3right);
			keys[Keys::eUp] = check_key(save.keyinput_2k3up);
		} else {
			keys[Keys::eDown] = check_key(save.keyinput_2kdown_2k3operators);
			keys[Keys::eLeft] = check_key(save.keyinput_2kleft_2k3shift);
			keys[Keys::eRight] = check_key(save.keyinput_2kright);
			keys[Keys::eUp] = check_key(save.keyinput_2kup);
		}
	}

	keys[Keys::eDecision] = check_key(save.keyinput_decision);
	keys[Keys::eCancel] = check_key(save.keyinput_cancel);

	if (Player::IsRPG2k3()) {
		keys[Keys::eShift] = check_key(save.keyinput_2kleft_2k3shift);
		keys[Keys::eNumbers] = check_key(save.keyinput_2kshift_2k3numbers);
		keys[Keys::eOperators] = check_key(save.keyinput_2kdown_2k3operators);

		if (is_maniac) {
			keys[Keys::eMouseLeft] = check_mouse(save.keyinput_decision);
			keys[Keys::eMouseRight] = check_mouse(save.keyinput_cancel);
			keys[Keys::eMouseMiddle] = check_mouse(save.keyinput_2kleft_2k3shift);
			keys[Keys::eMouseScrollUp] = check_mouse(save.keyinput_2k3up);
			keys[Keys::eMouseScrollDown] = check_mouse(save.keyinput_2k3down);
		}
	} else {
		keys[Keys::eShift] = check_key(save.keyinput_2kshift_2k3numbers);
	}

	time_variable = save.keyinput_time_variable;
	timed = save.keyinput_timed;
	// FIXME: Rm2k3 has no LSD chunk for this.
	wait_frames = 0;
}

void Game_Interpreter::KeyInputState::toSave(lcf::rpg::SaveEventExecState& save) const {
	save.keyinput_wait = 0;
	save.keyinput_variable = 0;
	save.keyinput_all_directions = 0;
	save.keyinput_decision = 0;
	save.keyinput_cancel = 0;
	save.keyinput_2kshift_2k3numbers = 0;
	save.keyinput_2kdown_2k3operators = 0;
	save.keyinput_2kleft_2k3shift = 0;
	save.keyinput_2kright = 0;
	save.keyinput_2kup = 0;
	save.keyinput_time_variable = 0;
	save.keyinput_2k3down = 0;
	save.keyinput_2k3left = 0;
	save.keyinput_2k3right = 0;
	save.keyinput_2k3up = 0;
	save.keyinput_timed = 0;

	save.keyinput_wait = wait;
	save.maniac_keyinput_encoding = Player::IsPatchManiac();
	save.keyinput_variable = save.maniac_keyinput_encoding ? variable : static_cast<uint8_t>(variable);

	if (keys[Keys::eDown]
			&& keys[Keys::eLeft]
			&& keys[Keys::eRight]
			&& keys[Keys::eUp]) {
		save.keyinput_all_directions = true;
	} else {
		if (Player::IsRPG2k3()) {
			save.keyinput_2k3down = keys[Keys::eDown];
			save.keyinput_2k3left = keys[Keys::eLeft];
			save.keyinput_2k3right = keys[Keys::eRight];
			save.keyinput_2k3up = keys[Keys::eUp];
		} else {
			// RM2k uses these chunks for directions.
			save.keyinput_2kdown_2k3operators = keys[Keys::eDown];
			save.keyinput_2kleft_2k3shift = keys[Keys::eLeft];
			save.keyinput_2kright = keys[Keys::eRight];
			save.keyinput_2kup = keys[Keys::eUp];
		}
	}

	save.keyinput_decision = keys[Keys::eDecision];
	save.keyinput_cancel = keys[Keys::eCancel];

	if (Player::IsRPG2k3()) {
		save.keyinput_2kleft_2k3shift = keys[Keys::eShift];
		save.keyinput_2kshift_2k3numbers = keys[Keys::eNumbers];
		save.keyinput_2kdown_2k3operators = keys[Keys::eOperators];

		if (Player::IsPatchManiac()) {
			if (keys[Keys::eMouseLeft]) {
				save.keyinput_decision |= 2;
			}
			if (keys[Keys::eMouseRight]) {
				save.keyinput_cancel |= 2;
			}
			if (keys[Keys::eMouseMiddle]) {
				save.keyinput_2kleft_2k3shift |= 2;
			}
			if (keys[Keys::eMouseScrollUp]) {
				save.keyinput_2k3up |= 2;
			}
			if (keys[Keys::eMouseScrollDown]) {
				save.keyinput_2k3down |= 2;
			}
		}
	} else {
		save.keyinput_2kshift_2k3numbers = keys[Keys::eShift];
	}

	save.keyinput_time_variable = time_variable;
	save.keyinput_timed = timed;
	// FIXME: Rm2k3 has no LSD chunk for this.
	//void = wait_frames;
}


const lcf::rpg::SaveEventExecState& Game_Interpreter::GetState() const {
	return _state;
}

lcf::rpg::SaveEventExecState Game_Interpreter::GetSaveState() {
	auto save = _state;
	_keyinput.toSave(save);
	return save;
}


void Game_Interpreter::SetupWait(int duration) {
	if (duration == 0) {
		// 0.0 waits 1 frame
		_state.wait_time = 1;
	} else {
		_state.wait_time = duration * DEFAULT_FPS / 10;
	}
}

void Game_Interpreter::SetupWaitFrames(int duration) {
	if (duration == 0) {
		// 0.0 waits 1 frame
		_state.wait_time = 1;
	} else {
		_state.wait_time = duration;
	}
}

bool Game_Interpreter::ReachedLoopLimit() const {
	return loop_count >= loop_limit;
}

int Game_Interpreter::GetThisEventId() const {
	auto event_id = GetCurrentEventId();

	if (event_id == 0 && (Player::IsRPG2k3E() || Player::IsPatchCommonThisEvent())) {
		// RM2k3E allows "ThisEvent" commands to run from called
		// common events. It operates on the last map event in
		// the call stack.
		for (auto iter = _state.stack.rbegin()++;
				iter != _state.stack.rend(); ++iter) {
			if (iter->event_id != 0) {
				event_id = iter->event_id;
				break;
			}
		}
	}

	return event_id;
}

uint8_t& Game_Interpreter::ReserveSubcommandIndex(int indent) {
	auto& frame = GetFrame();

	auto& path = frame.subcommand_path;
	if (indent >= (int)path.size()) {
		// This fixes an RPG_RT bug where RPG_RT would resize
		// the array with uninitialized values.
		path.resize(indent + 1, subcommand_sentinel);
	}
	return path[indent];
}

void Game_Interpreter::SetSubcommandIndex(int indent, int idx) {
	ReserveSubcommandIndex(indent) = idx;
}

int Game_Interpreter::GetSubcommandIndex(int indent) const {
	auto* frame = GetFramePtr();
	if (frame == nullptr) {
		return subcommand_sentinel;
	}
	auto& path = frame->subcommand_path;
	if ((int)path.size() <= indent) {
		return subcommand_sentinel;
	}
	return path[indent];
}

// Update
void Game_Interpreter::Update(bool reset_loop_count) {
#ifdef ENABLE_DYNAMIC_INTERPRETER_CONFIG
	auto* previous_flags = Player::active_interpreter_flags;
	Player::active_interpreter_flags = &_state.easyrpg_runtime_flags;
	auto flags_guard = lcf::makeScopeGuard([previous_flags]() {
		Player::active_interpreter_flags = previous_flags;
	});
#endif

	if (reset_loop_count) {
		loop_count = 0;
		loop_limit = Player::IsPatchManiac() ? Player::game_config.GetManiacExecutionLimit() : 10000;
		maniac_dispatch = ManiacDispatch::Return;
	}

	// Always reset async status when we enter interpreter loop.
	_async_op = {};

	if (!IsRunning()) {
		return;
	}
	if (Input::IsTriggered(Input::DEBUG_ABORT_EVENT) && Player::debug_flag && !Game_Battle::IsBattleRunning()) {
		if (Game_Message::IsMessageActive()) {
			Game_Message::GetWindow()->FinishMessageProcessing();
		}
		if (!Main_Data::game_player->GetMoveRoute().move_commands.empty()) {
			Main_Data::game_player->CancelMoveRoute();
		}
		EndEventProcessing();
		return;
	}

	while (loop_count < loop_limit) {
		// If something is calling a menu, we're allowed to execute only 1 command per interpreter. So we pass through if loop_count == 0, and stop at 1 or greater.
		// RPG_RT compatible behavior.
		if (maniac_dispatch != ManiacDispatch::Continue && loop_count > 0 && Scene::instance->HasRequestedScene()) {
			break;
		}

		// Previous command triggered an async operation.
		if (IsAsyncPending()) {
			break;
		}

		// A pf mode tail re-enters the native dispatcher after its wait
		// prelude. Async yields retain this position until Update(false).
		if (maniac_dispatch != ManiacDispatch::Continue) {
			if (main_flag) {
				if (Main_Data::game_player->IsBoardingOrUnboarding())
					break;

				if (Main_Data::game_player->InVehicle() && Main_Data::game_player->GetVehicle()->IsAscendingOrDescending())
					break;

				if (Game_Message::IsMessagePending())
					break;
			} else {
				if (Game_Message::IsMessageActive() && _state.show_message) {
					break;
				}
			}

			_state.show_message = false;
			_state.abort_on_escape = false;

			if (_state.wait_time > 0) {
				_state.wait_time--;
				break;
			}

			if (_state.wait_key_enter) {
				if (Game_Message::IsMessageActive()) {
					break;
				}

				if (!Input::IsTriggered(Input::DECISION)) {
					break;
				}
				_state.wait_key_enter = false;
			}

			if (_state.wait_movement) {
				if (Game_Map::IsAnyMovePending()) {
					break;
				}
				_state.wait_movement = false;
			}

			if (_keyinput.wait) {
				if (Game_Message::IsMessageActive()) {
					break;
				}

				const int key = _keyinput.CheckInput();
				Main_Data::game_variables->Set(_keyinput.variable, key);
				Game_Map::SetNeedRefreshForVarChange(_keyinput.variable);
				if (Player::IsPatchManiac()) Game_Map::RefreshAfterManiacChange();
				if (key == 0) {
					++_keyinput.wait_frames;
					break;
				}
				if (_keyinput.timed) {
					// 10 per second
					Main_Data::game_variables->Set(_keyinput.time_variable,
							(_keyinput.wait_frames * 10) / Game_Clock::GetTargetGameFps());
					Game_Map::SetNeedRefreshForVarChange(_keyinput.time_variable);
				}
				_keyinput.wait = false;
			}
		}

		auto* frame = GetFramePtr();
		if (frame == nullptr) {
			break;
		}

		if (!Player::IsPatchManiac() && Game_Map::GetNeedRefresh()) {
			Game_Map::Refresh();
		}

		// Previous operations could have modified the stack.
		// So we need to fetch the frame again.
		frame = GetFramePtr();
		if (frame == nullptr) {
			break;
		}

		// Pop any completed stack frames
		if (frame->current_command >= (int)frame->commands.size()) {
			const bool more = OnFinishStackFrame();
			maniac_dispatch = ManiacDispatch::Return;
			if (more || Player::IsPatchManiac()) ++loop_count;
			if (!more) {
				break;
			}
			continue;
		}

		// Save the frame index before we call events.
		int current_frame_idx = _state.stack.size() - 1;

		const int index_before_exec = frame->current_command;
		if (!ExecuteCommand()) {
			maniac_dispatch = ManiacDispatch::Return;
			break;
		}
		if (Player::IsPatchManiac() && _async_op.GetType() == AsyncOp::eYieldRepeat) {
			// Resource acquisition suspends the current native dispatch;
			// it has not returned and consumed its budget yet.
			maniac_dispatch = ManiacDispatch::Continue;
		}

		if (maniac_dispatch != ManiacDispatch::Continue
			&& Game_Battle::IsBattleRunning() && Player::IsRPG2k3() && Game_Battle::CheckWin()) {
			// Interpreter is cancelled when a win condition is fulfilled in RPG2k3 battle
			if (Player::IsPatchManiac() && maniac_dispatch != ManiacDispatch::Continue) ++loop_count;
			break;
		}

		if (maniac_dispatch != ManiacDispatch::Continue) ++loop_count;
		if (maniac_dispatch == ManiacDispatch::Finish) {
			maniac_dispatch = ManiacDispatch::Return;
			break;
		}

		// Last event command removed the frame? We're done.
		if (current_frame_idx >= (int)_state.stack.size() ) {
			continue;
		}

		// Note: In the case we executed a CallEvent command, be sure to
		// increment the old frame and not the new one we just pushed.
		frame = &_state.stack[current_frame_idx];

		// Only do auto increment if the command didn't manually
		// change the index.
		if (index_before_exec == frame->current_command) {
			frame->current_command++;
		}
	}

	if (_async_op.GetType() == AsyncOp::eYieldRepeat) {
		// Also rewind when a yielding command reaches the configured limit.
		// YieldRepeat commands must not change the command cursor themselves.
		if (auto* frame = GetFramePtr()) --frame->current_command;
	}

	if (loop_count > loop_limit - 1) {
		auto* frame = GetFramePtr();
		int event_id = frame ? frame->event_id : 0;
		Output::Debug("Event {} exceeded execution limit", event_id);
	}

	if (!Player::IsPatchManiac() && Game_Map::GetNeedRefresh()) {
		Game_Map::Refresh();
	}
}

// Setup Starting Event
void Game_Interpreter::Push(Game_Event* ev) {
	const auto depth = _state.stack.size();
	Push(ev->GetList(), ev->GetId(), ev->WasStartedByDecisionKey(), ev->GetActivePage() ? ev->GetActivePage()->ID : 0);
	if (Player::IsPatchManiac() && _state.stack.size() > depth) {
		_state.stack.back().maniac_event_info = 0x10 | static_cast<int>(ev->GetTrigger());
	}
}

void Game_Interpreter::Push(Game_Event* ev, const lcf::rpg::EventPage* page, bool triggered_by_decision_key) {
	const auto depth = _state.stack.size();
	Push(page->event_commands, ev->GetId(), triggered_by_decision_key, page->ID);
	if (Player::IsPatchManiac() && _state.stack.size() > depth) {
		_state.stack.back().maniac_event_info = 0x10 | static_cast<int>(page->trigger);
	}
}

void Game_Interpreter::Push(Game_CommonEvent* ev) {
	auto list = ev->GetList();
	if (!list.empty() || (Player::IsPatchManiac())) {
		Push(std::move(list), 0, false);
		auto& frame = _state.stack.back();
		if (Player::IsPatchManiac()) {
			frame.maniac_event_info = 0x20 | static_cast<int>(ev->GetTrigger());
			frame.maniac_event_id = ev->GetId();
		}
		frame.easyrpg_self_scope = 1;
		frame.easyrpg_self_map_id = 0;
		frame.easyrpg_self_event_id = ev->GetId();
	}
}

int32_t Game_Interpreter::ReadVariable(int id) const {
	if (id >= 0 || !Player::IsPatchManiac()) {
		return Main_Data::game_variables->Get(id);
	}
	const auto* frame = GetFramePtr();
	const auto* state = Main_Data::game_maniac_self_variables.get();
	const auto* bank = frame && state
		? state->Find(frame->easyrpg_self_scope, frame->easyrpg_self_map_id, frame->easyrpg_self_event_id)
		: nullptr;
	return Game_ManiacSelfVariables::ReadNumber(bank, id);
}

int Game_Interpreter::ReadSwitch(int id) const {
	if (id == 0 && Player::IsPatchManiac()) {
		// The native ordinary switch getter ignores ID 0 even though Maniac
		// commands can write its physical byte slot.
		return 0;
	}
	if (id >= 0 || !Player::IsPatchManiac()) {
		return Main_Data::game_switches->GetInt(id);
	}
	const auto* frame = GetFramePtr();
	const auto* state = Main_Data::game_maniac_self_variables.get();
	const auto* bank = frame && state
		? state->Find(frame->easyrpg_self_scope, frame->easyrpg_self_map_id, frame->easyrpg_self_event_id)
		: nullptr;
	return Game_ManiacSelfVariables::ReadSwitch(bank, id);
}

std::string_view Game_Interpreter::ReadString(int id) const {
	if (id >= 0 || !Player::IsPatchManiac()) {
		return Main_Data::game_strings->Get(id);
	}
	const auto* frame = GetFramePtr();
	const auto* state = Main_Data::game_maniac_self_variables.get();
	const auto* bank = frame && state
		? state->Find(frame->easyrpg_self_scope, frame->easyrpg_self_map_id, frame->easyrpg_self_event_id)
		: nullptr;
	const auto slot = -static_cast<int64_t>(id);
	return bank && slot < static_cast<int64_t>(bank->strings.size())
		? static_cast<std::string_view>(bank->strings[static_cast<size_t>(slot)]) : std::string_view{};
}

bool Game_Interpreter::IsManiacStringPresent(int id) const {
	if (id > 0) return Main_Data::game_strings->IsPresent(id);
	if (id == 0 || !Player::IsPatchManiac()) return false;
	const auto* frame = GetFramePtr();
	const auto* state = Main_Data::game_maniac_self_variables.get();
	const auto* bank = frame && state
		? state->Find(frame->easyrpg_self_scope, frame->easyrpg_self_map_id, frame->easyrpg_self_event_id)
		: nullptr;
	return Game_ManiacSelfVariables::IsStringPresent(bank, static_cast<size_t>(-static_cast<int64_t>(id)));
}

bool Game_Interpreter::CheckGameOver() {
	if (!Game_Battle::IsBattleRunning() && !Main_Data::game_party->IsAnyActive()) {
		// Empty party is allowed
		if (Main_Data::game_party->GetBattlerCount() > 0) {
			Scene::instance->SetRequestedScene(std::make_shared<Scene_Gameover>());
			return true;
		}
	}
	return false;
}

void Game_Interpreter::SkipToNextConditional(std::initializer_list<Cmd> codes, int indent) {
	auto& frame = GetFrame();
	const auto& list = frame.commands;
	auto& index = frame.current_command;

	if (index >= static_cast<int>(list.size())) {
		return;
	}

	for (++index; index < static_cast<int>(list.size()); ++index) {
		const auto& com = list[index];
		if (com.indent > indent) {
			continue;
		}
		if (std::find(codes.begin(), codes.end(), static_cast<Cmd>(com.code)) != codes.end()) {
			break;
		}
	}
}

// Execute Command.
bool Game_Interpreter::ExecuteCommand() {
	auto& frame = GetFrame();
	const auto& com = frame.commands[frame.current_command];
	return DispatchCommand(com);
}

bool Game_Interpreter::DispatchCommand(lcf::rpg::EventCommand const& com) {
	maniac_dispatch = ManiacDispatch::Return;
	if (Player::IsPatchManiac()) {
		if (com.code == 0) {
			// Native END returns once after popping a called frame or
			// resetting/finishing the base frame (045660:1804-1819).
			maniac_dispatch = OnFinishStackFrame() ? ManiacDispatch::Return : ManiacDispatch::Finish;
			return true;
		}
		switch (com.code) {
			case 10: case 40: case 3024: case 3035: case 12110:
			case 20110: case 20141: case 20713: case 20722: case 20732:
			case 22011: case 22410: case 23311:
				maniac_dispatch = ManiacDispatch::Continue;
				return true;
		}
		if (Player::IsManiacPf()) {
			// The shared native mode tails fetch the next command before
			// returning to the budget loop. 3019 redispatches through here
			// so the generated command supplies its own return behaviour.
			switch (com.code) {
				case 3001: case 3005: case 3006: case 3007: case 3008:
				case 3013: case 3016: case 3017: case 3018:
				case 3020: case 3021: case 3022:
				case 10120: case 10130: case 10210: case 10220:
				case 10670: case 10680:
				case 10820: case 10860: case 10870: case 10910: case 10920:
				case 11110: case 11130:
				case 11510: case 11520: case 11530: case 11540: case 11550:
				case 11710: case 12010: case 12120: case 12220:
				case 12410: case 22010:
					maniac_dispatch = ManiacDispatch::Continue;
					break;
			}
		}
		// These native reserved/marker routes have no payload, but return.
		if (com.code == 3022 || com.code == 3023 || com.code == 3027 || com.code == 3034) {
			return true;
		}
	}
	return ExecuteCommand(com);
}

bool Game_Interpreter::ExecuteCommand(lcf::rpg::EventCommand const& com) {
	switch (static_cast<Cmd>(com.code)) {
		case Cmd::CallCommonEvent:
			return Player::IsPatchManiac()
				? CommandManiacCallCommonEvent(com) : true;
		case Cmd::ShowMessage:
			return CmdSetup<&Game_Interpreter::CommandShowMessage, 0>(com);
		case Cmd::MessageOptions:
			return CmdSetup<&Game_Interpreter::CommandMessageOptions, 4>(com);
		case Cmd::ChangeFaceGraphic:
			return Player::IsPatchManiac() ? CommandChangeFaceGraphic(com)
				: CmdSetup<&Game_Interpreter::CommandChangeFaceGraphic, 3>(com);
		case Cmd::ShowChoice:
			return CmdSetup<&Game_Interpreter::CommandShowChoices, 1>(com);
		case Cmd::ShowChoiceOption:
			return CmdSetup<&Game_Interpreter::CommandShowChoiceOption, 1>(com);
		case Cmd::ShowChoiceEnd:
			return CmdSetup<&Game_Interpreter::CommandShowChoiceEnd, 0>(com);
		case Cmd::InputNumber:
			return CmdSetup<&Game_Interpreter::CommandInputNumber, 2>(com);
		case Cmd::ControlSwitches:
			return CmdSetup<&Game_Interpreter::CommandControlSwitches, 4>(com);
		case Cmd::ControlVars:
			return CmdSetup<&Game_Interpreter::CommandControlVariables, 7>(com);
		case Cmd::TimerOperation:
			return CmdSetup<&Game_Interpreter::CommandTimerOperation, 5>(com);
		case Cmd::ChangeGold:
			return CmdSetup<&Game_Interpreter::CommandChangeGold, 3>(com);
		case Cmd::ChangeItems:
			return CmdSetup<&Game_Interpreter::CommandChangeItems, 5>(com);
		case Cmd::ChangePartyMembers:
			return CmdSetup<&Game_Interpreter::CommandChangePartyMember, 3>(com);
		case Cmd::ChangeExp:
			return CmdSetup<&Game_Interpreter::CommandChangeExp, 6>(com);
		case Cmd::ChangeLevel:
			return CmdSetup<&Game_Interpreter::CommandChangeLevel, 6>(com);
		case Cmd::ChangeParameters:
			return CmdSetup<&Game_Interpreter::CommandChangeParameters, 6>(com);
		case Cmd::ChangeSkills:
			return CmdSetup<&Game_Interpreter::CommandChangeSkills, 5>(com);
		case Cmd::ChangeEquipment:
			return CmdSetup<&Game_Interpreter::CommandChangeEquipment, 5>(com);
		case Cmd::ChangeHP:
			return CmdSetup<&Game_Interpreter::CommandChangeHP, 6>(com);
		case Cmd::ChangeSP:
			return CmdSetup<&Game_Interpreter::CommandChangeSP, 5>(com);
		case Cmd::ChangeCondition:
			return CmdSetup<&Game_Interpreter::CommandChangeCondition, 4>(com);
		case Cmd::FullHeal:
			return CmdSetup<&Game_Interpreter::CommandFullHeal, 2>(com);
		case Cmd::SimulatedAttack:
			return CmdSetup<&Game_Interpreter::CommandSimulatedAttack, 8>(com);
		case Cmd::Wait:
			return CmdSetup<&Game_Interpreter::CommandWait, 1>(com);
		case Cmd::PlayBGM:
			return CmdSetup<&Game_Interpreter::CommandPlayBGM, 4>(com);
		case Cmd::FadeOutBGM:
			return CmdSetup<&Game_Interpreter::CommandFadeOutBGM, 1>(com);
		case Cmd::PlaySound:
			return CmdSetup<&Game_Interpreter::CommandPlaySound, 3>(com);
		case Cmd::EndEventProcessing:
			return CmdSetup<&Game_Interpreter::CommandEndEventProcessing, 0>(com);
		case Cmd::Comment:
		case Cmd::Comment_2:
			return CmdSetup<&Game_Interpreter::CommandComment, 0>(com);
		case Cmd::GameOver:
			return CmdSetup<&Game_Interpreter::CommandGameOver, 0>(com);
		case Cmd::ChangeHeroName:
			return CmdSetup<&Game_Interpreter::CommandChangeHeroName, 1>(com);
		case Cmd::ChangeHeroTitle:
			return CmdSetup<&Game_Interpreter::CommandChangeHeroTitle, 1>(com);
		case Cmd::ChangeSpriteAssociation:
			return CmdSetup<&Game_Interpreter::CommandChangeSpriteAssociation, 3>(com);
		case Cmd::ChangeActorFace:
			return CmdSetup<&Game_Interpreter::CommandChangeActorFace, 2>(com);
		case Cmd::ChangeVehicleGraphic:
			return CmdSetup<&Game_Interpreter::CommandChangeVehicleGraphic, 2>(com);
		case Cmd::ChangeSystemBGM:
			return CmdSetup<&Game_Interpreter::CommandChangeSystemBGM, 5>(com);
		case Cmd::ChangeSystemSFX:
			return CmdSetup<&Game_Interpreter::CommandChangeSystemSFX, 4>(com);
		case Cmd::ChangeSystemGraphics:
			return CmdSetup<&Game_Interpreter::CommandChangeSystemGraphics, 2>(com);
		case Cmd::ChangeScreenTransitions:
			return CmdSetup<&Game_Interpreter::CommandChangeScreenTransitions, 2>(com);
		case Cmd::MemorizeLocation:
			return CmdSetup<&Game_Interpreter::CommandMemorizeLocation, 3>(com);
		case Cmd::SetVehicleLocation:
			return CmdSetup<&Game_Interpreter::CommandSetVehicleLocation, 5>(com);
		case Cmd::ChangeEventLocation:
			return CmdSetup<&Game_Interpreter::CommandChangeEventLocation, 4>(com);
		case Cmd::TradeEventLocations:
			return CmdSetup<&Game_Interpreter::CommandTradeEventLocations, 2>(com);
		case Cmd::StoreTerrainID:
			return CmdSetup<&Game_Interpreter::CommandStoreTerrainID, 4>(com);
		case Cmd::StoreEventID:
			return CmdSetup<&Game_Interpreter::CommandStoreEventID, 4>(com);
		case Cmd::EraseScreen:
			return CmdSetup<&Game_Interpreter::CommandEraseScreen, 1>(com);
		case Cmd::ShowScreen:
			return CmdSetup<&Game_Interpreter::CommandShowScreen, 1>(com);
		case Cmd::TintScreen:
			return CmdSetup<&Game_Interpreter::CommandTintScreen, 6>(com);
		case Cmd::FlashScreen:
			return CmdSetup<&Game_Interpreter::CommandFlashScreen, 6>(com);
		case Cmd::ShakeScreen:
			return CmdSetup<&Game_Interpreter::CommandShakeScreen, 4>(com);
		case Cmd::WeatherEffects:
			return CmdSetup<&Game_Interpreter::CommandWeatherEffects, 2>(com);
		case Cmd::ShowPicture:
			return CommandShowPicture(com);
		case Cmd::MovePicture:
			return CommandMovePicture(com);
		case Cmd::ErasePicture:
			return CmdSetup<&Game_Interpreter::CommandErasePicture, 1>(com);
		case Cmd::PlayerVisibility:
			return CmdSetup<&Game_Interpreter::CommandPlayerVisibility, 1>(com);
		case Cmd::MoveEvent:
			return CmdSetup<&Game_Interpreter::CommandMoveEvent, 4>(com);
		case Cmd::MemorizeBGM:
			return CmdSetup<&Game_Interpreter::CommandMemorizeBGM, 0>(com);
		case Cmd::PlayMemorizedBGM:
			return CmdSetup<&Game_Interpreter::CommandPlayMemorizedBGM, 0>(com);
		case Cmd::KeyInputProc:
			return CmdSetup<&Game_Interpreter::CommandKeyInputProc, 5>(com);
		case Cmd::ChangeMapTileset:
			return CmdSetup<&Game_Interpreter::CommandChangeMapTileset, 1>(com);
		case Cmd::ChangePBG:
			return CmdSetup<&Game_Interpreter::CommandChangePBG, 6>(com);
		case Cmd::ChangeEncounterSteps:
			return CmdSetup<&Game_Interpreter::CommandChangeEncounterSteps, 1>(com);
		case Cmd::TileSubstitution:
			return CmdSetup<&Game_Interpreter::CommandTileSubstitution, 3>(com);
		case Cmd::TeleportTargets:
			return CmdSetup<&Game_Interpreter::CommandTeleportTargets, 6>(com);
		case Cmd::ChangeTeleportAccess:
			return CmdSetup<&Game_Interpreter::CommandChangeTeleportAccess, 1>(com);
		case Cmd::EscapeTarget:
			return CmdSetup<&Game_Interpreter::CommandEscapeTarget, 5>(com);
		case Cmd::ChangeEscapeAccess:
			return CmdSetup<&Game_Interpreter::CommandChangeEscapeAccess, 1>(com);
		case Cmd::ChangeSaveAccess:
			return CmdSetup<&Game_Interpreter::CommandChangeSaveAccess, 1>(com);
		case Cmd::ChangeMainMenuAccess:
			return CmdSetup<&Game_Interpreter::CommandChangeMainMenuAccess, 1>(com);
		case Cmd::ConditionalBranch:
			if (Player::IsPatchManiac() && com.parameters.size() < 6) {
				auto padded = com;
				padded.parameters = lcf::DBArray<int32_t>(6);
				std::copy(com.parameters.begin(), com.parameters.end(), padded.parameters.begin());
				return CommandConditionalBranch(padded);
			}
			return CmdSetup<&Game_Interpreter::CommandConditionalBranch, 6>(com);
		case Cmd::Label:
			return true;
		case Cmd::JumpToLabel:
			if (Player::IsPatchManiac()) return CommandJumpToLabel(com);
			return CmdSetup<&Game_Interpreter::CommandJumpToLabel, 1>(com);
		case Cmd::Loop:
			return CmdSetup<&Game_Interpreter::CommandLoop, 0>(com);
		case Cmd::BreakLoop:
			return CmdSetup<&Game_Interpreter::CommandBreakLoop, 2>(com);
		case Cmd::EndLoop:
			return CmdSetup<&Game_Interpreter::CommandEndLoop, 0>(com);
		case Cmd::EraseEvent:
			return CmdSetup<&Game_Interpreter::CommandEraseEvent, 0>(com);
		case Cmd::CallEvent:
			return CommandCallEvent(com);
		case Cmd::ReturntoTitleScreen:
			return CmdSetup<&Game_Interpreter::CommandReturnToTitleScreen, 0>(com);
		case Cmd::ChangeClass:
			return CmdSetup<&Game_Interpreter::CommandChangeClass, 7>(com);
		case Cmd::ChangeBattleCommands:
			return CmdSetup<&Game_Interpreter::CommandChangeBattleCommands, 4>(com);
		case Cmd::ElseBranch:
			return CmdSetup<&Game_Interpreter::CommandElseBranch, 0>(com);
		case Cmd::EndBranch:
			return CmdSetup<&Game_Interpreter::CommandEndBranch, 0>(com);
		case Cmd::ExitGame:
			return CmdSetup<&Game_Interpreter::CommandExitGame, 0>(com);
		case Cmd::ToggleFullscreen:
			return CmdSetup<&Game_Interpreter::CommandToggleFullscreen, 0>(com);
		case Cmd::OpenVideoOptions:
			return CmdSetup<&Game_Interpreter::CommandOpenVideoOptions, 0>(com);
		case Cmd::Maniac_GetSaveInfo:
			return CmdSetup<&Game_Interpreter::CommandManiacGetSaveInfo, 12>(com);
		case Cmd::Maniac_Load:
			return CmdSetup<&Game_Interpreter::CommandManiacLoad, 3>(com);
		case Cmd::Maniac_Save:
			return CmdSetup<&Game_Interpreter::CommandManiacSave, 4>(com);
		case Cmd::Maniac_EndLoadProcess:
			return CmdSetup<&Game_Interpreter::CommandManiacEndLoadProcess, 0>(com);
		case Cmd::Maniac_GetMousePosition:
			return CmdSetup<&Game_Interpreter::CommandManiacGetMousePosition, 2>(com);
		case Cmd::Maniac_SetMousePosition:
			return CmdSetup<&Game_Interpreter::CommandManiacSetMousePosition, 0>(com);
		case Cmd::Maniac_ShowStringPicture:
			return CommandManiacShowStringPicture(com);
		case Cmd::Maniac_GetPictureInfo:
			return CmdSetup<&Game_Interpreter::CommandManiacGetPictureInfo, 8>(com);
		case Cmd::Maniac_ControlVarArray:
			return CmdSetup<&Game_Interpreter::CommandManiacControlVarArray, 5>(com);
		case Cmd::Maniac_KeyInputProcEx:
			return CmdSetup<&Game_Interpreter::CommandManiacKeyInputProcEx, 4>(com);
		case Cmd::Maniac_RewriteMap:
			return CmdSetup<&Game_Interpreter::CommandManiacRewriteMap, 9>(com);
		case Cmd::Maniac_ControlGlobalSave:
			return CommandManiacControlGlobalSave(com);
		case Cmd::Maniac_ChangePictureId:
			return CmdSetup<&Game_Interpreter::CommandManiacChangePictureId, 6>(com);
		case Cmd::Maniac_SetGameOption:
			return CmdSetup<&Game_Interpreter::CommandManiacSetGameOption, 0>(com);
		case Cmd::Maniac_ControlStrings:
			return CmdSetup<&Game_Interpreter::CommandManiacControlStrings, 8>(com);
		case Cmd::Maniac_CallCommand:
			return CmdSetup<&Game_Interpreter::CommandManiacCallCommand, 0>(com);
		case Cmd::Maniac_GetGameInfo:
			return CmdSetup<&Game_Interpreter::CommandManiacGetGameInfo, 8>(com);
		case Cmd::Maniac_EditTile:
			return CommandManiacDrawTileOnPicture(com);
		case Cmd::Maniac_EditPicture:
			return CommandManiacEditPicture(com);
		case Cmd::Maniac_WritePicture:
			return CommandManiacWritePicture(com);
		case Cmd::Maniac_ControlTextProcessing:
			return CommandManiacControlTextProcessing(com);
		case Cmd::Maniac_ZoomScreen:
			return CommandManiacZoomScreen(com);
		case Cmd::Maniac_JavaScript:
			return CommandManiacJavaScript(com);
		case Cmd::Maniac_JavaScriptContinuation:
			return true;
		case Cmd::Maniac_Console:
			return CommandManiacConsole(com);
		case Cmd::Maniac_Selfvar:
			return CommandManiacSelfvar(com);
		case Cmd::Maniac_ChangeTroopMember:
			return CommandManiacChangeTroopMember(com);
		case Cmd::Maniac_ControlMediaOption:
			return CommandManiacControlMediaOption(com);
		case Cmd::EasyRpg_SetInterpreterFlag:
			return CmdSetup<&Game_Interpreter::CommandEasyRpgSetInterpreterFlag, 2>(com);
		case Cmd::EasyRpg_ProcessJson:
			return CmdSetup<&Game_Interpreter::CommandEasyRpgProcessJson, 8>(com);
		case Cmd::EasyRpg_CloneMapEvent:
			return CmdSetup<&Game_Interpreter::CommandEasyRpgCloneMapEvent, 10>(com);
		case Cmd::EasyRpg_DestroyMapEvent:
			return CmdSetup<&Game_Interpreter::CommandEasyRpgDestroyMapEvent, 2>(com);
		default:
			if (Player::IsPatchManiac()) maniac_dispatch = ManiacDispatch::Continue;
			return true;
	}
}

bool Game_Interpreter::OnFinishStackFrame() {
	auto& frame = GetFrame();

	const bool is_base_frame = _state.stack.size() == 1;

	if (main_flag && is_base_frame && !Game_Battle::IsBattleRunning()) {
		Main_Data::game_system->ClearMessageFace();
	}

	int event_id = frame.event_id;

	if (is_base_frame && event_id > 0) {
		Game_Event* evnt = Game_Map::GetEvent(event_id);
		if (!evnt) {
			Output::Error("Call stack finished with invalid event id {}. This can be caused by a vehicle teleport?", event_id);
		} else if (main_flag) {
			evnt->OnFinishForegroundEvent();
		}
	}

	if (!main_flag && is_base_frame) {
		// Parallel events will never clear the base stack frame. Instead we just
		// reset the index back to 0 and wait for a frame.
		// This not only optimizes away copying event code, it's also RPG_RT compatible.
		frame.current_command = 0;
	} else {
		// If a called frame, or base frame of foreground interpreter, pop the stack.
		_state.stack.pop_back();
	}

	if (is_base_frame) {
#ifdef ENABLE_DYNAMIC_INTERPRETER_CONFIG
		// Individual runtime flags that may still be set will be cleared by
		// CommandEasyRpgSetInterpreterFlag if neccessary
		_state.easyrpg_runtime_flags.conf_override_active = false;
#endif
	}

	return !is_base_frame;
}

std::vector<std::string> Game_Interpreter::GetChoices(int max_num_choices) {
	const auto& frame = GetFrame();
	const auto& list = frame.commands;
	auto& index = frame.current_command;

	// Let's find the choices
	int current_indent = list[index + 1].indent;
	std::vector<std::string> s_choices;
	for (int index_temp = index + 1; index_temp < static_cast<int>(list.size()); ++index_temp) {
		const auto& com = list[index_temp];
		if (com.indent != current_indent) {
			continue;
		}

		if (static_cast<Cmd>(com.code) == Cmd::ShowChoiceOption && com.parameters.size() > 0 && com.parameters[0] < max_num_choices) {
			// Choice found
			s_choices.push_back(ToString(list[index_temp].string));
		}

		if (static_cast<Cmd>(com.code) == Cmd::ShowChoiceEnd) {
			break;
		}
	}
	return s_choices;
}

bool Game_Interpreter::CommandOptionGeneric(lcf::rpg::EventCommand const& com, int option_sub_idx, std::initializer_list<Cmd> next) {
	const auto sub_idx = GetSubcommandIndex(com.indent);
	if (sub_idx == option_sub_idx) {
		// Executes this option, so clear the subidx to skip all other options.
		SetSubcommandIndex(com.indent, subcommand_sentinel);
	} else {
		SkipToNextConditional(next, com.indent);
	}
	return true;
}

bool Game_Interpreter::CommandShowMessage(lcf::rpg::EventCommand const& com) { // code 10110
	auto& frame = GetFrame();
	const auto& list = frame.commands;
	auto& index = frame.current_command;

	if (!Game_Message::CanShowMessage(main_flag)) {
		return false;
	}

	PendingMessage pm(Game_Message::MakeCommandCodeInserter(*this));
	pm.SetIsEventMessage(true);
	pm.SetFromForegroundInterpreter(main_flag);

	// Set first line
	pm.PushLine(ToString(com.string));

	++index;

	// Check for continued lines via ShowMessage_2
	while (index < static_cast<int>(list.size()) && static_cast<Cmd>(list[index].code) == Cmd::ShowMessage_2) {
		// Add second (another) line
		pm.PushLine(ToString(list[index].string));
		++index;
	}

	// Handle Choices or number
	if (index < static_cast<int>(list.size())) {
		// If next event command is show choices
		if (static_cast<Cmd>(list[index].code) == Cmd::ShowChoice) {
			std::vector<std::string> s_choices = GetChoices(4);
			// If choices fit on screen
			if (static_cast<int>(s_choices.size()) <= (4 - pm.NumLines())) {
				pm.SetChoiceCancelType(list[index].parameters[0]);
				SetupChoices(s_choices, com.indent, pm);
				++index;
			}
		} else if (static_cast<Cmd>(list[index].code) == Cmd::InputNumber) {
			// If next event command is input number
			// If input number fits on screen
			if (pm.NumLines() < 4) {
				int digits = list[index].parameters[0];
				int variable_id = list[index].parameters[1];
				pm.PushNumInput(variable_id, digits);
				++index;
			}
		}
	}

	_async_op = Game_Message::SetPendingMessage(std::move(pm), this);
	_state.show_message = true;

	return true;
}

bool Game_Interpreter::CommandMessageOptions(lcf::rpg::EventCommand const& com) { //code 10120
	if (!Game_Message::CanShowMessage(main_flag)) {
		return false;
	}

	Main_Data::game_system->SetMessageTransparent(com.parameters[0] != 0);
	Main_Data::game_system->SetMessagePosition(com.parameters[1]);
	Main_Data::game_system->SetMessagePositionFixed(com.parameters[2] == 0);
	Main_Data::game_system->SetMessageContinueEvents(com.parameters[3] != 0);

	// TODO: Maniac Patch Message Box Size and Font not implemented
	// see https://jetrotal.github.io/CSA/#Display%20Text%20Options

	return true;
}

bool Game_Interpreter::CommandChangeFaceGraphic(lcf::rpg::EventCommand const& com) { // Code 10130
	if (Player::IsPatchManiac()) {
		const auto parameter = [&](size_t index) -> int32_t {
			return index < com.parameters.size() ? com.parameters[index] : 0;
		};
		auto& system = *Main_Data::game_system;
		const int32_t flags = parameter(5);
		// The native command writes flags even when it then waits for the message.
		system.SetManiacFaceAnimationFlags(flags);
		if ((flags & 1) == 0 && !Game_Message::CanShowMessage(main_flag)) return false;
		const uint32_t modes = static_cast<uint32_t>(parameter(3));
		const auto number = [&](int index, int shift) -> int32_t {
			const int mode = (modes >> (shift * 4)) & 15;
			const int32_t value = parameter(index);
			return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
		};
		const int file_mode = modes & 15;
		const int file_id = parameter(4);
		system.SetMessageFaceName(file_mode == 0 ? ToString(com.string)
			: ToString(ReadString(file_mode == 1 ? file_id : ResolveVariableIndirectAddress(file_id))));
		system.SetMessageFaceIndex(number(0, 1));
		system.SetMessageFaceRightPosition(parameter(1) != 0);
		system.SetMessageFaceFlipped(static_cast<uint8_t>(parameter(2)) != 0);
		if ((flags & 2) != 0) {
			const int end = number(6, 2);
			const int interval = number(7, 3);
			const int interval_range = number(8, 4);
			const int frame_delay = number(9, 5);
			const int frame_delay_range = number(10, 6);
			system.SetManiacFaceAnimation(end, interval, interval_range, frame_delay, frame_delay_range);
		}
		system.ResetManiacFaceAnimation();
		if (auto* window = Game_Message::GetWindow()) window->ReloadManiacFace();
		return true;
	}
	if (!Game_Message::CanShowMessage(main_flag)) {
		return false;
	}

	Main_Data::game_system->SetMessageFaceName(ToString(CommandStringOrVariableBitfield(com, 3, 0, 4)));
	Main_Data::game_system->SetMessageFaceIndex(ValueOrVariableBitfield(com, 3, 1, 0));
	Main_Data::game_system->SetMessageFaceRightPosition(com.parameters[1] != 0);
	Main_Data::game_system->SetMessageFaceFlipped(com.parameters[2] != 0);

	return true;
}

void Game_Interpreter::SetupChoices(const std::vector<std::string>& choices, int indent, PendingMessage& pm) {
	// Set choices to message text
	pm.SetChoiceResetColors(false);
	for (int i = 0; i < 4 && i < static_cast<int>(choices.size()); i++) {
		pm.PushChoice(choices[i]);
	}

	pm.SetChoiceContinuation([this, indent](int choice_result) {
		SetSubcommandIndex(indent, choice_result);
		return AsyncOp();
	});

	// save game compatibility with RPG_RT
	ReserveSubcommandIndex(indent);
}

bool Game_Interpreter::CommandShowChoices(lcf::rpg::EventCommand const& com) { // code 10140
	auto& index = GetFrame().current_command;

	if (!Game_Message::CanShowMessage(main_flag)) {
		return false;
	}

	PendingMessage pm(Game_Message::MakeCommandCodeInserter(*this));
	pm.SetIsEventMessage(true);
	pm.SetFromForegroundInterpreter(main_flag);

	// Choices setup
	std::vector<std::string> choices = GetChoices(4);
	pm.SetChoiceCancelType(com.parameters[0]);
	SetupChoices(choices, com.indent, pm);

	_async_op = Game_Message::SetPendingMessage(std::move(pm), this);
	_state.show_message = true;

	++index;
	return false;
}


bool Game_Interpreter::CommandShowChoiceOption(lcf::rpg::EventCommand const& com) { //code 20140
	const auto opt_sub_idx = com.parameters[0];
	return CommandOptionGeneric(com, opt_sub_idx, {Cmd::ShowChoiceOption, Cmd::ShowChoiceEnd});
}

bool Game_Interpreter::CommandShowChoiceEnd(lcf::rpg::EventCommand const& /* com */) { //code 20141
	return true;
}

bool Game_Interpreter::CommandInputNumber(lcf::rpg::EventCommand const& com) { // code 10150
	if (!Game_Message::CanShowMessage(main_flag)) {
		return false;
	}

	PendingMessage pm(Game_Message::MakeCommandCodeInserter(*this));
	pm.SetIsEventMessage(true);
	pm.SetFromForegroundInterpreter(main_flag);

	int variable_id = com.parameters[1];
	int digits = com.parameters[0];

	pm.PushNumInput(variable_id, digits);

	_async_op = Game_Message::SetPendingMessage(std::move(pm), this);
	_state.show_message = true;

	// Continue
	return true;
}

bool Game_Interpreter::CommandControlSwitches(lcf::rpg::EventCommand const& com) { // code 10210
	if (Player::IsPatchManiac()) {
		const int mode = com.parameters[0];
		int first = com.parameters[1];
		int last = first;
		if (mode == 1) {
			last = com.parameters[2];
		} else if (mode != 0) {
			first = ResolveVariableIndirectAddress(first);
			last = first;
		}
		if ((first < 0) != (last < 0)) last = first;
		const int64_t first_slot = first < 0 ? -static_cast<int64_t>(first) : first;
		const int64_t last_slot = last < 0 ? -static_cast<int64_t>(last) : last;
		const int64_t low = std::min(first_slot, last_slot);
		const int64_t high = std::max(first_slot, last_slot);
		const int operation = com.parameters[3];
		if (first < 0) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) return true;
			bank->switches.resize(std::max(bank->switches.size(), static_cast<size_t>(high + 2)));
			for (int64_t slot = low; slot <= high; ++slot) {
				auto& value = bank->switches[static_cast<size_t>(slot)];
				value = operation == 0 ? 1 : operation == 1 ? 0 : value == 0 ? 1 : 0;
			}
		} else {
			if (operation == 0) Main_Data::game_switches->SetRange(static_cast<int>(low), static_cast<int>(high), true);
			else if (operation == 1) Main_Data::game_switches->SetRange(static_cast<int>(low), static_cast<int>(high), false);
			else Main_Data::game_switches->FlipRange(static_cast<int>(low), static_cast<int>(high));
		}
		Game_Map::RefreshAfterManiacChange();
		return true;
	}
	{
		int start, end;
		bool target_eval_result = DecodeTargetEvaluationMode<
			/* validate_patches */ true,
			/* support_range_indirect */ false,
			/* support_expressions */ false,
			/* support_bitmask */ false,
			/* support_scopes */ false
		>(com, start, end);
		if (!target_eval_result) {
			Output::Warning("ControlSwitches: Unsupported target evaluation mode {}", com.parameters[0]);
			return true;
		}

		int val = com.parameters[3];

		if (start == end) {
			if (val < 2) {
				Main_Data::game_switches->Set(start, val == 0);
			} else {
				Main_Data::game_switches->Flip(start);
			}
			Game_Map::SetNeedRefreshForSwitchChange(start);
		} else {
			if (val < 2) {
				Main_Data::game_switches->SetRange(start, end, val == 0);
			} else {
				Main_Data::game_switches->FlipRange(start, end);
			}
			Game_Map::SetNeedRefresh(true);
		}
	}
	return true;
}

bool Game_Interpreter::CommandControlVariables(lcf::rpg::EventCommand const& com) { // code 10220
	int start = 0, end = 0;
	const int target_mode = com.parameters[0];
	const bool resolved_maniac_target = Player::IsPatchManiac();
	if (resolved_maniac_target) {
		// Maniac resolves the destination before evaluating the operand.
		if (target_mode >= 0 && target_mode <= 3) {
			start = target_mode >= 2 ? ResolveVariableIndirectAddress(com.parameters[1]) : com.parameters[1];
			end = (target_mode & 1) != 0
				? (target_mode == 3 ? ResolveVariableIndirectAddress(com.parameters[2]) : com.parameters[2]) : start;
		} else {
			const int32_t index = com.parameters.size() > 1 ? com.parameters[1] : 0;
			if (index >= 0 && static_cast<size_t>(index) < com.parameters.size()) {
				const size_t payload = static_cast<size_t>(index) + 1;
				const int32_t words = com.parameters[index];
				const size_t count = words > 0 ? std::min(static_cast<size_t>(words), com.parameters.size() - payload) : 0;
				const auto bounds = ManiacPatch::ParseExpressionPair(MakeSpan(com.parameters).subspan(payload, count), *this);
				start = bounds.first;
				end = bounds.second;
			}
			if (target_mode != 4) {
				start = start < 0 ? start : -start;
				end = end < 0 ? end : -end;
			}
		}
		if ((start < 0) != (end < 0)) end = start;
	}
	int value = 0;
	int operand = com.parameters[4];
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const auto numeric = [&](int32_t raw, int mode) -> int32_t {
		return mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
	};
	const auto evaluate_lerp = [&]() -> int32_t {
		const uint32_t modes = static_cast<uint32_t>(parameter(5));
		const double a = numeric(parameter(6), modes & 15);
		const double b = numeric(parameter(7), (modes >> 4) & 15);
		const double c = numeric(parameter(8), (modes >> 8) & 15);
		const double raw_d = numeric(parameter(9), (modes >> 12) & 15);
		const double result = a + (b - a) * (c / (raw_d == 0 ? 1 : raw_d));
		if (!std::isfinite(result) || result < static_cast<double>(INT32_MIN) || result >= 2147483648.0) {
			return INT32_MIN; // CVTTSD2SI with the usual masked invalid-operation exception.
		}
		return static_cast<int32_t>(std::trunc(result));
	};
	const auto evaluate_array = [&]() -> int32_t {
		const uint32_t modes = static_cast<uint32_t>(parameter(5));
		const int base_mode = modes & 15;
		int32_t base = parameter(6);
		if (operand == 23) {
			base = numeric(base, base_mode);
		} else if (base_mode == 1) {
			base = ReadVariable(base);
			if (parameter(6) < 0) base = static_cast<int32_t>(-std::abs(static_cast<int64_t>(base)));
		} else if (base_mode != 0) {
			base = ResolveVariableIndirectAddress(ResolveVariableIndirectAddress(base));
		}
		const int32_t raw_count = numeric(parameter(7), (modes >> 4) & 15);
		const int64_t count = std::max<int64_t>(raw_count, operand == 23 ? 0 : 1);
		const bool self_array = base < 0;
		const int64_t first_slot = self_array ? -static_cast<int64_t>(base) : base;
		const int64_t last_slot = first_slot + count;
		if (self_array) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) return 0;
			bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(last_slot + 1)));
		} else {
			Main_Data::game_variables->GetManiacWritable(static_cast<int>(last_slot + 1));
		}
		uint32_t sum = 0;
		int32_t extremum = operand == 24 ? INT32_MAX : INT32_MIN;
		const int64_t read_count = operand == 25 ? count - 1 : count;
		for (int64_t index = 0; index < read_count; ++index) {
			const int id = static_cast<int>(self_array ? -(first_slot + index) : first_slot + index);
			const int32_t element = id == 0
				? Main_Data::game_variables->GetManiacRawSlot(0) : ReadVariable(id);
			if (operand == 23) sum += static_cast<uint32_t>(element);
			else if (operand == 24) extremum = std::min(extremum, element);
			else extremum = std::max(extremum, element);
		}
		if (operand != 23) return extremum;
		int32_t result;
		std::memcpy(&result, &sum, sizeof(result));
		return result;
	};

	if (EP_UNLIKELY(operand >= 9 && !Player::IsPatchManiac())) {
		Output::Warning("ControlVariables: Unsupported operand {}", operand);
		return true;
	}

	const auto evaluate_operand = [&](int32_t& value) -> bool {
	switch (operand) {
		case 0:
			// Constant
			value = com.parameters[5];
			break;
		case 1:
			// Var A ops B
			value = ReadVariable(com.parameters[5]);
			break;
		case 2:
			// Number of var A ops B
			value = ReadVariableIndirect(com.parameters[5]);
			break;
		case 3: {
			// Random between range
			int32_t arg1 = com.parameters[5];
			int32_t arg2 = com.parameters[6];
			if (Player::IsPatchManiac() && com.parameters.size() >= 8) {
				arg1 = ValueOrVariableBitfield(com.parameters[7], 0, arg1);
				arg2 = ValueOrVariableBitfield(com.parameters[7], 1, arg2);
			}

			value = ControlVariables::Random(arg1, arg2);
			break;
		}
		case 4: {
			// Items
			int item = com.parameters[5];
			if (Player::IsPatchManiac() && com.parameters.size() >= 8) {
				item = ValueOrVariable(com.parameters[7], item);
			}

			value = ControlVariables::Item(com.parameters[6], item);
			break;
		}
		case 5: { // Hero
			int actor_id = com.parameters[5];
			if (Player::IsPatchManiac() && com.parameters.size() >= 8) {
				actor_id = ValueOrVariable(com.parameters[7], actor_id);
			}
			value = ControlVariables::Actor(com.parameters[6], actor_id);
			break;
		}
		case 6: {
			// Characters
			int event_id = com.parameters[5];
			if (Player::IsPatchManiac() && com.parameters.size() >= 8) {
				event_id = ValueOrVariable(com.parameters[7], event_id);
			}
			value = ControlVariables::Event(com.parameters[6], event_id, *this);
			break;
		}
		case 7:
			// More
			value = ControlVariables::Other(com.parameters[5]);
			break;
		case 8: {
			int enemy_id = com.parameters[5];
			if (Player::IsPatchManiac() && com.parameters.size() >= 8) {
				enemy_id = ValueOrVariable(com.parameters[7], enemy_id);
			}

			// Battle related
			value = ControlVariables::Enemy(com.parameters[6], enemy_id);
			break;
		}
		case 9: { // Party Member (Maniac)
			int party_idx = com.parameters[5];
			if (Player::IsPatchManiac() && com.parameters.size() >= 8) {
				party_idx = ValueOrVariable(com.parameters[7], party_idx);
			}
			value = ControlVariables::Party(com.parameters[6], party_idx);
			break;
		}
		case 10: {
			// Switch (Maniac)
			value = com.parameters[5];
			value = com.parameters[6] == 0 ? ReadSwitch(value)
				: ReadSwitch(ResolveVariableIndirectAddress(value));
			break;
		}
		case 11: {
			// Pow (Maniac)
			int arg1 = ValueOrVariableBitfield(com.parameters[7], 0, com.parameters[5]);
			int arg2 = ValueOrVariableBitfield(com.parameters[7], 1, com.parameters[6]);
			value = ControlVariables::Pow(arg1, arg2);
			break;
		}
		case 12: {
			// Sqrt (Maniac)
			int arg = ValueOrVariableBitfield(com.parameters[7], 0, com.parameters[5]);
			int mul = ValueOrVariableBitfield(com.parameters[7], 1, com.parameters[6]);
			value = ControlVariables::Sqrt(arg, mul);
			break;
		}
		case 13: {
			// Sin (Maniac)
			int arg1 = ValueOrVariableBitfield(com.parameters[7], 0, com.parameters[5]);
			int arg2 = ValueOrVariableBitfield(com.parameters[7], 1, com.parameters[8]);
			int mul = com.parameters[6];
			value = ControlVariables::Sin(arg1, arg2, mul);
			break;
		}
		case 14: {
			// Cos (Maniac)
			int arg1 = ValueOrVariableBitfield(com.parameters[7], 0, com.parameters[5]);
			int arg2 = ValueOrVariableBitfield(com.parameters[7], 1, com.parameters[8]);
			int mul = com.parameters[6];
			value = ControlVariables::Cos(arg1, arg2, mul);
			break;
		}
		case 15: {
			// Atan2 (Maniac)
			int arg1 = ValueOrVariableBitfield(com.parameters[8], 0, com.parameters[5]);
			int arg2 = ValueOrVariableBitfield(com.parameters[8], 1, com.parameters[6]);
			int mul = com.parameters[7];
			value = ControlVariables::Atan2(arg1, arg2, mul);
			break;
		}
		case 16: {
			// Min (Maniac)
			int arg1 = ValueOrVariableBitfield(com.parameters[7], 0, com.parameters[5]);
			int arg2 = ValueOrVariableBitfield(com.parameters[7], 1, com.parameters[6]);
			value = ControlVariables::Min(arg1, arg2);
			break;
		}
		case 17: {
			// Max (Maniac)
			int arg1 = ValueOrVariableBitfield(com.parameters[7], 0, com.parameters[5]);
			int arg2 = ValueOrVariableBitfield(com.parameters[7], 1, com.parameters[6]);
			value = ControlVariables::Max(arg1, arg2);
			break;
		}
		case 18: {
			// Abs (Maniac)
			int arg = ValueOrVariableBitfield(com.parameters[6], 0, com.parameters[5]);
			value = ControlVariables::Abs(arg);
			break;
		}
		case 19: {
			// Binary (Maniac)
			int arg1 = ValueOrVariableBitfield(com.parameters[8], 0, com.parameters[6]);
			int arg2 = ValueOrVariableBitfield(com.parameters[8], 1, com.parameters[7]);
			value = ControlVariables::Binary(com.parameters[5], arg1, arg2);
			break;
		}
		case 20: {
			// Ternary (Maniac)
			int mode = com.parameters[10];
			int arg1 = ValueOrVariableBitfield(mode, 0, com.parameters[6]);
			int arg2 = ValueOrVariableBitfield(mode, 1, com.parameters[7]);
			int op = com.parameters[5];
			if (CheckOperator(arg1, arg2, op)) {
				value = ValueOrVariableBitfield(mode, 2, com.parameters[8]);
			} else {
				value = ValueOrVariableBitfield(mode, 3, com.parameters[9]);
			}
			break;
		}
		case 21: {
			// The operand envelope has a separate length header at A5.
			const int32_t words = parameter(5);
			const size_t available = com.parameters.size() > 6 ? com.parameters.size() - 6 : 0;
			const size_t count = words > 0 ? std::min(static_cast<size_t>(words), available) : 0;
			value = ManiacPatch::ParseExpression(MakeSpan(com.parameters).subspan(6, count), *this);
			break;
		}
		case 22:
			value = evaluate_lerp();
			break;
		case 23: case 24: case 25:
			value = evaluate_array();
			break;
		default:
			Output::Warning("ControlVariables: Unsupported operand {}", operand);
			return false;
	}
	return true;
	};
	if (!resolved_maniac_target && !evaluate_operand(value)) return true;

	bool target_eval_result = true;
	if (!resolved_maniac_target) {
		target_eval_result = DecodeTargetEvaluationMode<
		/* validate_patches */ true,
		/* support_range_indirect */ true,
		/* support_expressions */ true,
		/* support_bitmask */ false,
		/* support_scopes */ false
	>(com, start, end);
	}
	if (!target_eval_result) {
		Output::Warning("ControlVariables: Unsupported target evaluation mode {}", com.parameters[0]);
		return true;
	}
	if (Player::IsPatchManiac() && start >= 0 && end >= 0 && end < start) std::swap(start, end);
	if (Player::IsPatchManiac()) {
		if ((start < 0) != (end < 0)) end = start;
		const bool self_target = start < 0;
		const int64_t first_slot = self_target ? -static_cast<int64_t>(start) : start;
		const int64_t last_slot = self_target ? -static_cast<int64_t>(end) : end;
		const int64_t low = std::min(first_slot, last_slot);
		const int64_t high = std::max(first_slot, last_slot);
		Game_ManiacSelfVariables::Bank* bank = nullptr;
		if (self_target) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) return true;
			// 8c800 prepares through the endpoint plus one extra physical slot.
			bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(high + 2)));
		} else {
			// The global physical slot zero is stored separately from this vector.
			Main_Data::game_variables->EnsureSize(static_cast<size_t>(high + 1));
		}
		const int operation = com.parameters[3];
		const int64_t min_value = Main_Data::game_variables->GetMinValue();
		const int64_t max_value = Main_Data::game_variables->GetMaxValue();
		for (int64_t slot = low; slot <= high; ++slot) {
			int32_t operand_value = 0;
			if (!evaluate_operand(operand_value)) return true;
			int32_t* destination_ptr = self_target ? &bank->numbers[static_cast<size_t>(slot)]
				: Main_Data::game_variables->GetManiacWritable(static_cast<int>(slot));
			if (!destination_ptr) continue;
			auto& destination = *destination_ptr;
			int64_t result = 0;
			switch (operation) {
				case 0: destination = operand_value; continue;
				case 1: result = static_cast<int64_t>(destination) + operand_value; break;
				case 2: result = static_cast<int64_t>(destination) - operand_value; break;
				case 3: result = static_cast<int64_t>(destination) * operand_value; break;
				case 4: result = static_cast<int64_t>(destination) / (operand_value == 0 ? 1 : operand_value); break;
				case 5: result = static_cast<int64_t>(destination) % (operand_value == 0 ? 1 : operand_value); break;
				case 6: case 7: case 8: case 9: case 10: {
					const uint32_t lhs = static_cast<uint32_t>(destination);
					const uint32_t rhs = static_cast<uint32_t>(operand_value);
					uint32_t bits = operation == 6 ? lhs | rhs : operation == 7 ? lhs & rhs
						: operation == 8 ? lhs ^ rhs : operation == 9 ? lhs << (rhs & 31) : lhs >> (rhs & 31);
					std::memcpy(&destination, &bits, sizeof(bits));
					continue;
				}
				default: continue;
			}
			destination = static_cast<int32_t>(std::clamp(result, min_value, max_value));
		}
		Game_Map::RefreshAfterManiacChange();
		return true;
	}

	{
		int operation = com.parameters[3];
		if (EP_UNLIKELY(operation >= 6 && !Player::IsPatchManiac())) {
			Output::Warning("ControlVariables: Unsupported operation {}", operation);
			return true;
		}

		if (start == end) {
			// Single variable case - if this is random value, we already called the RNG earlier.
			switch (operation) {
				case 0:
					Main_Data::game_variables->Set(start, value);
					break;
				case 1:
					Main_Data::game_variables->Add(start, value);
					break;
				case 2:
					Main_Data::game_variables->Sub(start, value);
					break;
				case 3:
					Main_Data::game_variables->Mult(start, value);
					break;
				case 4:
					Main_Data::game_variables->Div(start, value);
					break;
				case 5:
					Main_Data::game_variables->Mod(start, value);
					break;
				case 6:
					Main_Data::game_variables->BitOr(start, value);
					break;
				case 7:
					Main_Data::game_variables->BitAnd(start, value);
					break;
				case 8:
					Main_Data::game_variables->BitXor(start, value);
					break;
				case 9:
					Main_Data::game_variables->BitShiftLeft(start, value);
					break;
				case 10:
					Main_Data::game_variables->BitShiftRight(start, value);
					break;
			}
			Game_Map::SetNeedRefreshForVarChange(start);
		} else if (com.parameters[4] == 1) {
			// Multiple variables - Direct variable lookup
			int var_id = com.parameters[5];
			switch (operation) {
				case 0:
					Main_Data::game_variables->SetRangeVariable(start, end, var_id);
					break;
				case 1:
					Main_Data::game_variables->AddRangeVariable(start, end, var_id);
					break;
				case 2:
					Main_Data::game_variables->SubRangeVariable(start, end, var_id);
					break;
				case 3:
					Main_Data::game_variables->MultRangeVariable(start, end, var_id);
					break;
				case 4:
					Main_Data::game_variables->DivRangeVariable(start, end, var_id);
					break;
				case 5:
					Main_Data::game_variables->ModRangeVariable(start, end, var_id);
					break;
				case 6:
					Main_Data::game_variables->BitOrRangeVariable(start, end, var_id);
					break;
				case 7:
					Main_Data::game_variables->BitAndRangeVariable(start, end, var_id);
					break;
				case 8:
					Main_Data::game_variables->BitXorRangeVariable(start, end, var_id);
					break;
				case 9:
					Main_Data::game_variables->BitShiftLeftRangeVariable(start, end, var_id);
					break;
				case 10:
					Main_Data::game_variables->BitShiftRightRangeVariable(start, end, var_id);
					break;
			}
			Game_Map::SetNeedRefresh(true);
		} else if (com.parameters[4] == 2) {
			// Multiple variables - Indirect variable lookup
			int var_id = com.parameters[5];
			switch (operation) {
				case 0:
					Main_Data::game_variables->SetRangeVariableIndirect(start, end, var_id);
					break;
				case 1:
					Main_Data::game_variables->AddRangeVariableIndirect(start, end, var_id);
					break;
				case 2:
					Main_Data::game_variables->SubRangeVariableIndirect(start, end, var_id);
					break;
				case 3:
					Main_Data::game_variables->MultRangeVariableIndirect(start, end, var_id);
					break;
				case 4:
					Main_Data::game_variables->DivRangeVariableIndirect(start, end, var_id);
					break;
				case 5:
					Main_Data::game_variables->ModRangeVariableIndirect(start, end, var_id);
					break;
				case 6:
					Main_Data::game_variables->BitOrRangeVariableIndirect(start, end, var_id);
					break;
				case 7:
					Main_Data::game_variables->BitAndRangeVariableIndirect(start, end, var_id);
					break;
				case 8:
					Main_Data::game_variables->BitXorRangeVariableIndirect(start, end, var_id);
					break;
				case 9:
					Main_Data::game_variables->BitShiftLeftRangeVariableIndirect(start, end, var_id);
					break;
				case 10:
					Main_Data::game_variables->BitShiftRightRangeVariableIndirect(start, end, var_id);
					break;
			}
			Game_Map::SetNeedRefresh(true);
		} else if (com.parameters[4] == 3) {
			// Multiple variables - random
			int rmax = max(com.parameters[5], com.parameters[6]);
			int rmin = min(com.parameters[5], com.parameters[6]);
			switch (operation) {
				case 0:
					Main_Data::game_variables->SetRangeRandom(start, end, rmin, rmax);
					break;
				case 1:
					Main_Data::game_variables->AddRangeRandom(start, end, rmin, rmax);
					break;
				case 2:
					Main_Data::game_variables->SubRangeRandom(start, end, rmin, rmax);
					break;
				case 3:
					Main_Data::game_variables->MultRangeRandom(start, end, rmin, rmax);
					break;
				case 4:
					Main_Data::game_variables->DivRangeRandom(start, end, rmin, rmax);
					break;
				case 5:
					Main_Data::game_variables->ModRangeRandom(start, end, rmin, rmax);
					break;
				case 6:
					Main_Data::game_variables->BitOrRangeRandom(start, end, rmin, rmax);
					break;
				case 7:
					Main_Data::game_variables->BitAndRangeRandom(start, end, rmin, rmax);
					break;
				case 8:
					Main_Data::game_variables->BitXorRangeRandom(start, end, rmin, rmax);
					break;
				case 9:
					Main_Data::game_variables->BitShiftLeftRangeRandom(start, end, rmin, rmax);
					break;
				case 10:
					Main_Data::game_variables->BitShiftRightRangeRandom(start, end, rmin, rmax);
					break;
			}
			Game_Map::SetNeedRefresh(true);
		} else {
			// Multiple variables - constant
			switch (operation) {
				case 0:
					Main_Data::game_variables->SetRange(start, end, value);
					break;
				case 1:
					Main_Data::game_variables->AddRange(start, end, value);
					break;
				case 2:
					Main_Data::game_variables->SubRange(start, end, value);
					break;
				case 3:
					Main_Data::game_variables->MultRange(start, end, value);
					break;
				case 4:
					Main_Data::game_variables->DivRange(start, end, value);
					break;
				case 5:
					Main_Data::game_variables->ModRange(start, end, value);
					break;
				case 6:
					Main_Data::game_variables->BitOrRange(start, end, value);
					break;
				case 7:
					Main_Data::game_variables->BitAndRange(start, end, value);
					break;
				case 8:
					Main_Data::game_variables->BitXorRange(start, end, value);
					break;
				case 9:
					Main_Data::game_variables->BitShiftLeftRange(start, end, value);
					break;
				case 10:
					Main_Data::game_variables->BitShiftRightRange(start, end, value);
					break;
			}
			Game_Map::SetNeedRefresh(true);
		}
	}

	return true;
}

int Game_Interpreter::OperateValue(int operation, int operand_type, int operand) {
	int value = ValueOrVariable(operand_type, operand);

	// Reverse sign of value if operation is substract
	if (operation == 1) {
		value = -value;
	}

	return value;
}

std::vector<Game_Actor*> Game_Interpreter::GetActors(int mode, int id) {
	std::vector<Game_Actor*> actors;
	Game_Actor* actor;

	switch (mode) {
	case 0:
		// Party
		actors = Main_Data::game_party->GetActors();
		break;
	case 1:
		// Hero
		actor = Main_Data::game_actors->GetActor(id);

		if (!actor) {
			Output::Warning("Invalid actor ID {}", id);
			return actors;
		}

		actors.push_back(actor);
		break;
	case 2:
		// Var hero
		actor = Main_Data::game_actors->GetActor(ReadVariable(id));
		if (!actor) {
			Output::Warning("Invalid actor ID {}", ReadVariable(id));
			return actors;
		}

		actors.push_back(actor);
		break;
	}

	return actors;
}

Game_Character* Game_Interpreter::GetCharacter(int event_id, std::string_view origin) const {
	if (event_id == Game_Character::CharThisEvent) {
		event_id = GetThisEventId();
		// Is a common event
		if (event_id == 0) {
			// With no map parent
			Output::Warning("{}: Can't use ThisEvent in common event: Not called from a map event", origin);
			return nullptr;
		}
	}

	Game_Character* ch = Game_Character::GetCharacter(event_id, event_id);
	if (!ch) {
		Output::Warning("{}: Unknown event with id {}", origin, event_id);
	}
	return ch;
}

bool Game_Interpreter::CommandTimerOperation(lcf::rpg::EventCommand const& com) { // code 10230
	int timer_id = 0;

	if (com.parameters.size() > 5 && Player::IsRPG2k3Commands()) {
		timer_id = com.parameters[5];
	}

	int seconds;
	bool visible, battle;

	switch (com.parameters[0]) {
	case 0:
		seconds = ValueOrVariable(com.parameters[1],
			com.parameters[2]);
		Main_Data::game_party->SetTimer(timer_id, seconds);
		break;
	case 1:
		visible = com.parameters[3] != 0;
		battle = com.parameters[4] != 0;
		Main_Data::game_party->StartTimer(timer_id, visible, battle);
		break;
	case 2:
		Main_Data::game_party->StopTimer(timer_id);
		break;
	default:
		return false;
	}
	if (Player::IsPatchManiac()) Game_Map::RefreshAfterManiacChange();
	return true;
}

bool Game_Interpreter::CommandChangeGold(lcf::rpg::EventCommand const& com) { // Code 10310
	int value;
	value = OperateValue(
		com.parameters[0],
		com.parameters[1],
		com.parameters[2]
	);

	Main_Data::game_party->GainGold(value);

	// Continue
	return true;
}

bool Game_Interpreter::CommandChangeItems(lcf::rpg::EventCommand const& com) { // Code 10320
	int value;
	value = OperateValue(
		com.parameters[0],
		com.parameters[3],
		com.parameters[4]
	);

	// Add item can't be used to remove an item and
	// remove item can't be used to add one
	if (com.parameters[0] == 1) {
		// Substract
		if (value > 0) {
			return true;
		}
	} else {
		// Add
		if (value < 0) {
			return true;
		}
	}

	if (com.parameters[1] == 0) {
		// Item by const number
		Main_Data::game_party->AddItem(com.parameters[2], value);
	} else {
		// Item by variable
		Main_Data::game_party->AddItem(
			Main_Data::game_variables->Get(com.parameters[2]),
			value
		);
	}
	if (Player::IsPatchManiac()) Game_Map::RefreshAfterManiacChange();
	else Game_Map::SetNeedRefresh(true);
	// Continue
	return true;
}

bool Game_Interpreter::CommandChangePartyMember(lcf::rpg::EventCommand const& com) { // Code 10330
	Game_Actor* actor;
	int id = ValueOrVariable(com.parameters[1], com.parameters[2]);

	// Some RM2k games clear or rebuild the party by issuing a remove-member
	// sequence that starts at actor ID 0. RPG_RT ignores actor 0 silently.
	if (id == 0) {
		return true;
	}

	actor = Main_Data::game_actors->GetActor(id);

	if (!actor) {
		Output::Warning("ChangePartyMember: Invalid actor ID {}", id);
		return true;
	}

	if (com.parameters[0] == 0) {
		// Add members
		Main_Data::game_party->AddActor(id);

	} else {
		// Remove members
		Main_Data::game_party->RemoveActor(id);
	}

	CheckGameOver();
	if (Player::IsPatchManiac()) Game_Map::RefreshAfterManiacChange();
	else Game_Map::SetNeedRefresh(true);

	// Continue
	return true;
}

void Game_Interpreter::ForegroundTextPush(PendingMessage pm) {
	auto& fg_interp = GetForegroundInterpreter();
	fg_interp._state.show_message = true;

	_async_op = Game_Message::SetPendingMessage(std::move(pm), this);
}

bool Game_Interpreter::CommandChangeExp(lcf::rpg::EventCommand const& com) { // Code 10410
	bool show_msg = com.parameters[5];

	if (show_msg && !Game_Message::CanShowMessage(true)) {
		return false;
	}
	int value = OperateValue(
		com.parameters[2],
		com.parameters[3],
		com.parameters[4]
	);

	PendingMessage pm(Game_Message::MakeCommandCodeInserter(*this));
	pm.SetEnableFace(false);
	pm.SetFromForegroundInterpreter(main_flag);

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		actor->ChangeExp(actor->GetExp() + value, show_msg ? &pm : nullptr);
	}

	if (CheckGameOver()) {
		return true;
	}

	if (show_msg) {
		ForegroundTextPush(std::move(pm));
	}
	return true;
}

bool Game_Interpreter::CommandChangeLevel(lcf::rpg::EventCommand const& com) { // Code 10420
	bool show_msg = com.parameters[5];

	if (show_msg && !Game_Message::CanShowMessage(true)) {
		return false;
	}

	int value = OperateValue(
		com.parameters[2],
		com.parameters[3],
		com.parameters[4]
	);

	PendingMessage pm(Game_Message::MakeCommandCodeInserter(*this));
	pm.SetEnableFace(false);
	pm.SetFromForegroundInterpreter(main_flag);

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		actor->ChangeLevel(actor->GetLevel() + value, show_msg ? &pm : nullptr);
	}

	if (CheckGameOver()) {
		return true;
	}

	if (show_msg && pm.IsActive()) {
		ForegroundTextPush(std::move(pm));
	}
	return true;
}

bool Game_Interpreter::CommandChangeParameters(lcf::rpg::EventCommand const& com) { // Code 10430
	int value = OperateValue(
		com.parameters[2],
		com.parameters[4],
		com.parameters[5]
	);

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		switch (com.parameters[3]) {
		case 0:
			// Max HP
			actor->SetBaseMaxHp(actor->GetBaseMaxHp() + value);
			break;
		case 1:
			// Max MP
			actor->SetBaseMaxSp(actor->GetBaseMaxSp() + value);
			break;
		case 2:
			// Attack
			actor->SetBaseAtk(actor->GetBaseAtk() + value);
			break;
		case 3:
			// Defense
			actor->SetBaseDef(actor->GetBaseDef() + value);
			break;
		case 4:
			// Spirit
			actor->SetBaseSpi(actor->GetBaseSpi() + value);
			break;
		case 5:
			// Agility
			actor->SetBaseAgi(actor->GetBaseAgi() + value);
			break;
		}
	}

	CheckGameOver();
	return true;
}

bool Game_Interpreter::CommandChangeSkills(lcf::rpg::EventCommand const& com) { // Code 10440
	bool remove = com.parameters[2] != 0;
	int skill_id = ValueOrVariable(com.parameters[3], com.parameters[4]);

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		if (remove)
			actor->UnlearnSkill(skill_id);
		else
			actor->LearnSkill(skill_id, nullptr);
	}

	CheckGameOver();
	return true;
}

bool Game_Interpreter::CommandChangeEquipment(lcf::rpg::EventCommand const& com) { // Code 10450
	int item_id;
	int slot;
	const lcf::rpg::Item* item;

	switch (com.parameters[2]) {
		case 0:
			item_id = ValueOrVariable(com.parameters[3],
									  com.parameters[4]);
			item = lcf::ReaderUtil::GetElement(lcf::Data::items, item_id);
			if (!item) {
				Output::Warning("ChangeEquipment: Invalid item ID {}", item_id);
				return true;
			}

			switch (item->type) {
				case lcf::rpg::Item::Type_weapon:
				case lcf::rpg::Item::Type_shield:
				case lcf::rpg::Item::Type_armor:
				case lcf::rpg::Item::Type_helmet:
				case lcf::rpg::Item::Type_accessory:
					slot = item->type;
					break;
				default:
					return true;
			}
			break;
		case 1:
			item_id = 0;
			slot = com.parameters[3] + 1;
			break;
		default:
			return false;
	}

	if (slot == 6) {
		for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
			actor->RemoveWholeEquipment();
		}
	} else {
		for (const auto &actor : GetActors(com.parameters[0], com.parameters[1])) {
			if (actor->HasTwoWeapons() && slot == lcf::rpg::Item::Type_shield && item_id != 0) {
				continue;
			}

			if (actor->HasTwoWeapons() && slot == lcf::rpg::Item::Type_weapon && item_id != 0) {
				lcf::rpg::Item* new_equipment = lcf::ReaderUtil::GetElement(lcf::Data::items, item_id);
				lcf::rpg::Item* equipment1 = lcf::ReaderUtil::GetElement(lcf::Data::items, actor->GetWeaponId());
				lcf::rpg::Item* equipment2 = lcf::ReaderUtil::GetElement(lcf::Data::items, actor->GetShieldId());

				if (equipment1 && !equipment2 && !equipment1->two_handed && !new_equipment->two_handed) {
					// Assign to 2nd weapon slot when empty
					actor->ChangeEquipment(slot + 1, item_id);
					continue;
				}
			}

			actor->ChangeEquipment(slot, item_id);
		}
	}

	CheckGameOver();
	return true;
}

bool Game_Interpreter::CommandChangeHP(lcf::rpg::EventCommand const& com) { // Code 10460
	bool remove = com.parameters[2] != 0;
	int amount = ValueOrVariable(com.parameters[3],
								 com.parameters[4]);
	bool lethal = com.parameters[5] != 0;

	if (remove)
		amount = -amount;

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		actor->ChangeHp(amount, lethal);

		auto& scene = Scene::instance;
		if (scene) {
			scene->OnEventHpChanged(actor, amount);
		}

		if (actor->IsDead() && actor->GetActorBattleSprite()) {
			actor->GetActorBattleSprite()->DetectStateChange();
		}
	}

	CheckGameOver();
	return true;
}

bool Game_Interpreter::CommandChangeSP(lcf::rpg::EventCommand const& com) { // Code 10470
	bool remove = com.parameters[2] != 0;
	int amount = ValueOrVariable(com.parameters[3], com.parameters[4]);

	if (remove)
		amount = -amount;

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		int sp = actor->GetSp() + amount;
		if (sp < 0)
			sp = 0;
		actor->SetSp(sp);
	}

	CheckGameOver();
	return true;
}

bool Game_Interpreter::CommandChangeCondition(lcf::rpg::EventCommand const& com) { // Code 10480
	bool remove = com.parameters[2] != 0;
	const int mode = Player::IsPatchManiac() && com.parameters.size() > 4 ? com.parameters[4] & 15 : 0;
	const int raw_state = com.parameters[3];
	// State has an indirect actor selector distinct from its state-ID mode.
	const int target_mode = Player::IsPatchManiac() && com.parameters[0] > 2 ? 1 : com.parameters[0];
	const int target = Player::IsPatchManiac() && com.parameters[0] > 2
		? ReadVariableIndirect(com.parameters[1]) : com.parameters[1];

	for (const auto& actor : GetActors(target_mode, target)) {
		const int state_id = mode == 0 ? raw_state : mode == 1 ? ReadVariable(raw_state) : ReadVariableIndirect(raw_state);
		if (remove) {
			// RPG_RT: On the map, will remove battle states even if actor has
			// state inflicted by equipment.
			actor->RemoveState(state_id, !Game_Battle::IsBattleRunning());
		} else {
			// RPG_RT always adds states from event commands, even battle states.
			actor->AddState(state_id, true);
		}
		if (actor->GetActorBattleSprite()) {
			actor->GetActorBattleSprite()->DetectStateChange();
		}
	}

	CheckGameOver();
	return true;
}

bool Game_Interpreter::CommandFullHeal(lcf::rpg::EventCommand const& com) { // Code 10490
	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		actor->FullHeal();
		if (actor->GetActorBattleSprite()) {
			actor->GetActorBattleSprite()->DetectStateChange();
		}
	}

	CheckGameOver();

	return true;
}

bool Game_Interpreter::CommandSimulatedAttack(lcf::rpg::EventCommand const& com) { // code 10500
	int atk = com.parameters[2];
	int def = com.parameters[3];
	int spi = com.parameters[4];
	int var = com.parameters[5];

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		int result = atk;
		result -= (actor->GetDef() * def) / 400;
		result -= (actor->GetSpi() * spi) / 800;
		result = std::max(result, 0);
		result = Algo::VarianceAdjustEffect(result, var);

		result = std::max(0, result);
		actor->ChangeHp(-result, true);

		if (com.parameters[6] != 0) {
			Main_Data::game_variables->Set(com.parameters[7], result);
			Game_Map::SetNeedRefresh(true);
		}
	}

	CheckGameOver();
	return true;
}

bool Game_Interpreter::CommandWait(lcf::rpg::EventCommand const& com) { // code 11410
	auto& index = GetFrame().current_command;

	bool maniac = Player::IsPatchManiac();
	if (maniac || Player::IsRPG2k3ECommands()) {
		// Look only through a short, straight sequence of waits and audio commands.
		// Names depending on variables must be resolved when the command executes.
		const auto& commands = GetFrame().commands;
		for (size_t i = index + 1; i < commands.size() && i <= index + 16; ++i) {
			const auto& next = commands[i];
			const auto code = static_cast<Cmd>(next.code);
			if (code == Cmd::ShowPicture) {
				const auto& p = next.parameters;
				if (p.size() >= 30 && (p[17] & 0xF00) == 0 && p[19] == 0
						&& p[22] > 0 && p[23] > 0 && (p[22] > 1 || p[23] > 1)
						&& p[24] == 2 && p[25] > 0 && !next.string.empty()) {
					Cache::PreloadPicture(ToString(next.string), p[7] > 0);
				}
				break;
			}
			if (code != Cmd::Wait && code != Cmd::PlayBGM && code != Cmd::PlaySound
					&& code != Cmd::Comment && code != Cmd::Comment_2) {
				break;
			}
		}
	}

	// Wait a given time
	if (com.parameters.size() <= 1 || (!maniac && !Player::IsRPG2k3Commands())) {
		SetupWait(com.parameters[0]);
		return true;
	}

	if (!maniac && com.parameters.size() > 1 && com.parameters[1] == 0) {
		SetupWait(com.parameters[0]);
		return true;
	}

	if (maniac && com.parameters.size() > 1 && com.parameters[1] != 1) {
		int wait_type = com.parameters[1];
		int mode = 0;

		if (com.parameters.size() > 2) {
			mode = com.parameters[2];
		}

		int duration = ValueOrVariable(mode, com.parameters[0]);

		if (wait_type == 256) {
			SetupWaitFrames(duration);
		} else {
			SetupWait(duration);
		}

		return true;
	}

	if (Game_Message::IsMessageActive()) {
		return false;
	}

	// Wait until decision key pressed, but skip the first frame so that
	// it ignores keys that were pressed before this command started.
	// FIXME: Is this behavior correct?
	_state.wait_key_enter = true;
	++index;
	return false;
}

bool Game_Interpreter::CommandPlayBGM(lcf::rpg::EventCommand const& com) { // code 11510
	lcf::rpg::Music music;
	music.name = ToString(CommandStringOrVariableBitfield(com, 4, 0, 5));

	music.fadein = ValueOrVariableBitfield(com, 4, 1, 0);
	music.volume = ValueOrVariableBitfield(com, 4, 2, 1);
	music.tempo = ValueOrVariableBitfield(com, 4, 3, 2);
	music.balance = ValueOrVariableBitfield(com, 4, 4, 3);

	Main_Data::game_system->BgmPlay(music);
	return true;
}

bool Game_Interpreter::CommandFadeOutBGM(lcf::rpg::EventCommand const& com) { // code 11520
	const int mode = Player::IsPatchManiac() && com.parameters.size() > 1 ? com.parameters[1] & 15 : 0;
	const int raw = com.parameters[0];
	const int fadeout = mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
	Main_Data::game_system->BgmFade(fadeout);
	return true;
}

bool Game_Interpreter::CommandPlaySound(lcf::rpg::EventCommand const& com) { // code 11550
	lcf::rpg::Sound sound;
	sound.name = ToString(CommandStringOrVariableBitfield(com, 3, 0, 4));

	sound.volume = ValueOrVariableBitfield(com, 3, 1, 0);
	sound.tempo = ValueOrVariableBitfield(com, 3, 2, 1);
	sound.balance = ValueOrVariableBitfield(com, 3, 3, 2);

	Main_Data::game_system->SePlay(sound, true);
	return true;
}

bool Game_Interpreter::CommandEndEventProcessing(lcf::rpg::EventCommand const& com) { // code 12310
	if (Player::IsPatchManiac() && com.parameters.size() >= 2 && Main_Data::game_maniac_self_variables) {
		const auto parse_decimal = [](std::string_view source) -> int32_t {
			const auto units = Utils::DecodeUTF16(source, true);
			const std::wstring wide(units.begin(), units.end());
			const long parsed = std::wcstol(wide.c_str(), nullptr, 10);
			return static_cast<int32_t>(std::clamp<long>(parsed, INT32_MIN, INT32_MAX));
		};
		const auto* frame = GetFramePtr();
		if (frame && frame->easyrpg_maniac_return_address != 0) {
			const int receiver_type = frame->easyrpg_maniac_return_type;
			const bool numeric_receiver = receiver_type == 2 || receiver_type == 3;
			const bool switch_receiver = receiver_type == 8 || receiver_type == 9;
			const bool string_receiver = receiver_type == 5 || receiver_type == 6;
			if (frame->easyrpg_maniac_return_address < 0 && _state.stack.size() >= 2
				&& (numeric_receiver || switch_receiver || string_receiver)) {
				// Native resolves the caller environment before converting the source.
				// A battle page has no map/common self bank and must fail here.
				const auto& caller = _state.stack[_state.stack.size() - 2];
				if (!Main_Data::game_maniac_self_variables->Ensure(caller.easyrpg_self_scope,
					caller.easyrpg_self_map_id, caller.easyrpg_self_event_id)) {
					Output::Error("Failed to create self-variables.");
				}
			}
			if (numeric_receiver || switch_receiver) {
				const int32_t raw_address = frame->easyrpg_maniac_return_address;
				const bool indirect = receiver_type == 3 || receiver_type == 9;
				const int positive_address = raw_address > 0
					? (indirect ? Main_Data::game_variables->Get(raw_address) : raw_address) : 0;
				const int source_type = com.parameters[1];
				const int32_t raw_source = com.parameters.size() >= 3 ? com.parameters[2] : 0;
				int32_t result = 0;
				switch (source_type) {
					case 1: result = raw_source; break;
					case 2: result = ReadVariable(raw_source); break;
					case 3: result = ReadVariableIndirect(raw_source); break;
					case 4: result = parse_decimal(com.string); break;
					case 5: result = parse_decimal(ReadString(raw_source)); break;
					case 6: result = parse_decimal(ReadString(ResolveVariableIndirectAddress(raw_source))); break;
					case 7: result = raw_source != 0; break;
					case 8: result = ReadSwitch(raw_source); break;
					case 9: result = ReadSwitch(ResolveVariableIndirectAddress(raw_source)); break;
					default: break;
				}
				if (raw_address > 0) {
					const int address = positive_address;
					if (address > 0) {
						if (numeric_receiver) {
							if (auto* value = Main_Data::game_variables->GetManiacWritable(address)) *value = result;
						} else Main_Data::game_switches->Set(address, result != 0);
					} else if (address < 0 && address != INT32_MIN) {
						auto* bank = Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
							frame->easyrpg_self_map_id, frame->easyrpg_self_event_id);
						if (!bank) {
							Output::Error("Failed to create self-variables.");
						}
						if (bank) {
							const size_t slot = static_cast<size_t>(-static_cast<int64_t>(address));
							if (numeric_receiver) {
								bank->numbers.resize(std::max(bank->numbers.size(), slot + 1));
								bank->numbers[slot] = result;
							} else {
								bank->switches.resize(std::max(bank->switches.size(), slot + 1));
								bank->switches[slot] = result != 0;
							}
						}
					}
				} else if (_state.stack.size() >= 2) {
					const auto& caller = _state.stack[_state.stack.size() - 2];
					auto* bank = Main_Data::game_maniac_self_variables->Ensure(caller.easyrpg_self_scope,
						caller.easyrpg_self_map_id, caller.easyrpg_self_event_id);
					if (bank) {
						const size_t reference = static_cast<size_t>(-static_cast<int64_t>(raw_address));
						if (reference <= INT32_MAX) {
							const int32_t value = indirect && reference < bank->numbers.size() ? bank->numbers[reference] : 0;
							const size_t slot = indirect ? static_cast<size_t>(std::abs(static_cast<int64_t>(value))) : reference;
							if (slot <= INT32_MAX) {
								if (numeric_receiver) {
									bank->numbers.resize(std::max(bank->numbers.size(), slot + 1));
									bank->numbers[slot] = result;
								} else {
									bank->switches.resize(std::max(bank->switches.size(), slot + 1));
									bank->switches[slot] = result != 0;
								}
							}
						}
					}
				}
			} else if (string_receiver && Main_Data::game_strings) {
				const int32_t raw_address = frame->easyrpg_maniac_return_address;
				const bool indirect = receiver_type == 6;
				const int positive_address = raw_address > 0
					? (indirect ? Main_Data::game_variables->Get(raw_address) : raw_address) : 0;
				const int source_type = com.parameters[1];
				const int32_t raw_source = com.parameters.size() >= 3 ? com.parameters[2] : 0;
				std::string result;
				bool result_present = false;
				switch (source_type) {
					case 1: result = std::to_string(raw_source); break;
					case 2: result = std::to_string(ReadVariable(raw_source)); break;
					case 3: result = std::to_string(ReadVariableIndirect(raw_source)); break;
					case 4: result = ToString(com.string); break;
					case 5:
						result = ToString(ReadString(raw_source));
						result_present = IsManiacStringPresent(raw_source);
						break;
					case 6: {
						const int id = ResolveVariableIndirectAddress(raw_source);
						result = ToString(ReadString(id));
						result_present = IsManiacStringPresent(id);
						break;
					}
					case 7: result = std::to_string(raw_source != 0); break;
					case 8: result = std::to_string(ReadSwitch(raw_source) != 0); break;
					case 9: result = std::to_string(ReadSwitch(ResolveVariableIndirectAddress(raw_source)) != 0); break;
					default: break;
				}
				if (source_type != 5 && source_type != 6) result_present = !result.empty();
				if (raw_address > 0) {
					if (positive_address > 0) {
						Main_Data::game_strings->SetManiacRawSlot(positive_address, result, result_present);
					} else if (positive_address < 0 && positive_address != INT32_MIN) {
						auto* bank = Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
							frame->easyrpg_self_map_id, frame->easyrpg_self_event_id);
						if (!bank) {
							Output::Error("Failed to create self-variables.");
						}
						if (bank) {
							const size_t slot = static_cast<size_t>(-static_cast<int64_t>(positive_address));
							bank->strings.resize(std::max(bank->strings.size(), slot + 1));
							bank->strings[slot] = lcf::DBString(result);
							Game_ManiacSelfVariables::SetStringPresence(bank, slot, result_present);
						}
					}
				} else if (_state.stack.size() >= 2) {
					const auto& caller = _state.stack[_state.stack.size() - 2];
					auto* bank = Main_Data::game_maniac_self_variables->Ensure(caller.easyrpg_self_scope,
						caller.easyrpg_self_map_id, caller.easyrpg_self_event_id);
					if (bank) {
						const size_t reference = static_cast<size_t>(-static_cast<int64_t>(raw_address));
						if (reference <= INT32_MAX) {
							const int32_t value = indirect && reference < bank->numbers.size() ? bank->numbers[reference] : 0;
							const size_t slot = indirect ? static_cast<size_t>(std::abs(static_cast<int64_t>(value))) : reference;
							if (slot > 0 && slot <= INT32_MAX) {
								bank->strings.resize(std::max(bank->strings.size(), slot + 1));
								bank->strings[slot] = lcf::DBString(result);
								Game_ManiacSelfVariables::SetStringPresence(bank, slot, result_present);
							}
						}
					}
				}
			}
			// Native return delivery refreshes map pages before callee cleanup.
			// Unknown receiver kinds still take this branch.
			if (frame->easyrpg_maniac_return_address > 0 || _state.stack.size() >= 2) {
				Game_Map::RefreshAfterManiacChange();
			}
		}
	}
	if (Player::IsPatchManiac() && !com.parameters.empty() && Main_Data::game_maniac_self_variables) {
		const int flags = com.parameters[0] & 7;
		if (flags != 0) {
			const auto* frame = GetFramePtr();
			auto* bank = frame ? Main_Data::game_maniac_self_variables->FindMutable(
				frame->easyrpg_self_scope, frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (bank) {
				if ((flags & 1) != 0) bank->numbers.clear();
				if ((flags & 2) != 0) {
					bank->strings.clear();
					bank->empty_string_slots.clear();
				}
				if ((flags & 4) != 0) bank->switches.clear();
			}
		}
	}
	EndEventProcessing();
	return true;
}

std::optional<bool> Game_Interpreter::HandleDynRpgScript(const lcf::rpg::EventCommand& com) {
	if (Player::IsPatchDynRpg() || Player::HasEasyRpgExtensions()) {
		if (com.string.empty() || com.string[0] != '@') {
			// Not a DynRPG command
			return std::nullopt;
		}

		if (!Player::IsPatchDynRpg() && Player::HasEasyRpgExtensions()) {
			// Only accept commands starting with @easyrpg_
			if (!StartsWith(com.string, "@easyrpg_")) {
				return std::nullopt;
			}
		}

		auto& frame = GetFrame();
		const auto& list = frame.commands;
		auto& index = frame.current_command;

		std::string command = ToString(com.string);

		// Concat everything that is not another command or a new comment block
		for (size_t i = index + 1; i < list.size(); ++i) {
			const auto& cmd = list[i];
			if (cmd.code == static_cast<uint32_t>(Cmd::Comment_2) &&
					!cmd.string.empty() && cmd.string[0] != '@') {
				command += ToString(cmd.string);
			} else {
				break;
			}
		}

		return Main_Data::game_dynrpg->Invoke(command, this);
	}

	return {};
}

std::optional<bool> Game_Interpreter::HandleDestinyScript(const lcf::rpg::EventCommand& com) {
	// DestinyScript
	if (Player::IsPatchDestiny()) {
		if (com.string.empty() || com.string[0] != '$') {
			// Not a DestinyScript
			return {};
		}

		return Main_Data::game_destiny->Main(GetFrame());
	}
	return {};
}

bool Game_Interpreter::CommandComment(const lcf::rpg::EventCommand &com) {
	if (auto handled = HandleDynRpgScript(com); handled.has_value()) {
		return handled.value();
	}
	if (auto handled = HandleDestinyScript(com); handled.has_value()) {
		return handled.value();
	}

	return true;
}

void Game_Interpreter::EndEventProcessing() {
	auto& frame = GetFrame();
	const auto& list = frame.commands;
	auto& index = frame.current_command;

	index = static_cast<int>(list.size());
}

bool Game_Interpreter::CommandGameOver(lcf::rpg::EventCommand const& /* com */) { // code 12420
	auto& index = GetFrame().current_command;

	if (Game_Message::IsMessageActive()) {
		return false;
	}

	Scene::instance->SetRequestedScene(std::make_shared<Scene_Gameover>());
	++index;
	return false;
}

bool Game_Interpreter::CommandChangeHeroName(lcf::rpg::EventCommand const& com) { // code 10610
	int id = ValueOrVariableBitfield(com, 1, 0, 0);
	Game_Actor* actor = Main_Data::game_actors->GetActor(id);

	if (!actor) {
		Output::Warning("ChangeHeroName: Invalid actor ID {}", id);
		return true;
	}

	const auto name = CommandStringOrVariableBitfield(com, 1, 1, 2);
	const int mode = com.parameters.size() > 2 ? (com.parameters[1] >> 4) & 15 : 0;
	const bool present = !name.empty() || (mode != 0 && Player::IsPatchManiac()
		&& IsManiacStringPresent(mode == 1 ? com.parameters[2] : ResolveVariableIndirectAddress(com.parameters[2])));
	actor->SetName(ToString(name), present);
	return true;
}

bool Game_Interpreter::CommandChangeHeroTitle(lcf::rpg::EventCommand const& com) { // code 10620
	int id = ValueOrVariableBitfield(com, 1, 0, 0);
	Game_Actor* actor = Main_Data::game_actors->GetActor(id);

	if (!actor) {
		Output::Warning("ChangeHeroTitle: Invalid actor ID {}", id);
		return true;
	}

	const auto title = CommandStringOrVariableBitfield(com, 1, 1, 2);
	const int mode = com.parameters.size() > 2 ? (com.parameters[1] >> 4) & 15 : 0;
	const bool present = !title.empty() || (mode != 0 && Player::IsPatchManiac()
		&& IsManiacStringPresent(mode == 1 ? com.parameters[2] : ResolveVariableIndirectAddress(com.parameters[2])));
	actor->SetTitle(ToString(title), present);
	return true;
}

bool Game_Interpreter::CommandChangeSpriteAssociation(lcf::rpg::EventCommand const& com) { // code 10630
	int id = ValueOrVariableBitfield(com, 3, 0, 0);
	Game_Actor* actor = Main_Data::game_actors->GetActor(id);

	if (!actor) {
		Output::Warning("ChangeSpriteAssociation: Invalid actor ID {}", id);
		return true;
	}

	auto file = ToString(CommandStringOrVariableBitfield(com, 3, 1, 4));
	int idx = ValueOrVariableBitfield(com, 3, 2, 1);
	bool transparent = com.parameters[2] != 0;

	actor->SetSprite(file, idx, transparent);
	Main_Data::game_player->ResetGraphic();
	return true;
}

bool Game_Interpreter::CommandChangeActorFace(lcf::rpg::EventCommand const& com) { // code 10640
	int id = ValueOrVariableBitfield(com, 2, 0, 0);
	Game_Actor* actor = Main_Data::game_actors->GetActor(id);
	if (!actor) {
		Output::Warning("CommandChangeActorFace: Invalid actor ID {}", id);
		return true;
	}

	actor->SetFace(
		ToString(CommandStringOrVariableBitfield(com, 2, 1, 3)),
		ValueOrVariableBitfield(com, 2, 2, 1));
	return true;
}

bool Game_Interpreter::CommandChangeVehicleGraphic(lcf::rpg::EventCommand const& com) { // code 10650
	Game_Vehicle::Type vehicle_id = (Game_Vehicle::Type) (com.parameters[0] + 1);
	Game_Vehicle* vehicle = Game_Map::GetVehicle(vehicle_id);

	if (!vehicle) {
		Output::Warning("ChangeVehicleGraphic: Invalid vehicle ID {}", static_cast<int>(vehicle_id));
		return true;
	}

	const std::string& name = ToString(com.string);
	int vehicle_index = com.parameters[1];

	vehicle->SetSpriteGraphic(name, vehicle_index);
	vehicle->SetOrigSpriteGraphic(name, vehicle_index);

	return true;
}

bool Game_Interpreter::CommandChangeSystemBGM(lcf::rpg::EventCommand const& com) { //code 10660
	lcf::rpg::Music music;
	int context = com.parameters[0];
	music.name = ToString(com.string);
	music.fadein = com.parameters[1];
	music.volume = com.parameters[2];
	music.tempo = com.parameters[3];
	music.balance = com.parameters[4];
	Main_Data::game_system->SetSystemBGM(context, std::move(music));
	return true;
}

bool Game_Interpreter::CommandChangeSystemSFX(lcf::rpg::EventCommand const& com) { //code 10670
	lcf::rpg::Sound sound;
	int context = com.parameters[0];
	sound.name = ToString(com.string);
	sound.volume = com.parameters[1];
	sound.tempo = com.parameters[2];
	sound.balance = com.parameters[3];
	Main_Data::game_system->SetSystemSE(context, std::move(sound));
	return true;
}

bool Game_Interpreter::CommandChangeSystemGraphics(lcf::rpg::EventCommand const& com) { // code 10680
	Main_Data::game_system->SetSystemGraphic(ToString(CommandStringOrVariable(com, 2, 3)),
			static_cast<lcf::rpg::System::Stretch>(com.parameters[0]),
			static_cast<lcf::rpg::System::Font>(com.parameters[1]));

	return true;
}

bool Game_Interpreter::CommandChangeScreenTransitions(lcf::rpg::EventCommand const& com) { // code 10690
	Main_Data::game_system->SetTransition(com.parameters[0], com.parameters[1]);
	return true;
}

bool Game_Interpreter::CommandMemorizeLocation(lcf::rpg::EventCommand const& com) { // code 10820
	Game_Character *player = Main_Data::game_player.get();
	int var_map_id = com.parameters[0];
	int var_x = com.parameters[1];
	int var_y = com.parameters[2];
	Main_Data::game_variables->Set(var_map_id, Game_Map::GetMapId());
	Main_Data::game_variables->Set(var_x, player->GetX());
	Main_Data::game_variables->Set(var_y, player->GetY());
	Game_Map::SetNeedRefreshForVarChange({var_map_id, var_x, var_y});
	return true;
}

bool Game_Interpreter::CommandSetVehicleLocation(lcf::rpg::EventCommand const& com) { // code 10850
	Game_Vehicle::Type vehicle_id = (Game_Vehicle::Type) (com.parameters[0] + 1);
	Game_Vehicle* vehicle = Game_Map::GetVehicle(vehicle_id);

	if (!vehicle) {
		// SetVehicleLocation moves the party, too, when she is in the referenced
		// vehicle. In RPG_RT a party that is in no vehicle has the vehicle_id -1.
		// Due to this implementation detail passing -1 as vehicle_id will move the
		// party instead.
		if (vehicle_id == 0) {
			// 0 because we adjust all vehicle IDs by +1 to match the lcf values
			Output::Debug("SetVehicleLocation: Party referenced");
		} else {
			Output::Warning("SetVehicleLocation: Invalid vehicle ID {}", static_cast<int>(vehicle_id));
			return true;
		}
	}

	int map_id = ValueOrVariable(com.parameters[1], com.parameters[2]);
	int x = ValueOrVariable(com.parameters[1], com.parameters[3]);
	int y = ValueOrVariable(com.parameters[1], com.parameters[4]);

	// Check if the party is in the current vehicle and transfer the party together with it
	if (Main_Data::game_player->GetVehicle() == vehicle) {
		if (map_id == Game_Map::GetMapId()) {
			if (vehicle) {
				vehicle->MoveTo(map_id, x, y);
			}
			Main_Data::game_player->MoveTo(map_id, x, y);
			if (vehicle_id == 0) {
				// This fixes a bug in Yume2kki on map 3D Underworld (ID 1884)
				// The map uses a MoveRoute with a jump and SetVehicleLocation for party movement in a tight loop which
				// causes heavy flickering in our Player.
				// TODO: This fix does not appear to be completely correct as RPG_RT does not reset the jump flag here
				// but the "damage" is reduced because SetVehicleLocation -1 cannot happen without patching the game.
				Main_Data::game_player->SetJumping(false);
			}
			return true;
		};

		// This implements a bug in RPG_RT which allows moving the party to a new map while boarded (or when using -1)
		// without doing a teleport + transition.
		// In player we implement this as an async "Quick Teleport" which immediately switches to
		// the other map with no transition and no change in screen effects such as pictures and
		// battle animations.

		if (vehicle) {
			vehicle->MoveTo(map_id, x, y);
		}

		auto event_id = GetOriginalEventId();
		if (!main_flag && event_id != 0) {
			Output::Error("VehicleTeleport not allowed from parallel map event! Id={}", event_id);
		}

		_async_op = AsyncOp::MakeQuickTeleport(map_id, x, y);
	} else if (vehicle) {
		vehicle->MoveTo(map_id, x, y);
	}

	return true;
}

bool Game_Interpreter::CommandChangeEventLocation(lcf::rpg::EventCommand const& com) { // Code 10860
	const int raw_mode = com.parameters[1];
	// Maniac packs the coordinate evaluation mode in the low byte and the
	// event-id evaluation mode in the low nibble of the high byte. The
	// receiver must be resolved before looking it up as a map event.
	const int event_mode = Player::IsPatchManiac()
		? (static_cast<uint32_t>(raw_mode) >> 8) & 0xF : 0;
	const int coordinate_mode = Player::IsPatchManiac()
		? static_cast<uint32_t>(raw_mode) & 0xFF : (raw_mode == 0x1000 ? 0 : raw_mode);
	const int event_id = Player::IsPatchManiac()
		? ValueOrVariable(event_mode, com.parameters[0]) : com.parameters[0];
	Game_Character *event = GetCharacter(event_id, "ChangeEventLocation");
	if (event != nullptr) {
		const auto x = ValueOrVariable(coordinate_mode, com.parameters[2]);
		const auto y = ValueOrVariable(coordinate_mode, com.parameters[3]);
		event->MoveTo(event->GetMapId(), x, y);

		// RPG2k3 feature
		int direction = -1;
		if (Player::IsRPG2k3Commands() && com.parameters.size() > 4) {
			direction = com.parameters[4] - 1;
		}

		// Only for the constant case, not for variables
		if (coordinate_mode == 0 && direction != -1) {
			event->SetDirection(direction);
			event->UpdateFacing();
		}
	}
	return true;
}

bool Game_Interpreter::CommandTradeEventLocations(lcf::rpg::EventCommand const& com) { // Code 10870
	int event1_id = com.parameters[0];
	int event2_id = com.parameters[1];

	Game_Character *event1 = GetCharacter(event1_id, "TradeEventLocations");
	Game_Character *event2 = GetCharacter(event2_id, "TradeEventLocations");

	if (event1 != nullptr && event2 != nullptr) {
		auto m1 = event1->GetMapId();
		auto x1 = event1->GetX();
		auto y1 = event1->GetY();

		auto m2 = event2->GetMapId();
		auto x2 = event2->GetX();
		auto y2 = event2->GetY();

		event1->MoveTo(m2, x2, y2);
		event2->MoveTo(m1, x1, y1);
	}

	return true;
}

bool Game_Interpreter::CommandStoreTerrainID(lcf::rpg::EventCommand const& com) { // code 10820
	int x = ValueOrVariable(com.parameters[0], com.parameters[1]);
	int y = ValueOrVariable(com.parameters[0], com.parameters[2]);
	int var_id = com.parameters[3];
	Main_Data::game_variables->Set(var_id, Game_Map::GetTerrainTag(x, y));
	Game_Map::SetNeedRefreshForVarChange(var_id);
	return true;
}

bool Game_Interpreter::CommandStoreEventID(lcf::rpg::EventCommand const& com) { // code 10920
	int x = ValueOrVariable(com.parameters[0], com.parameters[1]);
	int y = ValueOrVariable(com.parameters[0], com.parameters[2]);
	int var_id = com.parameters[3];
	auto* ev = Game_Map::GetEventAt(x, y, false);
	Main_Data::game_variables->Set(var_id, ev ? ev->GetId() : 0);
	Game_Map::SetNeedRefreshForVarChange(var_id);
	return true;
}

bool Game_Interpreter::CommandEraseScreen(lcf::rpg::EventCommand const& com) { // code 11010
	if (Game_Message::IsMessageActive()) {
		return false;
	}

	// Emulates RPG_RT behavior where any transition out is skipped when these scenes are pending.
	auto st = Scene::instance->GetRequestedSceneType();
	if (st == Scene::Battle || st == Scene::Gameover) {
		return true;
	}

	// Transition commands in battle have glitchy behavior in RPG_RT, but they don't affect the map.
	// We disable in them in Player.
	if (Game_Battle::IsBattleRunning()) {
		return true;
	}

	int tt = Transition::TransitionNone;

	switch (com.parameters[0]) {
	case -1:
		tt = Main_Data::game_system->GetTransition(Main_Data::game_system->Transition_TeleportErase);
		break;
	case 0:
		tt = Transition::TransitionFadeOut;
		break;
	case 1:
		tt = Transition::TransitionRandomBlocks;
		break;
	case 2:
		tt = Transition::TransitionRandomBlocksDown;
		break;
	case 3:
		tt = Transition::TransitionRandomBlocksUp;
		break;
	case 4:
		tt = Transition::TransitionBlindClose;
		break;
	case 5:
		tt = Transition::TransitionVerticalStripesOut;
		break;
	case 6:
		tt = Transition::TransitionHorizontalStripesOut;
		break;
	case 7:
		tt = Transition::TransitionBorderToCenterOut;
		break;
	case 8:
		tt = Transition::TransitionCenterToBorderOut;
		break;
	case 9:
		tt = Transition::TransitionScrollUpOut;
		break;
	case 10:
		tt = Transition::TransitionScrollDownOut;
		break;
	case 11:
		tt = Transition::TransitionScrollLeftOut;
		break;
	case 12:
		tt = Transition::TransitionScrollRightOut;
		break;
	case 13:
		tt = Transition::TransitionVerticalDivision;
		break;
	case 14:
		tt = Transition::TransitionHorizontalDivision;
		break;
	case 15:
		tt = Transition::TransitionCrossDivision;
		break;
	case 16:
		tt = Transition::TransitionZoomIn;
		break;
	case 17:
		tt = Transition::TransitionMosaicOut;
		break;
	case 18:
		tt = Transition::TransitionWaveOut;
		break;
	case 19:
		tt = Transition::TransitionCutOut;
		break;
	default:
		tt = Transition::TransitionNone;
		break;
	}

	_async_op = AsyncOp::MakeEraseScreen(tt);

	return true;
}

bool Game_Interpreter::CommandShowScreen(lcf::rpg::EventCommand const& com) { // code 11020
	if (Game_Message::IsMessageActive()) {
		return false;
	}

	// Transition commands in battle have glitchy behavior in RPG_RT, but they don't affect the map.
	// We disable in them in Player.
	if (Game_Battle::IsBattleRunning()) {
		return true;
	}

	int tt = Transition::TransitionNone;

	switch (com.parameters[0]) {
	case -1:
		tt = Main_Data::game_system->GetTransition(Main_Data::game_system->Transition_TeleportShow);
		break;
	case 0:
		tt = Transition::TransitionFadeIn;
		break;
	case 1:
		tt = Transition::TransitionRandomBlocks;
		break;
	case 2:
		tt = Transition::TransitionRandomBlocksDown;
		break;
	case 3:
		tt = Transition::TransitionRandomBlocksUp;
		break;
	case 4:
		tt = Transition::TransitionBlindOpen;
		break;
	case 5:
		tt = Transition::TransitionVerticalStripesIn;
		break;
	case 6:
		tt = Transition::TransitionHorizontalStripesIn;
		break;
	case 7:
		tt = Transition::TransitionBorderToCenterIn;
		break;
	case 8:
		tt = Transition::TransitionCenterToBorderIn;
		break;
	case 9:
		tt = Transition::TransitionScrollUpIn;
		break;
	case 10:
		tt = Transition::TransitionScrollDownIn;
		break;
	case 11:
		tt = Transition::TransitionScrollLeftIn;
		break;
	case 12:
		tt = Transition::TransitionScrollRightIn;
		break;
	case 13:
		tt = Transition::TransitionVerticalCombine;
		break;
	case 14:
		tt = Transition::TransitionHorizontalCombine;
		break;
	case 15:
		tt = Transition::TransitionCrossCombine;
		break;
	case 16:
		tt = Transition::TransitionZoomOut;
		break;
	case 17:
		tt = Transition::TransitionMosaicIn;
		break;
	case 18:
		tt = Transition::TransitionWaveIn;
		break;
	case 19:
		tt = Transition::TransitionCutIn;
		break;
	default:
		tt = Transition::TransitionNone;
		break;
	}

	_async_op = AsyncOp::MakeShowScreen(tt);
	return true;
}

bool Game_Interpreter::CommandTintScreen(lcf::rpg::EventCommand const& com) { // code 11030
	Game_Screen* screen = Main_Data::game_screen.get();
	int r = com.parameters[0];
	int g = com.parameters[1];
	int b = com.parameters[2];
	int s = com.parameters[3];
	int tenths = com.parameters[4];
	bool wait = com.parameters[5] != 0;

	screen->TintScreen(r, g, b, s, tenths * DEFAULT_FPS / 10);

	if (wait)
		SetupWait(tenths);

	return true;
}

bool Game_Interpreter::CommandFlashScreen(lcf::rpg::EventCommand const& com) { // code 11040
	Game_Screen* screen = Main_Data::game_screen.get();
	int r = com.parameters[0];
	int g = com.parameters[1];
	int b = com.parameters[2];
	int s = com.parameters[3];
	int tenths = com.parameters[4];
	bool wait = com.parameters[5] != 0;

	if (com.parameters.size() <= 6 || !Player::IsRPG2k3Commands()) {
		screen->FlashOnce(r, g, b, s, tenths * DEFAULT_FPS / 10);
		if (wait)
			SetupWait(tenths);
	} else {
		switch (com.parameters[6]) {
		case 0:
			screen->FlashOnce(r, g, b, s, tenths * DEFAULT_FPS / 10);
			if (wait)
				SetupWait(tenths);
			break;
		case 1:
			screen->FlashBegin(r, g, b, s, tenths * DEFAULT_FPS / 10);
			break;
		case 2:
			screen->FlashEnd();
			break;
		}
	}

	return true;
}

bool Game_Interpreter::CommandShakeScreen(lcf::rpg::EventCommand const& com) { // code 11050
	Game_Screen* screen = Main_Data::game_screen.get();
	int strength = com.parameters[0];
	int speed = com.parameters[1];
	int tenths = com.parameters[2];
	bool wait = com.parameters[3] != 0;
	// params array is size 4 in 2k and 2k games ported to 2k3.
	int shake_cmd = 0;
	if (com.parameters.size() > 4 && Player::IsRPG2k3Commands()) {
		shake_cmd = com.parameters[4];
	}

	switch (shake_cmd) {
		case 0:
			if (tenths > 0) {
				screen->ShakeOnce(strength, speed, tenths * DEFAULT_FPS / 10);
				if (wait) {
					SetupWait(tenths);
				}
			} else {
				screen->ShakeEnd();
			}
			break;
		case 1:
			screen->ShakeBegin(strength, speed);
			break;
		case 2:
			screen->ShakeEnd();
			break;
	}

	return true;
}

bool Game_Interpreter::CommandWeatherEffects(lcf::rpg::EventCommand const& com) { // code 11070
	Game_Screen* screen = Main_Data::game_screen.get();
	int type = com.parameters[0];
	int str = com.parameters[1];
	// Few games use a greater strength value to achieve more intense but glitchy weather
	int strength = std::min(str, 2);

	if (!Player::IsRPG2k3Commands() && type > 2) {
		type = 0;
	}

	screen->SetWeatherEffect(type, strength);
	return true;
}


// PicPointerPatch handling.
// See http://cherrytree.at/cms/download/?did=19
namespace PicPointerPatch {

	static void AdjustId(int& pic_id) {
		if (pic_id > 10000) {
			int new_id;
			if (pic_id > 50000) {
				new_id = Main_Data::game_variables->Get(pic_id - 50000);
			} else {
				new_id = Main_Data::game_variables->Get(pic_id - 10000);
			}

			if (new_id > 0) {
				Output::Debug("PicPointer: ID {} replaced with ID {}", pic_id, new_id);
				pic_id = new_id;
			}
		}
	}

	static void AdjustParams(Game_Pictures::Params& params) {
		if (params.magnify_width > 10000) {
			int new_magnify = Main_Data::game_variables->Get(params.magnify_width - 10000);
			Output::Debug("PicPointer: Zoom {} replaced with {}", params.magnify_width, new_magnify);
			params.magnify_width = new_magnify;
			params.magnify_height = new_magnify;
		}

		if (params.top_trans > 10000) {
			int new_top_trans = Main_Data::game_variables->Get(params.top_trans - 10000);
			Output::Debug("PicPointer: Top transparency {} replaced with {}", params.top_trans, new_top_trans);
			params.top_trans = new_top_trans;
		}

		if (params.bottom_trans > 10000) {
			int new_bottom_trans = Main_Data::game_variables->Get(params.bottom_trans - 10000);
			Output::Debug("PicPointer: Bottom transparency {} replaced with {}", params.bottom_trans, new_bottom_trans);
			params.bottom_trans = new_bottom_trans;
		}
	}

	static std::string ReplaceName(const std::string& str, int value, int digits) {
		// Replace last 4 characters with 0-padded pic_num
		std::u32string u_pic_name = Utils::DecodeUTF32(str);

		// Out of bounds test
		if ((int)u_pic_name.length() < digits) {
			return str;
		}

		// No substitution required
		if (digits == 0) {
			return str;
		}

		std::string new_pic_name = Utils::EncodeUTF(u_pic_name.substr(0, u_pic_name.size() - digits));
		std::stringstream ss;
		ss << new_pic_name << std::setfill('0') << std::setw(digits) << value;
		new_pic_name = ss.str();

		if (!Player::IsRPG2k3ECommands()) {
			// Prevent debug messages because this function is used by ShowPicture of RPG2k3E
			Output::Debug("PicPointer: File {} replaced with {}", str, new_pic_name);
		}
		return new_pic_name;
	}

	static void AdjustShowParams(int& pic_id, Game_Pictures::ShowParams& params) {
		// Adjust name
		if (pic_id >= 50000) {
			// Name substitution is pic_id + 1
			int pic_num = Main_Data::game_variables->Get(pic_id - 50000 + 1);

			if (pic_num >= 0) {
				params.name = ReplaceName(params.name, pic_num, 4);
			}
		}

		AdjustId(pic_id);
		AdjustParams(params);
	}

	static void AdjustMoveParams(int& pic_id, Game_Pictures::MoveParams& params) {
		AdjustId(pic_id);
		AdjustParams(params);

		if (params.duration > 10000) {
			int new_duration = Main_Data::game_variables->Get(params.duration - 10000);
			Output::Debug("PicPointer: Move duration {} replaced with {}", params.duration, new_duration);
			params.duration = new_duration;
		}
	}

}

bool Game_Interpreter::CommandShowPicture(lcf::rpg::EventCommand const& com) { // code 11110
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	// Older versions of RPG_RT block pictures when message active.
	if (!Player::IsPatchManiac() && !Player::IsEnglish() && !Player::IsPatchUnlockPics() && Game_Message::IsMessageActive()) {
		return false;
	}

	int pic_id = parameter(0);

	Game_Pictures::ShowParams params = {};
	bool maniac_name_present = true;
	params.name = ToString(com.string);
	// Maniac Patch uses the upper bits for X/Y origin, mask it away
	int pos_mode = ManiacBitmask(parameter(1), 0xFF);
	params.position_x = ValueOrVariable(pos_mode, parameter(2));
	params.position_y = ValueOrVariable(pos_mode, parameter(3));
	params.fixed_to_map = Player::IsPatchManiac() ? (parameter(4) & 255) != 0 : parameter(4) > 0;
	params.position_scaled = Player::IsPatchManiac() && (parameter(4) & 256);
	params.maniac_image_flags = static_cast<uint32_t>(parameter(4)) & 255;
	params.magnify_width = parameter(5);
	params.magnify_height = params.magnify_width;
	params.use_transparent_color = Player::IsPatchManiac() ? (parameter(7) & 255) != 0 : parameter(7) > 0;
	params.maniac_transparency_flags = static_cast<uint32_t>(parameter(7)) & 255;
	params.top_trans = parameter(6);
	params.red = parameter(8);
	params.green = parameter(9);
	params.blue = parameter(10);
	params.saturation = parameter(11);
	params.effect_mode = parameter(12);
	params.effect_power = parameter(13);

	size_t param_size = com.parameters.size();

	if (param_size > 14) {
		// RPG2k3 sets this chunk. Versions < 1.12 let you specify separate top and bottom
		// transparency. >= 1.12 Editor only let you set one transparency field but it affects
		// both chunks here.
		// Maniac Patch uses the upper bits for flags, mask it away
		params.bottom_trans = ManiacBitmask(parameter(14), 0xFF);
	} else if (Player::IsRPG2k3() && !Player::IsRPG2k3E()) {
		// Corner case when 2k maps are used in 2k3 (pre-1.10) and don't contain this chunk
		params.bottom_trans = params.top_trans;
	}

	if (Player::IsPatchManiac() || (param_size > 16 && Player::IsRPG2k3ECommands())) {
		if (!Player::IsPatchManiac() && param_size < 30) {
			// Ensure correct size
			return CmdSetup<&Game_Interpreter::CommandShowPicture, 30>(com);
		}

		// Handling of RPG2k3 1.12 chunks
		if (Player::IsPatchManiac()) {
			pic_id = ValueOrVariable(parameter(17) & 255, pic_id);
			const int name_mode = (static_cast<uint32_t>(parameter(17)) >> 8) & 15;
			params.name = name_mode == 0 ? ToString(com.string)
				: ToString(ReadString(name_mode == 1 ? parameter(30) : ResolveVariableIndirectAddress(parameter(30))));
			maniac_name_present = name_mode == 0 ? !com.string.empty()
				: IsManiacStringPresent(name_mode == 1 ? parameter(30) : ResolveVariableIndirectAddress(parameter(30)));
		} else {
			pic_id = ValueOrVariable(parameter(17), pic_id);
		}
		if (Player::IsPatchManiac() && parameter(19) > 0) {
			// 06e050 is a GLOBAL-only filename helper, measured in UTF16 units.
			const int value = Main_Data::game_variables->Get(parameter(19));
			auto name = Utils::DecodeUTF16(params.name, true);
			const int digits = parameter(18);
			if (digits > 0 || !maniac_name_present) {
				if (digits > 0 && static_cast<size_t>(digits) < name.size()) name.resize(name.size() - digits);
				else name.clear();
				auto number = std::to_string(value);
				const int width = Utils::Clamp(static_cast<int64_t>(digits) - (value < 0), int64_t{1}, int64_t{9});
				if (number.size() < static_cast<size_t>(width)) number.insert(value < 0 ? 1 : 0, width - number.size(), '0');
				name += Utils::DecodeUTF16(number, true);
			} else if (name.size() < 16) {
				// The native <=0 branch copies the filename again. Its >=16-unit
				// unterminated memory scan has no deterministic compatibility value.
				name += name;
			}
			params.name = Utils::EncodeUTF(name, true);
			maniac_name_present = true;
		} else if (!Player::IsPatchManiac() && parameter(19) != 0) {
			int var = 0;
			if (Main_Data::game_variables->IsValid(parameter(19))) {
				var = Main_Data::game_variables->Get(parameter(19));
			}
			params.name = PicPointerPatch::ReplaceName(params.name, var, parameter(18));
		}

		if (Player::IsPatchManiac()) {
			// Color tint using variables
			params.red = ValueOrVariableBitfield(parameter(17), 3, params.red);
			params.green = ValueOrVariableBitfield(parameter(17), 3,  params.green);
			params.blue = ValueOrVariableBitfield(parameter(17), 3, params.blue);
			params.saturation = ValueOrVariableBitfield(parameter(17), 3,  params.saturation);
		}

		params.magnify_width = ValueOrVariableBitfield(parameter(20), 0, params.magnify_width);
		if (Player::IsPatchManiac() && ((static_cast<uint32_t>(parameter(20)) >> 4) & 15)) {
			const int mode = (static_cast<uint32_t>(parameter(20)) >> 4) & 15;
			params.magnify_height = mode == 1 ? parameter(31) : mode == 2 ? ReadVariable(parameter(31)) : ReadVariableIndirect(parameter(31));
		} else {
			params.magnify_height = params.magnify_width;
		}

		params.top_trans = ValueOrVariable(parameter(21), params.top_trans);
		if (Player::IsPatchManiac() && parameter(22) != 0) {
			params.spritesheet_cols = ValueOrVariableBitfield(parameter(17), 4, parameter(22));
			params.spritesheet_rows = ValueOrVariableBitfield(parameter(17), 5, parameter(23));
			const int32_t last = static_cast<int32_t>(static_cast<uint32_t>(params.spritesheet_cols) * static_cast<uint32_t>(params.spritesheet_rows) - 1u);
			if (parameter(24) == 2 || parameter(24) == 3) {
				params.spritesheet_speed = parameter(25);
				params.maniac_spritesheet_finish = parameter(26);
				params.spritesheet_play_once = parameter(26) == 1;
				const auto endpoint = [last](int32_t value) {
					const int32_t frame = static_cast<int32_t>(static_cast<uint32_t>(value) - 1u);
					return frame < 0 ? 0 : std::min(frame, last);
				};
				params.maniac_spritesheet_start = parameter(24) == 3 ? endpoint(ValueOrVariableBitfield(parameter(17), 6, parameter(32))) : 0;
				params.maniac_spritesheet_end = parameter(24) == 3 ? endpoint(ValueOrVariableBitfield(parameter(17), 7, parameter(33))) : last;
				params.spritesheet_frame = params.maniac_spritesheet_start;
			} else {
				const int32_t frame = parameter(24) == 1 ? ReadVariable(parameter(25)) : parameter(25);
				params.spritesheet_frame = static_cast<int32_t>(static_cast<uint32_t>(frame) - 1u);
			}
		} else if (!Player::IsPatchManiac() && parameter(22) > 0) {
			// If spritesheet is enabled
			params.spritesheet_cols = parameter(22);
			params.spritesheet_rows = parameter(23);

			// Animate and index selection are exclusive
			if (parameter(24) == 2) {
				params.spritesheet_speed = parameter(25);
				params.spritesheet_play_once = parameter(26);
			} else {
				// Picture data / LSD data frame number is 0 based, while event parameter counts from 1.
				params.spritesheet_frame = ValueOrVariable(parameter(24), parameter(25)) - 1;
			}
		}

		params.map_layer = parameter(27);
		params.battle_layer = parameter(28);
		params.flags = Player::IsPatchManiac() && param_size < 30 ? 0x61 : parameter(29);

		if (Player::IsPatchManiac()) {
			params.maniac_style = static_cast<uint32_t>(parameter(14));
			int flags = parameter(14) >> 8;
			int blend_mode = flags & 3;
			if (blend_mode == 1) {
				params.blend_mode = (int)Bitmap::BlendMode::Multiply;
			} else if (blend_mode == 2) {
				params.blend_mode = (int)Bitmap::BlendMode::Additive;
			} else if (blend_mode == 3) {
				params.blend_mode = (int)Bitmap::BlendMode::Overlay;
			}
			params.flip_x = (flags & 16) == 16;
			params.flip_y = (flags & 32) == 32;
			params.origin = parameter(1) >> 8;

			if (params.effect_mode == lcf::rpg::SavePicture::Effect_maniac_fixed_angle) {
				params.effect_power = ValueOrVariableBitfield(parameter(16), 0, params.effect_power);
				int divisor = ValueOrVariableBitfield(parameter(16), 1, parameter(15));
				if (divisor == 0) {
					divisor = 1;
				}
				params.fixed_angle = ManiacFixedPictureAngle(params.effect_power, divisor);
			}
		}
	}

	if (Player::IsPatchManiac() && !maniac_name_present) {
		Main_Data::game_pictures->Erase(pic_id);
		return true;
	}
	if (!Player::IsPatchManiac()) PicPointerPatch::AdjustShowParams(pic_id, params);

	if (!Player::IsPatchManiac()) {
		// Sanitize input
		params.magnify_width = std::max(0, std::min(params.magnify_width, 2000));
		params.magnify_height = std::max(0, std::min(params.magnify_height, 2000));
		params.top_trans = std::max(0, std::min(params.top_trans, 100));
		params.bottom_trans = std::max(0, std::min(params.bottom_trans, 100));
	}

	if (pic_id <= 0) {
		Output::Error("ShowPicture: Requested invalid picture id ({})", pic_id);
	}

	// Maniac's configured pool is checked by Game_Pictures; other engines may grow lazily.

	if (Main_Data::game_pictures->Show(pic_id, params)) {
		if (params.origin > 0) {
			auto& pic = Main_Data::game_pictures->GetPicture(pic_id);
			if (pic.IsRequestPending()) {
				pic.MakeRequestImportant();
				_async_op = AsyncOp::MakeYield();
			}
		}
	}

	return true;
}

bool Game_Interpreter::CommandMovePicture(lcf::rpg::EventCommand const& com) { // code 11120
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	// Older versions of RPG_RT block pictures when message active.
	if (!Player::IsPatchManiac() && !Player::IsEnglish() && !Player::IsPatchUnlockPics() && Game_Message::IsMessageActive()) {
		return false;
	}

	int pic_id = parameter(0);

	Game_Pictures::MoveParams params;
	// Maniac Patch uses the upper bits for X/Y origin, mask it away
	int pos_mode = ManiacBitmask(parameter(1), 0xFF);
	params.position_x = ValueOrVariable(pos_mode, parameter(2));
	params.position_y = ValueOrVariable(pos_mode, parameter(3));
	params.magnify_width = parameter(5);
	params.magnify_height = params.magnify_width;
	params.top_trans = parameter(6);
	params.red = parameter(8);
	params.green = parameter(9);
	params.blue = parameter(10);
	params.saturation = parameter(11);
	params.effect_mode = parameter(12);
	params.effect_power = parameter(13);
	params.duration = parameter(14);

	bool wait = false;

	if (Player::IsPatchManiac()) {
		int opts = parameter(15);
		params.maniac_options = opts;
		const uint32_t keep = 0xffu | ((opts & 16) ? 0xf00u : 0) | ((opts & 32) ? 0xf000u : 0);
		params.maniac_style_mask = ~keep;
		params.maniac_style = parameter(16);
		wait = (opts & 1) != 0;
	} else {
		wait = parameter(15) != 0;
	}

	size_t param_size = com.parameters.size();

	if (Player::IsRPG2k() || Player::IsRPG2k3E() || Player::IsPatchManiac()) {
		if (Player::IsPatchManiac() || (param_size > 17 && Player::IsRPG2k3ECommands())) {
			if (!Player::IsPatchManiac() && param_size < 22) {
				// Ensure correct size
				return CmdSetup<&Game_Interpreter::CommandMovePicture, 22>(com);
			}

			// Handling of RPG2k3 1.12 chunks
			// Maniac Patch uses the upper bits for "wait is variable", mask it away
			pic_id = ValueOrVariable(ManiacBitmask(parameter(17), 0xFF), pic_id);
			// Currently unused by RPG Maker
			//int chars_to_replace = parameter(18);
			//int replace_with = parameter(19);

			if (Player::IsPatchManiac()) {
				// Color tint using variables
				params.red = ValueOrVariableBitfield(parameter(17), 3, params.red);
				params.green = ValueOrVariableBitfield(parameter(17), 3,  params.green);
				params.blue = ValueOrVariableBitfield(parameter(17), 3, params.blue);
				params.saturation = ValueOrVariableBitfield(parameter(17), 3,  params.saturation);
			}

			params.magnify_width = ValueOrVariableBitfield(parameter(20), 0, params.magnify_width);

			if (Player::IsPatchManiac() && ((static_cast<uint32_t>(parameter(20)) >> 4) & 15)) {
				const int mode = (static_cast<uint32_t>(parameter(20)) >> 4) & 15;
				params.magnify_height = mode == 1 ? parameter(18) : mode == 2 ? ReadVariable(parameter(18)) : ReadVariableIndirect(parameter(18));
			} else {
				params.magnify_height = params.magnify_width;
			}

			params.top_trans = ValueOrVariable(parameter(21), params.top_trans);
		}

		// RPG2k and RPG2k3 1.10 do not support this option
		params.bottom_trans = params.top_trans;

		if (Player::IsPatchManiac()) {
			int flags = parameter(16) >> 8;
			int blend_mode = flags & 3;
			if (blend_mode == 1) {
				params.blend_mode = (int)Bitmap::BlendMode::Multiply;
			} else if (blend_mode == 2) {
				params.blend_mode = (int)Bitmap::BlendMode::Additive;
			} else if (blend_mode == 3) {
				params.blend_mode = (int)Bitmap::BlendMode::Overlay;
			}

			if (param_size > 17) {
				params.duration = ValueOrVariableBitfield(parameter(17), 2, params.duration);
			}
			params.flip_x = (flags & 16) == 16;
			params.flip_y = (flags & 32) == 32;
			params.origin = parameter(1) >> 8;

			if (params.effect_mode == lcf::rpg::SavePicture::Effect_maniac_fixed_angle) {
				params.effect_power = ValueOrVariableBitfield(parameter(4), 0, params.effect_power);
				int divisor = ValueOrVariableBitfield(parameter(4), 1, parameter(7));
				if (divisor == 0) {
					divisor = 1;
				}
				params.fixed_angle = ManiacFixedPictureAngle(params.effect_power, divisor);
			}
		}
	} else {
		// Corner case when 2k maps are used in 2k3 (pre-1.10) and don't contain this chunk
		params.bottom_trans = param_size > 16 ? parameter(16) : params.top_trans;
	}

	if (!Player::IsPatchManiac()) PicPointerPatch::AdjustMoveParams(pic_id, params);

	if (!Player::IsPatchManiac()) {
		// Sanitize input
		params.magnify_width = std::max(0, std::min(params.magnify_width, 2000));
		params.magnify_height = std::max(0, std::min(params.magnify_height, 2000));
		params.top_trans = std::max(0, std::min(params.top_trans, 100));
		params.bottom_trans = std::max(0, std::min(params.bottom_trans, 100));
		params.duration = std::max(Player::IsPatchManiac() ? -10000 : 0, std::min(params.duration, 10000));
	}

	if (pic_id <= 0) {
		Output::Error("MovePicture: Requested invalid picture id ({})", pic_id);
	}

	Main_Data::game_pictures->Move(pic_id, params);

	if (params.origin > 0) {
		auto& pic = Main_Data::game_pictures->GetPicture(pic_id);
		if (pic.IsRequestPending()) {
			pic.MakeRequestImportant();
			_async_op = AsyncOp::MakeYield();
		}
	}

	if (wait) {
		if (Player::IsPatchManiac()) {
			// The native command waits for the same normalized/retained frame
			// count used by the picture, including negative-duration frame units.
			SetupWaitFrames(Main_Data::game_pictures->GetPicture(pic_id).data.time_left);
		} else {
			SetupWait(params.duration);
		}
	}

	return true;
}

bool Game_Interpreter::CommandErasePicture(lcf::rpg::EventCommand const& com) { // code 11130
	// Older versions of RPG_RT block pictures when message active.
	if (!Player::IsEnglish() && !Player::IsPatchUnlockPics() && Game_Message::IsMessageActive()) {
		return false;
	}

	int pic_id = com.parameters[0];

	if (com.parameters.size() > 1 && Player::IsRPG2k3ECommands()) {
		// Handling of RPG2k3 1.12 chunks
		int id_type = com.parameters[1];

		int pic_id_max = 0;
		switch (id_type) {
			case 0:
				// Erase single picture specified by constant
			case 1:
				// Erase single picture referenced by variable
				pic_id = ValueOrVariable(id_type, pic_id);
				pic_id_max = pic_id;
				break;
			case 2:
				// Erase [Arg0, Arg2]
				if (com.parameters.size() > 2) {
					pic_id_max = com.parameters[2];
				}
				break;
			case 3:
				// Erase [V[Arg0], V[Arg2]]
				if (!Player::IsPatchManiac()) {
					return true;
				}
				pic_id = Main_Data::game_variables->Get(pic_id);
				if (com.parameters.size() > 2) {
					pic_id_max = Main_Data::game_variables->Get(com.parameters[2]);
				}
				break;
			case 4:
				// Erase single picture referenced by variable indirect
				if (!Player::IsPatchManiac()) {
					return true;
				}
				pic_id = Main_Data::game_variables->GetIndirect(pic_id);
				pic_id_max = pic_id;
				break;
			case 5:
				// Erase all pictures
				if (!Player::IsPatchManiac()) {
					return true;
				}
				Main_Data::game_pictures->EraseAll();
				return true;
			default:
				return true;
		}

		for (int i = pic_id; i <= pic_id_max; ++i) {
			if (i <= 0) {
				Output::Error("ErasePicture: Requested invalid picture id ({})", i);
			}

			Main_Data::game_pictures->Erase(i);
		}
	} else {
		PicPointerPatch::AdjustId(pic_id);

		if (pic_id <= 0) {
			Output::Error("ErasePicture: Requested invalid picture id ({})", pic_id);
		}

		Main_Data::game_pictures->Erase(pic_id);
	}

	return true;
}

bool Game_Interpreter::CommandPlayerVisibility(lcf::rpg::EventCommand const& com) { // code 11310
	bool hidden = (com.parameters[0] == 0);
	Game_Character* player = Main_Data::game_player.get();
	player->SetSpriteHidden(hidden);
	// RPG_RT does this here.
	player->ResetThrough();

	return true;
}

bool Game_Interpreter::CommandMoveEvent(lcf::rpg::EventCommand const& com) { // code 11330
	int event_id = ValueOrVariableBitfield(com.parameters[2], 2, com.parameters[0]);
	int repeat = ManiacBitmask(com.parameters[2], 0x1);

	Game_Character* event = GetCharacter(event_id, "MoveEvent");
	if (event != NULL) {
		// If the event is a vehicle in use, push the commands to the player instead
		if (event_id >= Game_Character::CharBoat && event_id <= Game_Character::CharAirship)
			if (static_cast<Game_Vehicle*>(event)->IsInUse())
				event = Main_Data::game_player.get();

		lcf::rpg::MoveRoute route;
		int move_freq = com.parameters[1];

		if (move_freq <= 0 || move_freq > 8) {
			// Invalid values
			move_freq = 6;
		}

		route.repeat = repeat != 0;
		route.skippable = com.parameters[3] != 0;

		for (auto it = com.parameters.begin() + 4; it < com.parameters.end(); ) {
			route.move_commands.push_back(DecodeMove(it));
		}

		event->ForceMoveRoute(route, move_freq);
	}
	return true;
}

bool Game_Interpreter::CommandMemorizeBGM(lcf::rpg::EventCommand const& /* com */) { // code 11530
	Main_Data::game_system->MemorizeBGM();
	return true;
}

bool Game_Interpreter::CommandPlayMemorizedBGM(lcf::rpg::EventCommand const& /* com */) { // code 11540
	Main_Data::game_system->PlayMemorizedBGM();
	return true;
}

int Game_Interpreter::KeyInputState::CheckInput() const {
	auto check = wait ? Input::IsTriggered : Input::IsPressed;

	// Mouse buttons checked first (Maniac checks them last) to prevent conflict
	// when DECISION is mapped to MOUSE_LEFT
	// The order of checking matches the Maniac behaviour
	if (keys[Keys::eMouseScrollDown] && check(Input::SCROLL_DOWN)) {
		return 1001;
	}

	if (keys[Keys::eMouseScrollUp] && check(Input::SCROLL_UP)) {
		return 1004;
	}

	if (keys[Keys::eMouseMiddle] && check(Input::MOUSE_MIDDLE)) {
		return 1007;
	}

	if (keys[Keys::eMouseRight] && check(Input::MOUSE_RIGHT)) {
		return 1006;
	}

	if (keys[Keys::eMouseLeft] && check(Input::MOUSE_LEFT)) {
		return 1005;
	}

	// RPG processes keys from highest variable value to lowest.
	if (keys[Keys::eOperators]) {
		for (int i = 5; i > 0;) {
			--i;
			if (check((Input::InputButton)(Input::PLUS + i))) {
				return 20 + i;
			}
		}
	}
	if (keys[Keys::eNumbers]) {
		for (int i = 10; i > 0;) {
			--i;
			if (check((Input::InputButton)(Input::N0 + i))) {
				return 10 + i;
			}
		}
	}

	if (keys[Keys::eShift] && check(Input::SHIFT)) {
		return 7;
	}
	if (keys[Keys::eCancel] && check(Input::CANCEL)) {
		return 6;
	}
	if (keys[Keys::eDecision] && check(Input::DECISION)) {
		return 5;
	}
	if (keys[Keys::eUp] && check(Input::UP)) {
		return 4;
	}
	if (keys[Keys::eRight] && check(Input::RIGHT)) {
		return 3;
	}
	if (keys[Keys::eLeft] && check(Input::LEFT)) {
		return 2;
	}
	if (keys[Keys::eDown] && check(Input::DOWN)) {
		return 1;
	}

	return 0;
}

bool Game_Interpreter::CommandKeyInputProc(lcf::rpg::EventCommand const& com) { // code 11610
	int var_id = com.parameters[0];
	bool wait = com.parameters[1] != 0;

	if (wait) {
		// While waiting the variable is reset to 0 each frame.
		Main_Data::game_variables->Set(var_id, 0);
		Game_Map::SetNeedRefreshForVarChange(var_id);
	}

	if (wait && Game_Message::IsMessageActive()) {
		return false;
	}

	_keyinput = {};
	_keyinput.wait = wait;
	_keyinput.variable = var_id;

	const size_t param_size = com.parameters.size();

	// Maniac Patch aware check functions for parameters that handle
	// keyboard and mouse through a bitmask
	bool is_maniac = Player::IsPatchManiac();
	auto check_key = [&](auto idx, bool handle_maniac = false) {
		if (param_size <= idx) {
			return false;
		}
		if (handle_maniac && is_maniac) {
			return (com.parameters[idx] & 1) != 0;
		} else {
			return com.parameters[idx] != 0;
		}
	};

	_keyinput.keys[Keys::eDecision] = check_key(3u, true);
	_keyinput.keys[Keys::eCancel] = check_key(4u, true);

	// All engines support older versions of the command depending on the
	// length of the parameter list
	if (Player::IsRPG2k()) {
		if (param_size < 6 || Player::IsRPG2kLegacy()) {
			// For Rpg2k <1.50 (later versions got individual key checks)
			if (com.parameters[2] != 0) {
				_keyinput.keys[Keys::eDown] = true;
				_keyinput.keys[Keys::eLeft] = true;
				_keyinput.keys[Keys::eRight] = true;
				_keyinput.keys[Keys::eUp] = true;
			}
		} else {
			// For Rpg2k >=1.50
			_keyinput.keys[Keys::eShift] = com.parameters[5] != 0;
			_keyinput.keys[Keys::eDown] = check_key(6u);
			_keyinput.keys[Keys::eLeft] = check_key(7u);
			_keyinput.keys[Keys::eRight] = check_key(8u);
			_keyinput.keys[Keys::eUp] = check_key(9u);
		}
	} else {
		if (param_size != 10 || Player::IsRPG2k3Legacy()) {
			if ((param_size < 10 || Player::IsRPG2k3Legacy()) && com.parameters[2] != 0) {
				// For RPG2k3 <1.05 (later versions got individual key checks)
				_keyinput.keys[Keys::eDown] = true;
				_keyinput.keys[Keys::eLeft] = true;
				_keyinput.keys[Keys::eRight] = true;
				_keyinput.keys[Keys::eUp] = true;
			}
			_keyinput.keys[Keys::eNumbers] = check_key(5u);
			_keyinput.keys[Keys::eOperators] = check_key(6u);
			_keyinput.time_variable = param_size > 7 ? com.parameters[7] : 0; // Attention: int, not bool
			_keyinput.timed = check_key(8u);
			if (param_size > 10 && Player::IsMajorUpdatedVersion()) {
				// For Rpg2k3 >=1.05
				// ManiacPatch Middle & Wheel only handled for 2k3 Major Updated,
				// the only version that has this patch
				_keyinput.keys[Keys::eShift] = check_key(9u, true);
				_keyinput.keys[Keys::eDown] = check_key(10u, true);
				_keyinput.keys[Keys::eLeft] = check_key(11u);
				_keyinput.keys[Keys::eRight] = check_key(12u);
				_keyinput.keys[Keys::eUp] = check_key(13u, true);
			}
		} else {
			// Since RPG2k3 1.05
			// Support for RPG2k >=1.50 games imported into RPG2k3
			_keyinput.keys[Keys::eShift] = check_key(5u);
			_keyinput.keys[Keys::eDown] = check_key(6u);
			_keyinput.keys[Keys::eLeft] = check_key(7u);
			_keyinput.keys[Keys::eRight] = check_key(8u);
			_keyinput.keys[Keys::eUp] = check_key(9u);
		}
	}

	if (is_maniac) {
		auto check_mouse = [&](auto idx) {
			if (param_size <= idx) {
				return false;
			}

			bool result = (com.parameters[idx] & 2) != 0;
#if !defined(USE_MOUSE_OR_TOUCH) || !defined(SUPPORT_MOUSE_OR_TOUCH)
			if (result) {
				Output::Warning("ManiacPatch: Mouse input is not supported on this platform");
				result = false;
			}
#endif
			return result;
		};
		_keyinput.keys[Keys::eMouseLeft] = check_mouse(3u);
		_keyinput.keys[Keys::eMouseRight] = check_mouse(4u);
		_keyinput.keys[Keys::eMouseMiddle] = check_mouse(9u);
		_keyinput.keys[Keys::eMouseScrollDown] = check_mouse(10u);
		_keyinput.keys[Keys::eMouseScrollUp] = check_mouse(13u);
	}

	if (_keyinput.wait) {
		// RPG_RT will reset all trigger key states when a waiting key input proc command is executed,
		// which means we always wait at least 1 frame to continue. Keys which are held down are not reset.
		// This also prevents player actions for this frame such as summoning the menu or triggering events.
		Input::ResetTriggerKeys();
		return true;
	}

	int key = _keyinput.CheckInput();
	Main_Data::game_variables->Set(_keyinput.variable, key);
	Game_Map::SetNeedRefreshForVarChange(_keyinput.variable);
	if (Player::IsPatchManiac()) Game_Map::RefreshAfterManiacChange();

	return true;
}

bool Game_Interpreter::CommandChangeMapTileset(lcf::rpg::EventCommand const& com) { // code 11710
	int chipset_id = com.parameters[0];

	if (chipset_id == Game_Map::GetChipset()) {
		return true;
	}

	Game_Map::SetChipset(chipset_id);

	Scene_Map* scene = (Scene_Map*)Scene::Find(Scene::Map).get();

	if (!scene)
		return true;

	scene->spriteset->ChipsetUpdated();

	return true;
}

bool Game_Interpreter::CommandChangePBG(lcf::rpg::EventCommand const& com) { // code 11720
	Game_Map::Parallax::Params params;
	params.name = ToString(com.string);
	params.scroll_horz = com.parameters[0] != 0;
	params.scroll_vert = com.parameters[1] != 0;
	params.scroll_horz_auto = com.parameters[2] != 0;
	params.scroll_horz_speed = com.parameters[3];
	params.scroll_vert_auto = com.parameters[4] != 0;
	params.scroll_vert_speed = com.parameters[5];

	Game_Map::Parallax::ChangeBG(params);

	if (!params.name.empty()) {
		if (!AsyncHandler::RequestFile("Panorama", params.name)->IsReady()) {
			_async_op = AsyncOp::MakeYield();
		}
	}

	return true;
}

bool Game_Interpreter::CommandChangeEncounterSteps(lcf::rpg::EventCommand const& com) { // code 11740
	int steps = com.parameters[0];

	Game_Map::SetEncounterSteps(steps);

	return true;
}

bool Game_Interpreter::CommandTileSubstitution(lcf::rpg::EventCommand const& com) { // code 11750
	bool upper = com.parameters[0] != 0;
	int old_id = com.parameters[1];
	int new_id = com.parameters[2];
	Scene_Map* scene = (Scene_Map*)Scene::Find(Scene::Map).get();
	if (!scene)
		return true;

	if (upper)
		scene->spriteset->SubstituteUp(old_id, new_id);
	else
		scene->spriteset->SubstituteDown(old_id, new_id);

	return true;
}

bool Game_Interpreter::CommandTeleportTargets(lcf::rpg::EventCommand const& com) { // code 11810
	int map_id = com.parameters[1];

	if (com.parameters[0] != 0) {
		Main_Data::game_targets->RemoveTeleportTarget(map_id);
		return true;
	}

	int x = com.parameters[2];
	int y = com.parameters[3];
	bool switch_on = static_cast<bool>(com.parameters[4]);
	int switch_id = com.parameters[5];
	Main_Data::game_targets->AddTeleportTarget(map_id, x, y, switch_on, switch_id);
	return true;
}

bool Game_Interpreter::CommandChangeTeleportAccess(lcf::rpg::EventCommand const& com) { // code 11820
	Main_Data::game_system->SetAllowTeleport(com.parameters[0] != 0);
	return true;
}

bool Game_Interpreter::CommandEscapeTarget(lcf::rpg::EventCommand const& com) { // code 11830
	int map_id = com.parameters[0];
	int x = com.parameters[1];
	int y = com.parameters[2];
	bool switch_on = static_cast<bool>(com.parameters[3]);
	int switch_id = com.parameters[4];
	Main_Data::game_targets->SetEscapeTarget(map_id, x, y, switch_on, switch_id);
	return true;
}

bool Game_Interpreter::CommandChangeEscapeAccess(lcf::rpg::EventCommand const& com) { // code 11840
	Main_Data::game_system->SetAllowEscape(com.parameters[0] != 0);
	return true;
}

bool Game_Interpreter::CommandChangeSaveAccess(lcf::rpg::EventCommand const& com) { // code 11930
	if (Player::IsPatchManiac() && com.parameters[0] == -1 && com.parameters.size() >= 2) {
		Main_Data::game_system->SetManiacDisabledFeatures(com.parameters[1]);
		return true;
	}
	Main_Data::game_system->SetAllowSave(com.parameters[0] != 0);
	return true;
}

bool Game_Interpreter::CommandChangeMainMenuAccess(lcf::rpg::EventCommand const& com) { // code 11960
	Main_Data::game_system->SetAllowMenu(com.parameters[0] != 0);
	return true;
}

bool Game_Interpreter::CommandConditionalBranch(lcf::rpg::EventCommand const& com) { // Code 12010
	const auto& frame = GetFrame();
	const bool maniac = Player::IsPatchManiac();

	bool result = false;
	int value1, value2;
	int actor_id;
	Game_Actor* actor;
	Game_Character* character;

	switch (com.parameters[0]) {
	case 0:
		// Switch
		result = (maniac ? ReadSwitch(com.parameters[1]) != 0 : Main_Data::game_switches->Get(com.parameters[1])) == (com.parameters[2] == 0);
		break;
	case 1:
		// Variable
		value1 = maniac ? ReadVariable(com.parameters[1]) : Main_Data::game_variables->Get(com.parameters[1]);
		value2 = maniac ? (com.parameters[2] == 0 ? com.parameters[3] : com.parameters[2] == 1 ? ReadVariable(com.parameters[3]) : ReadVariableIndirect(com.parameters[3])) : ValueOrVariable(com.parameters[2], com.parameters[3]);
		result = CheckOperator(value1, value2, com.parameters[4]);
		break;
	case 2:
		value1 = Main_Data::game_party->GetTimerSeconds(Main_Data::game_party->Timer1);
		value2 = com.parameters[1];
		switch (com.parameters[2]) {
		case 0:
			result = (value1 >= value2);
			break;
		case 1:
			result = (value1 <= value2);
			break;
		}
		break;
	case 3:
		// Gold
		if (com.parameters[2] == 0) {
			// Greater than or equal
			result = (Main_Data::game_party->GetGold() >= com.parameters[1]);
		} else {
			// Less than or equal
			result = (Main_Data::game_party->GetGold() <= com.parameters[1]);
		}
		break;
	case 4: {
		// Item
		int item_id = com.parameters[1];

		if (Player::IsPatchManiac()) {
			item_id = ValueOrVariable(com.parameters[3], item_id);
		}

		if (com.parameters[2] == 0) {
			// Having
			result = Main_Data::game_party->GetItemCount(item_id)
				+ Main_Data::game_party->GetEquippedItemCount(item_id) > 0;
		} else {
			// Not having
			result = Main_Data::game_party->GetItemCount(item_id)
				+ Main_Data::game_party->GetEquippedItemCount(item_id) == 0;
		}
		break;
	}
	case 5:
		// Hero
		actor_id = com.parameters[1];

		if (Player::IsPatchManiac()) {
			actor_id = ValueOrVariable(com.parameters[4], actor_id);
		}

		actor = Main_Data::game_actors->GetActor(actor_id);

		if (!actor) {
			if (maniac) Output::Error("ConditionalBranch: Actor {} was not found.", actor_id);
			Output::Warning("ConditionalBranch: Invalid actor ID {}", actor_id);
			// Use Else Branch
			SetSubcommandIndex(com.indent, 1);
			SkipToNextConditional({Cmd::ElseBranch, Cmd::EndBranch}, com.indent);
			return true;
		}

		switch (com.parameters[2]) {
		case 0:
			// Is actor in party
			result = Main_Data::game_party->IsActorInParty(actor_id);
			break;
		case 1:
			// Name
			result = (actor->GetName() == com.string);
			break;
		case 2:
			// Higher or equal level
			result = (actor->GetLevel() >= com.parameters[3]);
			break;
		case 3:
			// Higher or equal HP
			result = (actor->GetHp() >= com.parameters[3]);
			break;
		case 4:
			// Is skill learned
			result = (actor->IsSkillLearned(com.parameters[3]));
			break;
		case 5:
			// Equipped object
			result = (
				(actor->GetShieldId() == com.parameters[3]) ||
				(actor->GetArmorId() == com.parameters[3]) ||
				(actor->GetHelmetId() == com.parameters[3]) ||
				(actor->GetAccessoryId() == com.parameters[3]) ||
				(actor->GetWeaponId() == com.parameters[3])
				);
			break;
		case 6:
			// Has state
			result = (actor->HasState(com.parameters[3]));
			break;
		default:
			;
		}
		break;
	case 6: {
		// Orientation of char
		int chara_id = com.parameters[1];
		if (maniac) {
			const int mode = com.parameters[3] & 15;
			chara_id = mode == 0 ? chara_id : mode == 1 ? ReadVariable(chara_id) : ReadVariableIndirect(chara_id);
			if (com.parameters[4] == 1) {
				if (chara_id == Game_Character::CharPlayer) result = true;
				else if (chara_id >= Game_Character::CharBoat && chara_id <= Game_Character::CharAirship) {
					const auto* vehicle = Game_Map::GetVehicle(static_cast<Game_Vehicle::Type>(chara_id - Game_Character::CharBoat + 1));
					result = vehicle && vehicle->GetMapId() == Game_Map::GetMapId();
				} else {
					if (chara_id == Game_Character::CharThisEvent) {
						chara_id = 0;
						for (auto i = _state.stack.rbegin(); i != _state.stack.rend(); ++i) {
							if (i->event_id > 0) { chara_id = i->event_id; break; }
						}
					}
					result = Game_Map::GetEvent(chara_id) != nullptr;
				}
				break;
			}
			if (com.parameters[4] != 0) break;
		} else

		if (Player::IsPatchManiac()) {
			chara_id = ValueOrVariable(com.parameters[3], chara_id);
		}

		character = GetCharacter(chara_id, "ConditionalBranch");
		if (character != NULL) {
			result = character->GetFacing() == com.parameters[2];
		}
		break;
	}
	case 7: {
		// Vehicle in use
		Game_Vehicle::Type vehicle_id = (Game_Vehicle::Type) (com.parameters[1] + 1);
		Game_Vehicle* vehicle = Game_Map::GetVehicle(vehicle_id);

		if (!vehicle) {
			Output::Warning("ConditionalBranch: Invalid vehicle ID {}", static_cast<int>(vehicle_id));
			return true;
		}

		result = vehicle->IsInUse();
		break;
	}
	case 8:
		// Key decision initiated this event
		result = frame.triggered_by_decision_key;
		break;
	case 9:
		// BGM looped at least once
		result = Main_Data::game_system->BgmPlayedOnce();
		break;
	case 10:
		if (Player::IsRPG2k3Commands()) {
			value1 = Main_Data::game_party->GetTimerSeconds(Main_Data::game_party->Timer2);
			value2 = com.parameters[1];
			switch (com.parameters[2]) {
				case 0:
					result = (value1 >= value2);
					break;
				case 1:
					result = (value1 <= value2);
					break;
			}
		}
		break;
	case 11:
		// RPG Maker 2003 v1.11 features
		if (Player::IsRPG2k3ECommands()) {
			switch (com.parameters[1]) {
				case 0:
					// Any savestate available
					result = FileFinder::HasSavegame();
					break;
				case 1:
					// Is Test Play mode?
					result = Player::debug_flag;
					break;
				case 2:
					// Is ATB wait on?
					result = Main_Data::game_system->GetAtbMode() == lcf::rpg::SaveSystem::AtbMode_atb_wait;
					break;
				case 3:
					// Is Fullscreen active?
					result = DisplayUi->IsFullscreen();
					break;
			}
		}
		break;
	case 12:
		// Maniac: Other
		if (Player::IsPatchManiac()) {
			switch (com.parameters[1]) {
				case 0:
					result = maniac ? Main_Data::game_system->IsManiacLoadPending() : Main_Data::game_system->IsLoadedThisFrame();
					break;
				case 1:
					if (maniac) {
						result = DisplayUi && DisplayUi->IsJoystickAvailable();
						break;
					}
#if defined(USE_JOYSTICK) && defined(SUPPORT_JOYSTICK)
					result = true;
#else
					result = false;
#endif
					break;
				case 2:
					result = !maniac || (DisplayUi && DisplayUi->HasInputFocus());
					break;
				case 3:
					result = maniac && Output::IsManiacFileOutputAvailable();
					break;
			}
		}
		break;
	case 13:
		// Maniac: Switch through Variable
		if (maniac) {
			result = (ReadSwitch(ResolveVariableIndirectAddress(com.parameters[1])) != 0) == (com.parameters[2] == 0);
		}
		break;
	case 14:
		// Maniac: Variable indirect
		if (maniac) {
			value1 = ReadVariableIndirect(com.parameters[1]);
			value2 = com.parameters[2] == 0 ? com.parameters[3] : com.parameters[2] == 1
				? ReadVariable(com.parameters[3]) : ReadVariableIndirect(com.parameters[3]);
			result = CheckOperator(value1, value2, com.parameters[4]);
		}
		break;
	case 15:
		// Maniac: String comparison
		if (maniac) {
			const uint32_t modes = static_cast<uint32_t>(com.parameters[1]);
			const int left_id = (modes & 15) == 0 ? com.parameters[2] : ResolveVariableIndirectAddress(com.parameters[2]);
			const int right_mode = (modes >> 4) & 15;
			const int right_id = right_mode == 0 ? 0 : right_mode == 1 ? com.parameters[3] : ResolveVariableIndirectAddress(com.parameters[3]);
			auto left = Utils::DecodeUTF16(ReadString(left_id), true);
			auto right = Utils::DecodeUTF16(right_mode == 0 ? std::string_view(com.string) : ReadString(right_id), true);
			bool left_present = IsManiacStringPresent(left_id);
			bool right_present = right_mode == 0 ? !com.string.empty() : IsManiacStringPresent(right_id);
			const uint32_t options = static_cast<uint32_t>(com.parameters[4]);
			const bool fold = ((options >> 8) & 255) != 0;
			if (fold) {
				left_present = !left.empty(); right_present = !right.empty();
				for (auto* text : {&left, &right}) for (auto& unit : *text) if (unit >= u'A' && unit <= u'Z') unit += u'a' - u'A';
			}
			const bool equal = left_present == right_present && left == right;
			const auto contains = [&]() {
				if (!right_present) return true;
				if (!left_present || left.empty()) return false;
				return std::u16string_view(left.data(), std::find(left.begin(), left.end(), u'\0') - left.begin()).find(
					std::u16string_view(right.data(), std::find(right.begin(), right.end(), u'\0') - right.begin())) != std::u16string_view::npos;
			};
			switch (options & 255) {
				case 0: result = equal; break;
				case 1: result = !equal; break;
				case 2: result = contains(); break;
				case 3: result = !contains(); break;
				default: result = fold && equal; break;
			}
			break;
		}
		break;
	case 16:
		// Maniac: Expression
		result = ManiacPatch::ParseExpression(MakeSpan(com.parameters).subspan(6), *this);
		break;
	default:
		Output::Warning("ConditionalBranch: Branch {} unsupported", com.parameters[0]);
	}

	int sub_idx = subcommand_sentinel;
	if (!result) {
		sub_idx = eOptionBranchElse;
		if (maniac) {
			auto& mutable_frame = GetFrame();
			for (int i = mutable_frame.current_command + 1; i < static_cast<int>(mutable_frame.commands.size()); ++i) {
				const auto& marker = mutable_frame.commands[i];
				if (marker.indent != com.indent) continue;
				mutable_frame.current_command = i;
				if (marker.code == static_cast<int>(Cmd::ElseBranch) || marker.code == static_cast<int>(Cmd::EndBranch)) {
					// liblcf omits the serialized END sentinel. Reaching the end
					// of this vector is normal completion, handled by Update().
					++mutable_frame.current_command;
				}
				break;
			}
		} else SkipToNextConditional({Cmd::ElseBranch, Cmd::EndBranch}, com.indent);
	}

	SetSubcommandIndex(com.indent, sub_idx);
	return true;
}


bool Game_Interpreter::CommandElseBranch(lcf::rpg::EventCommand const& com) { //code 22010
	if (Player::IsPatchManiac()) {
		auto& frame = GetFrame();
		for (int i = frame.current_command + 1; i < static_cast<int>(frame.commands.size()); ++i) {
			if (frame.commands[i].indent == com.indent) { frame.current_command = i; break; }
		}
		return true;
	}
	return CommandOptionGeneric(com, eOptionBranchElse, {Cmd::EndBranch});
}

bool Game_Interpreter::CommandEndBranch(lcf::rpg::EventCommand const& /* com */) { //code 22011
	return true;
}

bool Game_Interpreter::CommandJumpToLabel(lcf::rpg::EventCommand const& com) { // code 12120
	auto& frame = GetFrame();
	const auto& list = frame.commands;
	auto& index = frame.current_command;

	const bool maniac = Player::IsPatchManiac();
	int label_id = com.parameters.empty() ? 0 : com.parameters[0];

	for (int idx = 0; (size_t)idx < list.size(); idx++) {
		if (static_cast<Cmd>(list[idx].code) != Cmd::Label)
			continue;
		if ((list[idx].parameters.empty() ? 0 : list[idx].parameters[0]) != label_id || (!maniac && list[idx].parameters.empty()))
			continue;
		// Re-fetching the label uses the immediate skip and consumes no budget.
		// Keep the cursor different from this jump even for Label; Jump pairs,
		// so Update's automatic increment cannot skip a jump back to itself.
		index = idx;
		break;
	}

	return true;
}

bool Game_Interpreter::CommandManiacLoop(lcf::rpg::EventCommand const& com, bool end) {
	auto& frame = GetFrame();
	const auto arg = [&](int64_t i) -> int32_t {
		return i >= 0 && static_cast<uint64_t>(i) < com.parameters.size() ? com.parameters[i] : 0;
	};
	const int type = arg(0);
	const auto rewind = [&]() {
		for (int i = frame.current_command - 1; i >= 0; --i) {
			if (frame.commands[i].indent == com.indent && frame.commands[i].code == static_cast<int>(Cmd::Loop)) {
				frame.current_command = i + 1;
				break;
			}
		}
	};
	if (type == 0) {
		if (end) rewind();
		return true;
	}
	const auto offset = static_cast<size_t>(com.indent) * 2;
	if (frame.maniac_loop_info.size() < offset + 2) frame.maniac_loop_info.resize(offset + 2);
	frame.maniac_loop_info_size = static_cast<int32_t>(frame.maniac_loop_info.size() / 2);
	auto& current = frame.maniac_loop_info[offset];
	auto& bound = frame.maniac_loop_info[offset + 1];
	const uint32_t modes = static_cast<uint32_t>(arg(1));
	const auto scalar = [&](int32_t raw, int mode) -> int32_t {
		if (mode == 0) return raw;
		if (mode == 1) return ReadVariable(raw);
		if (mode == 2) return ReadVariableIndirect(raw);
		return ReadSwitch(mode == 3 ? raw : ResolveVariableIndirectAddress(raw));
	};
	const auto compare = [&]() {
		const int op = (modes >> 8) & 255;
		return op > 5 || CheckOperator(scalar(arg(2), modes & 15), scalar(arg(3), (modes >> 4) & 15), op);
	};
	const auto list_value = [&](int32_t i) {
		const uint32_t packed = static_cast<uint32_t>(arg(static_cast<int64_t>(arg(2)) + i / 4));
		const int mode = (packed >> ((static_cast<uint32_t>(i % 4) * 4) & 31)) & 15;
		const int32_t raw = arg(static_cast<int64_t>(i) + 5);
		return mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
	};
	const auto output = [&](int32_t value, bool pair) {
		const int32_t destination = arg(4);
		if (destination == 0) return;
		const size_t slot = destination < 0 ? -static_cast<int64_t>(destination) : destination;
		if (destination < 0) {
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = state ? state->Ensure(frame.easyrpg_self_scope, frame.easyrpg_self_map_id, frame.easyrpg_self_event_id) : nullptr;
			if (!bank) Output::Error("Failed to create self-variables.");
			bank->numbers.resize(std::max(bank->numbers.size(), slot + (pair ? 3 : 1)));
			bank->numbers[slot] = value;
			if (pair) bank->numbers[slot + 1] = current;
		} else {
			Main_Data::game_variables->EnsureSize(slot + (pair ? 2 : 0));
			*Main_Data::game_variables->GetManiacWritable(destination) = value;
			if (pair) *Main_Data::game_variables->GetManiacWritable(destination + 1) = current;
		}
		Game_Map::RefreshAfterManiacChange();
	};
	const auto advance = [&](int change) {
		current = static_cast<int32_t>(static_cast<uint32_t>(current) + static_cast<uint32_t>(change));
	};
	bool enter = true;
	int32_t value = 0;
	if (!end) {
		switch (type) {
			case 1:
				value = static_cast<int32_t>(static_cast<uint32_t>(scalar(arg(2), modes & 15)) - 1);
				enter = value >= 0;
				if (enter) { current = 0; bound = value; }
				value = 0;
				break;
			case 2: case 3: {
				value = scalar(arg(2), modes & 15);
				const int32_t limit = scalar(arg(3), (modes >> 4) & 15);
				enter = type == 2 ? value <= limit : value >= limit;
				if (enter) { current = value; bound = limit; }
				break;
			}
			case 4: case 5:
				enter = type == 5 || compare();
				if (enter) current = 0;
				break;
			case 6:
				enter = arg(3) > 0;
				if (enter) { current = 0; bound = arg(3) - 1; value = list_value(0); }
				break;
			case 8:
				current = 0;
				bound = Game_Map::GetMapId();
				enter = !Game_Map::GetEvents().empty();
				if (enter) value = Game_Map::GetEvents().front().GetId();
				break;
			default:
				value = scalar(arg(3), ((modes >> 4) & 15) == 0 ? 0 : ((modes >> 4) & 15) == 1 ? 1 : 2);
				enter = value > 0;
				if (enter) { current = 0; bound = value - 1; value = ReadVariableIndirect(arg(2)); }
				break;
		}
		if (!enter) {
			// Native skips the first equal-indent record, independently of opcode.
			const int next = frame.current_command + 1;
			frame.current_command += 2;
			for (int i = next; i < static_cast<int>(frame.commands.size()); ++i) {
				if (frame.commands[i].indent == com.indent) { frame.current_command = i + 1; break; }
			}
			return true;
		}
		output(value, type >= 6);
		return true;
	}
	if (type == 4 || type == 5) {
		if (!compare()) return true;
		advance(1);
	} else if (type == 8) {
		if (bound != Game_Map::GetMapId()) return true;
		advance(1);
		if (static_cast<uint32_t>(current) >= Game_Map::GetEvents().size()) return true;
	} else {
		advance(type == 3 ? -1 : 1);
		if (type == 3 ? current < bound : current > bound) return true;
	}
	if (type < 6) value = current;
	else if (arg(4) != 0) {
		if (type == 6) value = list_value(current);
		else if (type == 8) value = Game_Map::GetEvents()[current].GetId();
		else value = ReadVariableIndirect(static_cast<int32_t>(static_cast<uint32_t>(arg(2)) + static_cast<uint32_t>(current)));
	}
	// Type8's native end has a positive-only output gate; begin accepts SELF.
	if (type != 8 || arg(4) > 0) output(value, type >= 6);
	rewind();
	return true;
}

bool Game_Interpreter::CommandLoop(lcf::rpg::EventCommand const& com) { // code 12210
	if (Player::IsPatchManiac()) {
		return CommandManiacLoop(com, false);
	}
	if (!Player::IsPatchManiac() || com.parameters.size() < 5 || com.parameters[0] == 0) {
		// Infinite loop
		return true;
	}

	int type = com.parameters[0];

	auto& frame = GetFrame();
	auto& index = frame.current_command;
	frame.maniac_loop_info.resize((com.indent + 1) * 2);
	frame.maniac_loop_info_size = static_cast<int32_t>(frame.maniac_loop_info.size() / 2);

	int32_t& begin_loop_val = frame.maniac_loop_info[frame.maniac_loop_info.size() - 2];
	int32_t& end_loop_val = frame.maniac_loop_info[frame.maniac_loop_info.size() - 1];
	begin_loop_val = 0;
	end_loop_val = 0;

	int begin_arg = ValueOrVariableBitfield(com.parameters[1], 0, com.parameters[2]);
	int end_arg = ValueOrVariableBitfield(com.parameters[1], 1, com.parameters[3]);
	int op = com.parameters[1] >> 8;

	switch (type) {
		case 1: // X times
			end_loop_val = begin_arg - 1;
			break;
		case 2: // Count up
		case 3: // Count down
			begin_loop_val = begin_arg;
			end_loop_val = end_arg;
			break;
		case 4: // While
		case 5: // Do While
			break;
		default:
			SkipToNextConditional({Cmd::EndLoop}, com.indent);
			++index;
			return true;
	}

	int check_beg = begin_loop_val;
	int check_end = end_loop_val;
	if (type == 4) { // While
		check_beg = ValueOrVariableBitfield(com.parameters[1], 0, com.parameters[2]);
		check_end = ValueOrVariableBitfield(com.parameters[1], 1, com.parameters[3]);
	}

	// Do While (5) always runs the loop at least once
	if (type != 5 && !ManiacCheckContinueLoop(check_beg, check_end, type, op)) {
		SkipToNextConditional({Cmd::EndLoop}, com.indent);
		++index;
	}

	int loop_count_var = com.parameters[4];
	if (loop_count_var > 0) {
		Main_Data::game_variables->Set(loop_count_var, begin_loop_val);
		Game_Map::SetNeedRefreshForVarChange(loop_count_var);
	}

	return true;
}

bool Game_Interpreter::CommandBreakLoop(lcf::rpg::EventCommand const& com) { // code 12220
	auto& frame = GetFrame();
	const auto& list = frame.commands;
	auto& index = frame.current_command;

	// BreakLoop will jump to the end of the event if there is no loop.

	if (Player::IsPatchManiac()) {
		// Maniac adds continue (type 1) and a zero-based enclosing-loop level.
		// Find the open loop scopes first: a conditional or an already closed
		// sibling loop must not count as an enclosing loop.
		int level = std::max(0, com.parameters[1]);
		int indent = com.indent;
		int loop_indent = -1;
		for (int idx = index - 1; idx >= 0; --idx) {
			if (list[idx].indent >= indent) {
				continue;
			}
			indent = list[idx].indent;
			if (static_cast<Cmd>(list[idx].code) == Cmd::Loop && level-- == 0) {
				loop_indent = indent;
				break;
			}
		}

		if (loop_indent < 0) {
			index = static_cast<int>(list.size());
			return true;
		}

		const bool is_continue = com.parameters[0] == 1;
		SkipToNextConditional({ Cmd::EndLoop }, loop_indent);
		// Continue executes EndLoop so counted loops advance and while loops
		// recheck their condition. Break skips it entirely.
		if (!is_continue && index < static_cast<int>(list.size())) {
			++index;
		}

		const auto keep = static_cast<size_t>((loop_indent + (is_continue ? 1 : 0)) * 2);
		if (frame.maniac_loop_info.size() > keep) {
			frame.maniac_loop_info.resize(keep);
			frame.maniac_loop_info_size = static_cast<int32_t>(keep / 2);
		}
		return true;
	}

	// This emulates an RPG_RT bug where break loop ignores scopes and
	// unconditionally jumps to the next EndLoop command.
	auto pcode = static_cast<Cmd>(list[index].code);
	for (++index; index < (int)list.size(); ++index) {
		if (pcode == Cmd::EndLoop) {
			break;
		}
		pcode = static_cast<Cmd>(list[index].code);
	}

	return true;
}

bool Game_Interpreter::CommandEndLoop(lcf::rpg::EventCommand const& com) { // code 22210
	if (Player::IsPatchManiac()) {
		return CommandManiacLoop(com, true);
	}
	auto& frame = GetFrame();
	const auto& list = frame.commands;
	auto& index = frame.current_command;

	int indent = com.indent;

	if (Player::IsPatchManiac() && com.parameters.size() >= 5 && com.parameters[0] != 0) {
		int type = com.parameters[0];
		int offset = com.indent * 2;

		if (static_cast<int>(frame.maniac_loop_info.size()) < (offset + 1) * 2) {
			frame.maniac_loop_info.resize((offset + 1) * 2);
			frame.maniac_loop_info_size = frame.maniac_loop_info.size() / 2;
		}

		int32_t& cur_loop_val = frame.maniac_loop_info[offset];
		int32_t& end_loop_val = frame.maniac_loop_info[offset + 1];

		switch (type) {
			case 1: // X times
			case 2: // Count up
			case 4: // While
			case 5: // Do While
				++cur_loop_val;
				break;
			case 3: // Count down
				--cur_loop_val;
				break;
			default:
				++index;
				return true;
		}

		int check_cur = cur_loop_val;
		int check_end = end_loop_val;
		if (type >= 4) {
			// While (4) and Do While (5) fetch variables each loop
			// For the others it is constant
			check_cur = ValueOrVariableBitfield(com.parameters[1], 0, com.parameters[2]);
			check_end = ValueOrVariableBitfield(com.parameters[1], 1, com.parameters[3]);
		}
		int op = com.parameters[1] >> 8;
		if (!ManiacCheckContinueLoop(check_cur, check_end, type, op)) {
			// End loop
			frame.maniac_loop_info.resize(offset);
			frame.maniac_loop_info_size = offset / 2;
			++index;
			return true;
		}

		int loop_count_var = com.parameters[4];
		if (loop_count_var > 0) {
			Main_Data::game_variables->Set(loop_count_var, cur_loop_val);
			Game_Map::SetNeedRefreshForVarChange(loop_count_var);
		}
	}

	// Restart the loop
	for (int idx = index; idx >= 0; idx--) {
		if (list[idx].indent > indent)
			continue;
		if (list[idx].indent < indent)
			return false;
		if (static_cast<Cmd>(list[idx].code) != Cmd::Loop)
			continue;
		index = idx;
		break;
	}

	// Jump past the Cmd::Loop to the first command.
	if (index < (int)frame.commands.size()) {
		++index;
	}

	return true;
}

bool Game_Interpreter::CommandEraseEvent(lcf::rpg::EventCommand const& com) { // code 12320
	int event_id = 0; // default rm values
	bool is_active = 0; // In Vanilla RM event is always itself and it always becomes inactive

	if (Player::IsPatchManiac() && com.parameters.size() >= 3) {
		event_id = ValueOrVariableBitfield(com.parameters[0], 1, com.parameters[2]);
		is_active = com.parameters[1];
	}

	auto& frame = GetFrame();
	auto& index = frame.current_command;

	if (event_id == 0) {
		event_id = GetThisEventId();
	}

	// When a common event and not RPG2k3E engine ignore the call, otherwise
	// operate on last map_event
	if (event_id == 0)
		return true;

	Game_Event* evnt = Game_Map::GetEvent(event_id);
	if (evnt) {
		evnt->SetActive(is_active);

		// Parallel map events shall stop immediately
		if (!main_flag) {
			// When the event was called indirectly from a parallel process
			// event execution continues after a yield.
			// Increment the index for this special case.
			index++;
			return false;
		}
	}

	return true;
}

bool Game_Interpreter::CommandManiacSelfvar(lcf::rpg::EventCommand const& com) try { // code 3036
	if (!Player::IsPatchManiac() || !Main_Data::game_maniac_self_variables) {
		return true;
	}
	// The native dispatcher refreshes map pages after every 3036 helper path,
	// including unknown operations and branches that did not write a value.
	auto refresh_guard = lcf::makeScopeGuard([]() {
		Game_Map::RefreshAfterManiacChange();
	});
	const auto param = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t mode = static_cast<uint32_t>(param(0));
	const auto nibble = [&](unsigned index) -> int {
		return static_cast<int>((mode >> (index * 4)) & 15);
	};
	const uint32_t options = static_cast<uint32_t>(param(7));
	const int kind = (options >> 4) & 15;
	const int array_mode = (options >> 8) & 15;
	const int operation = param(9);
	if (operation < 0 || operation > 2) {
		return true;
	}
	if (operation == 1 && nibble(3) != 1 && nibble(3) != 2) {
		// Clear checks its target ID selector before resolving an environment.
		return true;
	}
	// Runtime consumes the shared command text as UTF-16 units. The byte
	// budget is an approximation and must not be measured in UTF-8 bytes.
	const auto text_units = Utils::DecodeUTF16(com.string, Player::IsPatchManiac());
	size_t text_cursor = 0;
	const auto take_text = [&](int32_t budget) -> std::u16string {
		const size_t begin = text_cursor;
		while (budget > 0 && text_cursor < text_units.size()) {
			const uint16_t unit = text_units[text_cursor];
			const int cost = unit <= 0x00ff || (unit >= 0xff61 && unit <= 0xff9f) ? 1 : 2;
			if (cost > budget) break;
			budget -= cost;
			++text_cursor;
		}
		return text_units.substr(begin, text_cursor - begin);
	};

	// The sign of a negative first reference is retained only when its result
	// is used as an address for another read (or as a Set destination ID).
	const auto propagate_negative_reference = [&](int32_t id) -> int32_t {
		const int32_t value = ReadVariable(id);
		if (id >= 0) {
			return value;
		}
		const int64_t magnitude = std::abs(static_cast<int64_t>(value));
		return static_cast<int32_t>(-magnitude);
	};
	const auto select = [&](int32_t raw, int selector) -> int32_t {
		if (selector == 1) return ReadVariable(raw);
		if (selector == 2) return ReadVariable(propagate_negative_reference(raw));
		return raw;
	};
	const auto select_map_for_clear_or_init = [&](int32_t raw, int selector) -> int32_t {
		if (operation != 0 && selector == 5) return propagate_negative_reference(raw);
		return select(raw, selector);
	};
	const int count_mode = options & 15;
	const auto read_count = [&]() -> int32_t {
		return count_mode == 0 ? param(8) : count_mode == 1 ? ReadVariable(param(8)) : ReadVariable(propagate_negative_reference(param(8)));
	};

	struct Key { int scope, map_id, event_id; };
	const auto resolve = [&](int env, int id_mode, int map_mode, int32_t raw_id, int32_t raw_map) -> std::optional<Key> {
		if (env == 3 || env == 4) {
			const size_t depth = env == 3 ? 1 : 2;
			if (_state.stack.size() >= depth) {
				const auto& frame = _state.stack[_state.stack.size() - depth];
				if (frame.easyrpg_self_scope == 1 || frame.easyrpg_self_scope == 2) {
					return Key{frame.easyrpg_self_scope, frame.easyrpg_self_map_id, frame.easyrpg_self_event_id};
				}
			}
			if (operation == 0) Output::Error("Failed to create self-variables.");
			return std::nullopt;
		}
		if (env == 1) {
			// Literal and numeric-reference selectors are independent of the chosen bank.
			int id = raw_id;
			if (id_mode == 1 || id_mode == 2) id = select(raw_id, id_mode);
			if (id_mode >= 3 && id_mode <= 5) {
				if (id_mode == 3 && com.easyrpg_maniac_common_name_cache != 0) {
					id = com.easyrpg_maniac_common_name_cache;
				} else {
					std::u16string name;
					if (id_mode == 3) {
						name = take_text(raw_id);
					} else {
						const auto value = ReadString(id_mode == 4 ? raw_id : ReadVariable(raw_id));
						name = Utils::DecodeUTF16(value.substr(0, value.find('\0')), true);
					}
					id = 0;
					for (const auto& candidate : lcf::Data::commonevents) {
						if (Utils::DecodeUTF16(static_cast<std::string_view>(candidate.name), true) == name) {
							id = candidate.ID;
							break;
						}
					}
					if (id == 0) Output::Error("Invalid Common Event: {}", Utils::EncodeUTF(name, true));
					if (id_mode == 3 && id != 0) com.easyrpg_maniac_common_name_cache = id;
				}
			}
			if (id == 0) {
				for (auto it = _state.stack.rbegin(); it != _state.stack.rend(); ++it) {
					if (it->easyrpg_self_scope == 1) { id = it->easyrpg_self_event_id; break; }
				}
			}
			return Key{1, 0, id};
		}
		if (env == 2) {
			int map_id = select_map_for_clear_or_init(raw_map, map_mode);
			if (map_id == 0 && Main_Data::game_player) map_id = Main_Data::game_player->GetMapId();
			int id = select_map_for_clear_or_init(raw_id, id_mode);
			if (id == 0 || id == 10005) {
				id = 0;
				for (auto it = _state.stack.rbegin(); it != _state.stack.rend(); ++it) {
					if (it->event_id > 0) {
						id = it->event_id;
						break;
					}
				}
			}
			return Key{2, map_id, id};
		}
		return Key{0, 0, 0};
	};
	const auto find_bank = [&](Key key) -> Game_ManiacSelfVariables::Bank* {
		for (auto& bank : Main_Data::game_maniac_self_variables->MutableBanks()) {
			if (bank.scope == key.scope && bank.map_id == key.map_id && bank.event_id == key.event_id) return &bank;
		}
		return nullptr;
	};
	// Clear reads its count and target ID before resolving the destination environment.
	const int32_t clear_count = operation == 1 ? read_count() : 0;
	const int32_t clear_target_id = operation == 1
		? (nibble(3) == 1 ? param(3) : ReadVariable(param(3))) : 0;
	const auto target = resolve(nibble(0), nibble(1), nibble(2), param(1), param(2));
	if (!target) return true;
	if (operation == 0 && target->scope != 0) {
		Main_Data::game_maniac_self_variables->Ensure(target->scope, target->map_id, target->event_id);
	}
	std::optional<Key> source;
	if (operation == 0) {
		source = resolve(nibble(4), nibble(5), nibble(6), param(4), param(5));
		if (source && source->scope != 0) {
			Main_Data::game_maniac_self_variables->Ensure(source->scope, source->map_id, source->event_id);
		}
	}

	if (operation == 2) {
		if (target->scope == 0) {
			Main_Data::game_variables->SetData({});
			Main_Data::game_switches->SetData({});
			Main_Data::game_strings->SetData(Game_Strings::Strings_t{});
		} else {
			const int32_t count = read_count();
			for (auto& bank : Main_Data::game_maniac_self_variables->MutableBanks()) {
				if (bank.scope != target->scope || (target->scope == 2 && target->map_id >= 0 && bank.map_id != target->map_id)) continue;
				if (target->event_id >= 0) {
					const int64_t offset = static_cast<int64_t>(bank.event_id) - target->event_id;
					if (offset < 0 || (count >= 0 && offset >= count)) continue;
				}
				bank.numbers.clear();
				bank.switches.clear();
				bank.strings.clear();
				bank.empty_string_slots.clear();
			}
		}
		return true;
	}

	if (nibble(3) != 1 && nibble(3) != 2) return true;
	int32_t target_id = operation == 1 ? clear_target_id : nibble(3) == 1 ? param(3) : propagate_negative_reference(param(3));
	const int32_t count = operation == 1 ? clear_count : read_count();
	if (operation == 1) {
		const auto clear_bank = [&](Game_ManiacSelfVariables::Bank* bank) {
			// Negative starts can address before native vectors. Skip that unsafe case.
			if (!bank || target_id < 0) return;
			const size_t start = static_cast<size_t>(target_id);
			const auto clear = [&](auto& values) {
				if (start >= values.size()) return;
				const size_t length = count < 0 ? values.size() - start : std::min<size_t>(count, values.size() - start);
				std::fill_n(values.begin() + start, length, typename std::decay_t<decltype(values)>::value_type{});
			};
			if (kind == 0 || kind == 3) clear(bank->numbers);
			if (kind == 1 || kind == 3) {
				clear(bank->strings);
				bank->empty_string_slots.erase(std::remove_if(bank->empty_string_slots.begin(),
					bank->empty_string_slots.end(), [&](int32_t id) {
						return id >= 0 && static_cast<size_t>(id) >= start
							&& (count < 0 || static_cast<int64_t>(id) < static_cast<int64_t>(start) + count);
					}), bank->empty_string_slots.end());
			}
			if (kind == 2 || kind == 3) clear(bank->switches);
		};
		if (target->scope == 0) {
			if (target_id < 0) return true; // Negative global starts can write before native vectors.
			const int64_t start = target_id;
			const int64_t end = count < 0 ? INT64_MAX : start + count;
			if (kind == 0 || kind == 3) {
				if (start == 0 && end > 0) *Main_Data::game_variables->GetManiacWritable(0) = 0;
				for (int64_t id = std::max<int64_t>(start, 1); id <= Main_Data::game_variables->GetSize() && id < end; ++id) {
					*Main_Data::game_variables->GetManiacWritable(static_cast<int>(id)) = 0;
				}
			}
			if (kind == 1 || kind == 3) {
				if (start == 0 && end > 0) Main_Data::game_strings->SetManiacZeroSaveData({});
				const size_t vector_size = Main_Data::game_strings->GetManiacVectorSize();
				auto values = Main_Data::game_strings->GetData();
				for (auto it = values.begin(); it != values.end();) {
					if (it->first >= start && it->first < end) it = values.erase(it);
					else ++it;
				}
				const auto zero = Main_Data::game_strings->GetManiacZeroSaveData();
				Main_Data::game_strings->SetData(std::move(values));
				Main_Data::game_strings->EnsureSize(vector_size);
				Main_Data::game_strings->SetManiacZeroSaveData(zero);
			}
			if (kind == 2 || kind == 3) {
				if (start == 0 && end > 0) Main_Data::game_switches->SetManiacRaw(0, 0);
				for (int64_t id = std::max<int64_t>(start, 1); id <= Main_Data::game_switches->GetSize() && id < end; ++id) {
					Main_Data::game_switches->SetManiacRaw(static_cast<int>(id), 0);
				}
			}
		} else if (target->event_id >= 0 && target->map_id >= 0) {
			clear_bank(find_bank(*target));
		} else {
			for (auto& bank : Main_Data::game_maniac_self_variables->MutableBanks()) {
				if (bank.scope == target->scope && (target->scope == 1 || target->map_id < 0 || bank.map_id == target->map_id)
					&& (target->event_id < 0 || bank.event_id == target->event_id)) clear_bank(&bank);
			}
		}
		return true;
	}

	if (!source) return true;
	if (array_mode == 1) {
		if (nibble(7) != 1 && nibble(7) != 2) return true;
		const int32_t source_id = nibble(7) == 1 ? param(6) : propagate_negative_reference(param(6));
		auto* source_bank = find_bank(*source);
		auto* target_bank = find_bank(*target);
		if ((source->scope != 0 && !source_bank) || (target->scope != 0 && !target_bank)) return true;
		// Negative same-vector IDs address before the native buffer after its
		// capacity check. Avoid reproducing that memory corruption.
		const bool same_bank = target->scope == source->scope && target->map_id == source->map_id && target->event_id == source->event_id;
		if (same_bank && (target_id < 0 || source_id < 0)) return true;
		if (count < 0) {
			const auto length = [&](int32_t id) {
				return static_cast<uint64_t>(std::abs(static_cast<int64_t>(id)))
					+ static_cast<uint32_t>(count) + 1;
			};
			const uint64_t required = same_bank ? length(std::max(target_id, source_id))
				: std::max(length(target_id), length(source_id));
			if (required > UINT32_MAX) Output::Error("Selfvar: Vector length exceeds uint32 range.");
			const auto grow = [&](Key key, int32_t id) {
				const auto size = static_cast<size_t>(length(id));
				if (key.scope == 0) {
					if (kind == 1) Main_Data::game_strings->EnsureSize(size - 1);
					else if (kind == 2) Main_Data::game_switches->EnsureSize(size - 1);
					else Main_Data::game_variables->EnsureSize(size - 1);
				} else if (auto* bank = find_bank(key)) {
					if (kind == 1) bank->strings.resize(std::max(bank->strings.size(), size));
					else if (kind == 2) bank->switches.resize(std::max(bank->switches.size(), size));
					else bank->numbers.resize(std::max(bank->numbers.size(), size));
				}
			};
			if (same_bank) grow(*target, std::max(target_id, source_id));
			else {
				grow(*target, target_id);
				grow(*source, source_id);
			}
			// Negative string counts skip their signed copy loop after growth.
			// Wrapped-zero numeric copies are safe; other negative byte copies
			// address outside storage in the fixed runtime.
			return true;
		}
		const auto copy_array = [&](auto& global, auto bank_vector, bool global_has_zero = false) -> bool {
			using Vector = std::decay_t<decltype(global)>;
			using Value = typename Vector::value_type;
			Vector* dst = target->scope == 0 ? &global : &bank_vector(*target_bank);
			Vector* src = source->scope == 0 ? &global : &bank_vector(*source_bank);
			const auto slot = [&](int32_t id, bool is_global) -> uint64_t {
				const uint64_t value = static_cast<uint64_t>(std::abs(static_cast<int64_t>(id)));
				return is_global && !global_has_zero && value > 0 ? value - 1 : value;
			};
			const uint64_t dst_index = slot(target_id, target->scope == 0);
			const uint64_t src_index = slot(source_id, source->scope == 0);
			const uint64_t dst_size = dst_index + static_cast<uint64_t>(count) + 1;
			const uint64_t src_size = src_index + static_cast<uint64_t>(count) + 1;
			if (dst_size > UINT32_MAX || src_size > UINT32_MAX) {
				Output::Error("Selfvar: Vector length exceeds uint32 range.");
			}
			if (dst == src) {
				dst->resize(std::max<size_t>(dst->size(), std::max(dst_size, src_size)));
			} else {
				dst->resize(std::max<size_t>(dst->size(), dst_size));
				src->resize(std::max<size_t>(src->size(), src_size));
			}
			const auto d = static_cast<size_t>(dst_index);
			const auto s = static_cast<size_t>(src_index);
			const auto n = static_cast<size_t>(count);
			if constexpr (std::is_same_v<Value, lcf::DBString>) {
				// Native string arrays retain/release in forward order. Overlap can
				// propagate a value that was written by an earlier iteration.
				for (size_t i = 0; i < n; ++i) (*dst)[d + i] = (*src)[s + i];
			} else if (dst == src && d > s && d < s + n) {
				std::copy_backward(src->begin() + s, src->begin() + s + n, dst->begin() + d + n);
			} else {
				std::copy_n(src->begin() + s, n, dst->begin() + d);
			}
			return true;
		};
		bool copied = false;
		if (kind == 1) {
			const auto previous_zero = Main_Data::game_strings->GetManiacZeroSaveData();
			auto global = Main_Data::game_strings->GetLcfData();
			global.insert(global.begin(), lcf::DBString(Main_Data::game_strings->GetManiacRawSlot(0)));
			std::vector<uint8_t> present(global.size());
			present[0] = !previous_zero.empty();
			for (const auto& [id, value] : Main_Data::game_strings->GetData()) {
				if (id > 0 && static_cast<size_t>(id) < present.size()) present[static_cast<size_t>(id)] = 1;
			}
			const auto bank_presence = [](const Game_ManiacSelfVariables::Bank* bank) {
				std::vector<uint8_t> flags(bank ? bank->strings.size() : 0);
				for (size_t id = 0; id < flags.size(); ++id) flags[id] = Game_ManiacSelfVariables::IsStringPresent(bank, id);
				return flags;
			};
			auto source_present = source->scope == 0 ? std::vector<uint8_t>{} : bank_presence(source_bank);
			auto target_present = target->scope == 0 ? std::vector<uint8_t>{} : bank_presence(target_bank);
			copied = copy_array(global, [](auto& bank) -> auto& { return bank.strings; }, true);
			if (copied) {
				present.resize(global.size());
				if (source->scope != 0) source_present.resize(source_bank->strings.size());
				if (target->scope != 0) target_present.resize(target_bank->strings.size());
				const size_t dst = static_cast<size_t>(std::abs(static_cast<int64_t>(target_id)));
				const size_t src = static_cast<size_t>(std::abs(static_cast<int64_t>(source_id)));
				for (int32_t offset = 0; offset < count; ++offset) {
					const bool value = source->scope == 0 ? present[src + offset]
						: same_bank ? target_present[src + offset] : source_present[src + offset];
					if (target->scope == 0) present[dst + offset] = value;
					else target_present[dst + offset] = value;
				}
				if (target->scope != 0) {
					target_bank->empty_string_slots.clear();
					for (size_t id = 0; id < target_present.size(); ++id) {
						if (target_present[id] && target_bank->strings[id].empty())
							target_bank->empty_string_slots.push_back(static_cast<int32_t>(id));
					}
				}
				if (target->scope == 0 || source->scope == 0) {
					Game_Strings::Strings_t empty_slots;
					for (size_t id = 1; id < global.size(); ++id) {
						if (present[id]) empty_slots[static_cast<int>(id)] = ToString(global[id]);
					}
					Main_Data::game_strings->SetData(std::move(empty_slots));
					Main_Data::game_strings->EnsureSize(global.size() - 1);
					if (present[0]) Main_Data::game_strings->SetManiacZeroSaveData({global[0]});
				}
			}
		} else if (kind == 2) {
			std::vector<uint8_t> global;
			global.push_back(Main_Data::game_switches->GetManiacRawSlot(0));
			for (int id = 1; id <= Main_Data::game_switches->GetSize(); ++id) {
				global.push_back(static_cast<uint8_t>(Main_Data::game_switches->GetInt(id)));
			}
			copied = copy_array(global, [](auto& bank) -> auto& { return bank.switches; }, true);
			if (copied && (target->scope == 0 || source->scope == 0)) {
				Game_Switches::Switches_t values;
				for (size_t index = 1; index < global.size(); ++index) values.push_back(global[index] != 0);
				Main_Data::game_switches->SetData(std::move(values));
				Main_Data::game_switches->SetManiacRaw(0, global[0]);
				for (size_t index = 1; index < global.size(); ++index) {
					if (global[index] > 1) Main_Data::game_switches->SetManiacRaw(static_cast<int>(index), global[index]);
				}
			}
		} else {
			Game_Variables::Variables_t global;
			global.push_back(Main_Data::game_variables->GetManiacRawSlot(0));
			const auto& ordinary = Main_Data::game_variables->GetData();
			global.insert(global.end(), ordinary.begin(), ordinary.end());
			copied = copy_array(global, [](auto& bank) -> auto& { return bank.numbers; }, true);
			if (copied && (target->scope == 0 || source->scope == 0)) {
				const int32_t zero = global[0];
				Main_Data::game_variables->SetData(Game_Variables::Variables_t(global.begin() + 1, global.end()));
				*Main_Data::game_variables->GetManiacWritable(0) = zero;
			}
		}
		return true;
	}
	if (array_mode != 0 || (target->scope != 0 && (target->event_id <= 0 || target->map_id < 0))) return true;
	const auto* source_bank = find_bank(*source);
	const auto self_index = [](int32_t id) -> size_t { return static_cast<size_t>(std::abs(static_cast<int64_t>(id))); };
	const auto self_number = [&](int32_t id) -> int32_t {
		const size_t index = self_index(id);
		return source_bank && index < source_bank->numbers.size() ? source_bank->numbers[index] : 0;
	};
	int32_t number = param(6);
	std::string string_value;
	bool string_present = false;
	if (kind == 1) {
		if (source->scope == 0) {
			if (nibble(7) == 1 || nibble(7) == 2) {
				const int id = nibble(7) == 1 ? ReadVariable(param(6))
					: ReadVariable(propagate_negative_reference(param(6)));
				string_value = ToString(ReadString(id));
				string_present = IsManiacStringPresent(id);
			} else {
				string_value = Utils::EncodeUTF(take_text(param(6)), true);
				string_present = !string_value.empty();
			}
		} else if (nibble(7) == 1 || nibble(7) == 2) {
			const auto id = nibble(7) == 1 ? param(6) : self_number(param(6));
			const size_t index = self_index(id);
			if (source_bank && index < source_bank->strings.size()) string_value = ToString(source_bank->strings[index]);
			string_present = Game_ManiacSelfVariables::IsStringPresent(source_bank, index);
		} else {
			string_value = Utils::EncodeUTF(take_text(param(6)), true);
			string_present = !string_value.empty();
		}
	} else if (nibble(7) == 1 || nibble(7) == 2) {
		if (source->scope == 0) number = ReadVariable(nibble(7) == 1 ? param(6) : propagate_negative_reference(param(6)));
		else {
			const auto id = nibble(7) == 1 ? param(6) : self_number(param(6));
			const size_t index = self_index(id);
			if (kind == 2) number = source_bank && index < source_bank->switches.size() ? source_bank->switches[index] : 0;
			else number = self_number(id);
		}
	}
	const size_t start = self_index(target_id);
	const uint64_t required = static_cast<uint64_t>(start) + static_cast<uint32_t>(count) + 1;
	if (required > UINT32_MAX) Output::Error("Selfvar: Vector length exceeds uint32 range.");
	if (target->scope != 0) {
		if (auto* bank = find_bank(*target)) {
			if (kind == 1 && bank->strings.size() < required) bank->strings.resize(required);
			else if (kind == 2 && bank->switches.size() < required) bank->switches.resize(required);
			else if (kind != 1 && kind != 2 && bank->numbers.size() < required) bank->numbers.resize(required);
		}
	} else {
		// Prepare the native extra slot even for zero count. Global ordinary
		// vectors exclude the physical slot zero, unlike self vectors.
		if (kind == 1) {
			Main_Data::game_strings->EnsureSize(static_cast<size_t>(required - 1));
		} else if (kind == 2) {
			Main_Data::game_switches->EnsureSize(required - 1);
		} else {
			Main_Data::game_variables->EnsureSize(required - 1);
		}
	}
	for (int32_t offset = 0; offset < count; ++offset) {
		const size_t slot = start + offset;
		if (target->scope == 0) {
			const int64_t id = static_cast<int64_t>(start) + offset;
			if (kind == 1) Main_Data::game_strings->SetManiacRawSlot(static_cast<int>(id), string_value, string_present);
			else if (kind == 2) Main_Data::game_switches->SetManiacRaw(static_cast<int>(id), static_cast<uint8_t>(number));
			else if (auto* destination = Main_Data::game_variables->GetManiacWritable(static_cast<int>(id))) *destination = number;
		} else if (auto* bank = find_bank(*target)) {
			if (kind == 1) {
				if (bank->strings.size() <= slot) bank->strings.resize(slot + 1);
				bank->strings[slot] = lcf::DBString(string_value);
				Game_ManiacSelfVariables::SetStringPresence(bank, slot, string_present);
			} else if (kind == 2) {
				if (bank->switches.size() <= slot) bank->switches.resize(slot + 1);
				bank->switches[slot] = static_cast<uint8_t>(number);
			} else {
				if (bank->numbers.size() <= slot) bank->numbers.resize(slot + 1);
				bank->numbers[slot] = number;
			}
		}
	}
	return true;
} catch (const std::bad_alloc&) {
	Output::Error("Selfvar: bad allocation.");
	return false;
} catch (const std::length_error&) {
	Output::Error("Selfvar: bad allocation.");
	return false;
}

bool Game_Interpreter::CommandManiacCallCommonEvent(lcf::rpg::EventCommand const& com) { // code 1005
	if (_state.stack.size() > 8192) {
		Output::Error("Call Event limit (8192) has been exceeded");
	}
	const int operand = com.parameters.empty() ? 0 : com.parameters[0];
	const int mode = com.parameters.size() > 1 ? com.parameters[1] & 0xf : 0;
	const int event_id = mode == 0 ? operand : mode == 1
		? ReadVariable(operand) : ReadVariableIndirect(operand);

	// Unlike 12330, this call creates a fresh frame without arguments or a
	// return destination. The original appends it before validating the ID.
	lcf::rpg::SaveEventExecFrame frame;
	frame.ID = _state.stack.size() + 1;
	frame.maniac_event_id = event_id;
	frame.maniac_event_info = 0x25;
	frame.easyrpg_self_scope = 1;
	frame.easyrpg_self_event_id = event_id;
	_state.stack.push_back(std::move(frame));
	Game_CommonEvent* common_event = lcf::ReaderUtil::GetElement(Game_Map::GetCommonEvents(), event_id);
	if (!common_event) {
		Output::Error("CallCommonEvent: Can't call invalid common event {}", event_id);
	}
	_state.stack.back().commands = common_event->GetList();
	return true;
}

bool Game_Interpreter::CommandCallEvent(lcf::rpg::EventCommand const& com) { // code 12330
	const bool maniac = Player::IsPatchManiac();
	// The native check precedes frame allocation, target lookup and arguments.
	if (maniac && _state.stack.size() > 8192) {
		Output::Error("Call Event limit (8192) has been exceeded");
	}
	int evt_id;
	int event_page;
	const auto param = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const auto text_units = Utils::DecodeUTF16(com.string, Player::IsPatchManiac());
	size_t text_cursor = 0;
	const auto take_text = [&](int32_t budget) -> std::u16string {
		const size_t begin = text_cursor;
		uint32_t remaining = static_cast<uint32_t>(budget);
		while (text_cursor < text_units.size()) {
			const char16_t unit = text_units[text_cursor];
			const int cost = unit <= 0x00ff || (unit >= 0xff61 && unit <= 0xff9f) ? 1 : 2;
			// The native wide-unit charge precedes the signed comparison. Preserve
			// its INT_MIN wraparound without relying on signed C++ overflow.
			if (cost == 2) --remaining;
			int32_t signed_remaining;
			std::memcpy(&signed_remaining, &remaining, sizeof(signed_remaining));
			if (int64_t(signed_remaining) <= int64_t(text_cursor - begin)) break;
			++text_cursor;
		}
		return text_units.substr(begin, text_cursor - begin);
	};
	const auto assign_arguments = [&](int scope, int map_id, int target_id) {
		if (!Player::IsPatchManiac() || !Main_Data::game_maniac_self_variables) return;
		const int32_t count = param(6);
		if (count <= 0) return;
		if (count > (INT32_MAX - 7) / 2) {
			Output::Error("CallEvent: Argument count {} overflows the native argument index", count);
		}
		if (!Main_Data::game_maniac_self_variables->Ensure(scope, map_id, target_id)) return;
		int number_slot = 0, string_slot = 0, switch_slot = 0;
		for (int64_t index = 0; index < count; ++index) {
			const int64_t type_index = 7 + index * 2;
			const int type = type_index < static_cast<int64_t>(com.parameters.size()) ? param(static_cast<size_t>(type_index)) : 0;
			const int32_t raw = type_index + 1 < static_cast<int64_t>(com.parameters.size()) ? param(static_cast<size_t>(type_index + 1)) : 0;
			int32_t numeric_value = raw;
			std::string string_value;
			bool string_present = false;
			if (type == 2) numeric_value = ReadVariable(raw);
			else if (type == 3) numeric_value = ReadVariableIndirect(raw);
			else if (type == 4) {
				string_value = Utils::EncodeUTF(take_text(raw), true);
				string_present = !string_value.empty();
			} else if (type == 5 || type == 6) {
				const int id = type == 5 ? raw : ResolveVariableIndirectAddress(raw);
				string_value = ToString(ReadString(id));
				string_present = IsManiacStringPresent(id);
			}
			else if (type == 8) numeric_value = ReadSwitch(raw);
			else if (type == 9) numeric_value = ReadSwitch(ResolveVariableIndirectAddress(raw));
			// Source reads precede this write. A recursive call may alias the same bank.
			auto* bank = Main_Data::game_maniac_self_variables->Ensure(scope, map_id, target_id);
			if (!bank) return;
			if (type >= 4 && type <= 6) {
				++string_slot;
				bank->strings.resize(std::max(bank->strings.size(), static_cast<size_t>(string_slot + 1)));
				bank->strings[string_slot] = lcf::DBString(string_value);
				Game_ManiacSelfVariables::SetStringPresence(bank, string_slot, string_present);
			} else if (type >= 7 && type <= 9) {
				++switch_slot;
				bank->switches.resize(std::max(bank->switches.size(), static_cast<size_t>(switch_slot + 1)));
				bank->switches[switch_slot] = static_cast<uint8_t>(numeric_value);
			} else {
				++number_slot;
				bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(number_slot + 1)));
				bank->numbers[number_slot] = numeric_value;
			}
		}
	};
	int32_t offset = 0;
	if (Player::IsPatchManiac()) {
		const int offset_mode = static_cast<uint32_t>(param(3)) & 15;
		offset = offset_mode == 0 ? param(4) : offset_mode == 1
			? ReadVariable(param(4)) : ReadVariableIndirect(param(4));
	}
	const auto add_offset = [&](int32_t id) -> int32_t {
		const uint32_t bits = static_cast<uint32_t>(id) + static_cast<uint32_t>(offset);
		int32_t result;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	};
	const auto set_return_destination = [&]() {
		if (!Player::IsPatchManiac() || com.parameters.size() < 6 || _state.stack.empty()) return;
		auto& frame = _state.stack.back();
		frame.easyrpg_maniac_return_type = (static_cast<uint32_t>(param(3)) >> 4) & 15;
		frame.easyrpg_maniac_return_address = param(5);
	};

	const int selector = Player::IsPatchManiac() && (param(0) < 0 || param(0) > 7)
		? 0 : param(0);
	switch (selector) {
	case 0:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7: { // Common Event
		if (selector == 0) {
			evt_id = param(1);
		} else if (selector == 3 && Player::IsPatchManiac()) {
			evt_id = ReadVariable(param(1));
		} else if (selector == 4 && Player::IsPatchManiac()) {
			evt_id = ReadVariableIndirect(param(1));
		} else if (selector >= 5 && selector <= 7 && Player::IsPatchManiac()) {
			std::u16string name;
			if (selector == 5) {
				name = take_text(param(1));
			} else {
				const int string_id = selector == 6 ? param(1) : ResolveVariableIndirectAddress(param(1));
				const auto value = ReadString(string_id);
				name = Utils::DecodeUTF16(value.substr(0, value.find('\0')), true);
			}
			evt_id = selector == 5 ? com.easyrpg_maniac_common_name_cache : 0;
			if (evt_id == 0) {
				for (const auto& candidate : lcf::Data::commonevents) {
					if (Utils::DecodeUTF16(static_cast<std::string_view>(candidate.name), true) == name) {
						evt_id = candidate.ID;
						break;
					}
				}
				if (evt_id == 0) {
					Output::Error("CallEvent: Can't find common event named '{}'", Utils::EncodeUTF(name, true));
				}
				if (selector == 5 && evt_id != 0) com.easyrpg_maniac_common_name_cache = evt_id;
			}
		} else {
			return true;
		}
		evt_id = add_offset(evt_id);

		Game_CommonEvent* common_event = lcf::ReaderUtil::GetElement(Game_Map::GetCommonEvents(), evt_id);
		if (!common_event) {
			if (maniac) Output::Error("CallEvent: Can't call invalid common event {}", evt_id);
			Output::Warning("CallEvent: Can't call invalid common event {}", evt_id);
			return true;
		}

		assign_arguments(1, 0, evt_id);
		const size_t old_stack_size = _state.stack.size();
		Push(common_event);
		if (_state.stack.size() > old_stack_size) {
			if (Player::IsPatchManiac()) {
				auto& frame = _state.stack.back();
				frame.maniac_event_id = evt_id;
				frame.maniac_event_info = 0x25;
			}
			set_return_destination();
		}

		return true;
	}
	case 1: // Map Event
		evt_id = param(1);
		if (maniac && evt_id == Game_Character::CharThisEvent) {
			evt_id = 0;
			for (auto it = _state.stack.rbegin(); it != _state.stack.rend(); ++it) {
				if (it->event_id > 0) { evt_id = it->event_id; break; }
			}
		}
		event_page = param(2);
		break;
	case 2: // Indirect
		evt_id = ReadVariable(param(1));
		event_page = ReadVariable(param(2));
		break;
	default:
		return true;
	}
	evt_id = add_offset(evt_id);

	if (maniac) {
		const auto& definitions = Game_Map::GetMap().events;
		const auto find_definition = [&](int id) -> const lcf::rpg::Event* {
			auto it = std::find_if(definitions.begin(), definitions.end(),
				[id](const auto& definition) { return definition.ID == id; });
			return it == definitions.end() ? nullptr : &*it;
		};
		const auto* definition = find_definition(evt_id);
		if (!definition) {
			if (auto* live = Game_Map::GetEvent(evt_id)) {
				const int source_id = live->data()->easyrpg_clone_event_id;
				definition = find_definition(source_id > 0 ? source_id : live->GetId());
			}
		}
		if (!definition) Output::Error("CallEvent: Can't call non-existent event {}", evt_id);
		auto page = std::find_if(definition->pages.begin(), definition->pages.end(),
			[event_page](const auto& candidate) { return candidate.ID == event_page; });
		if (page == definition->pages.end()) {
			Output::Error("CallEvent: Can't call non-existent page {} of event {}", event_page, evt_id);
		}
		assign_arguments(2, Main_Data::game_player->GetMapId(), definition->ID);
		Push(page->event_commands, evt_id, false, event_page);
		auto& frame = _state.stack.back();
		frame.maniac_event_info = 0x15;
		frame.maniac_event_id = definition->ID;
		frame.easyrpg_self_event_id = definition->ID;
		set_return_destination();
		return true;
	}

	Game_Event* event = static_cast<Game_Event*>(GetCharacter(evt_id, "CallEvent"));
	if (!event) {
		Output::Warning("CallEvent: Can't call non-existent event {}", evt_id);
		return true;
	}

	const lcf::rpg::EventPage* page = event->GetPage(event_page);
	if (!page) {
		Output::Warning("CallEvent: Can't call non-existent page {} of event {}", event_page, evt_id);
		return true;
	}

	Push(event, page, false);

	return true;
}

bool Game_Interpreter::CommandReturnToTitleScreen(lcf::rpg::EventCommand const& /* com */) { // code 12510
	if (Game_Message::IsMessageActive()) {
		return false;
	}

	_async_op = AsyncOp::MakeToTitle();
	return true;
}

bool Game_Interpreter::CommandChangeClass(lcf::rpg::EventCommand const& com) { // code 1008
	if (!Player::IsRPG2k3Commands()) {
		return true;
	}

	int class_id = com.parameters[2]; // 0: No class, 1+: Specific class
	bool level1 = com.parameters[3] > 0;
	int skill_mode = com.parameters[4]; // no change, replace, add
	int param_mode = com.parameters[5]; // no change, halve, level 1, current level
	bool show_msg = com.parameters[6] > 0;

	if (show_msg && !Game_Message::CanShowMessage(true)) {
		return false;
	}

	PendingMessage pm(Game_Message::MakeCommandCodeInserter(*this));
	pm.SetEnableFace(false);
	pm.SetFromForegroundInterpreter(main_flag);

	const lcf::rpg::Class* cls = lcf::ReaderUtil::GetElement(lcf::Data::classes, class_id);
	if (!cls && class_id != 0) {
		Output::Warning("ChangeClass: Can't change class. Class {} is invalid", class_id);
		return true;
	}

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		int level = level1 ? 1 : actor->GetLevel();
		actor->ChangeClass(class_id, level,
				static_cast<Game_Actor::ClassChangeSkillMode>(skill_mode),
				static_cast<Game_Actor::ClassChangeParamMode>(param_mode),
				show_msg ? &pm : nullptr
				);
	}

	if (CheckGameOver()) {
		return true;
	}

	if (show_msg && pm.IsActive()) {
		ForegroundTextPush(std::move(pm));
	}

	return true;
}

bool Game_Interpreter::CommandChangeBattleCommands(lcf::rpg::EventCommand const& com) { // code 1009
	if (!Player::IsRPG2k3Commands()) {
		return true;
	}

	int cmd_id = com.parameters[2];
	bool add = com.parameters[3] != 0;

	for (const auto& actor : GetActors(com.parameters[0], com.parameters[1])) {
		actor->ChangeBattleCommands(add, cmd_id);
	}

	return true;
}

bool Game_Interpreter::CommandExitGame(lcf::rpg::EventCommand const& /* com */) {
	if (!Player::IsRPG2k3ECommands()) {
		return true;
	}

	if (Game_Message::IsMessageActive()) {
		return false;
	}

	_async_op = AsyncOp::MakeExitGame();
	return true;
}

bool Game_Interpreter::CommandToggleFullscreen(lcf::rpg::EventCommand const& /* com */) {
	if (!Player::IsRPG2k3ECommands()) {
		return true;
	}

	auto cfg = DisplayUi->GetConfig();
	if (!cfg.fullscreen.IsOptionVisible() || cfg.fullscreen.IsLocked()) {
		Output::Debug("ToggleFullscreen: Not supported on this platform");
		return true;
	}

	DisplayUi->ToggleFullscreen();
	return true;
}

bool Game_Interpreter::CommandOpenVideoOptions(lcf::rpg::EventCommand const& /* com */) {
	if (!Player::IsRPG2k3ECommands()) {
		return true;
	}

	auto& frame = GetFrame();
	auto& index = frame.current_command;

	if (Game_Message::IsMessageActive()) {
		return false;
	}

	// Don't interrupt other pending game scenes for the settings menu.
	if (Scene::instance->HasRequestedScene()) {
		return false;
	}

	// FIXME: RPG_RT pops up an immediate modal dialog, while this
	// does a normal scene call. This could result in interpreter timing differences.
	Scene::instance->SetRequestedScene(std::make_shared<Scene_Settings>());
	++index;
	return false;
}

bool Game_Interpreter::CommandManiacGetGameInfo(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}
	const uint32_t modes = com.parameters[0];
	const bool dynamic_graphic = (com.parameters[6] & 1) != 0;
	const auto number = [&](int field, int shift) {
		const int mode = (modes >> (shift * 4)) & 15;
		return mode == 0 ? com.parameters[field] : mode == 1
			? ReadVariable(com.parameters[field]) : ReadVariableIndirect(com.parameters[field]);
	};
	const auto write_numbers = [&](int32_t destination, const auto& values) {
		const size_t first = static_cast<size_t>(std::abs(static_cast<int64_t>(destination)));
		if (destination < 0) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) Output::Error("GetGameInfo: Self destination requires current self context.");
			bank->numbers.resize(std::max(bank->numbers.size(), first + values.size() + 1));
			std::copy(values.begin(), values.end(), bank->numbers.begin() + first);
		} else {
			Main_Data::game_variables->EnsureSize(first + values.size());
			if (destination == 0) {
				*Main_Data::game_variables->GetManiacWritable(0) = values[0];
				std::copy(values.begin() + 1, values.end(), Main_Data::game_variables->GetManiacWritable(1));
			} else {
				std::copy(values.begin(), values.end(), Main_Data::game_variables->GetManiacWritable(destination));
			}
		}
	};

	const auto write_scalar = [&](int32_t destination, int32_t value) {
		if (destination != 0) write_numbers(destination, std::array<int32_t, 1>{value});
	};
	const auto write_string = [&](int32_t destination, std::string_view value) {
		if (destination > 0) {
			Main_Data::game_strings->SetManiacRawSlot(destination, value, !value.empty());
		} else if (destination < 0) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) Output::Error("GetGameInfo: Self destination requires current self context.");
			const size_t slot = static_cast<size_t>(-static_cast<int64_t>(destination));
			bank->strings.resize(std::max(bank->strings.size(), slot + 1));
			bank->strings[slot] = lcf::DBString(value);
			Game_ManiacSelfVariables::SetStringPresence(bank, slot, !value.empty());
		}
	};

	int event_id;
	int var = com.parameters[2];
	const auto write_pair = [&](int32_t first, int32_t second) {
		write_numbers(var, std::array<int32_t, 2>{first, second});
	};

	switch (com.parameters[1]) {
		case 0: // Get map size
			write_pair(Game_Map::GetTilesX(), Game_Map::GetTilesY());
			break;
		case 1: { // Get tile info
			const int x = number(3, 0), y = number(4, 1);
			const int width = number(5, 2), height = number(6, 3);
			const int64_t count = static_cast<int64_t>(width) * height;
			const int map_width = Game_Map::GetTilesX(), map_height = Game_Map::GetTilesY();
			if (width <= 0 || height <= 0 || count > INT32_MAX || map_width <= 0 || map_height <= 0) return true;
			const auto& layer = com.parameters[2] == 0 ? Game_Map::GetMapDataDown() : Game_Map::GetMapDataUp();
			std::vector<int32_t> values(static_cast<size_t>(count), 0);
			for (int row = 0; row < height; ++row) {
				int64_t sy = static_cast<int64_t>(y) + row;
				if (Game_Map::LoopVertical()) sy = (sy % map_height + map_height) % map_height;
				for (int column = 0; column < width; ++column) {
					int64_t sx = static_cast<int64_t>(x) + column;
					if (Game_Map::LoopHorizontal()) sx = (sx % map_width + map_width) % map_width;
					if (sx < 0 || sx >= map_width || sy < 0 || sy >= map_height) continue;
					const int tile = static_cast<uint16_t>(layer[static_cast<size_t>(sy * map_width + sx)]);
					const int index = tile < 3000 ? tile / 1000 : tile < 4000 ? 3 + (tile - 3000) / 50
						: tile < 5000 ? 6 + (tile - 4000) / 50 : tile < 10000 ? tile - 4982 : std::min(tile - 10000, 143);
					values[static_cast<size_t>(row) * width + column] = index;
				}
			}
			write_numbers(com.parameters[7], values);
			break;
		}
		case 2: // Get window size
			write_pair(Player::screen_width, Player::screen_height);
			break;
		case 3: { // Get pixel info

			const int x = number(3, 0), y = number(4, 1);
			const int width = number(5, 2), height = number(6, 3);
			const int32_t destination = number(7, 4);
			const int64_t count = static_cast<int64_t>(width) * height;
			if (width <= 0 || height <= 0 || count > INT32_MAX
				|| static_cast<int64_t>(x) + width <= 0 || static_cast<int64_t>(y) + height <= 0) return true;
			const auto bitmap = DisplayUi ? DisplayUi->GetDisplaySurface() : BitmapRef{};
			if (!bitmap) return true;
			std::vector<int32_t> values(static_cast<size_t>(count), 0);
			const int left = std::max(x, 0), top = std::max(y, 0);
			const int64_t right = std::min(static_cast<int64_t>(x) + width, static_cast<int64_t>(bitmap->GetWidth()));
			const int64_t bottom = std::min(static_cast<int64_t>(y) + height, static_cast<int64_t>(bitmap->GetHeight()));
			for (int64_t sy = top; sy < bottom; ++sy) {
				for (int64_t sx = left; sx < right; ++sx) {
					const auto color = bitmap->GetManiacColorAt(static_cast<int>(sx), static_cast<int>(sy));
					uint32_t pixel = (uint32_t(color.red) << 16) | (uint32_t(color.green) << 8) | color.blue;
					if (com.parameters[2] == 0) pixel |= 0xff000000;
					values[static_cast<size_t>((sy - y) * width + sx - x)] = static_cast<int32_t>(pixel);
				}
			}
			write_numbers(destination, values);
			break;
		}
		case 4: { // Get command interpreter state

			const int32_t destination = number(2, 0);
			std::array<int32_t, 5> values{};
			if (com.parameters[3] == 0) {
				const int32_t nest = number(4, 2);
				const uint32_t count = static_cast<uint32_t>(_state.stack.size());
				const uint32_t index = nest < 0 ? count + static_cast<uint32_t>(nest) : static_cast<uint32_t>(nest);
				if (index < count) {
					const auto& frame = _state.stack[count - 1 - index];
					const uint8_t info = static_cast<uint8_t>(frame.maniac_event_info);
					values = {info >> 4, frame.maniac_event_id,
						(info & 0x10) ? frame.maniac_event_page_id : 0,
						info & 15, frame.current_command};
				}
			}
			write_numbers(destination, values);
			break;
		}
		case 5: // Get tileset ID
			write_scalar(var, Game_Map::GetChipset());
			break;
		case 6: // Get actor/message face graphic
			event_id = number(5, 0);
			if (com.parameters[4] != 0 && com.parameters[4] != 1) {
				write_string(var, "");
				write_scalar(com.parameters[3], 0);
				break;
			}
			if (com.parameters[4] == 1) {
				// Message
				write_string(var, Main_Data::game_system->GetMessageFaceName());
				write_scalar(com.parameters[3], Main_Data::game_system->GetMessageFaceIndex());
			} else {
				// Actor

				auto* actor = Main_Data::game_actors->GetActor(event_id);
				if (!actor) {
					Output::Error("GetGameInfo: Actor {} was not found.", event_id);
					Output::Warning("GetGameInfo: Invalid actor ID {}", event_id);
					return true;
				}

				if (dynamic_graphic) {
					// Dynamic
					write_string(var, actor->GetFaceName());
					write_scalar(com.parameters[3], actor->GetFaceIndex());
				} else {
					// Original
					write_string(var, actor->GetOriginalFaceName());
					write_scalar(com.parameters[3], actor->GetOriginalFaceIndex());
				}
			}
			break;
		case 7: { // Get actor/event body graphic
			event_id = number(5, 0);
			if (com.parameters[4] != 0 && com.parameters[4] != 1) {
				write_string(var, "");
				write_scalar(com.parameters[3], 0);
				break;
			}
			auto* character = com.parameters[4] == 0 ? nullptr : GetCharacter(event_id, "GetGameInfo");
			if (!character && (com.parameters[4] == 1)) {
				Output::Error("GetGameInfo: Map Event {} was not found.", event_id);
				return true;
			}

			if (com.parameters[4] == 1) {
				// Get event graphic
				// Bug: .static 10001 gives current sprite of Player. .dynamic 10001 gives out nothing.
				// Bug: Cannot get .static 10005 sprite of self. .dynamic 10005 works however
				if (dynamic_graphic) {
					// Dynamic
					if (event_id == Game_Character::CharPlayer) {
							// Return nothing as per Maniac Patch
							write_string(var, "");
							write_scalar(com.parameters[3], 0);
							break;
					}
					write_string(var, std::string_view(character->GetSpriteName()));
					write_scalar(com.parameters[3], character->GetSpriteIndex());
					break;
				} else {
					// Static
					switch (event_id) {
						case Game_Character::CharPlayer:
							// Return dynamic player sprite
							write_string(var, std::string_view(character->GetSpriteName()));
							write_scalar(com.parameters[3], character->GetSpriteIndex());
							break;
						case Game_Character::CharBoat:
							write_string(var, std::string_view(lcf::Data::system.boat_name));
							write_scalar(com.parameters[3], lcf::Data::system.boat_index);
							break;
						case Game_Character::CharShip:
							write_string(var, std::string_view(lcf::Data::system.ship_name));
							write_scalar(com.parameters[3], lcf::Data::system.ship_index);
							break;
						case Game_Character::CharAirship:
							write_string(var, std::string_view(lcf::Data::system.airship_name));
							write_scalar(com.parameters[3], lcf::Data::system.airship_index);
							break;
						default: {
							auto* event = static_cast<Game_Event*>(character);
							auto* page = event->GetActivePage();
							if (page == nullptr) {
								// return nothing
								write_string(var, "");
								write_scalar(com.parameters[3], 0);
							} else {
								write_string(var, std::string_view(page->character_name));
								write_scalar(com.parameters[3], page->character_index);
							}
						}
					}
				}
			} else {
				// Get actor graphic
				auto* actor = Main_Data::game_actors->GetActor(event_id);
				if (!actor) {
					Output::Error("GetGameInfo: Actor {} was not found.", event_id);
					Output::Warning("GetGameInfo: Invalid actor ID {}", event_id);
					return true;
				}

				if (dynamic_graphic) {
					// Dynamic
					write_string(var, actor->GetSpriteName());
					write_scalar(com.parameters[3], actor->GetSpriteIndex());
				} else {
					// Default one
					write_string(var, actor->GetOriginalSpriteName());
					write_scalar(com.parameters[3], actor->GetOriginalSpriteIndex());
				}
			}
			break;
		}
		case 8:
			// Screen position
			write_pair(Game_Map::GetPositionX() / 16, Game_Map::GetPositionY() / 16);
			break;
		case 9:
			// Screen shake
			write_pair(Main_Data::game_screen->GetShakeOffsetX(), Main_Data::game_screen->GetShakeOffsetY());
			break;
		case 10: {
			// Current BGM
			const auto& bgm = Main_Data::game_system->GetCurrentBGM();
			write_string(var, bgm.name);
			if (com.parameters[3] != 0) {
				write_numbers(com.parameters[3], std::array<int32_t, 4>{bgm.fadein, bgm.volume, bgm.tempo, bgm.balance});
			}
			break;
		}
	}

	Game_Map::SetNeedRefresh(true);

	return true;
}

bool Game_Interpreter::CommandManiacGetSaveInfo(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}
	auto refresh_guard = lcf::makeScopeGuard([]() {
		Game_Map::RefreshAfterManiacChange();
	});

	const int mode = static_cast<uint32_t>(com.parameters[0]) & 15;
	const int save_number = mode == 0 ? com.parameters[1] : mode == 1
		? ReadVariable(com.parameters[1]) : ReadVariableIndirect(com.parameters[1]);
	const auto self_bank = [&]() {
		const auto* frame = GetFramePtr();
		auto* state = Main_Data::game_maniac_self_variables.get();
		auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
			frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
		if (!bank) Output::Error("GetSaveInfo: Self destination requires current self context.");
		return bank;
	};
	const auto write_number = [&](int id, int32_t value) {
		if (id == 0) return;
		if (id > 0) {
			Main_Data::game_variables->Set(id, value);
		} else {
			auto* bank = self_bank();
			const size_t slot = static_cast<size_t>(-static_cast<int64_t>(id));
			bank->numbers.resize(std::max(bank->numbers.size(), slot + 2));
			bank->numbers[slot] = value;
			Game_Map::SetNeedRefresh(true);
		}
	};

	// Error case, set later on success
	write_number(com.parameters[2], 0);
	write_number(com.parameters[3], 0);
	write_number(com.parameters[4], 0);
	write_number(com.parameters[5], 0);

	if (save_number <= 0) {
		Output::Debug("ManiacGetSaveInfo: Invalid save number {}", save_number);
		return true;
	}

	auto savefs = FileFinder::Save();
	std::string save_name = Scene_Save::GetSaveFilename(savefs, save_number);
	auto save_stream = FileFinder::Save().OpenInputStream(save_name);

	if (!save_stream) {
		Output::Debug("ManiacGetSaveInfo: Save not found {}", save_number);
		return true;
	}

	auto save = lcf::LSD_Reader::Load(save_stream, Player::encoding, true);
	if (!save) {
		Output::Debug("ManiacGetSaveInfo: Save corrupted {}", save_number);
		// Maniac Patch writes this for whatever reason
		write_number(com.parameters[2], 8991230);
		return true;
	}

	std::time_t t = lcf::LSD_Reader::ToUnixTimestamp(save->title.timestamp);
	std::tm* tm = std::gmtime(&t);

	const int date = ((tm->tm_year + 1900) % 1000 * 100 + tm->tm_mon + 1) * 100 + tm->tm_mday;
	write_number(com.parameters[2], date);
	write_number(com.parameters[3], atoi(Utils::FormatDate(tm, Utils::DateFormat_HHMMSS).c_str()));
	write_number(com.parameters[4], save->title.hero_level);
	write_number(com.parameters[5], save->title.hero_hp);
	if ((com.parameters[0] & 0xf0) != 0) {
		const int id = com.parameters[6];
		const auto& name = save->title.hero_name;
		if (id > 0) Main_Data::game_strings->SetManiacRawSlot(id, name, !name.empty());
		else if (id < 0) {
			auto* bank = self_bank();
			const size_t slot = static_cast<size_t>(-static_cast<int64_t>(id));
			bank->strings.resize(std::max(bank->strings.size(), slot + 1));
			bank->strings[slot] = lcf::DBString(name);
			Game_ManiacSelfVariables::SetStringPresence(bank, slot, !name.empty());
		}
	}
	Game_Map::SetNeedRefresh(true);

	auto face_ids = Utils::MakeArray(save->title.face1_id, save->title.face2_id, save->title.face3_id, save->title.face4_id);
	auto face_names = Utils::MakeArray(save->title.face1_name, save->title.face2_name, save->title.face3_name, save->title.face4_name);

	for (int i = 0; i <= 3; ++i) {
		const int param = 8 + i;

		int pic_id = ValueOrVariable(com.parameters[7], com.parameters[param]);
		if (pic_id <= 0) {
			continue;
		}

		if (face_names[i].empty()) {
			Main_Data::game_pictures->Erase(pic_id);
			continue;
		}

		// An existing picture is overwritten
		// Default data is used with the exceptions listed below
		Game_Pictures::ShowParams params;
		params.use_transparent_color = true;
		params.top_trans = 100; // Full transparent by default
		params.map_layer = 7;
		params.battle_layer = 7;

		// Spritesheet configured to match the FaceSet layout
		params.name = FileFinder::MakePath("..\\FaceSet", face_names[i]);
		params.spritesheet_cols = 4;
		params.spritesheet_rows = 4;
		params.spritesheet_frame = face_ids[i];
		params.spritesheet_speed = 0;
		Main_Data::game_pictures->Show(pic_id, params);
	}

	return true;
}

bool Game_Interpreter::CommandManiacSave(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	// 250921 treats every nonzero A0 as the same indirect numeric lookup.
	const int slot = com.parameters[0] == 0 ? com.parameters[1] : ReadVariable(com.parameters[1]);
	const bool result_enabled = com.parameters[2] != 0;
	const int out_var = result_enabled ? com.parameters[3] : 0;
	const auto* frame = GetFramePtr();
	const auto operation = AsyncOp::MakeSave(slot, out_var,
		frame ? frame->easyrpg_self_scope : 0,
		frame ? frame->easyrpg_self_map_id : 0,
		frame ? frame->easyrpg_self_event_id : 0, result_enabled);
	if (slot <= 0) {
		Output::Debug("ManiacSave: Invalid save slot {}", slot);
		// The original still writes result 0 after skipping the save.
		if (result_enabled) {
			if (out_var > 0) Main_Data::game_variables->Set(out_var, 0);
			else ApplyManiacSaveResult(operation, false, Main_Data::game_maniac_self_variables.get());
			Game_Map::RefreshAfterManiacChange();
		}
		return true;
	}

	Output::BeginManiacFileOutput();
	if ((maniac_message_hook || Game_Battle::IsManiacHookPumping())) {
		// Hook dispatch has no ordinary scene update between this command and
		// its suffix. A subsequent Update would clear an unhandled async save.
		// RPG_RT saves here, writes the result, then dispatches the next command.
		const bool success = Scene_Save::Save(FileFinder::Save(), slot, true, slot <= 15);
		if (result_enabled) {
			if (out_var > 0) Main_Data::game_variables->Set(out_var, success ? 1 : 0);
			else ApplyManiacSaveResult(operation, success, Main_Data::game_maniac_self_variables.get());
			Game_Map::RefreshAfterManiacChange();
		}
		return true;
	}

	// Ordinary events yield to the update loop before saving their live state.
	_async_op = operation;

	return true;
}

bool Game_Interpreter::CommandManiacLoad(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	// The 250921 load dispatcher uses the same single numeric lookup as Save:
	// every nonzero A0 reads A1 once, including negative self addresses.
	const int slot = com.parameters[0] == 0 ? com.parameters[1] : ReadVariable(com.parameters[1]);
	if (slot <= 0) {
		Output::Debug("ManiacLoad: Invalid save slot {}", slot);
		return true;
	}

	// Not implemented (kinda useless feature):
	// When com.parameters[2] is 1 the check whether the file exists is skipped
	// When skipped and missing RPG_RT will crash
	auto savefs = FileFinder::Save();
	std::string save_name = Scene_Save::GetSaveFilename(savefs, slot);
	auto save_stream = FileFinder::Save().OpenInputStream(save_name);
	std::unique_ptr<lcf::rpg::Save> save = lcf::LSD_Reader::Load(save_stream, Player::encoding, true);

	if (!save) {
		Output::Debug("ManiacLoad: Save not found {}", slot);
		return true;
	}

	// 250921 A3 skips the reset clear/F12-release/20-tick presentation only.
	const int presentation = com.parameters.size() > 3 && com.parameters[3] != 0 ? 2 : 1;
	_async_op = AsyncOp::MakeLoad(slot, presentation);

	return true;
}

bool Game_Interpreter::CommandManiacEndLoadProcess(lcf::rpg::EventCommand const&) {
	if (Player::IsPatchManiac()) {
		Main_Data::game_system->EndManiacLoadProcess();
	}
	// no-op
	return true;
}

bool Game_Interpreter::CommandManiacGetMousePosition(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

#if !defined(USE_MOUSE_OR_TOUCH) || !defined(SUPPORT_MOUSE_OR_TOUCH)
	static bool warned = false;
	if (!warned) {
		// This command is polled, prevent excessive spam
		Output::Warning("Maniac Patch: Mouse input is not supported on this platform");
		warned = true;
	}
	return true;
#endif

	Point mouse_pos = Input::GetMousePosition();
	if (DisplayUi && DisplayUi->GetMouseClientPosition(mouse_pos)) {
		const bool raw = com.parameters.size() >= 4 && (com.parameters[3] & 1) != 0;
		if (!raw) {
			Point size = {};
			DisplayUi->GetMouseClientSize(size);
			mouse_pos.x = Utils::RoundTo<int>(mouse_pos.x * (size.x > 0 ? 320.0 / size.x : 1.0));
			mouse_pos.y = Utils::RoundTo<int>(mouse_pos.y * (size.y > 0 ? 240.0 / size.y : 1.0));
		}
	}
	const auto write_number = [&](int32_t id, int32_t value) {
		if (id == 0) return;
		if (id > 0) {
			Main_Data::game_variables->Set(id, value);
		} else {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) Output::Error("GetMousePosition: Self destination requires current self context.");
			const size_t slot = static_cast<size_t>(-static_cast<int64_t>(id));
			bank->numbers.resize(std::max(bank->numbers.size(), slot + 2));
			bank->numbers[slot] = value;
		}
	};
	write_number(com.parameters[0], mouse_pos.x);
	write_number(com.parameters[1], mouse_pos.y);

	Game_Map::RefreshAfterManiacChange();

	return true;
}

bool Game_Interpreter::CommandManiacSetMousePosition(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	const auto arg = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const bool literal = arg(0) == 0;
	const auto read_coordinate = [&](int32_t operand) -> int32_t {
		if (literal) return operand;
		// Every nonzero A0 selects one global or current-self lookup.
		return ReadVariable(operand);
	};
	const int32_t x = read_coordinate(arg(1));
	const int32_t y = read_coordinate(arg(2));

	Point client_size = {};
	if (!DisplayUi || !DisplayUi->GetMouseClientSize(client_size)) {
		static bool warned = false;
		if (!warned) {
			Output::Warning("Maniac SetMousePosition: System cursor positioning is unsupported on this platform");
			warned = true;
		}
		return true;
	}
	Point point = {};
	if (com.parameters.size() >= 4 && (arg(3) & 1) != 0) {
		const int scale = std::clamp(DisplayUi->GetManiacWindowZoom(), 1, 4);
		point.x = std::clamp(x, 0, scale * 320 - 1);
		point.y = std::clamp(y, 0, scale * 240 - 1);
	} else {
		const int clamped_x = std::clamp(x, 0, 319);
		const int clamped_y = std::clamp(y, 0, 239);
		const int width = client_size.x;
		const int height = client_size.y;
		// Original factors are 320/client_width and 240/client_height;
		// nonpositive dimensions leave the factor at one.
		const double x_factor = width > 0 ? 320.0 / width : 1.0;
		const double y_factor = height > 0 ? 240.0 / height : 1.0;
		point.x = static_cast<int>(std::nearbyint(clamped_x / x_factor));
		point.y = static_cast<int>(std::nearbyint(clamped_y / y_factor));
	}
	DisplayUi->SetMouseClientPosition(point);
	return true;
}



bool Game_Interpreter::CommandManiacGetPictureInfo(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t modes = static_cast<uint32_t>(parameter(0));
	const auto number = [&](int index) -> int32_t {
		const int mode = (modes >> ((index - 3) * 4)) & 15;
		return mode == 0 ? parameter(index) : mode == 1
			? ReadVariable(parameter(index)) : ReadVariableIndirect(parameter(index));
	};
	int pic_id = number(3);
	if ((pic_id <= 0 || pic_id > Main_Data::game_pictures->GetMaxPictureId()))
		Output::Error("GetPictureInfo: Invalid picture ID {}", pic_id);
	auto& pic = Main_Data::game_pictures->GetPicture(pic_id);

	if (pic.IsRequestPending()) {
		// Cannot do anything useful here without the dimensions
		pic.MakeRequestImportant();
		_async_op = AsyncOp::MakeYieldRepeat();
		return true;
	}

	const auto& data = pic.data;
	if (parameter(1) == 3) {
		const int x = number(4), y = number(5), width = number(6), height = number(7);
		const int32_t destination = number(8);
		const int64_t count = static_cast<int64_t>(width) * height;
		if (width <= 0 || height <= 0 || count > INT32_MAX || !pic.sprite || !pic.sprite->GetBitmap()) return true;
		auto& sprite = *pic.sprite;
		const int frame_width = sprite.GetFrameWidth(), frame_height = sprite.GetFrameHeight();
		const int left = std::max(x, 0), top = std::max(y, 0);
		const int64_t right = std::min(static_cast<int64_t>(x) + width, static_cast<int64_t>(frame_width));
		const int64_t bottom = std::min(static_cast<int64_t>(y) + height, static_cast<int64_t>(frame_height));
		// A wholly clipped rectangle does not allocate or change the destination.
		if (right <= left || bottom <= top) return true;
		const bool processed = (parameter(2) & 1) != 0;
		auto bitmap = pic.GetManiacReadBitmap(processed);
		if (!bitmap) return true;
		std::vector<int32_t> values(static_cast<size_t>(count), 0);
		for (int64_t sy = top; sy < bottom; ++sy) {
			for (int64_t sx = left; sx < right; ++sx) {
				const auto color = pic.ReadManiacPixel(*bitmap, processed, static_cast<int>(sx), static_cast<int>(sy));
				uint32_t pixel = (uint32_t(color.red) << 16) | (uint32_t(color.green) << 8) | color.blue;
				if (!(parameter(2) & 2)) pixel |= uint32_t(color.alpha) << 24;
				values[static_cast<size_t>((sy - y) * width + sx - x)] = static_cast<int32_t>(pixel);
			}
		}
		const size_t first = static_cast<size_t>(std::abs(static_cast<int64_t>(destination)));
		if (destination < 0) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) Output::Error("GetPictureInfo: Self destination requires current self context.");
			bank->numbers.resize(std::max(bank->numbers.size(), first + values.size() + 1));
			std::copy(values.begin(), values.end(), bank->numbers.begin() + first);
		} else {
			Main_Data::game_variables->EnsureSize(first + values.size());
			if (destination == 0) {
				*Main_Data::game_variables->GetManiacWritable(0) = values[0];
				std::copy(values.begin() + 1, values.end(), Main_Data::game_variables->GetManiacWritable(1));
			} else {
				std::copy(values.begin(), values.end(), Main_Data::game_variables->GetManiacWritable(destination));
			}
		}
		return true;
	}

	const int type = parameter(1);
	const bool active = pic.sprite && pic.sprite->GetBitmap();
	if ((type < 0 || type > 2) && !(type < 3 && !active)) return true;
	int x = 0;
	int y = 0;
	int width = active ? pic.sprite->GetFrameWidth() : 0;
	int height = active ? pic.sprite->GetFrameHeight() : 0;

	if (active) switch (type) {
		case 0:
			x = ManiacPicture::Nearest(data.current_x);
			y = ManiacPicture::Nearest(data.current_y);
			break;
		case 1:
			x = ManiacPicture::Nearest(data.current_x);
			y = ManiacPicture::Nearest(data.current_y);
			width = ManiacPicture::Nearest(width * (data.current_magnify / 100.0));
			height = ManiacPicture::Nearest(height * (data.maniac_current_magnify_height / 100.0));
			break;
		case 2:
			x = ManiacPicture::Nearest(data.finish_x);
			y = ManiacPicture::Nearest(data.finish_y);
			width = ManiacPicture::Nearest(width * (data.finish_magnify / 100.0));
			height = ManiacPicture::Nearest(height * (data.maniac_finish_magnify_height / 100.0));
			break;
	}

	switch (com.parameters[2]) {
		case 1:
			// X/Y is top-left corner
			x -= (width / 2);
			y -= (height / 2);
			break;
		case 2: {
			// Left, Top, Right, Bottom
			x -= (width / 2);
			y -= (height / 2);
			width += x;
			height += y;
			break;
		}
	}

	const auto write_number = [&](int32_t id, int32_t value) {
		if (id == 0) return;
		if (id > 0) {
			Main_Data::game_variables->Set(id, value);
		} else {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) Output::Error("GetPictureInfo: Self destination requires current self context.");
			const size_t slot = static_cast<size_t>(-static_cast<int64_t>(id));
			bank->numbers.resize(std::max(bank->numbers.size(), slot + 2));
			bank->numbers[slot] = value;
		}
	};
	write_number(parameter(4), x);
	write_number(parameter(5), y);
	write_number(parameter(6), width);
	write_number(parameter(7), height);

	Game_Map::RefreshAfterManiacChange();

	return true;
}

bool Game_Interpreter::CommandManiacControlVarArray(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const auto negative_abs = [](int32_t value) -> int32_t {
		const uint32_t bits = value < 0 ? static_cast<uint32_t>(value) : uint32_t{0} - static_cast<uint32_t>(value);
		int32_t result;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	};
	const auto negate32 = [](int32_t value) -> int32_t {
		const uint32_t bits = uint32_t{0} - static_cast<uint32_t>(value);
		int32_t result;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	};
	const auto numeric = [&](int32_t raw, int mode) -> int32_t {
		return mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
	};
	const auto address = [&](int32_t raw, int mode) -> int32_t {
		if (mode == 0) return raw;
		if (mode == 1) {
			const int32_t value = ReadVariable(raw);
			return raw < 0 ? negative_abs(value) : value;
		}
		const int32_t first = raw < 0 ? negative_abs(ReadVariable(raw)) : ReadVariable(raw);
		const int32_t second = ReadVariable(first);
		return first < 0 ? negative_abs(second) : second;
	};
	const int operation = parameter(0);
	const uint32_t modes = static_cast<uint32_t>(parameter(1));
	const int32_t base = address(parameter(2), modes & 15);
	const int32_t count = numeric(parameter(3), (modes >> 4) & 15);
	if (base == 0 || count <= 0 || operation < 0 || operation > 22) return true;
	const bool needs_array = operation == 0 || operation == 1 ||
		(operation >= 6 && operation <= 19) || operation == 22;
	const int32_t rhs = needs_array ? address(parameter(4), (modes >> 8) & 15)
		: (operation == 5 || operation == 20 ? numeric(parameter(4), (modes >> 8) & 15) : 0);
	const bool prepare_before_zero_rhs = operation == 0 || operation == 1 || operation == 22;
	if (needs_array && rhs == 0 && !prepare_before_zero_rhs) return true;
	const auto* frame = GetFramePtr();
	auto* state = Main_Data::game_maniac_self_variables.get();
	Game_ManiacSelfVariables::Bank* bank = nullptr;
	const auto prepare = [&](int32_t first) -> bool {
		const int64_t last = std::abs(static_cast<int64_t>(first)) + count;
		// Prepare the same signed-address range as the variable accessors. A
		// historical one-million cutoff silently rejected ordinary existing banks.
		if (last >= std::numeric_limits<int32_t>::max()) {
			Output::Warning("ManiacControlVarArray: Array exceeds signed address range");
			return false;
		}
		if (first < 0) {
			if (!bank && frame && state) {
				bank = state->Ensure(frame->easyrpg_self_scope,
					frame->easyrpg_self_map_id, frame->easyrpg_self_event_id);
			}
			if (!bank) return false;
			bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(last + 1)));
		} else {
			Main_Data::game_variables->GetManiacWritable(static_cast<int>(last + 1));
		}
		return true;
	};
	if (!prepare(base)) return true;
	if (needs_array && rhs == 0) return true;
	if (needs_array && !prepare(rhs)) return true;
	const auto pointer = [&](int32_t first) -> int32_t* {
		const int64_t physical = std::abs(static_cast<int64_t>(first));
		return first < 0 ? &bank->numbers[static_cast<size_t>(physical)]
			: Main_Data::game_variables->GetManiacWritable(static_cast<int>(physical));
	};
	int32_t* a = pointer(base);
	int32_t* b = needs_array ? pointer(rhs) : nullptr;
	if (!a || (needs_array && !b)) return true;
	const auto assign_arithmetic = [&](int32_t old, int32_t value) -> int32_t {
		int64_t result = 0;
		switch (operation) {
			case 6: result = static_cast<int64_t>(old) + value; break;
			case 7: result = static_cast<int64_t>(old) - value; break;
			case 8: result = static_cast<int64_t>(old) * value; break;
			case 9: result = static_cast<int64_t>(old) / (value == 0 ? 1 : value); break;
			case 10: result = static_cast<int64_t>(old) % (value == 0 ? 1 : value); break;
			case 11: return old | value;
			case 12: return old & value;
			case 13: return old ^ value;
			case 14: {
				const uint32_t bits = static_cast<uint32_t>(old) << (static_cast<uint32_t>(value) & 31);
				int32_t out; std::memcpy(&out, &bits, sizeof(out)); return out;
			}
			case 15: {
				const uint32_t bits = static_cast<uint32_t>(old) >> (static_cast<uint32_t>(value) & 31);
				int32_t out; std::memcpy(&out, &bits, sizeof(out)); return out;
			}
		}
		return static_cast<int32_t>(std::clamp(result,
			static_cast<int64_t>(Main_Data::game_variables->GetMinValue()),
			static_cast<int64_t>(Main_Data::game_variables->GetMaxValue())));
	};
	switch (operation) {
		case 0: std::memmove(b, a, static_cast<size_t>(count) * sizeof(int32_t)); break;
		case 1:
			for (int i = 0; i < count; ++i) std::swap(a[i], b[i]);
			break;
		case 2:
		case 3:
			if (base < 0) {
				Output::Warning("ManiacControlVarArray: Unsafe native self sort skipped");
				return true;
			}
			if (operation == 2) std::sort(a, a + count);
			else std::sort(a, a + count, std::greater<int32_t>());
			break;
		case 4:
			for (int i = count; i > 0; --i) {
				const int j = static_cast<int>((static_cast<uint64_t>(Main_Data::NextManiacRandom()) * i) >> 32);
				std::swap(a[i - 1], a[j]);
			}
			break;
		case 5: {
			uint32_t value = static_cast<uint32_t>(rhs);
			for (int i = 0; i < count; ++i) {
				std::memcpy(&a[i], &value, sizeof(value));
				++value;
			}
			break;
		}
		case 6: case 7: case 8: case 9: case 10:
		case 11: case 12: case 13: case 14: case 15:
			for (int i = 0; i < count; ++i) a[i] = assign_arithmetic(a[i], b[i]);
			break;
		case 16:
			for (int i = 0; i < count; ++i) {
				const int32_t reference = base < 0 ? negate32(a[i]) : a[i];
				b[i] = ReadVariable(reference);
			}
			return true;
		case 17:
		case 18: {
			const auto compare = [&](int32_t left, int32_t right) {
				return operation == 17 ? left < right : left > right;
			};
			const auto paired_swap = [&](int left, int right) {
				std::swap(a[left], a[right]);
				std::swap(b[left], b[right]);
			};
			const auto sort = [&](const auto& recurse, int left, int right) -> void {
				while (left < right) {
					int low = left, high = right;
					const int32_t pivot = a[left + (right - left) / 2];
					while (low <= high) {
						while (low <= right && compare(a[low], pivot)) ++low;
						while (high >= left && compare(pivot, a[high])) --high;
						if (low <= high) paired_swap(low++, high--);
					}
					if (left < high) recurse(recurse, left, high);
					left = low;
				}
			};
			sort(sort, 0, count - 1);
			break;
		}
		case 19:
			for (int i = count; i > 0; --i) {
				const int j = static_cast<int>((static_cast<uint64_t>(Main_Data::NextManiacRandom()) * i) >> 32);
				std::swap(a[i - 1], a[j]);
				std::swap(b[i - 1], b[j]);
			}
			break;
		case 20:
			std::fill(a, a + count, rhs);
			break;
		case 21:
			std::reverse(a, a + count);
			return true;
		case 22:
			std::memmove(a, b, static_cast<size_t>(count) * sizeof(int32_t));
			break;
	}
	Game_Map::RefreshAfterManiacChange();
	return true;
}

bool Game_Interpreter::CommandManiacKeyInputProcEx(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

#if !defined(SUPPORT_KEYBOARD)
	Output::Warning("Maniac KeyInputProc: Keyboard input is not supported on this platform");
	// Fallthrough: Variables are still set to 0
#endif

	int operation = com.parameters[0];
	int start_var_id = com.parameters[1];

	if (operation == 0 || operation == 1) {
		// The difference between 0 and 1 is whether the Joypad is checked
		// We do not implement the Joypad code so both behave the same
		auto keys = ManiacPatch::GetKeyRange();

		for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
			Main_Data::game_variables->Set(start_var_id + i, keys[i] ? 1 : 0);
		}
	} else if (operation == 2) {
		int key_id = ValueOrVariable(com.parameters[2], com.parameters[3]);
		bool key_state = ManiacPatch::GetKeyState(key_id);
		Main_Data::game_variables->Set(start_var_id, key_state ? 1 : 0);
	} else {
		Output::Warning("Maniac KeyInputProcEx: Joypad not supported");
	}

	Game_Map::RefreshAfterManiacChange();

	return true;
}

bool Game_Interpreter::CommandManiacEditPicture(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t modes = parameter(0);
	const auto numeric = [&](size_t index) -> int32_t {
		const int mode = (modes >> (4 * (index - 1))) & 15;
		const int32_t raw = parameter(index);
		return mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
	};
	const int pic_id = numeric(1), x = numeric(2), y = numeric(3);
	const int width = numeric(4), height = numeric(5);
	int32_t source = parameter(6);
	const int source_mode = (modes >> 20) & 15;
	if (source_mode == 1) source = ResolveVariableIndirectAddress(source);
	else if (source_mode != 0) source = ResolveVariableIndirectAddress(ResolveVariableIndirectAddress(source));
	if (pic_id <= 0 || pic_id > Main_Data::game_pictures->GetMaxPictureId())
		Output::Error("ManiacEditPicture: Invalid picture ID {}", pic_id);
	auto* picture = Main_Data::game_pictures->GetPicturePtr(pic_id);
	if (!picture) return true;
	if (picture->IsRequestPending()) {
		picture->MakeRequestImportant();
		_async_op = AsyncOp::MakeYieldRepeat();
		return true;
	}
	// Native detaches the selected frame and clears animation even for an empty edit.
	auto bitmap = picture->GetMutableBitmap(true);
	if (!bitmap) return true;
	const int64_t skip_x = x < 0 ? -static_cast<int64_t>(x) : 0;
	const int64_t skip_y = y < 0 ? -static_cast<int64_t>(y) : 0;
	// 0x6d60f expands the source stride for a negative X; do not substitute ordinary clipping.
	const int64_t stride = static_cast<int64_t>(width) + skip_x;
	const int64_t rows = static_cast<int64_t>(height) + skip_y;
	const int64_t base = static_cast<int64_t>(source) + skip_x + skip_y * width;
	if (stride <= 0 || rows <= 0) return true;
	if (stride > INT32_MAX || rows > INT32_MAX || base < INT32_MIN || base > INT32_MAX) {
		Output::Warning("ManiacEditPicture: Source rectangle exceeds signed dimensions");
		return true;
	}
	const int64_t count = stride * rows;
	const int64_t first = std::abs(base), last = first + count;
	if (count > INT32_MAX || last > INT32_MAX) {
		Output::Warning("ManiacEditPicture: Source range exceeds signed slot addresses");
		return true;
	}
	// Native 0x8c800 exposes the whole adjusted range before clipping right/bottom.
	if (base < 0) {
		const auto* frame = GetFramePtr();
		auto* state = Main_Data::game_maniac_self_variables.get();
		auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope, frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
		if (!bank) return true;
		bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(last + 1)));
	} else {
		Main_Data::game_variables->GetManiacWritable(static_cast<int>(last));
	}
	const int dx = std::max(x, 0), dy = std::max(y, 0);
	const int draw_width = static_cast<int>(std::min(stride, static_cast<int64_t>(bitmap->GetWidth()) - dx));
	const int draw_height = static_cast<int>(std::min(rows, static_cast<int64_t>(bitmap->GetHeight()) - dy));
	const int options = parameter(7);
	for (int py = 0; py < draw_height; ++py) {
		for (int px = 0; px < draw_width; ++px) {
			const int64_t slot = first + static_cast<int64_t>(py) * stride + px;
			uint32_t pixel = slot == 0 ? Main_Data::game_variables->GetManiacRawSlot(0)
				: ReadVariable(static_cast<int>(base < 0 ? -slot : slot));
			if (options & 1) pixel |= 0xff000000;
			else if ((options & 2) && !(pixel & 0xff000000)) continue;
			bitmap->SetManiacColorAt(dx + px, dy + py, Color(pixel >> 16, pixel >> 8, pixel, pixel >> 24));
		}
	}
	return true;
}

AsyncOp Game_Interpreter::RunManiacMessageHook(int common_event_id, std::unique_ptr<Game_Interpreter>& invocation) {
	const auto* event = lcf::ReaderUtil::GetElement(lcf::Data::commonevents, common_event_id);
	if (!event) Output::Error("Maniac message hook: Invalid common event ID {}", common_event_id);
	invocation = std::make_unique<Game_Interpreter>(false);
	auto& hook = *invocation;
	hook.maniac_message_hook = true;
	hook.Push(event->event_commands, 0);
	if (!hook.IsRunning()) { invocation.reset(); return {}; }
	auto& frame = hook._state.stack.back();
	frame.easyrpg_self_scope = 1;
	frame.easyrpg_self_map_id = 0;
	frame.easyrpg_self_event_id = common_event_id;
	frame.maniac_event_info = 0x28;
	frame.maniac_event_id = common_event_id;
	// Each invocation has its own budget. A wait/failed dispatch stops this
	// invocation; do not spin until completion or consume the caller's budget.
	hook.Update();
	const auto operation = hook.GetAsyncOp();
	if (operation.GetType() != AsyncOp::eYieldRepeat && operation.GetType() != AsyncOp::eYield) invocation.reset();
	return operation;
}

AsyncOp Game_Interpreter::ResumeManiacMessageHook(std::unique_ptr<Game_Interpreter>& invocation) {
	if (!invocation) return {};
	// Only host acquisition retains an invocation. Script wait/busy/budget
	// already ended the local slice and never reach this continuation.
	invocation->Update(false);
	const auto operation = invocation->GetAsyncOp();
	if (operation.GetType() != AsyncOp::eYieldRepeat && operation.GetType() != AsyncOp::eYield) invocation.reset();
	return operation;
}

bool Game_Interpreter::CommandManiacControlTextProcessing(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const int selector = parameter(0) & 15;
	int event_id = 0;
	if (selector == 0) event_id = parameter(2);
	else if (selector == 1) event_id = ReadVariable(parameter(2));
	else if (selector == 2) event_id = ReadVariableIndirect(parameter(2));
	else if (selector >= 3 && selector <= 5) {
		event_id = selector == 3 ? com.easyrpg_maniac_common_name_cache : 0;
		if (event_id == 0) {
			const auto value = selector == 3 ? static_cast<std::string_view>(com.string)
				: ReadString(selector == 4 ? parameter(2) : ResolveVariableIndirectAddress(parameter(2)));
			const auto name = Utils::DecodeUTF16(value.substr(0, value.find('\0')), true);
			for (const auto& candidate : lcf::Data::commonevents) {
				const auto candidate_name = static_cast<std::string_view>(candidate.name);
				if (Utils::DecodeUTF16(candidate_name.substr(0, candidate_name.find('\0')), true) == name) {
					event_id = candidate.ID;
					break;
				}
			}
			if (selector == 3 && event_id != 0) com.easyrpg_maniac_common_name_cache = event_id;
		}
	}
	Main_Data::game_system->SetManiacMessageHook(event_id, parameter(1),
		parameter(3), parameter(4), parameter(5), parameter(6));
	return true;
}

bool Game_Interpreter::CommandManiacWritePicture(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t modes = parameter(0);
	const int filename_mode = (modes >> 4) & 15;
	const int string_id = filename_mode == 0 ? 0
		: filename_mode == 1 ? parameter(3) : ResolveVariableIndirectAddress(parameter(3));
	std::string filename = filename_mode == 0 ? ToString(com.string) : ToString(ReadString(string_id));
	const bool filename_present = filename_mode == 0 ? !com.string.empty() : IsManiacStringPresent(string_id);
	const int picture_mode = modes & 15;
	const int pic_id = picture_mode == 0 ? parameter(2)
		: picture_mode == 1 ? ReadVariable(parameter(2)) : ReadVariableIndirect(parameter(2));
	const auto game_fs = FileFinder::Game();
	std::error_code path_error;
	const auto game_path = game_fs.GetFullPath();
	const auto absolute_root = std::filesystem::absolute(std::filesystem::u8path(game_path.empty() ? "." : game_path), path_error);
	if (path_error) Output::Error("ManiacWritePicture: Cannot resolve game directory {}", game_path);
	const auto root = absolute_root.lexically_normal().generic_u8string();
	const auto prefix = EndsWith(root, '/') ? root : root + "/";
	// Native appends the suffix before canonicalising and opening the file.
	filename += ".png";
	if (!filename_present || Utils::DecodeUTF16(prefix + filename, true).size() >= 1024)
		Output::Error("ManiacWritePicture: Invalid file name {}", filename);
	const auto terminator = filename.find('\0');
	if (terminator != std::string::npos) filename.resize(terminator);
	const auto canonical_path = FileFinder::MakeCanonical(prefix + filename);
	if (!StartsWith(canonical_path, prefix) || canonical_path.size() == prefix.size())
		Output::Error("ManiacWritePicture: Invalid file name {}", filename);
	// A repeated asynchronous load attempt is still the same file output.
	if (parameter(1) != 0 && pic_id > 0 && pic_id <= Main_Data::game_pictures->GetMaxPictureId()) {
		auto* pending = Main_Data::game_pictures->GetPicturePtr(pic_id);
		if (pending && pending->IsRequestPending()) {
			pending->MakeRequestImportant();
			_async_op = AsyncOp::MakeYieldRepeat();
			return true;
		}
	}
	Output::BeginManiacFileOutput();
	filename = canonical_path.substr(prefix.size());
	// Read-only archives/remote resources use the established per-game save filesystem.
#ifdef EMSCRIPTEN
	// The browser's writable game mount is temporary; only the save mount persists.
	const auto output_fs = FileFinder::Save();
#else
	const auto output_fs = game_fs.IsFeatureSupported(Filesystem::Feature::Write) ? game_fs : FileFinder::Save();
#endif
	const auto directory = FileFinder::GetPathAndFilename(filename).first;
	if (!directory.empty()) output_fs.MakeDirectory(directory, true);
	auto stream = output_fs.OpenOutputStream(filename);
	if (!stream) return true; // Native silently continues when opening fails, before validating the picture.
	BitmapRef bitmap;
	bool force_opaque = true;
	if (parameter(1) == 0) {
		if (DisplayUi) bitmap = DisplayUi->GetDisplaySurface();
	} else {
		if (pic_id <= 0 || pic_id > Main_Data::game_pictures->GetMaxPictureId())
			Output::Error("ManiacWritePicture: Invalid picture ID {}", pic_id);
		auto* picture = Main_Data::game_pictures->GetPicturePtr(pic_id);
		if (picture && picture->sprite) {
			const bool processed = (parameter(4) & 1) != 0;
			const auto source = picture->GetManiacReadBitmap(processed);
			if (!source) return true;
			const int width = picture->sprite->GetFrameWidth(), height = picture->sprite->GetFrameHeight();
			if (width <= 0 || height <= 0) return true;
			bitmap = Bitmap::Create(width, height, true);
			for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
				bitmap->SetManiacColorAt(x, y, picture->ReadManiacPixel(*source, processed, x, y));
		}
		force_opaque = (parameter(4) & 2) != 0;
	}
	if (bitmap && !bitmap->WritePNG(stream, true, force_opaque))
		Output::Warning("ManiacWritePicture: Could not write {}", filename);
#ifdef EMSCRIPTEN
	stream.Close();
	AsyncHandler::SaveFilesystem();
#endif
	return true;
}

bool Game_Interpreter::CommandManiacZoomScreen(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t modes = static_cast<uint32_t>(parameter(0));
	const auto number = [&](int index) -> int32_t {
		const int mode = (modes >> ((index - 1) * 4)) & 15;
		const int32_t value = parameter(index);
		return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
	};
	const int x = number(1);
	const int y = number(2);
	const int scale = number(3);
	const int frames = number(4);
	const int layer = number(5);
	const int duration = Main_Data::game_screen->ZoomScreen(x, y, scale, frames, layer);
	if (parameter(6) != 0) SetupWaitFrames(duration);
	return true;
}

bool Game_Interpreter::CommandManiacJavaScript(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&com](size_t i) { return i < com.parameters.size() ? com.parameters[i] : 0; };
	std::string source;
	if (parameter(0) == 0) {
		source = ToString(com.string);
		auto& frame = GetFrame();
		// Consume the parent and its continuation records in one dispatch.
		++frame.current_command;
		while (frame.current_command < static_cast<int>(frame.commands.size())
			&& frame.commands[frame.current_command].code == static_cast<int>(Cmd::Maniac_JavaScriptContinuation)) {
			source += "\r\n";
			source += ToString(frame.commands[frame.current_command++].string);
		}
	} else if (parameter(0) == 1 || parameter(0) == 2) {
		source = ToString(ReadString(parameter(0) == 1 ? parameter(1) : ResolveVariableIndirectAddress(parameter(1))));
	}
	Game_ManiacJs::Execute(source, MakeSpan(com.parameters), *this);
	return true;
}

bool Game_Interpreter::CommandManiacChangeTroopMember(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	auto parameter = [&](size_t i) { return i < com.parameters.size() ? com.parameters[i] : 0; };
	const auto flags = static_cast<uint32_t>(parameter(0));
	auto number = [&](size_t i, unsigned nibble) {
		const auto mode = (flags >> (4 * nibble)) & 0xf;
		return mode == 0 ? parameter(i) : mode == 1 ? ReadVariable(parameter(i)) : ReadVariableIndirect(parameter(i));
	};
	auto& party = *Main_Data::game_enemyparty;
	const int operation = parameter(1);
	const int index = number(3, 0);
	int enemy_id = number(4, 1);
	if (operation == 2) {
		party.RemoveEnemies(index, enemy_id);
		return true;
	}
	if (operation < 0 || operation > 3) return true;
	if ((operation == 1 && index < 0) || (operation == 3 && !party.GetEnemy(index))) {
		Output::Warning("Maniac Change Troop Member: Invalid index {}", index);
		return true;
	}
	const auto inherit_flags = static_cast<uint32_t>(parameter(2));
	auto inherits = [&](unsigned nibble) { return ((inherit_flags >> (4 * nibble)) & 0xf) != 0; };
	const auto* old = operation == 3 ? party.GetEnemy(index) : nullptr;
	lcf::rpg::TroopMember member;
	member.ID = party.GetNextTroopMemberId();
	if (old && inherits(1)) enemy_id = old->GetId();
	member.enemy_id = enemy_id;
	member.x = number(5, 2);
	member.y = number(6, 3);
	if (Game_Battle::GetBattleCondition() == lcf::rpg::System::BattleCondition_back)
		member.x = static_cast<int32_t>(320u - static_cast<uint32_t>(member.x));
	if (old && inherits(2)) member.x = old->GetBattlePosition().x;
	if (old && inherits(3)) member.y = old->GetBattlePosition().y;
	// Native 05e6e0 resets Stat to visible after consuming parameter 7.
	// Only replacement's inherited Stat survives that initialization.
	member.invisible = old && inherits(4) && old->IsHidden();
	const bool reversed = old && inherits(5) ? old->IsDirectionFlipped() : number(8, 5) != 0;
	auto enemy = std::make_unique<Game_Enemy>(&member);
	enemy->SetDirectionFlipped(reversed);
	if (old) {
		const auto mode = (flags >> 24) & 0xf;
		const int name_id = mode == 1 ? parameter(9) : ResolveVariableIndirectAddress(parameter(9));
		if (inherits(6)) enemy->SetName(ToString(old->GetName()));
		else if (mode == 0) {
			if (!com.string.empty()) enemy->SetName(ToString(com.string));
		} else if (IsManiacStringPresent(name_id)) enemy->SetName(ToString(ReadString(name_id)));
	}
	if (Game_Battle::IsBattleRunning()) {
		auto sprite = std::make_unique<Sprite_Enemy>(enemy.get());
		sprite->ResetFixedFacingDirection();
		sprite->SetVisible(true);
		enemy->SetBattleSprite(std::move(sprite));
	}
	if (operation == 3) party.ReplaceEnemy(index, std::move(enemy));
	else party.InsertEnemy(operation == 0 ? party.GetBattlerCount() : index, std::move(enemy));
	return true;
}

bool Game_Interpreter::CommandManiacConsole(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&com](size_t i) { return i < com.parameters.size() ? com.parameters[i] : 0; };
	const uint32_t modes = parameter(0);
	const auto number = [&](size_t i) {
		const auto mode = (modes >> ((i - 1) * 4)) & 15;
		return mode == 0 ? parameter(i) : mode == 1 ? ReadVariable(parameter(i)) : ReadVariableIndirect(parameter(i));
	};
	const auto extract = [&](std::string value, uint32_t options) {
		value.resize(value.find('\0') == std::string::npos ? value.size() : value.find('\0'));
		return options & 1 ? Game_Strings::Extract(value, (options & 2) != 0, this) : value;
	};
	const int type = number(1);
	if (type >= 0 && type <= 5 && type != 1) Output::ManiacConsoleOpen();
	switch (type) {
	case 0:
		break;
	case 1:
		Output::ManiacConsoleClose();
		break;
	case 2: {
		const uint32_t options = number(2);
		const auto mode = (modes >> 8) & 15;
		std::string text = mode == 0 ? ToString(com.string)
			: ToString(ReadString(mode == 1 ? parameter(3) : ResolveVariableIndirectAddress(parameter(3))));
		text = extract(std::move(text), options);
		if (!(options & 4)) text += "\r\n";
		if (!Output::ManiacConsoleWrite(text)) Output::Warning("Maniac Console: Output unavailable");
		break;
	}
	case 3: {
		const uint32_t options = number(2);
		const int id = number(3);
		std::string text;
		if (!Output::ManiacConsoleRead(number(4), text)) {
			Output::Warning("Maniac Console: Line input unavailable on this platform");
			break;
		}
		text = extract(std::move(text), options);
		if (id >= 0) Main_Data::game_strings->SetManiacRawSlot(id, text, !text.empty());
		else {
			if (!Main_Data::game_maniac_self_variables)
				Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
			const auto& frame = GetFrame();
			auto* bank = Main_Data::game_maniac_self_variables->Ensure(frame.easyrpg_self_scope,
				frame.easyrpg_self_map_id, frame.easyrpg_self_event_id);
			const size_t slot = static_cast<size_t>(-static_cast<int64_t>(id));
			bank->strings.resize(std::max(bank->strings.size(), slot + 1));
			bank->strings[slot] = lcf::DBString(text);
			Game_ManiacSelfVariables::SetStringPresence(bank, slot, !text.empty());
		}
		break;
	}
	case 4:
		if (!Output::ManiacConsoleColor(number(2), number(3), number(4)))
			Output::Warning("Maniac Console: Color control unavailable on this platform");
		break;
	case 5:
		if (!Output::ManiacConsoleFocus(number(3) != 0))
			Output::Warning("Maniac Console: Window focus control unavailable on this platform");
		break;
	}
	return true;
}

bool Game_Interpreter::CommandManiacDrawTileOnPicture(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t modes = parameter(0);
	const auto numeric = [&](size_t index, int nibble) -> int32_t {
		const int mode = (modes >> (4 * nibble)) & 15;
		const int32_t raw = parameter(index);
		return mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
	};
	const int pic_id = numeric(1, 0);
	const int x = numeric(2, 1), y = numeric(3, 2);
	const int width = numeric(4, 3), height = numeric(5, 4);
	int tileset_id = numeric(10, 6);
	int time = numeric(11, 7);
	const bool range = parameter(8) != 0;
	const int source_mode = (modes >> 20) & 15;
	int32_t source;
	if (range) {
		source = parameter(6);
		if (source_mode == 1) source = ResolveVariableIndirectAddress(source);
		else if (source_mode != 0) source = ResolveVariableIndirectAddress(ResolveVariableIndirectAddress(source));
	} else {
		source = numeric(6, 5);
	}
	if (width <= 0 || height <= 0) return true;
	if (pic_id <= 0 || pic_id > Main_Data::game_pictures->GetMaxPictureId()) {
		Output::Error("ManiacDrawTileOnPicture: Invalid picture ID {}", pic_id);
	}
	auto* picture = Main_Data::game_pictures->GetPicturePtr(pic_id);
	if (!picture) return true; // A valid, unused slot has no image backing.
	if (picture->IsRequestPending()) {
		picture->MakeRequestImportant();
		_async_op = AsyncOp::MakeYieldRepeat();
		return true;
	}
	if (!picture->sprite || !picture->sprite->GetBitmap()) return true;
	const int64_t count = static_cast<int64_t>(width) * height;
	const int64_t pixel_width = static_cast<int64_t>(width) * 16;
	const int64_t pixel_height = static_cast<int64_t>(height) * 16;
	if (count > INT32_MAX || pixel_width > INT32_MAX || pixel_height > INT32_MAX
			|| static_cast<int64_t>(x) + pixel_width > INT32_MAX
			|| static_cast<int64_t>(y) + pixel_height > INT32_MAX) {
		Output::Warning("ManiacDrawTileOnPicture: Rectangle exceeds representable dimensions");
		return true;
	}
	const bool current_chipset = tileset_id <= 0 || tileset_id == Game_Map::GetChipset();
	if (current_chipset) tileset_id = Game_Map::GetChipset();
	const auto* tileset = lcf::ReaderUtil::GetElement(lcf::Data::chipsets, tileset_id);
	BitmapRef chipset;
	if (tileset) {
		const auto name = current_chipset ? Game_Map::GetChipsetName() : std::string_view(tileset->chipset_name);
		auto* request = AsyncHandler::RequestFile("ChipSet", name);
		request->SetGraphicFile(true);
		request->SetImportantFile(true);
		request->Start();
		if (!request->IsReady()) {
			_async_op = AsyncOp::MakeYieldRepeat();
			return true;
		}
		chipset = request->IsSuccess() ? Cache::Chipset(name) : Bitmap::Create(480, 256, true);
	}
	auto bitmap = picture->GetMutableBitmap();
	if (!bitmap) return true;
	const int options = parameter(7);
	if (options & 2) picture->ClearManiacTileRect(Rect(x, y, static_cast<int>(pixel_width), static_cast<int>(pixel_height)));
	// Native 3028 clears the unoffset rectangle, then draws into the selected sheet frame.
	int64_t draw_x = x, draw_y = y;
	if (picture->NumSpriteSheetFrames() > 1) {
		const auto& data = picture->data;
		draw_x += static_cast<int64_t>(data.spritesheet_frame % data.spritesheet_cols) * picture->sprite->GetFrameWidth();
		draw_y += static_cast<int64_t>(data.spritesheet_frame / data.spritesheet_cols) * picture->sprite->GetFrameHeight();
	}
	if (draw_x < INT32_MIN || draw_y < INT32_MIN || draw_x + pixel_width > INT32_MAX || draw_y + pixel_height > INT32_MAX) {
		Output::Warning("ManiacDrawTileOnPicture: Sheet frame exceeds representable coordinates");
		return true;
	}
	std::vector<int32_t> values(static_cast<size_t>(count), source);
	if (range) {
		const int64_t first = std::abs(static_cast<int64_t>(source));
		// 0x8c800 / 0x8c900 also allocate the zero slot following the requested range.
		const int64_t last = first + count;
		if (last > INT32_MAX) {
			Output::Warning("ManiacDrawTileOnPicture: Source range exceeds signed slot addresses");
			return true;
		}
		if (source < 0) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope, frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) return true;
			bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(last + 1)));
		} else if (last > 0) {
			Main_Data::game_variables->GetManiacWritable(static_cast<int>(last));
		}
		for (int64_t i = 0; i < count; ++i)
			values[static_cast<size_t>(i)] = first + i == 0 ? Main_Data::game_variables->GetManiacRawSlot(0)
				: ReadVariable(static_cast<int>(source < 0 ? -first - i : first + i));
	}
	if (chipset && tileset) {
		if (time == -1) time = Main_Data::game_system->GetFrameCounter();
		TilemapLayer::DrawManiacTileRectangle(*picture, *chipset,
			Rect(static_cast<int>(draw_x), static_cast<int>(draw_y), width, height), std::move(values),
			parameter(9) != 0, options, time, tileset->animation_type, tileset->animation_speed, current_chipset);
	}
	return true;
}

bool Game_Interpreter::CommandManiacRewriteMap(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}
	const bool range = (com.parameters[1] & 1) != 0;
	const uint32_t modes = com.parameters[0];
	const auto operand = [&](int field, int shift) {
		const int mode = (modes >> (shift * 4)) & 0xf;
		return mode == 0 ? com.parameters[field] : mode == 1
			? ReadVariable(com.parameters[field]) : ReadVariableIndirect(com.parameters[field]);
	};
	// Native resolves the rectangle, and rejects empty sizes, before the source.
	const int x_start = operand(4, 1), y_start = operand(5, 2);
	const int width = operand(6, 3), height = operand(7, 4);
	if (width <= 0 || height <= 0) return true;
	const int source_mode = modes & 0xf;
	const int source = range ? (source_mode == 0 ? com.parameters[3] : source_mode == 1
		? ResolveVariableIndirectAddress(com.parameters[3]) : ReadVariableIndirect(com.parameters[3])) : operand(3, 0);
	uint32_t source_index = static_cast<uint32_t>(std::abs(static_cast<int64_t>(source)));
	const bool upper = (com.parameters[2] & 1) != 0;
	const bool disable_autotile = com.parameters[8] != 0;
	auto* scene = static_cast<Scene_Map*>(Scene::Find(Scene::Map).get());
	const int map_width = Game_Map::GetTilesX(), map_height = Game_Map::GetTilesY();
	if (map_width <= 0 || map_height <= 0) return true;
	for (int row = 0; row < height; ++row) {
		for (int column = 0; column < width; ++column, ++source_index) {
			// Consume each source immediately, even for a skipped destination.
			const int id = static_cast<int32_t>(source < 0 ? 0u - source_index : source_index);
			int tile = range ? ReadVariable(id) : source;
			int x = static_cast<int32_t>(static_cast<uint32_t>(x_start) + column);
			int y = static_cast<int32_t>(static_cast<uint32_t>(y_start) + row);
			if (Game_Map::LoopHorizontal()) x = Utils::PositiveModulo(x, map_width);
			if (Game_Map::LoopVertical()) y = Utils::PositiveModulo(y, map_height);
			if (!Game_Map::IsValid(x, y)) continue;
			if (upper) {
				if (static_cast<uint32_t>(tile) >= NUM_UPPER_TILES) tile = 0;
				if (scene && scene->spriteset) scene->spriteset->ReplaceUpAt(x, y, tile);
				else Game_Map::ReplaceTileAt(x, y, IndexToChipId(tile + BLOCK_F_INDEX), 1);
			} else if (!range && disable_autotile) {
				// The fixed native single/lower/direct branch stores startY,
				// not the converted source tile, in its uint16 map cell.
				const auto raw_tile = static_cast<uint16_t>(y_start);
				if (scene && scene->spriteset) scene->spriteset->ReplaceDownTileAt(x, y, raw_tile, true);
				else Game_Map::ReplaceTileAt(x, y, raw_tile, 0);
			} else {
				if (static_cast<uint32_t>(tile) >= NUM_LOWER_TILES) tile = NUM_LOWER_TILES - 1;
				// The fixed 250921 lower table stores 3074 for animated index 4.
				// Its renderer groups both IDs alike, but retain the native raw cell.
				const int raw_tile = tile == 4 ? 3074 : IndexToChipId(tile);
				if (scene && scene->spriteset) scene->spriteset->ReplaceDownTileAt(x, y, raw_tile, disable_autotile);
				else if (disable_autotile) Game_Map::ReplaceTileAt(x, y, raw_tile, 0);
			}
		}
	}

	return true;
}

bool Game_Interpreter::CommandManiacControlGlobalSave(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	const auto param = [&](size_t i) { return i < com.parameters.size() ? com.parameters[i] : 0; };
	int operation = param(0);

	auto close_global_save = []() {
		Main_Data::global_save_opened = false;
		Main_Data::game_switches_global->SetData({});
		Main_Data::game_variables_global->SetData({});
		Main_Data::game_strings_global->SetData(Game_Strings::Strings_t{});
	};

	auto load_global_save = [&]() {
		close_global_save();
		Main_Data::global_save_opened = true;

		// Load
		auto save_fs = FileFinder::Save();
		auto lgs = save_fs.IsFile("SaveData/Save.lgs")
			? save_fs.OpenInputStream("SaveData/Save.lgs") : save_fs.OpenFile("Save.lgs");
		if (!lgs) {
			return;
		}

		lcf::LcfReader reader(lgs);
		std::string header;
		reader.ReadString(header, reader.ReadInt());
		if (header.length() != 13 || header != "LcfGlobalSave") {
			Output::Debug("This is not a valid global save.");
			return;
		}

		lcf::LcfReader::Chunk chunk;

		while (!reader.Eof()) {
			chunk.ID = reader.ReadInt();
			if (chunk.ID == 0) break;
			chunk.length = reader.ReadInt();
			switch (chunk.ID) {
				case 1: {
					std::vector<uint8_t> switches;
					reader.Read(switches, chunk.length);
					Main_Data::game_switches_global->SetData({});
					Main_Data::game_switches_global->EnsureSize(switches.size());
					for (size_t i = 0; i < switches.size(); ++i)
						Main_Data::game_switches_global->SetManiacRaw(static_cast<int>(i + 1), switches[i]);
					break;
				}
				case 2: {
					Game_Variables::Variables_t variables;
					reader.Read(variables, chunk.length);
					Main_Data::game_variables_global->SetData(std::move(variables));
					break;
				}
				case 3: {
					std::vector<uint8_t> bytes;
					reader.Read(bytes, chunk.length);
					ManiacOriginalSave::Cursor field(bytes.data(), bytes.size());
					Game_ManiacSelfVariables::Bank bank;
					if (!ManiacOriginalSave::DecodeStringSlots(field, bank)) {
						Output::Warning("Maniac ControlGlobalSave: Invalid string field");
						return;
					}
					Main_Data::game_strings_global->SetData(std::vector<lcf::DBString>(bank.strings.begin() + 1, bank.strings.end()));
					break;
				}
				default:
					reader.Skip(chunk, "CommandManiacControlGlobalSave");
			}
		}
	};

	if (operation == 0) {
		// Open
		if (!Main_Data::global_save_opened) load_global_save();
	} else if (operation == 1) {
		// Close
		close_global_save();
	} else if (operation == 2 || operation == 3) {
		// 2: Save (write to file)
		// 3: Save and Close
		if (!Main_Data::global_save_opened) {
			return true;
		}
		Output::BeginManiacFileOutput();

		Game_ManiacSelfVariables::Bank strings;
		strings.strings = Main_Data::game_strings_global->GetLcfData();
		strings.strings.insert(strings.strings.begin(), lcf::DBString{});
		strings.empty_string_slots = Main_Data::game_strings_global->GetManiacEmptySlotSaveData();
		std::vector<uint8_t> string_payload;
		if (!ManiacOriginalSave::EncodeStringSlots(strings, string_payload)) {
			Output::Warning("Maniac ControlGlobalSave: Saving string field failed");
			return true;
		}

		auto save_fs = FileFinder::Save();
		auto savelgs_name = save_fs.IsDirectory("SaveData", true)
			? std::string("SaveData/Save.lgs") : save_fs.FindFile("Save.lgs");
		if (savelgs_name.empty()) {
			savelgs_name = "Save.lgs";
		}

		auto lgs_out = FileFinder::Save().OpenOutputStream(savelgs_name);
		if (!lgs_out) {
			Output::Warning("Maniac ControlGlobalSave: Saving failed");
			return true;
		}

		lcf::LcfWriter writer(lgs_out, lcf::EngineVersion::e2k3);
		writer.WriteInt(13);
		const std::string header = "LcfGlobalSave";
		writer.Write(header);
		std::vector<uint8_t> switches;
		for (int id = 1; id <= Main_Data::game_switches_global->GetSize(); ++id)
			switches.push_back(Main_Data::game_switches_global->GetManiacRawSlot(id));
		while (!switches.empty() && switches.back() == 0) switches.pop_back();
		writer.WriteInt(1);
		writer.WriteInt(static_cast<int>(switches.size()));
		writer.Write(switches);
		auto variables = Main_Data::game_variables_global->GetData();
		while (!variables.empty() && variables.back() == 0) variables.pop_back();
		writer.WriteInt(2);
		writer.WriteInt(static_cast<int>(variables.size() * sizeof(int32_t)));
		writer.Write(variables);
		if (!string_payload.empty()) {
			writer.WriteInt(3);
			writer.WriteInt(static_cast<int>(string_payload.size()));
			writer.Write(string_payload);
		}
		writer.WriteInt(0);
		lgs_out.Close(); // Finish the file before requesting platform persistence.

		AsyncHandler::SaveFilesystem();

		if (operation == 3) {
			close_global_save();
		}
	} else if (operation == 4 || operation == 5) {
		if (!Main_Data::global_save_opened) {
			load_global_save();
		}

		int type = param(2);
		const auto numeric = [&](int shift, int raw) {
			const int mode = (param(1) >> (shift * 4)) & 15;
			return mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
		};
		int length = numeric(2, param(5));
		int game_state_idx;
		{
			const int mode = param(1) & 15;
			game_state_idx = mode == 0 ? param(3) : mode == 1 ? ResolveVariableIndirectAddress(param(3))
				: ResolveVariableIndirectAddress(ResolveVariableIndirectAddress(param(3)));
		}
		int global_save_idx = numeric(1, param(4));
		if (length <= 0 || game_state_idx == 0 || global_save_idx <= 0) return true;
		if (type < 0 || type > 2) return true;
		const uint32_t local_start = game_state_idx < 0 ? 0u - static_cast<uint32_t>(game_state_idx) : game_state_idx;
		const uint32_t shared_start = global_save_idx;
		const size_t local_size = local_start + static_cast<uint32_t>(length) + 1u;
		const size_t shared_size = shared_start + static_cast<uint32_t>(length) + 1u;
		Game_ManiacSelfVariables::Bank* bank = nullptr;
		if (game_state_idx < 0) {
			const auto* frame = GetFramePtr();
			if (frame && Main_Data::game_maniac_self_variables)
				bank = Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
					frame->easyrpg_self_map_id, frame->easyrpg_self_event_id);
			if (!bank) return true;
			if (type == 0) bank->switches.resize(std::max(bank->switches.size(), local_size));
			else if (type == 1) bank->numbers.resize(std::max(bank->numbers.size(), local_size));
			else bank->strings.resize(std::max(bank->strings.size(), local_size));
		} else {
			// Ordinary global vectors omit their reserved physical slot zero.
			if (type == 0) Main_Data::game_switches->EnsureSize(local_size - 1);
			else if (type == 1) Main_Data::game_variables->EnsureSize(local_size - 1);
			else Main_Data::game_strings->EnsureSize(local_size - 1);
		}
		if (type == 0) Main_Data::game_switches_global->EnsureSize(shared_size - 1);
		else if (type == 1) Main_Data::game_variables_global->EnsureSize(shared_size - 1);
		else Main_Data::game_strings_global->EnsureSize(shared_size - 1);
		for (uint32_t i = 0; i < static_cast<uint32_t>(length); ++i) {
			const uint32_t local = local_start + i, shared = shared_start + i;
			if (type == 0) {
				if (operation == 4) {
					const auto value = Main_Data::game_switches_global->GetManiacRawSlot(shared);
					if (bank) bank->switches[local] = value;
					else Main_Data::game_switches->SetManiacRaw(local, value);
				} else Main_Data::game_switches_global->SetManiacRaw(shared,
					bank ? bank->switches[local] : Main_Data::game_switches->GetManiacRawSlot(local));
			} else if (type == 1) {
				if (operation == 4) {
					const auto value = Main_Data::game_variables_global->GetManiacRawSlot(shared);
					if (bank) bank->numbers[local] = value;
					else *Main_Data::game_variables->GetManiacWritable(local) = value;
				} else *Main_Data::game_variables_global->GetManiacWritable(shared) =
					bank ? bank->numbers[local] : Main_Data::game_variables->GetManiacRawSlot(local);
			} else if (operation == 4) {
				const auto value = Main_Data::game_strings_global->Get(shared);
				const bool present = Main_Data::game_strings_global->GetData().count(shared) != 0;
				if (bank) {
					bank->strings[local] = lcf::DBString(value);
					Game_ManiacSelfVariables::SetStringPresence(bank, local, present);
				} else Main_Data::game_strings->SetManiacRawSlot(local, value, present);
			} else Main_Data::game_strings_global->SetManiacRawSlot(shared,
				bank ? static_cast<std::string_view>(bank->strings[local]) : Main_Data::game_strings->Get(local),
				bank ? Game_ManiacSelfVariables::IsStringPresent(bank, local) : Main_Data::game_strings->IsPresent(local));
		}
		// The fixed native copy does not request a map refresh.

	}

	return true;
}

bool Game_Interpreter::CommandManiacChangePictureId(const lcf::rpg::EventCommand& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&](size_t index) -> int32_t { return index < com.parameters.size() ? com.parameters[index] : 0; };
	const uint32_t modes = parameter(1);
	const auto numeric = [&](int32_t value, unsigned mode) {
		return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
	};
	const int32_t count = numeric(parameter(3), (modes >> 4) & 15);
	const int32_t target = numeric(parameter(4), (modes >> 8) & 255);
	const int32_t source = numeric(parameter(2), modes & 15);
	Main_Data::game_pictures->ChangeManiacIds(parameter(0), source, count, target, (parameter(5) & 1) != 0);
	return true;
}

bool Game_Interpreter::CommandManiacControlMediaOption(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) return true;
	const auto parameter = [&](size_t index) -> int32_t {
		return index < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t modes = static_cast<uint32_t>(parameter(0));
	const auto number = [&](int index) -> int32_t {
		const int mode = (modes >> ((index - 1) * 4)) & 15;
		return mode == 0 ? parameter(index) : mode == 1
			? ReadVariable(parameter(index)) : ReadVariableIndirect(parameter(index));
	};
	const int operation = number(1), type = number(2);
	const int32_t value = number(3);
	if (operation != 0 && operation != 1) return true;
	if (type < 0 || type > 9) return true;
	if (!DisplayUi) return true;
	int32_t result = 0;
	if (type == 8) {
		if (operation == 1) {
			DisplayUi->SetManiacSaveVideoDefaults(value != 0);
			return true;
		}
		result = DisplayUi->GetManiacSaveVideoDefaults();
	} else if (type == 4) {
		if (operation == 1) {
			DisplayUi->SetManiacFullscreenUnevenPixels(value != 0);
			return true;
		}
		result = DisplayUi->GetManiacFullscreenUnevenPixels();
	} else if (type == 9) {
		if (operation == 1) {
			Audio().SetManiacVolume(value);
			return true;
		}
		result = Audio().GetManiacVolume();
	} else if (type == 0) {
		if (operation == 1) {
			if (!DisplayUi->SetManiacScreenMode(value != 0))
				Output::Warning("Maniac screen mode could not be applied");
			Graphics::Update();
			return true;
		}
		result = DisplayUi->GetManiacScreenMode();
	} else if (type == 1) {
		if (operation == 1) {
			if (!DisplayUi->SetManiacWindowZoom(std::clamp(value, 1, 4)))
				Output::Warning("Maniac window zoom could not be applied");
			Graphics::Update();
			return true;
		}
		result = DisplayUi->GetManiacWindowZoom();
	} else if (type == 2) {
		if (operation == 1) {
			if (!DisplayUi->SetManiacWindowInterpolation(value != 0))
				Output::Warning("Maniac window interpolation could not be applied");
			return true;
		}
		result = DisplayUi->GetManiacWindowInterpolation();
	} else if (type == 3) {
		if (operation == 1) {
			if (!DisplayUi->SetManiacWindowFit(value != 0))
				Output::Warning("Maniac window fit could not be applied");
			Graphics::Update();
			return true;
		}
		result = DisplayUi->GetManiacWindowFit();
	} else if (type == 5 || type == 6) {
		if (operation == 1) {
			DisplayUi->SetManiacFullscreenLayoutOption(type, value != 0);
			return true;
		}
		result = DisplayUi->GetManiacFullscreenLayoutOption(type);
	} else if (operation == 1) {
#ifdef _WIN32
		const auto shown = ConfigEnum::ShowFps::ON;
#else
		// Mobile and browser surfaces present the counter inside the game image.
		const auto shown = ConfigEnum::ShowFps::Overlay;
#endif
		DisplayUi->SetShowFps(value != 0 ? shown : ConfigEnum::ShowFps::OFF);
		Graphics::Update();
		return true;
	} else {
		// Query active display state: menu config can hide an active overlay option.
		result = DisplayUi->RenderFps() || DisplayUi->ShowFpsOnTitle();
	}
	if (value > 0) {
		Main_Data::game_variables->Set(value, result);
	} else if (value < 0) {
		const auto* frame = GetFramePtr();
		auto* state = Main_Data::game_maniac_self_variables.get();
		auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
			frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
		if (!bank) Output::Error("ControlMediaOption: Self destination requires current self context.");
		const size_t slot = static_cast<size_t>(-static_cast<int64_t>(value));
		bank->numbers.resize(std::max(bank->numbers.size(), slot + 2));
		bank->numbers[slot] = result;
	}
	return true;
}

bool Game_Interpreter::CommandManiacSetGameOption(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	if (com.parameters.size() < 2) {
		Main_Data::game_system->SetManiacRunWhenInactive(0);
		return true;
	}
	int operation = com.parameters[1];
	const auto operand = [&](int index, int mode) {
		if (index >= static_cast<int>(com.parameters.size())) return int32_t(0);
		const auto value = com.parameters[index];
		return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
	};

	switch (operation) {
		case 2:
			Main_Data::game_pictures->SetManiacPictureLimit(operand(2, com.parameters[0]));
			break;
		case 4:
			Main_Data::game_system->SetManiacMessageMouseFlags(operand(2, com.parameters[0]));
			break;
		case 5:
			Main_Data::game_system->SetManiacBattleAlignment(operand(2, com.parameters[0]));
			break;
		case 7: {
			Main_Data::game_system->SetManiacFaceSize(operand(2, com.parameters[0] & 15),
				operand(3, (com.parameters[0] >> 4) & 15));
			break;
		}
		case 0:
			Main_Data::game_system->SetManiacRunWhenInactive(com.parameters.size() > 2 ? com.parameters[2] : 0);
			break;
		case 1:
			Main_Data::game_system->SetManiacFps(operand(2, com.parameters[0]),
				com.parameters.size() > 3, com.parameters.size() > 3 ? com.parameters[3] : 0);
			break;
		case 6:
			Main_Data::game_screen->SetManiacAnimationBufferCount(operand(2, com.parameters[0]));
			break;
		default:
			// Option3 and unknown options consume the raw operand; A0 is ignored.
			Main_Data::game_system->SetManiacFrameSkip(com.parameters.size() > 2 ? com.parameters[2] : 0);
	}

	return true;
}

bool Game_Interpreter::CommandManiacControlStrings(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}
	//*parameter 0 - Modifier of the operation members
	//	-bits 0..3:
	//		Refers to the String mode: t[x], t[x..y], t[v[x]], t[v[x]..v[y]]
	//	-bits 4..7:
	//		Refers to the first argument, which can change depending on the type of operation:  x, v[x], v[v[x]] | string, t[x], t[v[x]]
	//	-bits 8..11:
	//		Refers to the second argument, which can change depending on the type of operation: x, v[x], v[v[x]] | string, t[x], t[v[x]]
	//	-bits 12..15:
	//		Refers to the third argument, which can change depending on the type of operation:  x, v[x], v[v[x]] | string, t[x], t[v[x]]
	//	-bits 16..19:
	//		Refers to the fourth argument, which can change depending on the type of operation: x, v[x], v[v[x]] | string, t[x], t[v[x]] (edge case for exMatch)
	//
	//*parameter 1 - String index 0
	//
	//*parameter 2 - String index 1 (optional for ranges)
	//
	//*parameter 3 - general flags
	//	-byte0:
	//		Refers to the type of operation: asg, cat, toNum...
	//	-byte1:
	//		It is a flag that indicates sub-operation: actor[x].name, .actor[x].desc, ins, rep, subs, join... Used only in asg and cat operations
	//	-byte2:
	//		Flags, such as: extract, hex... There is also an edge case where the last argument of exRep is here
	//
	//*parameters 4..n - arguments
	bool db_source_present = false;
	const int target_mode = com.parameters[0] & 15;
	const bool is_range = target_mode == 1 || target_mode >= 3;
	const int string_id_0 = target_mode < 2 ? com.parameters[1] : ResolveVariableIndirectAddress(com.parameters[1]);
	const int string_id_1 = target_mode == 1 ? com.parameters[2]
		: target_mode >= 3 ? ResolveVariableIndirectAddress(com.parameters[2]) : string_id_0;
	const bool self_target = string_id_0 < 0;
	int64_t first_slot = std::abs(static_cast<int64_t>(string_id_0));
	int64_t last_slot = first_slot;
	if (is_range && self_target == (string_id_1 < 0)) {
		last_slot = std::abs(static_cast<int64_t>(string_id_1));
		if (last_slot < first_slot) std::swap(first_slot, last_slot);
	}
	if (last_slot - first_slot + 1 > INT32_MAX) return true;
	// Native 8ce30 reserves a global tail even for SELF and no-op targets,
	// before reading the operation or computing an aliased source.
	if (last_slot + 1 > INT32_MAX) Output::Error("Failed to create string-variables.");
	Main_Data::game_strings->SetManiacVectorSize(static_cast<int32_t>(last_slot + 1));
	if ((com.parameters[3] & 255) == 6 || (com.parameters[3] & 255) > 10) return true;

	int op = (com.parameters[3] >>  0) & 255;
	int fn = (com.parameters[3] >>  8) & 255;
	int flags = (com.parameters[3] >> 16) & 255;

	int hex_flag     = (flags >> 1) & 1;
	int extract_flag = (flags >> 2) & 1;
	int first_flag = (flags >> 3) & 1;

	int args[] = {
		com.parameters[4],
		com.parameters[5],
		com.parameters[6],
		0
	};
	if (com.parameters.size() > 7) {
		args[3] = com.parameters[7]; //The exMatch command is a edge case that uses 4 arguments
	}
	int modes[] = {
		(com.parameters[0] >>  4) & 15,
		(com.parameters[0] >>  8) & 15,
		(com.parameters[0] >> 12) & 15,

		(com.parameters[0] >> 16) & 15
	};

	std::string str_param = ToString(com.string);
	std::string result;
	const auto literal_units = Utils::DecodeUTF16(str_param, true);
	const auto source_number = [&](int mode, int value) {
		return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
	};
	const auto get_string_source = [&](int mode, int id) -> std::string_view {
		return mode == 0 ? std::string_view(str_param)
			: ReadString(mode == 1 ? id : ResolveVariableIndirectAddress(id));
	};
	const auto read_target = [&](int id) {
		// Direct native arrays include physical global slot0. Whole S operands
		// continue to use their separate checked lookup through ReadString.
		return id < 0 ? ReadString(id) : Main_Data::game_strings->GetManiacRawSlot(id);
	};
	const auto get_string_source_with_pos = [&](int mode, int id, int& pos) -> std::string {
		if (mode != 0) return ToString(get_string_source(mode, id));
		const size_t start = static_cast<size_t>(pos);
		size_t accepted = 0;
		int32_t budget = id;
		while (start + accepted < literal_units.size()) {
			const char16_t unit = literal_units[start + accepted];
			if (unit > 0x00ff && !(unit >= 0xff61 && unit <= 0xff9f))
				budget = static_cast<int32_t>(static_cast<uint32_t>(budget) - 1);
			if (static_cast<int64_t>(budget) <= static_cast<int64_t>(accepted)) break;
			++accepted;
		}
		pos += static_cast<int>(accepted);
		return Utils::EncodeUTF(literal_units.substr(start, accepted), true);
	};
	const auto expand_source = [&](std::string text) {
		if (extract_flag) {
			text.resize(text.find('\0') == std::string::npos ? text.size() : text.find('\0'));
			text = Game_Strings::Extract(text, hex_flag, this);
			text.resize(text.find('\0') == std::string::npos ? text.size() : text.find('\0'));
		}
		return text;
	};
	const auto unit_position = [](size_t length, int32_t index) {
		int64_t position = index;
		if (position < 0) position += static_cast<int64_t>(length);
		return static_cast<size_t>(std::clamp<int64_t>(position, 0, static_cast<int64_t>(length)));
	};
	Game_Strings::Str_Params str_params = {
		string_id_0,
		hex_flag,
		extract_flag,
	};
	const auto pad_native = [](std::string_view text, int32_t width) {
		if (width == 0) return ToString(text);
		// The formatter consumes a terminated string and measures UTF16 units.
		text = text.substr(0, text.find('\0'));
		const uint32_t minimum = width < 0 ? 0u - static_cast<uint32_t>(width) : static_cast<uint32_t>(width);
		const size_t length = Utils::DecodeUTF16(text, true).size();
		std::string padded = ToString(text);
		if (minimum > length) {
			if (width < 0) padded.append(minimum - length, ' ');
			else padded.insert(0, minimum - length, ' ');
		}
		return padded;
	};

	switch (op)
	{
	case 0: //asg <fn(string text)>
	case 1: //cat <fn(string text)>
	{
		switch (fn)
		{
		case 0: //String <fn(string text, int min_size)>
			result = ToString(get_string_source(modes[0], args[0]));
			args[1] = source_number(modes[1], args[1]);

			// min_size
			if (extract_flag) {
				result.resize(result.find('\0') == std::string::npos ? result.size() : result.find('\0'));
				result = Game_Strings::Extract(result, hex_flag, this);
				result.resize(result.find('\0') == std::string::npos ? result.size() : result.find('\0'));
			}
			result = pad_native(result, args[1]);
			str_params.extract = 0;
			break;
		case 1: //Number <fn(int number, int min_size)>
		{
			args[0] = source_number(modes[0], args[0]);
			args[1] = source_number(modes[1], args[1]);

			const int width = Utils::Clamp(args[1], 0, 12);
			result = hex_flag ? fmt::format("{:0{}x}", static_cast<uint32_t>(args[0]), width)
				: fmt::format("{:0{}d}", args[0], width);
			str_params.extract = 0; // This source ignores bit18.
			break;
		}
		case 2: //Switch <fn(int number, int min_size)>
		{
			// Every nonzero mode resolves one address, including SELF references.
			if (modes[0] != 0) args[0] = ResolveVariableIndirectAddress(args[0]);
			args[1] = source_number(modes[1], args[1]);

			if (ReadSwitch(args[0]) != 0) {
				result = "ON";
			} else {
				result = "OFF";
			}
			result = pad_native(result, args[1]);
			str_params.extract = 0; // This source ignores bit18.
			break;
		}
		case 3: //Database Names <fn(int database_id, int entity_id, int flags)>
		case 4: //Database Descriptions
		{
			args[1] = source_number(modes[1], args[1]);
			result = ToString(fn == 3 ? ManiacPatch::GetLcfName(args[0], args[1], args[2] != 0, &db_source_present)
				: ManiacPatch::GetLcfDescription(args[0], args[1], args[2] != 0, &db_source_present));
			// These sources expand only for A6 bit4, independently of bit18.
			if (args[2] & 4) {
				result.resize(result.find('\0') == std::string::npos ? result.size() : result.find('\0'));
				result = Game_Strings::Extract(result, hex_flag, this);
				result.resize(result.find('\0') == std::string::npos ? result.size() : result.find('\0'));
			}
			str_params.extract = 0;
			break;
		}
		case 6: //Concatenate (cat) <fn(int id_or_length_a, int id_or_length_b, int id_or_length_c)>
		{
			int pos = 0;
			std::string op_string;
			for (int i = 0; i < 3; i++) {
				auto part = get_string_source_with_pos(modes[i], args[i], pos);
				op_string += expand_source(std::move(part));
			}
			// Slices can leave adjacent surrogate halves in separate operands.
			result = Utils::EncodeUTF(Utils::DecodeUTF16(op_string, true), true);
			str_params.extract = 0;
			break;
		}
		case 7: //Insert (ins) <fn(string base, int index, string insert)>
		{
			int pos = 0;
			std::string base, insert;

			args[1] = source_number(modes[1], args[1]);
			base = get_string_source_with_pos(modes[0], args[0], pos);
			insert = get_string_source_with_pos(modes[2], args[2], pos);

			auto units = Utils::DecodeUTF16(expand_source(std::move(base)), true);
			units.insert(unit_position(units.size(), args[1]), Utils::DecodeUTF16(expand_source(std::move(insert)), true));
			result = Utils::EncodeUTF(units, true);
			str_params.extract = 0;
			break;
		}
		case 8: //Replace (rep) <fn(string base, string search, string replacement)>
		{
			int pos = 0;
			std::string base, search, replacement;

			base = get_string_source_with_pos(modes[0], args[0], pos);
			search = get_string_source_with_pos(modes[1], args[1], pos);
			replacement = get_string_source_with_pos(modes[2], args[2], pos);
			base = expand_source(std::move(base));
			search = expand_source(std::move(search));
			replacement = expand_source(std::move(replacement));
			const auto terminated_units = [](const std::string& text) {
				return Utils::DecodeUTF16(text.substr(0, text.find('\0')), true);
			};
			auto units = terminated_units(base);
			const auto needle = terminated_units(search), inserted = terminated_units(replacement);
			if (!needle.empty()) {
				for (size_t position = 0; (position = units.find(needle, position)) != std::u16string::npos; position += inserted.size())
					units.replace(position, needle.size(), inserted);
			}
			result = Utils::EncodeUTF(units, true);
			str_params.extract = 0;

			break;
		}
		case 9: //Substring (subs) <fn(string base, int index, int size)>
		{
			args[1] = source_number(modes[1], args[1]);
			args[2] = source_number(modes[2], args[2]);

			result = ToString(get_string_source(modes[0], args[0]));
			const auto units = Utils::DecodeUTF16(expand_source(std::move(result)), true);
			result = Utils::EncodeUTF(units.substr(unit_position(units.size(), args[1]), static_cast<size_t>(std::max(args[2], 0))), true);
			str_params.extract = 0;
			break;
		}
		case 10: //Join (join) <fn(string delimiter, int id, int size)>
		{
			std::string op_string;
			const auto join_input = [&](std::string_view text) {
				text = text.substr(0, text.find('\0'));
				std::string expanded = extract_flag ? Game_Strings::Extract(text, hex_flag, this) : ToString(text);
				expanded.resize(expanded.find('\0') == std::string::npos ? expanded.size() : expanded.find('\0'));
				return expanded;
			};
			const auto source_delimiter = get_string_source(modes[0], args[0]);
			std::string delimiter = join_input(source_delimiter);
			// Expansion happens once per input, before joining; inserted text
			// must not be scanned again when assigning or appending the result.
			str_params.extract = 0;

			// args[1] & mode[1] relates to starting ID for strings to join
			// mode 0 = id literal, 1 = direct var, 2 = var literal, 3 = direct var
			args[1] = source_number(modes[1] % 2, args[1]);
			args[2] = source_number(modes[2], args[2]);
			if (modes[1] >= 6) break;

			while (args[2] > 0) {
				const int id = args[1];
				args[1] = static_cast<int32_t>(static_cast<uint32_t>(id) + 1);
				if (modes[1] < 2) op_string += join_input(ReadString(id));
				else if (modes[1] < 4) {
					const int32_t value = ReadVariable(id);
					op_string += hex_flag ? fmt::format("{:x}", static_cast<uint32_t>(value)) : std::to_string(value);
				}
				else op_string += ReadSwitch(id) != 0 ? "ON" : "OFF";

				if (--args[2] > 0) op_string += delimiter;
			}

			result = op_string;
			break;
		}
		case 12: //File (file) <fn(string filename, int encode)>
		{
			// maniacs does not like a file extension
			std::string filename = ToString(get_string_source(modes[0], args[0]));
			{
				args[1] = source_number(modes[1], args[1]);
				const bool present = modes[0] == 0 ? !str_param.empty()
					: IsManiacStringPresent(modes[0] == 1 ? args[0] : ResolveVariableIndirectAddress(args[0]));
				filename = expand_source(std::move(filename));
				str_params.extract = 0; // Only the filename is expanded.
				if (filename.empty() && (extract_flag || !present)) break;
			}
			// args[1] is the encoding... 0 for ansi, 1 for utf8
			bool do_yield;
			result = Game_Strings::FromFile(filename, args[1], do_yield);

			if (do_yield) {
				// Wait for text file download and repeat
				_async_op = AsyncOp::MakeYieldRepeat();
				return true;
			}

			break;
		}
		case 13: //Remove (rem) <fn(string base, int index, int size)>
			args[1] = source_number(modes[1], args[1]);
			args[2] = source_number(modes[2], args[2]);
			{
				int pos = 0;
				auto units = Utils::DecodeUTF16(expand_source(get_string_source_with_pos(modes[0], args[0], pos)), true);
				units.erase(unit_position(units.size(), args[1]), static_cast<size_t>(std::max(args[2], 0)));
				result = Utils::EncodeUTF(units, true);
				str_params.extract = 0;
			}
			break;
		case 14: //Replace Ex (exRep) <fn(string base, string search, string replacement, bool first)>, edge case: the arg "first" is at ((flags >> 19) & 1). Wtf BingShan
		{
			int pos = 0;
			std::string base, search, replacement;

			base = get_string_source_with_pos(modes[0], args[0], pos);
			search = get_string_source_with_pos(modes[1], args[1], pos);
			replacement = get_string_source_with_pos(modes[2], args[2], pos);

			auto flags = std::regex_constants::match_default;

			if (first_flag) {
				flags = std::regex_constants::format_first_only;
			}

			{
				base = expand_source(std::move(base));
				search = expand_source(std::move(search));
				replacement = expand_source(std::move(replacement));
				result = Game_Strings::ManiacRegExReplace(base, search, replacement, flags);
				str_params.extract = 0;
			}
			break;
		}
		default:

			break;
		}
		if (str_params.extract) result = Game_Strings::Extract(result, hex_flag, this);
		const int source = modes[0] == 1 ? args[0]
			: modes[0] >= 2 ? ResolveVariableIndirectAddress(args[0]) : 0;
		const bool source_present = !result.empty() || ((fn == 3 || fn == 4) && !(args[2] & 4) && db_source_present)
			|| (fn == 0 && !extract_flag
			&& modes[0] != 0 && args[1] == 0 && IsManiacStringPresent(source));
		// Compute the source once, before any aliased destination is changed.
		for (int64_t slot = first_slot; slot <= last_slot; ++slot) {
			const int id = self_target ? static_cast<int32_t>(0u - static_cast<uint32_t>(slot)) : static_cast<int>(slot);
			std::string value = result;
			bool present = source_present;
			if (op == 1) {
				const auto destination = id >= 0 ? Main_Data::game_strings->GetManiacRawSlot(id) : ReadString(id);
				const bool destination_present = id == 0 ? !Main_Data::game_strings->GetManiacZeroSaveData().empty() : IsManiacStringPresent(id);
				value = ToString(destination) + result;
				present = source_present || destination_present;
			}
			if (id >= 0) {
				Main_Data::game_strings->SetManiacRawSlot(id, value, present);
			} else {
				if (!Main_Data::game_maniac_self_variables) Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
				const auto* frame = GetFramePtr();
				auto* bank = frame ? Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
					frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
				if (!bank) Output::Error("Failed to create self-variables.");
				bank->strings.resize(std::max(bank->strings.size(), static_cast<size_t>(slot + 1)));
				bank->strings[slot] = lcf::DBString(value);
				Game_ManiacSelfVariables::SetStringPresence(bank, static_cast<size_t>(slot), present);
			}
		}
		break;
	}
	case 2: //toNum <fn(int var_id)> takes hex
	case 3: //getLen <fn(int var_id)>
		{
			// A4 is a raw numeric-array address; nibble1 is ignored. Allocate
			// before the first read, then advance magnitudes inside its bank.
			const int64_t count = last_slot - first_slot + 1;
			const int64_t output_first = std::abs(static_cast<int64_t>(args[0]));
			if (output_first + count > INT32_MAX) Output::Error("Failed to create variables.");
			Game_ManiacSelfVariables::Bank* output_bank = nullptr;
			if (args[0] < 0) {
				if (!Main_Data::game_maniac_self_variables) Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
				const auto* frame = GetFramePtr();
				output_bank = frame ? Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
					frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
				if (!output_bank) Output::Error("Failed to create self-variables.");
				output_bank->numbers.resize(std::max(output_bank->numbers.size(), static_cast<size_t>(output_first + count + 1)));
			} else Main_Data::game_variables->EnsureSize(static_cast<size_t>(output_first + count));
			for (int64_t index = 0; index < count; ++index) {
				const int id = self_target ? static_cast<int32_t>(0u - static_cast<uint32_t>(first_slot + index))
					: static_cast<int>(first_slot + index);
				std::string text = ToString(read_target(id));
				int32_t value;
				if (op == 3) value = static_cast<int32_t>(Utils::DecodeUTF16(text, true).size());
				else {
					if (extract_flag) text = Game_Strings::Extract(text.substr(0, text.find('\0')), hex_flag, this);
					const auto units = Utils::DecodeUTF16(text, true);
					const std::wstring wide(units.begin(), units.end());
					const long parsed = std::wcstol(wide.c_str(), nullptr, hex_flag ? 16 : 10);
					value = static_cast<int32_t>(std::clamp<long>(parsed, INT32_MIN, INT32_MAX));
				}
				const int slot = static_cast<int>(output_first + index);
				if (output_bank) output_bank->numbers[slot] = value;
				else {
					*Main_Data::game_variables->GetManiacWritable(slot) = value;
					Game_Map::SetNeedRefreshForVarChange(slot);
				}
			}
			break;
		}
	case 4: //inStr <fn(string text, int var_id, int begin)> FIXME: takes hex?
	{
		// Unlike regex A5, Instr A5 is an ordinary numeric value operand.
		const int output = source_number(modes[1], args[1]);
		const int begin = source_number(modes[2], args[2]);
		const int64_t output_first = std::abs(static_cast<int64_t>(output));
		const int64_t count = last_slot - first_slot + 1;
		if (output_first + count > INT32_MAX) Output::Error("Failed to create variables.");
		Game_ManiacSelfVariables::Bank* output_bank = nullptr;
		if (output < 0) {
			if (!Main_Data::game_maniac_self_variables) Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
			const auto* frame = GetFramePtr();
			output_bank = frame ? Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!output_bank) Output::Error("Failed to create self-variables.");
			output_bank->numbers.resize(std::max(output_bank->numbers.size(), static_cast<size_t>(output_first + count + 1)));
		} else Main_Data::game_variables->EnsureSize(static_cast<size_t>(output_first + count));
		const auto raw_needle = get_string_source(modes[0], args[0]);
		const bool needle_present = extract_flag ? false : modes[0] == 0 ? !str_param.empty()
			: IsManiacStringPresent(modes[0] == 1 ? args[0] : ResolveVariableIndirectAddress(args[0]));
		const auto needle_text = expand_source(ToString(raw_needle));
		const auto needle = Utils::DecodeUTF16(needle_text.substr(0, needle_text.find('\0')), true);
		const bool null_needle = extract_flag ? needle_text.empty() : !needle_present;
		for (int64_t index = 0; index < count; ++index) {
			const int id = self_target ? static_cast<int32_t>(0u - static_cast<uint32_t>(first_slot + index)) : static_cast<int>(first_slot + index);
			const auto source = read_target(id);
			const auto stored = Utils::DecodeUTF16(source, true);
			int value = null_needle ? 0 : -1;
			if (!null_needle && begin >= 0 && static_cast<size_t>(begin) < stored.size()) {
				// wcsstr consumes a terminated suffix, although start is bounded
				// by the stored UTF16 length (which can include embedded NUL).
				const auto suffix = stored.substr(static_cast<size_t>(begin));
				const auto found = suffix.substr(0, suffix.find(u'\0')).find(needle);
				if (found != std::u16string::npos) value = static_cast<int>(begin + found);
			}
			const int slot = static_cast<int>(output_first + index);
			if (output_bank) output_bank->numbers[slot] = value;
			else {
				*Main_Data::game_variables->GetManiacWritable(slot) = value;
				Game_Map::SetNeedRefreshForVarChange(slot);
			}
		}
		break;
	}
	case 5: //split <fn(string text, int str_id, int var_id)> takes hex
	{
		const auto raw_delimiter = get_string_source(modes[0], args[0]);
		const bool delimiter_present = extract_flag ? false : modes[0] == 0 ? !str_param.empty()
			: IsManiacStringPresent(modes[0] == 1 ? args[0] : ResolveVariableIndirectAddress(args[0]));
		const auto delimiter_text = expand_source(ToString(raw_delimiter));
		const auto delimiter = Utils::DecodeUTF16(delimiter_text.substr(0, delimiter_text.find('\0')), true);
		const bool null_delimiter = extract_flag ? delimiter_text.empty() : !delimiter_present;
		int32_t output = modes[1] == 0 ? args[1] : modes[1] == 1 ? ResolveVariableIndirectAddress(args[1]) : ReadVariableIndirect(args[1]);
		const int64_t count = last_slot - first_slot + 1;
		// A6 is a raw optional count array; zero disables it, regardless of n3.
		const int64_t count_first = std::abs(static_cast<int64_t>(args[2]));
		Game_ManiacSelfVariables::Bank* bank = nullptr;
		const auto self_bank = [&]() {
			if (!bank) {
				if (!Main_Data::game_maniac_self_variables) Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
				const auto* frame = GetFramePtr();
				bank = frame ? Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
					frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
				if (!bank) Output::Error("Failed to create self-variables.");
			}
			return bank;
		};
		if (args[2] != 0) {
			if (count_first + count > INT32_MAX) Output::Error("Failed to create variables.");
			if (args[2] < 0) {
				auto* counts = self_bank();
				counts->numbers.resize(std::max(counts->numbers.size(), static_cast<size_t>(count_first + count + 1)));
			} else Main_Data::game_variables->EnsureSize(static_cast<size_t>(count_first + count));
		}
		for (int64_t index = 0; index < count; ++index) {
			const int id = self_target ? static_cast<int32_t>(0u - static_cast<uint32_t>(first_slot + index)) : static_cast<int>(first_slot + index);
			const bool present = id == 0 ? !Main_Data::game_strings->GetManiacZeroSaveData().empty() : IsManiacStringPresent(id);
			const auto source = Utils::DecodeUTF16(read_target(id), true);
			std::vector<std::u16string> parts;
			if (present) {
				if (!null_delimiter && delimiter.empty()) {
					// Only a present input enters native's nonprogressing find loop.
					Output::Warning("ControlStrings: Split with a nonnull empty delimiter cannot advance");
					break;
				}
				if (null_delimiter) {
					for (const auto unit : source) parts.emplace_back(1, unit);
				} else {
					const auto terminated = source.substr(0, source.find(u'\0'));
					size_t position = 0, found;
					while ((found = terminated.find(delimiter, position)) != std::u16string::npos) {
						parts.push_back(terminated.substr(position, found - position));
						position = found + delimiter.size();
					}
					parts.push_back(terminated.substr(position));
				}
			} else if (!null_delimiter) {
				// Native emits the trailing NULL part without entering find.
				parts.emplace_back();
			}
			const int64_t output_first = std::abs(static_cast<int64_t>(output));
			if (!parts.empty()) {
				if (output_first + parts.size() > INT32_MAX) Output::Error("Failed to create string-variables.");
				auto* strings = output < 0 ? self_bank() : nullptr;
				if (strings) {
					strings->strings.resize(std::max(strings->strings.size(), static_cast<size_t>(output_first + parts.size() + 1)));
					strings->string_vector_size = strings->strings.size();
				} else Main_Data::game_strings->SetManiacVectorSize(static_cast<int>(output_first + parts.size()));
				for (size_t part = 0; part < parts.size(); ++part) {
					const int slot = static_cast<int>(output_first + part);
					const auto text = Utils::EncodeUTF(parts[part], true);
					if (strings) {
						strings->strings[slot] = lcf::DBString(text);
						Game_ManiacSelfVariables::SetStringPresence(strings, slot, false);
					} else Main_Data::game_strings->SetManiacRawSlot(slot, text, !text.empty());
				}
			}
			const int32_t segments = static_cast<int32_t>(parts.size());
			if (args[2] != 0) {
				const int slot = static_cast<int>(count_first + index);
				if (args[2] < 0) self_bank()->numbers[slot] = segments;
				else {
					*Main_Data::game_variables->GetManiacWritable(slot) = segments;
					Game_Map::SetNeedRefreshForVarChange(slot);
				}
			}
			// The helper advances magnitude; the caller adds its count to the
			// signed base, so negative ranges can next move towards global0.
			output = static_cast<int32_t>(static_cast<uint32_t>(output) + segments);
		}
		break;
	}
	case 7: //toFile <fn(string filename, int encode)>  takes hex
	{
		std::string body;
		for (int64_t slot = first_slot; slot <= last_slot; ++slot) {
			const int id = self_target ? static_cast<int32_t>(0u - static_cast<uint32_t>(slot)) : static_cast<int>(slot);
			body += read_target(id);
		}
		const auto filename = expand_source(ToString(get_string_source(modes[0], args[0])));
		const int encoding = source_number(modes[1], args[1]);
		Game_Strings::ToFile(std::string_view(body), filename, encoding);
		break;
	}
	case 8: //popLine <fn(int output_str_id)> takes hex
		{
			const int output = modes[0] == 0 ? args[0] : modes[0] == 1
				? ResolveVariableIndirectAddress(args[0]) : ReadVariableIndirect(args[0]);
			const int64_t output_first = std::abs(static_cast<int64_t>(output));
			const int64_t count = last_slot - first_slot + 1;
			if (output_first + count > INT32_MAX) Output::Error("Failed to create string-variables.");
			Game_ManiacSelfVariables::Bank* bank = nullptr;
			if (self_target || output < 0) {
				if (!Main_Data::game_maniac_self_variables) Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
				const auto* frame = GetFramePtr();
				bank = frame ? Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
					frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
				if (!bank) Output::Error("Failed to create self-variables.");
			}
			const auto reserve = [&](bool self, int64_t first) {
				if (self) {
					bank->strings.resize(std::max(bank->strings.size(), static_cast<size_t>(first + count + 1)));
					bank->string_vector_size = bank->strings.size();
				} else Main_Data::game_strings->SetManiacVectorSize(static_cast<int>(first + count));
			};
			reserve(self_target, first_slot);
			reserve(output < 0, output_first);
			const auto write = [&](bool self, int slot, std::string_view text) {
				if (self) {
					bank->strings[slot] = lcf::DBString(text);
					Game_ManiacSelfVariables::SetStringPresence(bank, slot, false);
				} else Main_Data::game_strings->SetManiacRawSlot(slot, text, !text.empty());
			};
			for (int64_t index = 0; index < count; ++index) {
				const int slot = static_cast<int>(first_slot + index);
				// Read each source after preceding overlapping output writes.
				const std::string source = self_target ? ToString(bank->strings[slot])
					: ToString(Main_Data::game_strings->GetManiacRawSlot(slot));
				const auto newline = source.find('\n');
				const auto prefix = expand_source(source.substr(0, newline));
				write(output < 0, static_cast<int>(output_first + index), prefix);
				write(self_target, slot, newline == std::string::npos ? std::string_view{} : std::string_view(source).substr(newline + 1));
			}
			break;
		}
	case 9: //exInStr <fn(string text, int var_id, int begin)>
	case 10: //exMatch <fn(string text, int var_id, int begin, int str_id)>, edge case: the only command that generates 8 parameters instead of 7
	{
		const auto address = [&](int mode, int raw) {
			return mode == 0 ? raw : mode == 1 ? ResolveVariableIndirectAddress(raw) : ReadVariableIndirect(raw);
		};
		const int output = address(modes[1], args[1]);
		const int begin = source_number(modes[2], args[2]);
		// Resolve capture output before numeric writes can change its operand.
		const int capture_output = op == 10 ? address(modes[3], args[3]) : 0;
		const int64_t count = last_slot - first_slot + 1;
		const int64_t output_first = std::abs(static_cast<int64_t>(output));
		if (output_first + count > INT32_MAX) Output::Error("Failed to create variables.");
		const auto self_bank = [&]() {
			if (!Main_Data::game_maniac_self_variables) Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
			const auto* frame = GetFramePtr();
			auto* bank = frame ? Main_Data::game_maniac_self_variables->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) Output::Error("Failed to create self-variables.");
			return bank;
		};
		Game_ManiacSelfVariables::Bank* output_bank = output < 0 ? self_bank() : nullptr;
		if (output_bank) output_bank->numbers.resize(std::max(output_bank->numbers.size(), static_cast<size_t>(output_first + count + 1)));
		else Main_Data::game_variables->EnsureSize(static_cast<size_t>(output_first + count));
		const std::string expr = expand_source(ToString(get_string_source(modes[0], args[0])));
		std::vector<std::string> captures;
		for (int64_t index = 0; index < count; ++index) {
			const int id = self_target ? static_cast<int32_t>(0u - static_cast<uint32_t>(first_slot + index)) : static_cast<int>(first_slot + index);
			const int value = Game_Strings::ManiacRegExSearch(read_target(id), expr, begin, op == 10 ? &captures : nullptr);
			const int slot = static_cast<int>(output_first + index);
			if (output_bank) output_bank->numbers[slot] = value;
			else {
				*Main_Data::game_variables->GetManiacWritable(slot) = value;
				Game_Map::SetNeedRefreshForVarChange(slot);
			}
		}
		if (op == 10) {
			const int64_t capture_first = std::abs(static_cast<int64_t>(capture_output));
			if (capture_first + captures.size() > INT32_MAX) Output::Error("Failed to create string-variables.");
			auto* bank = capture_output < 0 ? self_bank() : nullptr;
			if (bank) {
				bank->strings.resize(std::max(bank->strings.size(), static_cast<size_t>(capture_first + captures.size() + 1)));
				bank->string_vector_size = bank->strings.size();
			} else Main_Data::game_strings->SetManiacVectorSize(static_cast<int>(capture_first + captures.size()));
			for (size_t index = 0; index < captures.size(); ++index) {
				const int slot = static_cast<int>(capture_first + index);
				if (bank) {
					bank->strings[slot] = lcf::DBString(captures[index]);
					Game_ManiacSelfVariables::SetStringPresence(bank, slot, false);
				} else Main_Data::game_strings->SetManiacRawSlot(slot, captures[index], !captures[index].empty());
			}
		}
		break;
	}
	default:
		Output::Warning("Unknown or unimplemented string operation {}", op);
		break;
	}
	return true;
}

bool Game_Interpreter::CommandManiacCallCommand(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}
	if (maniac_generated_command_pending) {
		// YieldRepeat is still the same native dispatch. Busy/false retries
		// instead regenerate below, including their expression side effects.
		const bool result = DispatchCommand(*maniac_generated_command_pending);
		ManiacPatch::SetGeneratedCommonNameCache(maniac_generated_command_pending->easyrpg_maniac_common_name_cache);
		if (!result || _async_op.GetType() != AsyncOp::eYieldRepeat) maniac_generated_command_pending.reset();
		return result;
	}

	enum class ProcessingMode {
		Constant = 0, // 0 and 1: Parameters read from variables
		Variable = 1,
		VariableIndirect = 2,
		Inline = 3, // Parameters are directly provided by the command
		Expression = 4 // Like 3, but the parameters are calculated from expressions
	};

	std::vector<int32_t> values;
	const auto parameter = [&](int index) -> int32_t {
		return index >= 0 && static_cast<size_t>(index) < com.parameters.size() ? com.parameters[index] : 0;
	};
	const uint32_t modes = static_cast<uint32_t>(parameter(0));
	const auto numeric_mode = [&](int mode, int32_t raw) -> int32_t {
		return mode == 0 ? raw : mode == 1 ? ReadVariable(raw) : ReadVariableIndirect(raw);
	};

	// Create command with basic parameters
	lcf::rpg::EventCommand cmd;
	cmd.easyrpg_maniac_common_name_cache = ManiacPatch::GetGeneratedCommonNameCache();
	cmd.code = numeric_mode(modes & 15, parameter(1));
	cmd.indent = com.indent;
	if (cmd.code == static_cast<int32_t>(Cmd::Maniac_CallCommand)) {
		return true; // Native 3019 skips a generated 3019 before building its arguments.
	}

	const int text_mode = (modes >> 12) & 15;
	cmd.string = lcf::DBString(text_mode == 0 ? std::string_view(com.string)
		: text_mode == 1 ? ReadString(parameter(4)) : ReadString(ReadVariable(parameter(4))));

	// Determine processing mode
	auto processing_mode = static_cast<ProcessingMode>((modes >> 4) & 15);

	switch (processing_mode) {
	case ProcessingMode::Constant:
	case ProcessingMode::Variable:
	case ProcessingMode::VariableIndirect:
	default: {
		const int start_index = numeric_mode(static_cast<int>(processing_mode), parameter(2));
		const int length = numeric_mode((modes >> 8) & 15, parameter(3));
		if (start_index == 0 || length < 1) break;
		const int64_t high = std::abs(static_cast<int64_t>(start_index)) + length;
		if (high + 1 > UINT32_MAX) {
			Output::Error("Call Command: Vector length exceeds uint32 range.");
		}

		if (start_index < 0) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) break;
			bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(high + 1)));
		} else {
			Main_Data::game_variables->EnsureSize(static_cast<size_t>(high + 1));
		}
		for (int i = 0; i < length; ++i) {
			const int id = static_cast<int32_t>(start_index < 0
				? static_cast<uint32_t>(start_index) - static_cast<uint32_t>(i)
				: static_cast<uint32_t>(start_index) + static_cast<uint32_t>(i));
			values.push_back(ReadVariable(id));
		}
		break;
	}
	case ProcessingMode::Inline: {
		const int mode_index = parameter(2);
		const int length = std::max(0, numeric_mode((modes >> 8) & 15, parameter(3)));

		for (int i = 0; i < length; ++i) {
			const int64_t word_index = static_cast<int64_t>(mode_index) + i / 4;
			const uint32_t word = word_index >= 0 && word_index < com.parameters.size()
				? static_cast<uint32_t>(com.parameters[static_cast<size_t>(word_index)]) : 0;
			const int mode = (word >> ((i % 4) * 4)) & 15;
			values.push_back(numeric_mode(mode, parameter(5 + i)));
		}
		break;
	}
	case ProcessingMode::Expression: {
		if (com.parameters.size() > 5) {
			values = ManiacPatch::ParseExpressions(MakeSpan(com.parameters).subspan(5), *this);
		}
		break;
	}
	}

	// Finalize command parameters
	cmd.parameters = lcf::DBArray<int32_t>(values.begin(), values.end());

	// Debug output
	/*Output::Warning("Processing mode: {}", static_cast<int>(processing_mode));
	Output::Warning("Command code: {}", cmd.code);
	Output::Warning("Command string: {}", cmd.string);
	std::string params_str;
	for (const auto& param : values) {
		params_str += " " + std::to_string(param);
	}
	Output::Warning("Command parameters:{}", params_str);
	Output::Info("--------------------\n");*/

	// Native redispatches the generated command in this interpreter invocation.
	// A busy command returns false against the original 3019 cursor and is regenerated on retry.
	const bool result = DispatchCommand(cmd);
	ManiacPatch::SetGeneratedCommonNameCache(cmd.easyrpg_maniac_common_name_cache);
	if (result && _async_op.GetType() == AsyncOp::eYieldRepeat) maniac_generated_command_pending = std::move(cmd);
	return result;
}

bool Game_Interpreter::CommandEasyRpgSetInterpreterFlag(lcf::rpg::EventCommand const& com) {
	if (!Player::HasEasyRpgExtensions()) {
		return true;
	}

#ifndef ENABLE_DYNAMIC_INTERPRETER_CONFIG
	Output::Warning("CommandEasyRpgSetInterpreterFlag: Not supported on this platform");
	return true;
#else
	constexpr std::array<std::pair<const char*, int>, 9> config_names = {{
		{ "destiny",          1 },
		{ "dynrpg",           2 },
		{ "maniac",           3 },
		{ "common-this",      4 },
		{ "pic-unlock",       5 },
		{ "key-patch",        6 },
		{ "rpg2k3-cmds",      7 },
		{ "rpg2k3-commands",  7 },
		{ "rpg2k-battle",     8 }
	}};

	std::string flag_name = Utils::LowerCase(ToString(com.string));
	int flag_value = ValueOrVariable(com.parameters[0], com.parameters[1]);
	int flag_id = 0;

	if (flag_name.empty() && com.parameters.size() > 2) {
		flag_id = com.parameters[2];
	} else {
		auto it = std::find_if(config_names.begin(), config_names.end(), [&flag_name](auto& p) { return p.first == flag_name; });
		if (it != config_names.end()) {
			flag_id = it->second;
		}
	}

	// Clear any inactive flags that might be left over from a previous InterpreterFlag command
	if (!_state.easyrpg_runtime_flags.conf_override_active) {
		_state.easyrpg_runtime_flags.flags.fill(false);
	}

	switch (flag_id) {
		case 1:
			if (flag_value) {
				_state.easyrpg_runtime_flags.patch_destiny_on = true;
			} else {
				_state.easyrpg_runtime_flags.patch_destiny_off = true;
			}
			break;
		case 2:
			if (flag_value) {
				_state.easyrpg_runtime_flags.patch_dynrpg_on = true;
			} else {
				_state.easyrpg_runtime_flags.patch_dynrpg_off = true;
			}
			break;
		case 3:
			if (flag_value) {
				_state.easyrpg_runtime_flags.patch_maniac_on = true;
			} else {
				_state.easyrpg_runtime_flags.patch_maniac_off = true;
			}
			break;
		case 4:
			if (flag_value) {
				_state.easyrpg_runtime_flags.patch_common_this_event_on = true;
			} else {
				_state.easyrpg_runtime_flags.patch_common_this_event_off = true;
			}
			break;
		case 5:
			if (flag_value) {
				_state.easyrpg_runtime_flags.patch_unlock_pics_on = true;
			} else {
				_state.easyrpg_runtime_flags.patch_unlock_pics_off = true;
			}
			break;
		case 6:
			if (flag_value) {
				_state.easyrpg_runtime_flags.patch_keypatch_on = true;
			} else {
				_state.easyrpg_runtime_flags.patch_keypatch_off = true;
			}
			break;
		case 7:
			if (flag_value) {
				_state.easyrpg_runtime_flags.patch_rpg2k3_cmds_on = true;
			} else {
				_state.easyrpg_runtime_flags.patch_rpg2k3_cmds_off = true;
			}
			break;
		case 8:
			if (flag_value) {
				_state.easyrpg_runtime_flags.use_rpg2k_battle_system_on = true;
			} else {
				_state.easyrpg_runtime_flags.use_rpg2k_battle_system_off = true;
			}
			break;
		default:
			return true;
	}
	_state.easyrpg_runtime_flags.conf_override_active = true;
#endif

	return true;
}

bool Game_Interpreter::CommandEasyRpgProcessJson(lcf::rpg::EventCommand const& com) {
	if (!Player::HasEasyRpgExtensions()) {
		return true;
	}

#ifndef HAVE_NLOHMANN_JSON
	Output::Warning("CommandEasyRpgProcessJson: JSON not supported on this platform");
	return true;
#else

	// Helper lambda for getting values from variables
	auto get_var_value = [](int var_type, int var_id) -> std::string {
		switch (var_type) {
		case 0: // Switch
			return std::to_string(Main_Data::game_switches->Get(var_id));
		case 1: // Variable
			return std::to_string(Main_Data::game_variables->Get(var_id));
		case 2: // String
			return ToString(Main_Data::game_strings->Get(var_id));
		default:
			Output::Warning("CommandEasyRpgProcessJson: Unsupported var_type {}", var_type);
			return {};
		}
		};

	// Helper lambda for setting values to variables
	auto set_var_value = [](int var_type, int var_id, const std::string& value) -> bool {
		switch (var_type) {
		case 0: // Switch
			Main_Data::game_switches->Set(var_id, atoi(value.c_str()) != 0);
			break;
		case 1: // Variable
			Main_Data::game_variables->Set(var_id, atoi(value.c_str()));
			break;
		case 2: // String
			Main_Data::game_strings->Asg(var_id, value);
			break;
		default:
			Output::Warning("CommandEasyRpgProcessJson: Unsupported var_type {}", var_type);
			return false;
		}
		return true;
		};

	int operation = ValueOrVariable(com.parameters[0], com.parameters[1]);

	json* json_data = nullptr;
	std::optional<json> json_data_imm;

	int source_var_id = -1;

	int pos = 0;
	std::string json_path = Main_Data::game_strings->GetWithModeAndPos(com.string, com.parameters[8], com.parameters[9], pos, *Main_Data::game_variables);

	if (com.parameters[2] == 0) {
		std::string json_str = Main_Data::game_strings->GetWithModeAndPos(com.string, com.parameters[2], com.parameters[3], pos, *Main_Data::game_variables);
		json_data_imm = Json_Helper::Parse(json_str);
		if (json_data_imm) {
			json_data = &*json_data_imm;
		} else {
			Output::Warning("JSON Parse error for {}", json_str);
			return true;
		}
	} else {
		source_var_id = ValueOrVariable(com.parameters[2] - 1, com.parameters[3]);
		json_data = Main_Data::game_strings->ParseJson(source_var_id);

		if (!json_data) {
			Output::Warning("JSON Parse error for {}", Main_Data::game_strings->Get(source_var_id));
			return true;
		}
	}

	int target_var_type = ValueOrVariable(com.parameters[4], com.parameters[5]);
	int target_var_id = ValueOrVariable(com.parameters[6], com.parameters[7]);

	int flags = com.parameters[10];
	bool pretty_print = (flags & 4) == 4;

	if ((flags & 1) == 1) { // parse command codes
		json_path = Game_Strings::Extract(json_path, false);
	} else if ((flags & 2) == 2) { // parse command codes, numbers as hex
		json_path = Game_Strings::Extract(json_path, true);
	}

	if (!Json_Helper::CheckJsonPointer(json_path)) {
		return true;
	}

	if (target_var_type == 2 && !Player::IsPatchManiac()) {
		Output::Warning("CommandEasyRpgProcessJson: String operations require Maniac Patch support");
		return true;
	}

	std::optional<std::string> result;

	switch (operation) {
	case 0: { // Get operation: Extract a value from JSON data
		result = Json_Helper::GetValue(*json_data, json_path);
		if (result) {
			set_var_value(target_var_type, target_var_id, *result);
		}
		break;
	}
	case 1: { // Set operation: Update JSON data with a new value
		if (json_data_imm) {
			Output::Warning("CommandEasyRpgProcessJson: Cannot modify constant JSON string");
			return true;
		}

		std::string new_value = get_var_value(target_var_type, target_var_id);
		result = Json_Helper::SetValue(*json_data, json_path, new_value);
		if (result) {
			Main_Data::game_strings->Asg(source_var_id, *result);
		}
		break;
	}
	case 2: { // GetLength operation
		size_t length = Json_Helper::GetLength(*json_data, json_path);
		std::string length_str;
		if (target_var_type == 0) {
			// For switches, true if length > 0
			length_str = (length > 0) ? "1" : "0";
		}
		else {
			length_str = std::to_string(length);
		}
		set_var_value(target_var_type, target_var_id, length_str);
		break;
	}
	case 3: { // GetKeys operation
		bool create_keys_obj = (flags & 8) == 8;
		auto keys = Json_Helper::GetKeys(*json_data, json_path);
		std::string keys_str;
		for (size_t i = 0; i < keys.size(); ++i) {
			if (i > 0) keys_str += ",";
			keys_str += "\"" + (keys)[i] + "\"";
		}
		std::string json_str = "[" + keys_str + "]";
		if (create_keys_obj) {
			json_str = fmt::format(R"({{ "keys": {} }})", json_str);
		}
		set_var_value(target_var_type, target_var_id, json_str);
		break;
	}
	case 4: { // GetType operation
		std::string type = Json_Helper::GetType(*json_data, json_path);
		std::string value;
		if (target_var_type == 0) {
			// For switches, true if it exists and not null
			value = (!type.empty() && type != "null");
		}
		else if (target_var_type == 1) {
			// For variables, numeric code for type
			int type_code = 0;
			if (type == "object") type_code = 1;
			else if (type == "array") type_code = 2;
			else if (type == "string") type_code = 3;
			else if (type == "number") type_code = 4;
			else if (type == "boolean") type_code = 5;
			else if (type == "null") type_code = 6;
			value = std::to_string(type_code);
		}
		else {
			value = type;
		}
		set_var_value(target_var_type, target_var_id, value);
		break;
	}
	case 5: { // Remove operation: Remove value from JSON data
		if (json_data_imm) {
			Output::Warning("CommandEasyRpgProcessJson: Cannot modify constant JSON string");
			return true;
		}

		std::string result = Json_Helper::RemoveValue(*json_data, json_path);
		if (!result.empty()) {
			Main_Data::game_strings->Asg(source_var_id, result);
		}
		break;
	}
	case 6: { // Push operation: Add value to end of array
		if (json_data_imm) {
			Output::Warning("CommandEasyRpgProcessJson: Cannot modify constant JSON string");
			return true;
		}

		std::string value = get_var_value(target_var_type, target_var_id);
		std::string result = Json_Helper::PushValue(*json_data, json_path, value);
		if (!result.empty()) {
			Main_Data::game_strings->Asg(source_var_id, result);
		}
		break;
	}
	case 7: { // Pop operation: Remove and return last element of array
		auto [json_obj, element] = Json_Helper::PopValue(*json_data, json_path);
		if (!json_obj.empty()) {
			// Set popped value to target variable
			set_var_value(target_var_type, target_var_id, element);
			// Update source with modified JSON after pop
			if (!json_data_imm) {
				Main_Data::game_strings->Asg(source_var_id, json_obj);
			}
		}
		break;
	}
	case 8: { // Contains operation: Check if path exists
		bool exists = Json_Helper::Contains(*json_data, json_path);
		set_var_value(target_var_type, target_var_id, exists ? "1" : "0");
		break;
	}
	default:
		Output::Warning("CommandEasyRpgProcessJson: Invalid Operation {}", operation);
	}

	if (target_var_type == 2 && pretty_print == 1) { // Only works with strings
		std::string target_str = ToString(Main_Data::game_strings->Get(target_var_id));
		if (auto parsed_json = Json_Helper::Parse(target_str)) {
			std::string formatted = Json_Helper::PrettyPrint(*parsed_json, 2);
			Main_Data::game_strings->Asg({ target_var_id }, formatted);
		}
	}

	return true;

#endif // !HAVE_NLOHMANN_JSON
}

bool Game_Interpreter::CommandEasyRpgCloneMapEvent(lcf::rpg::EventCommand const& com) {
	if (!Player::HasEasyRpgExtensions()) {
		return true;
	}

	if (com.parameters.size() < 8) {
		return true;
	}

	int src_map = ValueOrVariable(com.parameters[0], com.parameters[1]);
	int src_event = ValueOrVariable(com.parameters[2], com.parameters[3]);
	int target_x = ValueOrVariable(com.parameters[4], com.parameters[5]);
	int target_y = ValueOrVariable(com.parameters[6], com.parameters[7]);
	int target_event = ValueOrVariable(com.parameters[8], com.parameters[9]);

	std::string target_name = ToString(CommandStringOrVariable(com, 10, 11));

	if (src_map == 0) {
		src_map = Game_Map::GetMapId();
	} else {
		auto* request = Game_Map::RequestMap(src_map);
		request->Start();

		if (!request->IsReady()) {
			// Download the map and try again
			_async_op = AsyncOp::MakeYieldRepeat();
			return true;
		}
	}

	_async_op = AsyncOp::MakeCloneMapEvent(target_name, src_event, target_event, src_map, target_x, target_y);

	return true;
}

bool Game_Interpreter::CommandEasyRpgDestroyMapEvent(lcf::rpg::EventCommand const& com) {
	if (!Player::HasEasyRpgExtensions()) {
		return true;
	}

	int target_event = ValueOrVariable(com.parameters[0], com.parameters[1]);

	if (target_event == 0 || target_event == Game_Character::CharThisEvent) {
		target_event = GetThisEventId();
	}

	_async_op = AsyncOp::MakeDestroyMapEvent(target_event);

	return true;
}

Game_Interpreter& Game_Interpreter::GetForegroundInterpreter() {
	return Game_Battle::IsBattleRunning()
		? Game_Battle::GetInterpreter()
		: Game_Map::GetInterpreter();
}

bool Game_Interpreter::IsWaitingForWaitCommand() const {
	return (_state.wait_time > 0) || _state.wait_key_enter;
}

int Game_Interpreter::ManiacBitmask(int value, int mask) const {
	if (Player::IsPatchManiac()) {
		return value & mask;
	}

	return value;
}
