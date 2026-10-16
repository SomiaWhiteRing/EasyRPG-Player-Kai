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
#include "game_actors.h"
#include "game_battle.h"
#include "game_battlealgorithm.h"
#include "game_enemyparty.h"
#include "game_interpreter_battle.h"
#include "game_party.h"
#include "game_switches.h"
#include "game_system.h"
#include "game_variables.h"
#include "game_maniac_self_variables.h"
#include <lcf/data.h>
#include <lcf/reader_util.h>
#include "main_data.h"
#include "output.h"
#include "player.h"
#include "game_map.h"
#include "input.h"
#include "spriteset_battle.h"
#include <cassert>
#include "scene_battle.h"
#include "utils.h"
#include "game_clock.h"
#include <lcf/scope_guard.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

enum BranchBattleSubcommand {
	eOptionBranchBattleElse = 1
};

enum TargetType {
	Actor,
	Member,
	Enemy,
};

// Implemented as a static map, since maniac hooks can only have one common event callback at a time.
// Subsequent calls will simply override the previous common event callback.
std::map<Game_Interpreter_Battle::ManiacBattleHookType, std::tuple<int, int>> Game_Interpreter_Battle::maniac_hooks;

void Game_Interpreter_Battle::InitBattle() {
	if (Player::IsPatchManiac()) {
		Game_Interpreter_Battle::maniac_hooks = {
			{Game_Interpreter_Battle::ManiacBattleHookType::AtbIncrement, std::make_tuple(0, 0)},
			{Game_Interpreter_Battle::ManiacBattleHookType::DamagePop, std::make_tuple(0, 0)},
			{Game_Interpreter_Battle::ManiacBattleHookType::Targetting, std::make_tuple(0, 0)},
			{Game_Interpreter_Battle::ManiacBattleHookType::SetState, std::make_tuple(0, 0)},
			{Game_Interpreter_Battle::ManiacBattleHookType::StatChange, std::make_tuple(0, 0)}
		};
	}
}

static const char* target_text[] = { "actor", "party member", "enemy" };

static void MissingTargetWarning(const char* command_name, TargetType target_type, int target_id) {
	Output::Warning("{}: Invalid {} ID: {}", command_name, target_text[target_type], target_id);
}

// Provides a facility for battle sub-events to be run immediately
// without blocking the standard interpreter from actually processing them.
std::unique_ptr<Game_Interpreter_Battle> maniac_interpreter;
std::unique_ptr<Game_Interpreter_Battle> maniac_state_interpreter;
bool maniac_state_hook_running = false;
std::unique_ptr<Game_Interpreter_Battle> maniac_param_interpreter;
bool maniac_param_hook_running = false;
std::unique_ptr<Game_Interpreter_Battle> maniac_damage_interpreter;
bool maniac_damage_hook_running = false;
std::unique_ptr<Game_Interpreter_Battle> maniac_atb_interpreter;
std::unique_ptr<Game_Interpreter_Battle> maniac_target_interpreter;

Game_Interpreter_Battle::Game_Interpreter_Battle(Span<const lcf::rpg::TroopPage> pages, int troop_id)
	: Game_Interpreter(true), pages(pages), executed(pages.size(), false), troop_id(troop_id)
{
	maniac_interpreter.reset(new Game_Interpreter_Battle());
	maniac_state_interpreter.reset(new Game_Interpreter_Battle());
	maniac_state_hook_running = false;
	maniac_param_interpreter.reset(new Game_Interpreter_Battle());
	maniac_param_hook_running = false;
	maniac_damage_interpreter.reset(new Game_Interpreter_Battle());
	maniac_damage_hook_running = false;
	maniac_atb_interpreter.reset(new Game_Interpreter_Battle());
	maniac_target_interpreter.reset(new Game_Interpreter_Battle());
}

Game_Interpreter_Battle::Game_Interpreter_Battle(bool main_flag)
	: Game_Interpreter(main_flag)
{
}

bool Game_Interpreter_Battle::OnFinishStackFrame() {
	const bool has_parent_frame = Game_Interpreter::OnFinishStackFrame();
	if (Player::IsPatchManiac()
			&& !Game_Battle::IsManiacHookPumping()) {
		// Only the native mode-2 battle interpreter polls input after cleanup.
		// The five independent hook interpreters have mode 0.
		Input::Update();
		Input::Update();
	}
	return has_parent_frame;
}

bool Game_Interpreter_Battle::AreConditionsMet(const lcf::rpg::TroopPageCondition& condition, Game_Battler* source) {
	if (!condition.flags.switch_a &&
		!condition.flags.switch_b &&
		!condition.flags.variable &&
		!condition.flags.turn &&
		!condition.flags.turn_enemy &&
		!condition.flags.turn_actor &&
		!condition.flags.fatigue &&
		!condition.flags.enemy_hp &&
		!condition.flags.actor_hp &&
		!condition.flags.command_actor
		) {
		// Pages without trigger are never run
		return false;
	}

	if (condition.flags.switch_a && !Main_Data::game_switches->Get(condition.switch_a_id))
		return false;

	if (condition.flags.switch_b && !Main_Data::game_switches->Get(condition.switch_b_id))
		return false;

	if (condition.flags.variable && !(Main_Data::game_variables->Get(condition.variable_id) >= condition.variable_value))
		return false;

	if (condition.flags.turn && !Game_Battle::CheckTurns(Game_Battle::GetTurn(), condition.turn_b, condition.turn_a))
		return false;

	if (Player::IsRPG2k3Commands() && condition.flags.turn_enemy) {
		const auto* enemy = Main_Data::game_enemyparty->GetEnemy(condition.turn_enemy_id);
		if (!enemy) {
			Output::Warning("AreConditionsMet: Invalid enemy ID {}", condition.turn_enemy_id);
			return false;
		}

		if (source && source != enemy)
			return false;
		if (!Game_Battle::CheckTurns(enemy->GetBattleTurn(), condition.turn_enemy_b, condition.turn_enemy_a))
			return false;
	}

	if (Player::IsRPG2k3Commands() && condition.flags.turn_actor) {
		const auto* actor = Main_Data::game_actors->GetActor(condition.turn_actor_id);
		if (!actor) {
			Output::Warning("AreConditionsMet: Invalid actor ID {}", condition.turn_actor_id);
			return false;
		}

		if (source && source != actor)
			return false;
		if (!Game_Battle::CheckTurns(actor->GetBattleTurn(), condition.turn_actor_b, condition.turn_actor_a))
			return false;
	}

	if (Player::IsRPG2k3Commands() && condition.flags.fatigue) {
		int fatigue = Main_Data::game_party->GetFatigue();
		if (fatigue < condition.fatigue_min || fatigue > condition.fatigue_max)
			return false;
	}

	if (condition.flags.enemy_hp) {
		const auto* enemy = Main_Data::game_enemyparty->GetEnemy(condition.enemy_id);
		if (!enemy) {
			Output::Warning("AreConditionsMet: Invalid enemy ID {}", condition.enemy_id);
			return false;
		}

		int result = 100 * enemy->GetHp() / enemy->GetMaxHp();
		if (result < condition.enemy_hp_min || result > condition.enemy_hp_max)
			return false;
	}

	if (condition.flags.actor_hp) {
		const auto* actor = Main_Data::game_actors->GetActor(condition.actor_id);
		if (!actor) {
			Output::Warning("AreConditionsMet: Invalid actor ID {}", condition.actor_id);
			return false;
		}

		int result = 100 * actor->GetHp() / actor->GetMaxHp();
		if (result < condition.actor_hp_min || result > condition.actor_hp_max)
			return false;
	}

	if (Player::IsRPG2k3Commands() && condition.flags.command_actor) {
		if (!source)
			return false;
		const auto* actor = Main_Data::game_actors->GetActor(condition.command_actor_id);
		if (!actor) {
			Output::Warning("AreConditionsMet: Invalid actor ID {}", condition.command_actor_id);
			return false;
		}

		if (source != actor)
			return false;
		if (condition.command_id != actor->GetLastBattleAction())
			return false;
	}

	return true;
}

int Game_Interpreter_Battle::ScheduleNextPage(Game_Battler* source) {
	lcf::rpg::TroopPageCondition::Flags f;
	for (auto& ff: f.flags) ff = true;

	return ScheduleNextPage(f, source);
}

static bool HasRequiredCondition(lcf::rpg::TroopPageCondition::Flags page, lcf::rpg::TroopPageCondition::Flags required) {
	for (size_t i = 0; i < page.flags.size(); ++i) {
		if (required.flags[i] && page.flags[i]) {
			return true;
		}
	}
	return false;
}

int Game_Interpreter_Battle::ScheduleNextPage(lcf::rpg::TroopPageCondition::Flags required_conditions, Game_Battler* source) {
	if (IsRunning()) {
		return 0;
	}

	for (size_t i = 0; i < pages.size(); ++i) {
		auto& page = pages[i];
		if (executed[i]
				|| !HasRequiredCondition(page.condition.flags, required_conditions)
				|| !AreConditionsMet(page.condition, source)) {
			continue;
		}
		Clear();
		Push(page.event_commands, 0, false, Player::IsPatchManiac() ? page.ID : 0);
		if (Player::IsPatchManiac()) {
			auto& frame = GetFrame();
			frame.maniac_event_info = 0x40;
			frame.maniac_event_id = troop_id;
		}
		executed[i] = true;
		return i + 1;
	}
	return 0;
}

// Execute Command.
bool Game_Interpreter_Battle::ExecuteCommand(lcf::rpg::EventCommand const& com) {
	switch (static_cast<Cmd>(com.code)) {
		case Cmd::CallCommonEvent:
			return CmdSetup<&Game_Interpreter_Battle::CommandCallCommonEvent, 1>(com);
		case Cmd::ForceFlee:
			return CmdSetup<&Game_Interpreter_Battle::CommandForceFlee, 3>(com);
		case Cmd::EnableCombo:
			return CmdSetup<&Game_Interpreter_Battle::CommandEnableCombo, 3>(com);
		case Cmd::ChangeMonsterHP:
			return CmdSetup<&Game_Interpreter_Battle::CommandChangeMonsterHP, 5>(com);
		case Cmd::ChangeMonsterMP:
			return CmdSetup<&Game_Interpreter_Battle::CommandChangeMonsterMP, 4>(com);
		case Cmd::ChangeMonsterCondition:
			return CmdSetup<&Game_Interpreter_Battle::CommandChangeMonsterCondition, 3>(com);
		case Cmd::ShowHiddenMonster:
			return CmdSetup<&Game_Interpreter_Battle::CommandShowHiddenMonster, 1>(com);
		case Cmd::ChangeBattleBG:
			return CmdSetup<&Game_Interpreter_Battle::CommandChangeBattleBG, 0>(com);
		case Cmd::ShowBattleAnimation_B:
			return CmdSetup<&Game_Interpreter_Battle::CommandShowBattleAnimation, 3>(com);
		case Cmd::TerminateBattle:
			return CmdSetup<&Game_Interpreter_Battle::CommandTerminateBattle, 0>(com);
		case Cmd::ConditionalBranch_B:
			return CmdSetup<&Game_Interpreter_Battle::CommandConditionalBranchBattle, 5>(com);
		case Cmd::ElseBranch_B:
			return CmdSetup<&Game_Interpreter_Battle::CommandElseBranchBattle, 0>(com);
		case Cmd::EndBranch_B:
			return CmdSetup<&Game_Interpreter_Battle::CommandEndBranchBattle, 0>(com);
		case Cmd::Maniac_ControlBattle:
			return CmdSetup<&Game_Interpreter_Battle::CommandManiacControlBattle, 4>(com);
		case Cmd::Maniac_ControlAtbGauge:
			return CmdSetup<&Game_Interpreter_Battle::CommandManiacControlAtbGauge, 7>(com);
		case Cmd::Maniac_ChangeBattleCommandEx:
			return CmdSetup<&Game_Interpreter_Battle::CommandManiacChangeBattleCommandEx, 2>(com);
		case Cmd::Maniac_GetBattleInfo:
			return CmdSetup<&Game_Interpreter_Battle::CommandManiacGetBattleInfo, 5>(com);
		default:
			return Game_Interpreter::ExecuteCommand(com);
	}
}

// Commands

bool Game_Interpreter_Battle::CommandCallCommonEvent(lcf::rpg::EventCommand const& com) {
	if (Player::IsPatchManiac()) {
		return CommandManiacCallCommonEvent(com);
	}
	if (!Player::IsRPG2k3Commands()) {
		return true;
	}

	int evt_id = com.parameters[0];

	Game_CommonEvent* common_event = lcf::ReaderUtil::GetElement(Game_Map::GetCommonEvents(), evt_id);
	if (!common_event) {
		Output::Warning("CallCommonEvent: Can't call invalid common event {}", evt_id);
		return true;
	}

	Push(common_event);

	return true;
}

bool Game_Interpreter_Battle::CommandForceFlee(lcf::rpg::EventCommand const& com) {
	if (!Player::IsRPG2k3Commands()) {
		return true;
	}

	bool check = com.parameters[2] == 0;

	switch (com.parameters[0]) {
	case 0:
		if (!check || Game_Battle::GetBattleCondition() != lcf::rpg::System::BattleCondition_pincers) {
			this->force_flee_enabled = true;
		}
		break;
	case 1:
		if (!check || Game_Battle::GetBattleCondition() != lcf::rpg::System::BattleCondition_surround) {
			int num_escaped = 0;
			for (auto* enemy: Main_Data::game_enemyparty->GetEnemies()) {
				if (enemy->Exists()) {
					enemy->SetHidden(true);
					enemy->SetDeathTimer();
					++num_escaped;
				}
			}
			if (num_escaped) {
				Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Game_System::SFX_Escape));
			}
		}
		break;
	case 2:
		if (!check || Game_Battle::GetBattleCondition() != lcf::rpg::System::BattleCondition_surround) {
			auto* enemy = Main_Data::game_enemyparty->GetEnemy(com.parameters[1]);
			if (!enemy) {
				Output::Warning("ForceFlee: Invalid enemy ID {}", com.parameters[1]);
				return true;
			}

			if (enemy->Exists()) {
				enemy->SetHidden(true);
				enemy->SetDeathTimer();
				Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Game_System::SFX_Escape));
			}
		}
		break;
	}

	return true;
}

bool Game_Interpreter_Battle::CommandEnableCombo(lcf::rpg::EventCommand const& com) {
	if (!Player::IsRPG2k3Commands()) {
		return true;
	}

	const bool maniac = Player::IsPatchManiac();
	const uint32_t modes = maniac && com.parameters.size() > 3 ? com.parameters[3] : 0;
	const auto operand = [&](int index) {
		const int mode = (modes >> (index * 4)) & 15;
		const int value = com.parameters[index];
		return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
	};
	const int actor_id = operand(0);
	const int command_id = operand(1);
	const int multiple = operand(2);

	if (!maniac && !Main_Data::game_party->IsActorInParty(actor_id)) {
		return true;
	}

	Game_Actor* actor = Main_Data::game_actors->GetActor(actor_id);

	if (!actor) {
		Output::Warning("EnableCombo: Invalid actor ID {}", actor_id);
		return true;
	}

	actor->SetBattleCombo(command_id, maniac ? std::max(1, multiple) : multiple);

	return true;
}

bool Game_Interpreter_Battle::CommandChangeMonsterHP(lcf::rpg::EventCommand const& com) {
	auto* enemy = Main_Data::game_enemyparty->GetEnemy(com.parameters[0]);
	if (!enemy) {
		Output::Warning("ChangeMonsterHP: Invalid enemy ID {}", com.parameters[0]);
		return true;
	}

	bool lose = com.parameters[1] > 0;
	bool lethal = com.parameters[4] > 0;
	int hp = enemy->GetHp();

	if (enemy->IsDead())
		return true;

	int change = 0;
	switch (com.parameters[2]) {
	case 0:
		change = com.parameters[3];
		break;
	case 1:
		change = Main_Data::game_variables->Get(com.parameters[3]);
		break;
	case 2:
		change = com.parameters[3] * hp / 100;
		break;
	}

	if (lose) {
		change = -change;
	}

	enemy->ChangeHp(change, lethal);

	auto& scene = Scene::instance;
	if (scene) {
		scene->OnEventHpChanged(enemy, change);
	}

	if (enemy->IsDead()) {
		Main_Data::game_system->SePlay(Main_Data::game_system->GetSystemSE(Main_Data::game_system->SFX_EnemyKill));
		enemy->SetDeathTimer();
	}

	return true;
}

bool Game_Interpreter_Battle::CommandChangeMonsterMP(lcf::rpg::EventCommand const& com) {
	auto* enemy = Main_Data::game_enemyparty->GetEnemy(com.parameters[0]);
	if (!enemy) {
		Output::Warning("ChangeMonsterMP: Invalid enemy ID {}", com.parameters[0]);
		return true;
	}

	bool lose = com.parameters[1] > 0;
	int sp = enemy->GetSp();

	int change = 0;
	switch (com.parameters[2]) {
	case 0:
		change = com.parameters[3];
		break;
	case 1:
		change = Main_Data::game_variables->Get(com.parameters[3]);
		break;
	}

	if (lose)
		change = -change;

	sp += change;

	enemy->SetSp(sp);

	return true;
}

bool Game_Interpreter_Battle::CommandChangeMonsterCondition(lcf::rpg::EventCommand const& com) {
	auto* enemy = Main_Data::game_enemyparty->GetEnemy(com.parameters[0]);
	if (!enemy) {
		Output::Warning("ChangeMonsterCondition: Invalid enemy ID {}", com.parameters[0]);
		return true;
	}

	bool remove = com.parameters[1] > 0;
	int state_id = com.parameters[2];
	if (remove) {
		// RPG_RT BUG: Monster dissapears immediately and doesn't animate death
		enemy->RemoveState(state_id, false);
	} else {
		enemy->AddState(state_id, true);
	}
	return true;
}

bool Game_Interpreter_Battle::CommandShowHiddenMonster(lcf::rpg::EventCommand const& com) {
	auto* enemy = Main_Data::game_enemyparty->GetEnemy(com.parameters[0]);
	if (!enemy) {
		Output::Warning("ShowHiddenMonster: Invalid enemy ID {}", com.parameters[0]);
		return true;
	}

	enemy->SetHidden(false);
	return true;
}

bool Game_Interpreter_Battle::CommandChangeBattleBG(lcf::rpg::EventCommand const& com) {
	Game_Battle::ChangeBackground(ToString(com.string));
	return true;
}

bool Game_Interpreter_Battle::CommandShowBattleAnimation(lcf::rpg::EventCommand const& com) {
	int animation_id = com.parameters[0];
	int target = com.parameters[1];
	bool waiting_battle_anim = com.parameters[2] != 0;
	bool allies = false;

	if (Player::IsRPG2k3() && com.parameters.size() > 3) {
		allies = com.parameters[3] != 0;
	}

	int frames = 0;

	if (target < 0) {
		std::vector<Game_Battler*> v;

		if (allies) {
			Main_Data::game_party->GetBattlers(v);
		} else {
			Main_Data::game_enemyparty->GetBattlers(v);
		}
		auto iter = std::remove_if(v.begin(), v.end(),
				[](auto* target) { return !(target->Exists() || (target->GetType() == Game_Battler::Type_Ally && target->IsDead())); });
		v.erase(iter, v.end());

		frames = Game_Battle::ShowBattleAnimation(animation_id, v, false);
	}
	else {
		Game_Battler* battler_target = nullptr;

		if (allies) {
			if (Player::IsPatchManiac()) {
				// Maniac selects a party member by actor ID, not party position.
				if (Main_Data::game_party->IsActorInParty(target)) {
					battler_target = Main_Data::game_actors->GetActor(target);
				}
			} else {
				// Ordinary 2k3 allies are counted from 1.
				target -= 1;
				if (target >= 0 && target < Main_Data::game_party->GetBattlerCount()) {
					battler_target = &(*Main_Data::game_party)[target];
				}
			}
		}
		else {
			if (target < Main_Data::game_enemyparty->GetBattlerCount()) {
				battler_target = &(*Main_Data::game_enemyparty)[target];
			}
		}

		if (battler_target) {
			frames = Game_Battle::ShowBattleAnimation(animation_id, { battler_target });
		}
	}

	if (waiting_battle_anim) {
		_state.wait_time = frames;
	}

	return true;
}

bool Game_Interpreter_Battle::CommandTerminateBattle(lcf::rpg::EventCommand const& /* com */) {
	_async_op = AsyncOp::MakeTerminateBattle(static_cast<int>(BattleResult::Abort));
	return false;
}

// Conditional branch.
bool Game_Interpreter_Battle::CommandConditionalBranchBattle(lcf::rpg::EventCommand const& com) {
	bool result = false;
	int value1, value2;
	const bool maniac = Player::IsPatchManiac();
	const auto arg = [&](size_t index) { return index < com.parameters.size() ? com.parameters[index] : 0; };
	const auto resolve = [&](int value, int mode) {
		return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
	};
	const auto can_act = [](Game_Battler& battler) {
		if (!battler.CanManiacMove()) return false;
		const auto& states = battler.GetStates();
		for (size_t i = 0; i < lcf::Data::states.size(); ++i) {
			const auto restriction = lcf::Data::states[i].restriction;
			if (static_cast<uint16_t>(states[i]) != 0
				&& (restriction == lcf::rpg::State::Restriction_attack_enemy
					|| restriction == lcf::rpg::State::Restriction_attack_ally)) return false;
		}
		return true;
	};

	switch (maniac ? arg(0) : com.parameters[0]) {
		case 0:
			// Switch
			result = maniac ? (ReadSwitch(arg(1)) != 0) == (arg(2) == 0)
				: Main_Data::game_switches->Get(com.parameters[1]) == (com.parameters[2] == 0);
			break;
		case 1:
			// Variable
			value1 = maniac ? ReadVariable(arg(1)) : Main_Data::game_variables->Get(com.parameters[1]);
			if (maniac) {
				value2 = resolve(arg(3), arg(2));
			} else if (com.parameters[2] == 0) {
				value2 = com.parameters[3];
			} else {
				value2 = Main_Data::game_variables->Get(com.parameters[3]);
			}
			switch (maniac ? arg(4) : com.parameters[4]) {
				case 0:
					// Equal to
					result = (value1 == value2);
					break;
				case 1:
					// Greater than or equal
					result = (value1 >= value2);
					break;
				case 2:
					// Less than or equal
					result = (value1 <= value2);
					break;
				case 3:
					// Greater than
					result = (value1 > value2);
					break;
				case 4:
					// Less than
					result = (value1 < value2);
					break;
				case 5:
					// Different
					result = (value1 != value2);
					break;
			}
			break;
		case 2: {
			// Hero can act
			const int actor_id = maniac ? resolve(arg(1), arg(2) & 15) : com.parameters[1];
			auto* actor = Main_Data::game_actors->GetActor(actor_id);

			if (!actor) {
				if (maniac) Output::Error("ConditionalBranchBattle: Invalid actor ID: {}", actor_id);
				Output::Warning("ConditionalBranchBattle: Invalid actor ID {}", actor_id);
			} else {
				result = maniac ? can_act(*actor) : actor->CanAct();
			}
			break;
		}
		case 3: {
			// Monster can act
			const int enemy_id = maniac ? resolve(arg(1), arg(2) & 15) : com.parameters[1];
			auto* enemy = Main_Data::game_enemyparty->GetEnemy(enemy_id);

			if (!enemy) {
				if (maniac) Output::Error("ConditionalBranchBattle: Invalid enemy ID: {}", enemy_id);
				Output::Warning("ConditionalBranchBattle: Invalid enemy ID {}", enemy_id);
			} else {
				result = maniac ? can_act(*enemy) : enemy->CanAct();
			}
			break;
		}
		case 4:
			// Monster is the current target
			if (maniac) {
				const auto& context = Game_Battle::GetPrimaryInterpreterBattle();
				const int enemy_index = resolve(arg(1), arg(2) & 15);
				result = (context.current_maniac_target_type == 3 || context.current_maniac_target_type == 4)
					&& context.target_enemy_index == enemy_index;
			} else if (Player::IsRPG2k3Commands()) {
				result = (targets_single_enemy && target_enemy_index == com.parameters[1]);
			}
			break;
		case 5: {
			// Hero uses the ... command
			if (maniac) {
				const int actor_id = resolve(arg(1), arg(3) & 15);
				const int command_id = resolve(arg(2), (arg(3) >> 4) & 15);
				if (Game_Battle::GetPrimaryInterpreterBattle().current_actor_id == actor_id) {
					auto* actor = Main_Data::game_actors->GetActor(actor_id);
					if (!actor) Output::Error("ConditionalBranchBattle: Invalid actor ID: {}", actor_id);
					result = actor->GetLastBattleAction() == command_id;
				}
			} else if (Player::IsRPG2k3Commands() && current_actor_id == com.parameters[1]) {
				auto *actor = Main_Data::game_actors->GetActor(current_actor_id);
				if (actor) {
					result = actor->GetLastBattleAction() == com.parameters[2];
				}
			}
			break;
		}
		case 6: {
			if (!maniac) break;
			const int enemy_id = resolve(arg(1), arg(3) & 15);
			const int state_id = resolve(arg(2), (arg(3) >> 4) & 15);
			auto* enemy = Main_Data::game_enemyparty->GetEnemy(enemy_id);
			if (!enemy) Output::Error("ConditionalBranchBattle: Invalid enemy ID: {}", enemy_id);
			if (state_id <= 0 || static_cast<size_t>(state_id) > lcf::Data::states.size()) {
				Output::Error("ConditionalBranchBattle: Invalid state ID: {}", state_id);
			}
			auto& states = enemy->GetStates();
			states.resize(std::max(states.size(), lcf::Data::states.size()));
			// The native branch consumes AL from the unsigned state counter getter.
			result = static_cast<uint8_t>(states[state_id - 1]) != 0;
			break;
		}
	}

	int sub_idx = subcommand_sentinel;
	if (!result) {
		sub_idx = eOptionBranchBattleElse;
		SkipToNextConditional({Cmd::ElseBranch_B, Cmd::EndBranch_B}, com.indent);
	}

	SetSubcommandIndex(com.indent, sub_idx);
	return true;
}

bool Game_Interpreter_Battle::CommandElseBranchBattle(lcf::rpg::EventCommand const& com) { //code 23310
	return CommandOptionGeneric(com, eOptionBranchBattleElse, {Cmd::EndBranch_B});
}

bool Game_Interpreter_Battle::CommandEndBranchBattle(lcf::rpg::EventCommand const& /* com */) { //code 23311
	return true;
}

bool Game_Interpreter_Battle::ManiacBattleHook(ManiacBattleHookType hook_type, int var1, int var2, int var3, int var4, int var5, int var6, int* hook_result) {
	if (!Player::IsPatchManiac()) {
		return false;
	}
	const bool native_state = hook_type == ManiacBattleHookType::SetState;
	if (native_state && maniac_state_hook_running) return false;
	const bool native_param = hook_type == ManiacBattleHookType::StatChange;
	if (native_param && maniac_param_hook_running) return false;
	const bool native_damage = hook_type == ManiacBattleHookType::DamagePop;
	if (native_damage && maniac_damage_hook_running) return false;
	const bool native_atb = hook_type == ManiacBattleHookType::AtbIncrement;
	const bool native_target = hook_type == ManiacBattleHookType::Targetting;
	auto* hook_interpreter = native_state ? maniac_state_interpreter.get()
		: native_param ? maniac_param_interpreter.get()
		: native_damage ? maniac_damage_interpreter.get()
		: native_atb ? maniac_atb_interpreter.get()
		: native_target ? maniac_target_interpreter.get() : maniac_interpreter.get();

	int common_event_id = std::get<0>(maniac_hooks[hook_type]);
	int variable_start_id = std::get<1>(maniac_hooks[hook_type]);

	if (common_event_id == 0) {
		return false;
	}

	Game_CommonEvent* common_event = lcf::ReaderUtil::GetElement(Game_Map::GetCommonEvents(), common_event_id);
	if (!common_event) {
		Output::Error("CommandManiacControlBattle: Can't call invalid common event {}", common_event_id);
		return false;
	}

	const size_t packet_size = hook_type == ManiacBattleHookType::AtbIncrement ? 4
		: hook_type == ManiacBattleHookType::SetState ? 3 : 6;
	const int values[] = {var1, var2, var3, var4, var5, var6};
	const int64_t first = variable_start_id < 0 ? -static_cast<int64_t>(variable_start_id) : variable_start_id;
	if (variable_start_id < 0) {
		const auto* frame = GetFramePtr();
		auto* state = Main_Data::game_maniac_self_variables.get();
		auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
			frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
		if (bank) {
			bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(first + packet_size + 1)));
			std::copy_n(values, packet_size, bank->numbers.begin() + first);
		}
	} else {
		Main_Data::game_variables->EnsureSize(static_cast<size_t>(first + packet_size + 1));
		for (size_t i = 0; i < packet_size; ++i) {
			*Main_Data::game_variables->GetManiacWritable(static_cast<int>(static_cast<uint32_t>(variable_start_id) + i)) = values[i];
		}
	}
	// The native packet belongs to the calling interpreter before switching
	// to the hook's independent SELF frame.
	hook_interpreter->Push(common_event);

	// Necessary to start the sub-event.
	const auto hook_scene = Scene::instance;
	const auto retire_hook = [&]() {
		if (!hook_interpreter->IsAsyncPending()) return false;
		const auto operation = hook_interpreter->GetAsyncOp();
		const auto type = operation.GetType();
		if (type != AsyncOp::eLoad && type != AsyncOp::eExitGame
				&& type != AsyncOp::eToTitle && type != AsyncOp::eTerminateBattle) return false;
		// Stop the suspended caller as well as this hook. The old scene is
		// retained until both C++ call stacks return safely.
		_async_op = operation;
		hook_interpreter->EndEventProcessing();
		if (Scene::instance == hook_scene) {
			if (auto battle = std::dynamic_pointer_cast<Scene_Battle>(hook_scene)) {
				battle->HandleManiacHookOperation(operation);
			}
		}
		return true;
	};
	if (native_state || native_param || native_damage) {
		auto& running = native_state ? maniac_state_hook_running
			: native_param ? maniac_param_hook_running : maniac_damage_hook_running;
		running = true;
		auto* previous = Game_Battle::SetManiacActiveInterpreter(hook_interpreter);
		auto restore = lcf::makeScopeGuard([previous, &running]() {
			Game_Battle::SetManiacActiveInterpreter(previous);
			running = false;
		});
		const auto scene = Scene::instance;
		hook_interpreter->Update();
		if (retire_hook()) return true;
		while (hook_interpreter->IsRunning() && Game_Battle::IsBattleRunning()
				&& Scene::instance == scene && !Player::exit_flag && !Player::reset_flag) {
			// Battle updates temporarily execute this hook rather than re-entering
			// its suspended caller. This also advances waits, messages and timers.
			Player::Update();
			if (retire_hook()) return true;
			Player::Draw();
			#ifdef __EMSCRIPTEN__
			// Host keyboard/stop messages need the worker event loop while a
			// pumped hook keeps the caller suspended.
			emscripten_sleep(1000 / Game_Clock::GetTargetGameFps());
			#else
			Game_Clock::SleepFor(Game_Clock::GetTargetGameTimeStep());
			#endif
		}
	} else if (native_atb || native_target) {
		// These hooks run one slice and return even when a wait is pending.
		// A different hook owns a separate wait/stack, and readback below is
		// performed after restoring the caller.
		auto* previous = Game_Battle::SetManiacActiveInterpreter(hook_interpreter);
		auto restore = lcf::makeScopeGuard([previous]() {
			Game_Battle::SetManiacActiveInterpreter(previous);
		});
		hook_interpreter->Update();
		if (retire_hook()) return true;
	} else {
		hook_interpreter->Update();
	}
	if (hook_type == ManiacBattleHookType::Targetting && hook_result) {
		// Native reads slot five from the base captured before the hook ran.
		const uint32_t slot = static_cast<uint32_t>(variable_start_id)
			+ (variable_start_id < 0 ? -5u : 5u);
		*hook_result = ReadVariable(static_cast<int32_t>(slot));
	} else if (hook_type == ManiacBattleHookType::AtbIncrement && hook_result) {
		// Native reads the increment from the currently registered ATB base.
		const int base = std::get<1>(maniac_hooks[hook_type]);
		const uint32_t slot = static_cast<uint32_t>(base) + (base < 0 ? -3u : 3u);
		*hook_result = ReadVariable(static_cast<int32_t>(slot));
	}

	return true;
}

bool Game_Interpreter_Battle::CommandManiacControlBattle(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	int common_event_id = 0;
	const int mode = com.parameters[1] & 15;
	const int reference = com.parameters[2];
	if (mode == 0) common_event_id = reference;
	else if (mode == 1) common_event_id = ReadVariable(reference);
	else if (mode == 2) common_event_id = ReadVariableIndirect(reference);
	else if (mode >= 3 && mode <= 5) {
		const auto text = mode == 3 ? std::string_view(com.string)
			: ReadString(mode == 4 ? reference : ResolveVariableIndirectAddress(reference));
		const auto name = Utils::DecodeUTF16(text.substr(0, text.find('\0')), true);
		common_event_id = mode == 3 ? com.easyrpg_maniac_common_name_cache : 0;
		if (common_event_id == 0) {
			for (const auto& candidate : lcf::Data::commonevents) {
				if (Utils::DecodeUTF16(static_cast<std::string_view>(candidate.name), true) == name) {
					common_event_id = candidate.ID;
					break;
				}
			}
			if (mode == 3) com.easyrpg_maniac_common_name_cache = common_event_id;
		}
	}
	const int selector = com.parameters[0];
	if (selector >= 0 && selector <= 4) {
		maniac_hooks[static_cast<ManiacBattleHookType>(selector)] = {common_event_id, com.parameters[3]};
	}
	return true;
}

bool Game_Interpreter_Battle::CommandManiacControlAtbGauge(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	if (!Game_Battle::IsBattleRunning()) {
		return true;
	}
	auto resolve = [this](int mode, int value) {
		return mode == 0 ? value : mode == 1 ? ReadVariable(value) : ReadVariableIndirect(value);
	};
	int amount = resolve(com.parameters[5], com.parameters[6]);
	if (com.parameters[4] != 0) {
		amount = static_cast<int32_t>(static_cast<uint32_t>(amount) * uint32_t{3000});
	}
	const int target_id = resolve(com.parameters[1], com.parameters[2]);
	auto apply = [amount, target_id, &com](Game_Battler* battler, TargetType type) {
		if (!battler) {
			Output::Error("ManiacControlAtbGauge: Invalid {} ID: {}", target_text[type], target_id);
			return;
		}
		const auto old = static_cast<uint32_t>(battler->GetAtbGauge());
		switch (com.parameters[3]) {
			case 0: battler->SetManiacAtbGauge(amount); break;
			case 1: battler->SetManiacAtbGauge(static_cast<int32_t>(old + static_cast<uint32_t>(amount))); break;
			case 2: battler->SetManiacAtbGauge(static_cast<int32_t>(old - static_cast<uint32_t>(amount))); break;
		}
	};
	switch (com.parameters[0]) {
		case 0: apply(Main_Data::game_actors->GetActor(target_id), Actor); break;
		case 1: apply(Main_Data::game_party->GetActor(target_id), Member); break;
		case 2:
			for (auto* battler : Main_Data::game_party->GetActors()) apply(battler, Member);
			break;
		case 3: apply(Main_Data::game_enemyparty->GetEnemy(target_id), Enemy); break;
		case 4:
			for (auto* battler : Main_Data::game_enemyparty->GetEnemies()) apply(battler, Enemy);
			break;
	}

	return true;
}

bool Game_Interpreter_Battle::CommandManiacChangeBattleCommandEx(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}
	if (Scene::instance && Scene::instance->type == Scene::Battle && Game_Battle::IsBattleRunning()) {
		const auto arg = [&](size_t i) { return i < com.parameters.size() ? com.parameters[i] : 0; };
		static_cast<Scene_Battle*>(Scene::instance.get())->SetManiacBattleCommands(arg(0) != 0, arg(1));
	}
	return true;
}

bool Game_Interpreter_Battle::CommandManiacGetBattleInfo(lcf::rpg::EventCommand const& com) {
	if (!Player::IsPatchManiac()) {
		return true;
	}

	if (!Game_Battle::IsBattleRunning()) return true;
	const int selector = com.parameters[0];
	const int property = com.parameters[1];
	const int mode = com.parameters[2];
	const int target = mode == 0 ? com.parameters[3] : mode == 1
		? ReadVariable(com.parameters[3]) : ReadVariableIndirect(com.parameters[3]);
	const int base = com.parameters[4];
	const auto write = [&](const std::vector<int32_t>& values, size_t capacity) {
		const int64_t first = base < 0 ? -static_cast<int64_t>(base) : base;
		if (base < 0) {
			const auto* frame = GetFramePtr();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = frame && state ? state->Ensure(frame->easyrpg_self_scope,
				frame->easyrpg_self_map_id, frame->easyrpg_self_event_id) : nullptr;
			if (!bank) return;
			bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(first) + capacity + 1));
			std::copy(values.begin(), values.end(), bank->numbers.begin() + first);
		} else {
			Main_Data::game_variables->EnsureSize(static_cast<size_t>(first) + capacity + 1);
			for (size_t i = 0; i < values.size(); ++i) {
				auto* slot = Main_Data::game_variables->GetManiacWritable(static_cast<int>(first + i));
				if (slot) *slot = values[i];
			}
		}
	};
	const auto movable = [](Game_Battler& battler) {
		return battler.CanManiacMove();
	};
	if (selector == 2 || selector == 4) {
		std::vector<Game_Battler*> members;
		if (selector == 2) Main_Data::game_party->GetBattlers(members);
		else Main_Data::game_enemyparty->GetBattlers(members);
		std::vector<int32_t> values(1, 0);
		for (size_t i = 0; i < members.size(); ++i) {
			auto& member = *members[i];
			const bool matches = property == 0 ? !member.IsHidden()
				: property == 1 ? member.GetHp() > 0
				: property == 2 ? movable(member) : false;
			if (matches) values.push_back(static_cast<int32_t>(i));
		}
		values[0] = static_cast<int32_t>(values.size() - 1);
		write(values, members.size() + 1);
		return true;
	}
	Game_Battler* battler = nullptr;
	switch (selector) {
		case 0: battler = Main_Data::game_actors->GetActor(target); break;
		case 1: battler = Main_Data::game_party->GetActor(target); break;
		case 3: battler = Main_Data::game_enemyparty->GetEnemy(target); break;
		default: return true;
	}
	if (!battler) {
		Output::Error("ManiacGetBattleInfo: Invalid {} ID: {}", selector == 3 ? "enemy" : "actor", target);
	}
	std::vector<int32_t> values;
	if (property == 0) {
		values = {battler->GetAtkModifier(), battler->GetDefModifier(), battler->GetSpiModifier(), battler->GetAgiModifier()};
	} else if (property == 1) {
		const auto& states = battler->GetStates();
		values.push_back(static_cast<int32_t>(states.size()));
		for (const auto state : states) values.push_back(static_cast<uint16_t>(state));
	} else if (property == 2) {
		values = battler->GetManiacAttributeRates();
	} else if (property == 3) {
		const auto action = battler->GetBattleAlgorithm();
		// Native retains basic Defense after removing it from the action queue.
		const bool defending_action = action
			? action->GetActionType() == lcf::rpg::EnemyAction::Kind_basic
				&& action->GetActionId() == lcf::rpg::EnemyAction::Basic_defense
			: battler->IsDefending();
		values = {battler->GetBattlePosition().x, battler->GetBattlePosition().y,
			movable(*battler), defending_action, battler->IsCharged(), !battler->IsHidden()};
	} else {
		return true;
	}
	write(values, values.size());
	return true;
}

