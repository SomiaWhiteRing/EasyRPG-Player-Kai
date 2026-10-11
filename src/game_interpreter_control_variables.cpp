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

#include "game_interpreter_shared.h"
#include "game_interpreter_control_variables.h"
#include "game_actors.h"
#include "game_battle.h"
#include "game_enemyparty.h"
#include "game_event.h"
#include "game_ineluki.h"
#include "game_interpreter.h"
#include "game_party.h"
#include "game_player.h"
#include "game_system.h"
#include "game_variables.h"
#include "game_vehicle.h"
#include "main_data.h"
#include "output.h"
#include "player.h"
#include "rand.h"
#include "utils.h"
#include "audio.h"
#include <cmath>
#include <cstdint>
#include <lcf/rpg/savepartylocation.h>

namespace {

bool IsManiac250921() {
	return Player::IsPatchManiac() && Player::game_config.maniac_version.Get() == 250921;
}

int SignedBits(uint32_t bits) {
	return bits <= INT32_MAX ? static_cast<int>(bits) : static_cast<int>(static_cast<int64_t>(bits) - (int64_t{1} << 32));
}

int TruncateNative(double value) {
	// CVTTSD2SI returns the indefinite integer for NaN or an out-of-range result.
	if (!(value >= INT32_MIN && value < static_cast<double>(INT32_MAX) + 1.0)) return INT32_MIN;
	return static_cast<int>(value);
}

int BoundNative(double value) {
	value = std::max(value, static_cast<double>(Main_Data::game_variables->GetMinValue()));
	value = std::min(value, static_cast<double>(Main_Data::game_variables->GetMaxValue()));
	return TruncateNative(value);
}

int BoundNative(int64_t value) {
	return static_cast<int>(std::clamp(value, static_cast<int64_t>(Main_Data::game_variables->GetMinValue()),
		static_cast<int64_t>(Main_Data::game_variables->GetMaxValue())));
}

}

int ControlVariables::Random(int value, int value2) {
	if (Player::IsPatchManiac()) {
		return Main_Data::ManiacRandomInclusive(value, value2);
	}
	int rmax = std::max(value, value2);
	int rmin = std::min(value, value2);

	return Rand::GetRandomNumber(rmin, rmax);
}

int ControlVariables::Item(int op, int item) {
	switch (op) {
		case 0:
			// Number of items posessed
			return Main_Data::game_party->GetItemCount(item);
			break;
		case 1:
			// How often the item is equipped
			return Main_Data::game_party->GetEquippedItemCount(item);
			break;
	}

	Output::Warning("ControlVariables::Item: Unknown op {}", op);
	return 0;
}

int ControlVariables::Actor(int op, int actor_id) {
	auto actor = Main_Data::game_actors->GetActor(actor_id);

	if (!actor) {
		if (IsManiac250921()) Output::Error("ControlVariables::Actor: Actor {} was not found.", actor_id);
		Output::Warning("ControlVariables::Actor: Bad actor_id {}", actor_id);
		return 0;
	}

	switch (op) {
		case 0:
			// Level
			return actor->GetLevel();
			break;
		case 1:
			// Experience
			return actor->GetExp();
			break;
		case 2:
			// Current HP
			return actor->GetHp();
			break;
		case 3:
			// Current MP
			return actor->GetSp();
			break;
		case 4:
			// Max HP
			return actor->GetMaxHp();
			break;
		case 5:
			// Max MP
			return actor->GetMaxSp();
			break;
		case 6:
			// Attack
			return actor->GetAtk();
			break;
		case 7:
			// Defense
			return actor->GetDef();
			break;
		case 8:
			// Intelligence
			return actor->GetSpi();
			break;
		case 9:
			// Agility
			return actor->GetAgi();
			break;
		case 10:
			// Weapon ID
			return actor->GetWeaponId();
			break;
		case 11:
			// Shield ID
			return actor->GetShieldId();
			break;
		case 12:
			// Armor ID
			return actor->GetArmorId();
			break;
		case 13:
			// Helmet ID
			return actor->GetHelmetId();
			break;
		case 14:
			// Accessory ID
			return actor->GetAccessoryId();
			break;
		case 15:
			// ID
			if (Player::IsPatchManiac()) {
				return actor->GetId();
			}
			break;
		case 16:
			// ATB
			if (Player::IsPatchManiac()) {
				if (IsManiac250921()) return std::min(actor->GetAtbGauge(), Game_Battler::GetMaxAtbGauge()) / 3000;
				return actor->GetAtbGauge();
			}
			break;
		case 17:
			if (IsManiac250921()) return actor->GetManiacNextExp();
			break;
	}

	Output::Warning("ControlVariables::Actor: Unknown op {}", op);
	return 0;
}

int ControlVariables::Party(int op, int party_idx) {
	auto actor = Main_Data::game_party->GetActor(party_idx);

	if (!actor) {
		if (IsManiac250921()) return ControlVariables::Actor(op, 0);
		Output::Warning("ControlVariables::Party: Bad party_idx {}", party_idx);
		return 0;
	}

	return ControlVariables::Actor(op, actor->GetId());
}

int ControlVariables::Event(int op, int event_id, const Game_BaseInterpreterContext& interpreter) {
	auto character = interpreter.GetCharacter(event_id, "ControlVariables::Event");
	if (!character && IsManiac250921()) Output::Error("ControlVariables::Event: Character {} was not found.", event_id);
	if (character) {
		switch (op) {
			case 0:
				// Map ID
				if (!Player::IsRPG2k()
					|| event_id == Game_Character::CharPlayer
					|| event_id == Game_Character::CharBoat
					|| event_id == Game_Character::CharShip
					|| event_id == Game_Character::CharAirship) {
					return character->GetMapId();
				} else {
					// This is an RPG_RT bug for 2k only. Requesting the map id of an event always returns 0.
					return 0;
				}
				break;
			case 1:
				// X Coordinate
				return character->GetX();
				break;
			case 2:
				// Y Coordinate
				return character->GetY();
				break;
			case 3:
				// Orientation
				int dir;
				dir = character->GetFacing();
				return dir == 0 ? 8 :
						dir == 1 ? 6 :
						dir == 2 ? 2 : 4;
				break;
			case 4: {
				// Screen X
				if (Player::game_config.fake_resolution.Get()) {
					int pan_delta = (Game_Player::GetDefaultPanX() - lcf::rpg::SavePartyLocation::kPanXDefault) / TILE_SIZE;
					return character->GetScreenX() - pan_delta;
				} else {
					return character->GetScreenX();
				}
			}
			case 5: {
				// Screen Y
				if (Player::game_config.fake_resolution.Get()) {
					int pan_delta = (Game_Player::GetDefaultPanY() - lcf::rpg::SavePartyLocation::kPanYDefault) / TILE_SIZE;
					return character->GetScreenY() - pan_delta;
				} else {
					return character->GetScreenY();
				}
			}
			case 6:
				// Event ID
				if (IsManiac250921()) {
					switch (character->GetType()) {
						case Game_Character::Player: return Game_Character::CharPlayer;
						case Game_Character::Vehicle: return Game_Character::CharPlayer + static_cast<Game_Vehicle*>(character)->GetVehicleType();
						case Game_Character::Event: return static_cast<Game_Event*>(character)->GetId();
					}
				}
				return Player::IsPatchManiac() ? interpreter.GetThisEventId() : 0;
		}

		Output::Warning("ControlVariables::Event: Unknown op {}", op);
	}

	return 0;
}

int ControlVariables::Other(int op) {
	switch (op) {
		case 0:
			// Gold
			return Main_Data::game_party->GetGold();
			break;
		case 1:
			// Timer 1 remaining time
			return Main_Data::game_party->GetTimerSeconds(Main_Data::game_party->Timer1);
			break;
		case 2:
			// Number of heroes in party
			return Main_Data::game_party->GetActors().size();
			break;
		case 3:
			// Number of saves
			return Main_Data::game_system->GetSaveCount();
			break;
		case 4:
			// Number of battles
			return Main_Data::game_party->GetBattleCount();
			break;
		case 5:
			// Number of wins
			return Main_Data::game_party->GetWinCount();
			break;
		case 6:
			// Number of defeats
			return Main_Data::game_party->GetDefeatCount();
			break;
		case 7:
			// Number of escapes (aka run away)
			return Main_Data::game_party->GetRunCount();
			break;
		case 8:
			// MIDI play position
			if (Player::IsPatchKeyPatch()) {
				return Main_Data::game_ineluki->GetMidiTicks();
			} else {
				return Audio().BGM_GetTicks();
			}
			break;
		case 9:
			// Timer 2 remaining time
			return Main_Data::game_party->GetTimerSeconds(Main_Data::game_party->Timer2);
			break;
		case 10:
			// Current date (YYMMDD)
			if (Player::IsPatchManiac()) {
				std::time_t t = std::time(nullptr);
				std::tm* tm = std::localtime(&t);
				return atoi(Utils::FormatDate(tm, Utils::DateFormat_YYMMDD).c_str());
			}
			break;
		case 11:
			// Current time (HHMMSS)
			if (Player::IsPatchManiac()) {
				std::time_t t = std::time(nullptr);
				std::tm* tm = std::localtime(&t);
				return atoi(Utils::FormatDate(tm, Utils::DateFormat_HHMMSS).c_str());
			}
			break;
		case 12:
			// Frames
			if (Player::IsPatchManiac()) {
				return Main_Data::game_system->GetFrameCounter();
			}
			break;
		case 13:
			// Patch version
			if (Player::IsPatchManiac()) {
				if (IsManiac250921()) return SignedBits(250921u | (Game_Battle::IsBattleRunning() ? 0x80000000u : 0u));
				// Latest version before the engine rewrite
				return 200128;
			}
			break;
	}

	Output::Warning("ControlVariables::Other: Unknown op {}", op);
	return 0;
}

int ControlVariables::Enemy(int op, int enemy_idx) {
	auto enemy = Main_Data::game_enemyparty->GetEnemy(enemy_idx);

	if (!enemy) {
		if (IsManiac250921()) Output::Error("ControlVariables::Enemy: Enemy index {} was not found.", enemy_idx);
		Output::Warning("ControlVariables::Enemy: Bad enemy_idx {}", enemy_idx);
		return 0;
	}

	switch (op) {
		case 0:
			// Enemy HP
			return enemy->GetHp();
			break;
		case 1:
			// Enemy SP
			return enemy->GetSp();
			break;
		case 2:
			// Enemy MaxHP
			return enemy->GetMaxHp();
			break;
		case 3:
			// Enemy MaxSP
			return enemy->GetMaxSp();
			break;
		case 4:
			// Enemy Attack
			return enemy->GetAtk();
			break;
		case 5:
			// Enemy Defense
			return enemy->GetDef();
			break;
		case 6:
			// Enemy Spirit
			return enemy->GetSpi();
			break;
		case 7:
			// Enemy Agility
			return enemy->GetAgi();
			break;
		case 8:
			// ID
			if (Player::IsPatchManiac()) {
				return enemy->GetId();
			}
			break;
		case 9:
			// ATB
			if (Player::IsPatchManiac()) {
				if (IsManiac250921()) return std::min(enemy->GetAtbGauge(), Game_Battler::GetMaxAtbGauge()) / 3000;
				return enemy->GetAtbGauge();
			}
			break;
	}

	Output::Warning("ControlVariables::Enemy: Unknown op {}", op);
	return 0;
}

int ControlVariables::Pow(int arg1, int arg2) {
	if (IsManiac250921()) return BoundNative(std::pow(arg1, arg2));
	return static_cast<int>(std::pow(arg1, arg2));
}

int ControlVariables::Sqrt(int arg, int mul) {
	if (IsManiac250921()) {
		const int magnitude = arg < 0 ? SignedBits(0u - static_cast<uint32_t>(arg)) : arg;
		const int result = BoundNative(std::sqrt(static_cast<double>(magnitude)) * mul);
		return arg < 0 ? SignedBits(0u - static_cast<uint32_t>(result)) : result;
	}
	// This is not how negative sqrt works, just following the implementation here
	int res = static_cast<int>(sqrt(abs(arg)) * mul);
	if (arg < 0) {
		res = -res;
	}
	return res;
}

int ControlVariables::Sin(int arg1, int arg2, int mul) {
	if (IsManiac250921()) return TruncateNative(std::sin((static_cast<double>(arg1) / (arg2 == 0 ? 1 : arg2)) * M_PI / 180.0) * mul);
	float res = static_cast<float>(arg1);
	if (arg2 != 0) {
		res /= static_cast<float>(arg2);
	}
	return static_cast<int>(std::sin(res * M_PI / 180.f) * mul);
}

int ControlVariables::Cos(int arg1, int arg2, int mul) {
	if (IsManiac250921()) return TruncateNative(std::cos((static_cast<double>(arg1) / (arg2 == 0 ? 1 : arg2)) * M_PI / 180.0) * mul);
	float res = static_cast<float>(arg1);
	if (arg2 != 0) {
		res /= static_cast<float>(arg2);
	}
	return static_cast<int>(std::cos(res * M_PI / 180.f) * mul);
}

int ControlVariables::Atan2(int arg1, int arg2, int mul) {
	if (IsManiac250921()) return TruncateNative(std::atan2(arg1, arg2) * 57.2957795131 * mul);
	return static_cast<int>(std::atan2(arg1, arg2) * 180.f / M_PI * mul);
}

int ControlVariables::Min(int arg1, int arg2) {
	return std::min(arg1, arg2);
}

int ControlVariables::Max(int arg1, int arg2) {
	return std::max(arg1, arg2);
}

int ControlVariables::Abs(int arg) {
	if (IsManiac250921()) return arg < 0 ? SignedBits(0u - static_cast<uint32_t>(arg)) : arg;
	return abs(arg);
}

int ControlVariables::Binary(int op, int arg1, int arg2) {
	if (IsManiac250921()) {
		const int64_t a = arg1, b = arg2 == 0 ? 1 : arg2;
		const uint32_t lhs = static_cast<uint32_t>(arg1), rhs = static_cast<uint32_t>(arg2);
		switch (op) {
			case 1: return BoundNative(a + arg2);
			case 2: return BoundNative(a - arg2);
			case 3: return BoundNative(a * arg2);
			case 4: return BoundNative(a / b);
			case 5: return BoundNative(a % b);
			case 6: return SignedBits(lhs | rhs);
			case 7: return SignedBits(lhs & rhs);
			case 8: return SignedBits(lhs ^ rhs);
			case 9: return SignedBits(lhs << (rhs & 31));
			case 10: return SignedBits(lhs >> (rhs & 31));
			default: return 0;
		}
	}
	// 64 Bit for overflow protection
	int64_t result = 0;

	auto arg1_64 = static_cast<int64_t>(arg1);
	auto arg2_64 = static_cast<int64_t>(arg2);

	switch (op) {
		case 1:
			result = arg1_64 + arg2_64;
			break;
		case 2:
			result = arg1_64 - arg2_64;
			break;
		case 3:
			result = arg1_64 * arg2_64;
			break;
		case 4:
			if (arg2_64 != 0) {
				result = arg1_64 / arg2_64;
			} else {
				result = arg1_64;
			}
			break;
		case 5:
			if (arg2_64 != 0) {
				result = arg1_64 % arg2_64;
			} else {
				result = arg1_64;
			}
			break;
		case 6:
			result = arg1_64 | arg2_64;
			break;
		case 7:
			result = arg1_64 & arg2_64;
			break;
		case 8:
			result = arg1_64 ^ arg2_64;
			break;
		case 9:
			result = arg1_64 << arg2_64;
			break;
		case 10:
			result = arg1_64 >> arg2_64;
			break;
		default:
			Output::Warning("ControlVariables::Binary: Unknown op {}", op);
			return 0;
	}

	return static_cast<int>(Utils::Clamp<int64_t>(result, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

int ControlVariables::Clamp(int arg1, int arg2, int arg3) {
	if (IsManiac250921()) return std::min(std::max(arg1, arg2), arg3);
	return Utils::Clamp(arg1, arg2, arg3);
}

int ControlVariables::Muldiv(int arg1, int arg2, int arg3) {
	auto arg1_64 = static_cast<int64_t>(arg1);
	auto arg2_64 = static_cast<int64_t>(arg2);
	auto arg3_64 = static_cast<int64_t>(arg3);

	if (arg3_64 == 0) {
		arg3_64 = 1;
	}

	const int64_t result = arg1_64 * arg2_64 / arg3_64;
	return IsManiac250921() ? BoundNative(result) : static_cast<int>(result);
}

int ControlVariables::Divmul(int arg1, int arg2, int arg3) {
	auto arg1_64 = static_cast<int64_t>(arg1);
	auto arg2_d = static_cast<double>(arg2);
	auto arg3_64 = static_cast<int64_t>(arg3);

	if (arg2_d == 0) {
		arg2_d = 1.0;
	}

	const double result = arg1_64 / arg2_d * arg3_64;
	return IsManiac250921() ? BoundNative(result) : static_cast<int>(result);
}

int ControlVariables::Between(int arg1, int arg2, int arg3) {
	if (IsManiac250921()) return arg1 >= arg2 && arg1 <= arg3 ? 1 : 0;
	return (arg1 >= arg2 && arg2 <= arg3) ? 0 : 1;
}
