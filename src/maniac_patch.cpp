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

#include "maniac_patch.h"

#include "input.h"
#include "game_actors.h"
#include "game_interpreter_control_variables.h"
#include "game_map.h"
#include "game_interpreter.h"
#include "game_maniac_self_variables.h"
#include "game_party.h"
#include "game_switches.h"
#include "game_variables.h"
#include "main_data.h"
#include "output.h"
#include "player.h"

#include <lcf/reader_util.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

/*
The native runtime has additional malformed-stream and slot-zero behavior that
is not reproduced here. EasyRPG Extensions retain their existing policy for
in-place assignments.
*/

namespace {
	int32_t generated_common_name_cache = 0;
	template<typename T>
	const T* FindLcfStringRecord(std::string_view type, const std::vector<T>& records, int id) {
		if (!Player::IsPatchManiac()) {
			return lcf::ReaderUtil::GetElement(records, id);
		}
		const auto it = std::find_if(records.begin(), records.end(), [id](const T& record) {
			return record.ID == id;
		});
		if (it == records.end()) {
			Output::Error("GetLcfData: {} {} was not found.", type, id);
			return nullptr;
		}
		return &*it;
	}
	enum class Op {
		Null = 0,
		U8,
		U16,
		S32,
		UX8,
		UX16,
		SX32,
		Var = 8,
		Switch,
		SelfVar = 11,
		SelfSwitch,
		VarIndirect = 13,
		SwitchIndirect,
		SelfVarIndirect = 16,
		SelfSwitchIndirect,
		Array = 19,
		Abs = 20,
		AbsAlternate,
		SelfAddress,
		SelfAddressAlternate,
		Negate = 24,
		Not,
		Flip,
		AssignInplace = 34,
		AddInplace,
		SubInplace,
		MulInplace,
		DivInplace,
		ModInplace,
		BitOrInplace,
		BitAndInplace,
		BitXorInplace,
		BitShiftLeftInplace,
		BitShiftRightInplace,
		Add = 48,
		Sub,
		Mul,
		Div,
		Mod,
		BitOr,
		BitAnd,
		BitXor,
		BitShiftLeft,
		BitShiftRight,
		Equal,
		GreaterEqual,
		LessEqual,
		Greater,
		Less,
		NotEqual,
		Or,
		And,
		Range,
		Subscript,
		Ternary = 72,
		Function = 78
	};

	enum class Fn {
		Rand = 0,
		Item,
		Event,
		Actor,
		Party,
		Enemy,
		Misc,
		Pow,
		Sqrt,
		Sin,
		Cos,
		Atan2,
		Min,
		Max,
		Abs,
		Clamp,
		Muldiv,
		Divmul,
		Between,
		Lerp,
		ArraySum,
		ArrayMin,
		ArrayMax
	};
}

int32_t NegativeAbs32(int32_t value);
int32_t ClampExpressionArithmetic(int64_t value);
int32_t FromBits(uint32_t bits);
int32_t Abs32(int32_t value);
int32_t Negate32(int32_t value);

struct ProcessAssignmentRet {
	Op op = Op::Null;
	int id = 0;
	const Game_BaseInterpreterContext* interpreter = nullptr;
	bool prepared_range = false;

	int target() const {
		if (!interpreter) return 0;
		if (op == Op::VarIndirect || op == Op::SwitchIndirect) return interpreter->ReadVariable(id);
		if (op == Op::SelfVarIndirect || op == Op::SelfSwitchIndirect) {
			const int32_t address = Negate32(id);
			const int32_t value = interpreter->ReadVariable(address);
			return address < 0 ? NegativeAbs32(value) : value;
		}
		return id;
	}

	int fetch() const {
		if (!interpreter) return 0;
		const int address = target();
		if (address == 0 && (op == Op::Var || op == Op::SelfVar ||
			op == Op::VarIndirect || op == Op::SelfVarIndirect)) {
			return Main_Data::game_variables->GetManiacRawSlot(0);
		}
		if (address == 0 && (op == Op::Switch || op == Op::SelfSwitch ||
			op == Op::SwitchIndirect || op == Op::SelfSwitchIndirect)) {
			return Main_Data::game_switches->GetManiacRawSlot(0);
		}
		switch (op) {
		case Op::Var:
		case Op::SelfVar:
		case Op::VarIndirect:
		case Op::SelfVarIndirect:
			return interpreter->ReadVariable(address);
		case Op::Switch:
		case Op::SelfSwitch:
		case Op::SwitchIndirect:
		case Op::SelfSwitchIndirect:
			return interpreter->ReadSwitch(address);
		default:
			Output::Warning("Maniac: Expression assignment {} is not a lvalue", static_cast<int>(op));
			return 0;
		}
	}

	int assign(int value, bool compound = false) const {
		if (!interpreter) return 0;
		if (Player::HasEasyRpgExtensions()) {
			Output::Warning("Maniac: Inplace assignments are not allowed in expressions when running in EasyRpg Mode");
			return fetch();
		}

		const int address = target();
		const bool numeric = op == Op::Var || op == Op::SelfVar || op == Op::VarIndirect || op == Op::SelfVarIndirect;
		const bool boolean = op == Op::Switch || op == Op::SelfSwitch || op == Op::SwitchIndirect || op == Op::SelfSwitchIndirect;
		const bool maniac = Player::IsPatchManiac();
		if (!numeric && !boolean) {
			Output::Warning("Maniac: Expression assignment {} is not a lvalue", static_cast<int>(op));
			return 0;
		}
		// Native 8c730 sign-extends index + 1 before resizing. INT_MAX
		// therefore fails the vector's maximum-length check before allocation.
		if (maniac && numeric && !prepared_range && (address == INT32_MAX || address == -INT32_MAX)) {
			Output::Error("Maniac: Expression vector length exceeds signed index range.");
		}
		if (address < 0) {
			const auto& frame = interpreter->GetFrame();
			auto* state = Main_Data::game_maniac_self_variables.get();
			auto* bank = state ? state->Ensure(frame.easyrpg_self_scope,
				frame.easyrpg_self_map_id, frame.easyrpg_self_event_id) : nullptr;
			const int64_t slot = -static_cast<int64_t>(address);
			if (!bank || (!maniac && slot > 1000000)) return 0;
			if (numeric) {
				bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(slot + 1)));
				bank->numbers[static_cast<size_t>(slot)] = value;
			} else {
				bank->switches.resize(std::max(bank->switches.size(), static_cast<size_t>(slot + 1)));
				bank->switches[static_cast<size_t>(slot)] = value != 0;
			}
			Game_Map::SetNeedRefresh(true);
			return numeric ? value : value != 0;
		}
		if (!maniac && address > 1000000) return 0;
		if (numeric) {
			if (auto* destination = Main_Data::game_variables->GetManiacWritable(address)) {
				*destination = value;
				Game_Map::SetNeedRefreshForVarChange(address);
				return value;
			}
			return 0;
		}
		if (address == 0) {
			if (compound) Main_Data::game_switches->SetManiacRaw(0, value != 0);
			return value != 0;
		}
		Game_Map::SetNeedRefreshForSwitchChange(address);
		return Main_Data::game_switches->Set(address, value != 0);
	}

	int apply(int token, int rhs) const {
		if (token == static_cast<int>(Op::AssignInplace)) return assign(rhs);
		const int64_t old = fetch();
		const bool boolean = op == Op::Switch || op == Op::SelfSwitch ||
			op == Op::SwitchIndirect || op == Op::SelfSwitchIndirect;
		int32_t result = 0;
		switch (token) {
			case 35: result = ClampExpressionArithmetic(old + rhs); break;
			case 36: result = ClampExpressionArithmetic(old - rhs); break;
			case 37: result = ClampExpressionArithmetic(old * rhs); break;
			case 38: result = ClampExpressionArithmetic(old / (rhs == 0 ? 1 : rhs)); break;
			case 39: result = ClampExpressionArithmetic(old % (rhs == 0 ? 1 : rhs)); break;
			case 40: result = static_cast<int32_t>(old) | rhs; break;
			case 41: result = boolean ? (static_cast<int32_t>(old) & (rhs & 0xff)) : (static_cast<int32_t>(old) & rhs); break;
			case 42: result = boolean ? (old != rhs) : (static_cast<int32_t>(old) ^ rhs); break;
			case 43: result = FromBits(static_cast<uint32_t>(old) << (static_cast<uint32_t>(rhs) & 31)); break;
			case 44: result = FromBits(static_cast<uint32_t>(old) >> (static_cast<uint32_t>(rhs) & 31)); break;
			default: return 0;
		}
		return assign(boolean ? result != 0 : result, true);
	}
};

int EvaluateAssignment(int token, std::vector<int32_t>::iterator& it,
	std::vector<int32_t>::iterator end, const Game_BaseInterpreterContext& ip);
int EvaluateNumericArrayFunction(Fn selector, int32_t base, int32_t raw_count,
	const Game_BaseInterpreterContext& ip);
int Process(std::vector<int32_t>::iterator& it, std::vector<int32_t>::iterator end, const Game_BaseInterpreterContext& ip);
std::pair<int32_t, int32_t> ProcessPair(std::vector<int32_t>::iterator& it, std::vector<int32_t>::iterator end, const Game_BaseInterpreterContext& ip);

int32_t FromBits(uint32_t bits) {
	int32_t result;
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

int32_t Abs32(int32_t value) {
	return value < 0 ? FromBits(uint32_t{0} - static_cast<uint32_t>(value)) : value;
}

int32_t NegativeAbs32(int32_t value) {
	return FromBits(uint32_t{0} - static_cast<uint32_t>(Abs32(value)));
}

int32_t Negate32(int32_t value) {
	return FromBits(uint32_t{0} - static_cast<uint32_t>(value));
}

int32_t ClampExpressionArithmetic(int64_t value) {
	return static_cast<int32_t>(std::clamp(value,
		static_cast<int64_t>(Main_Data::game_variables->GetMinValue()),
		static_cast<int64_t>(Main_Data::game_variables->GetMaxValue())));
}

int32_t ReadExpressionCount(std::vector<int32_t>::iterator& it, std::vector<int32_t>::iterator end) {
	if (it == end) return 0;
	const int marker = *it++;
	if (marker < 128) return marker;
	if (end - it < 4) { it = end; return 0; }
	const uint32_t bits = static_cast<uint32_t>(*it) |
		(static_cast<uint32_t>(*(it + 1)) << 8) |
		(static_cast<uint32_t>(*(it + 2)) << 16) |
		(static_cast<uint32_t>(*(it + 3)) << 24);
	it += 4;
	return FromBits(bits);
}

void Skip(std::vector<int32_t>::iterator& it, std::vector<int32_t>::iterator end) {
	if (it == end) return;
	const int token = *it++;
	const auto consume = [&](size_t count) {
		it += std::min(count, static_cast<size_t>(end - it));
	};
	if (token == 1 || token == 4) { consume(1); return; }
	if (token == 2 || token == 5) { consume(2); return; }
	if (token == 3 || token == 6) { consume(4); return; }
	if (token == 19) {
		const int32_t count = ReadExpressionCount(it, end);
		for (int32_t i = 0; i < count && it != end; ++i) Skip(it, end);
		return;
	}
	if (token == 78) {
		if (it == end) return;
		const int selector = *it++;
		consume(1); // Native structural skip does not consume an extended count.
		static constexpr int arities[] = {2, 2, 2, 2, 2, 2, 1, 2, 2, 3, 3, 3,
			2, 2, 1, 3, 3, 3, 3, 4, 2, 2, 2};
		if (selector >= 0 && selector < static_cast<int>(std::size(arities))) {
			for (int i = 0; i < arities[selector]; ++i) Skip(it, end);
		}
		return;
	}
	if (token == 8 || token == 9 || (token >= 11 && token <= 14) || token == 16 || token == 17 ||
		(token >= 20 && token <= 26)) {
		Skip(it, end);
	} else if ((token >= 34 && token <= 44) || (token >= 48 && token <= 67)) {
		Skip(it, end);
		Skip(it, end);
	} else if (token == 72) {
		Skip(it, end);
		Skip(it, end);
		Skip(it, end);
	}
}

int Process(std::vector<int32_t>::iterator& it, std::vector<int32_t>::iterator end, const Game_BaseInterpreterContext& ip) {
	int value = 0;
	int imm = 0;
	int imm2 = 0;
	int imm3 = 0;

	if (it == end) {
		return 0;
	}

	auto op = static_cast<Op>(*it);
	++it;

	// When entering the switch it is on the first argument
		switch (op) {
		case Op::Null:
			return 0;
		case Op::U8:
		case Op::UX8:
			if (it == end) return 0;
			value = *it++;
			return value;
		case Op::U16:
		case Op::UX16:
			if (end - it < 2) { it = end; return 0; }
			imm = *it++;
			imm2 = *it++;
			value = (imm2 << 8) + imm;
			return value;
		case Op::S32:
		case Op::SX32:
			if (end - it < 4) { it = end; return 0; }
			imm = *it++;
			imm2 = *it++;
			imm3 = *it++;
			value = *it++;
			return FromBits((static_cast<uint32_t>(value) << 24) |
				(static_cast<uint32_t>(imm3) << 16) | (static_cast<uint32_t>(imm2) << 8) |
				static_cast<uint32_t>(imm));
		case Op::Var:
			imm = Process(it, end, ip);
			return ip.ReadVariable(imm);
		case Op::Switch:
			imm = Process(it, end, ip);
			return ip.ReadSwitch(imm);
		case Op::SelfVar:
			return ip.ReadVariable(NegativeAbs32(Process(it, end, ip)));
		case Op::SelfSwitch:
			return ip.ReadSwitch(NegativeAbs32(Process(it, end, ip)));
		case Op::VarIndirect:
			imm = Process(it, end, ip);
			return ip.ReadVariable(ip.ReadVariable(imm));
		case Op::SwitchIndirect:
			imm = Process(it, end, ip);
			return ip.ReadSwitch(ip.ReadVariable(imm));
		case Op::SelfVarIndirect:
			imm = NegativeAbs32(Process(it, end, ip));
			return ip.ReadVariable(NegativeAbs32(ip.ReadVariable(imm)));
		case Op::SelfSwitchIndirect:
			imm = NegativeAbs32(Process(it, end, ip));
			return ip.ReadSwitch(NegativeAbs32(ip.ReadVariable(imm)));
		case Op::Array: {
			const int32_t count = ReadExpressionCount(it, end);
			for (int32_t i = 0; i < count && it != end; ++i) Process(it, end, ip);
			return 0;
		}
		case Op::Abs:
		case Op::AbsAlternate:
			return Abs32(Process(it, end, ip));
		case Op::SelfAddress:
		case Op::SelfAddressAlternate:
			return NegativeAbs32(Process(it, end, ip));
		case Op::Negate:
			imm = Process(it, end, ip);
			return Negate32(imm);
		case Op::Not:
			imm = Process(it, end, ip);
			return imm == 0 ? 1 : 0;
		case Op::Flip:
			imm = Process(it, end, ip);
			return ~imm;
		case Op::AssignInplace:
		case Op::AddInplace:
		case Op::SubInplace:
		case Op::MulInplace:
		case Op::DivInplace:
		case Op::ModInplace:
		case Op::BitOrInplace:
		case Op::BitAndInplace:
		case Op::BitXorInplace:
		case Op::BitShiftLeftInplace:
		case Op::BitShiftRightInplace:
			return EvaluateAssignment(static_cast<int>(op), it, end, ip);
		case Op::Add:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return ClampExpressionArithmetic(static_cast<int64_t>(imm) + imm2);
		case Op::Sub:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return ClampExpressionArithmetic(static_cast<int64_t>(imm) - imm2);
		case Op::Mul:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return ClampExpressionArithmetic(static_cast<int64_t>(imm) * imm2);
		case Op::Div:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return ClampExpressionArithmetic(static_cast<int64_t>(imm) / (imm2 == 0 ? 1 : imm2));
		case Op::Mod:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return ClampExpressionArithmetic(static_cast<int64_t>(imm) % (imm2 == 0 ? 1 : imm2));
		case Op::BitOr:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm | imm2;
		case Op::BitAnd:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm & imm2;
		case Op::BitXor:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm ^ imm2;
		case Op::BitShiftLeft:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return FromBits(static_cast<uint32_t>(imm) << (static_cast<uint32_t>(imm2) & 31));
		case Op::BitShiftRight:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return FromBits(static_cast<uint32_t>(imm) >> (static_cast<uint32_t>(imm2) & 31));
		case Op::Equal:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm == imm2 ? 1 : 0;
		case Op::GreaterEqual:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm >= imm2 ? 1 : 0;
		case Op::LessEqual:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm <= imm2 ? 1 : 0;
		case Op::Greater:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm > imm2 ? 1 : 0;
		case Op::Less:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm < imm2 ? 1 : 0;
		case Op::NotEqual:
			imm = Process(it, end, ip);
			imm2 = Process(it, end, ip);
			return imm != imm2 ? 1 : 0;
		case Op::Or:
			imm = Process(it, end, ip);
			if (imm != 0) { Skip(it, end); return 1; }
			imm2 = Process(it, end, ip);
			return imm2 != 0 ? 1 : 0;
		case Op::And:
			imm = Process(it, end, ip);
			if (imm == 0) { Skip(it, end); return 0; }
			imm2 = Process(it, end, ip);
			return imm2 != 0 ? 1 : 0;
		case Op::Range:
			imm = Process(it, end, ip);
			Process(it, end, ip);
			return imm;
		case Op::Subscript:
			--it;
			return ProcessPair(it, end, ip).first;
		case Op::Ternary:
			imm = Process(it, end, ip);
			if (imm != 0) {
				imm2 = Process(it, end, ip);
				Skip(it, end);
				return imm2;
			}
			Skip(it, end);
			return Process(it, end, ip);
		case Op::Function: {
			const int selector = it == end ? 0 : *it++;
			const int marker = it == end ? 0 : *it++;
			if (marker >= 128) it += std::min<size_t>(4, end - it);
			static constexpr int arities[] = {2, 2, 2, 2, 2, 2, 1, 2, 2, 3, 3, 3,
				2, 2, 1, 3, 3, 3, 3, 4, 2, 2, 2};
			if (selector < 0 || selector >= static_cast<int>(std::size(arities))) return 0;
			std::array<int32_t, 4> args{};
			for (int i = 0; i < arities[selector]; ++i) args[i] = Process(it, end, ip);
			switch (static_cast<Fn>(selector)) {
				case Fn::Rand: return ControlVariables::Random(args[0], args[1]);
				case Fn::Item: return ControlVariables::Item(args[1], args[0]);
				case Fn::Event: return ControlVariables::Event(args[1], args[0], ip);
				case Fn::Actor: return ControlVariables::Actor(args[1], args[0]);
				case Fn::Party: return ControlVariables::Party(args[1], args[0]);
				case Fn::Enemy: return ControlVariables::Enemy(args[1], args[0]);
				case Fn::Misc: return ControlVariables::Other(args[0]);
				case Fn::Pow: return ControlVariables::Pow(args[0], args[1]);
				case Fn::Sqrt: return ControlVariables::Sqrt(args[0], args[1]);
				case Fn::Sin: return ControlVariables::Sin(args[0], args[1], args[2]);
				case Fn::Cos: return ControlVariables::Cos(args[0], args[1], args[2]);
				case Fn::Atan2: return ControlVariables::Atan2(args[0], args[1], args[2]);
				case Fn::Min: return ControlVariables::Min(args[0], args[1]);
				case Fn::Max: return ControlVariables::Max(args[0], args[1]);
				case Fn::Abs: return ControlVariables::Abs(args[0]);
				case Fn::Clamp: return ControlVariables::Clamp(args[0], args[1], args[2]);
				case Fn::Muldiv: return ControlVariables::Muldiv(args[0], args[1], args[2]);
				case Fn::Divmul: return ControlVariables::Divmul(args[0], args[1], args[2]);
				case Fn::Between: return ControlVariables::Between(args[0], args[1], args[2]);
				case Fn::Lerp: {
					const double divisor = args[3] == 0 ? 1.0 : static_cast<double>(args[3]);
					const double value = static_cast<double>(args[0]) +
						(static_cast<double>(args[1]) - args[0]) * (static_cast<double>(args[2]) / divisor);
					if (!std::isfinite(value) || value < static_cast<double>(INT32_MIN) || value >= 2147483648.0) {
						return INT32_MIN;
					}
					return static_cast<int32_t>(std::trunc(value));
				}
				case Fn::ArraySum:
				case Fn::ArrayMin:
				case Fn::ArrayMax:
					return EvaluateNumericArrayFunction(static_cast<Fn>(selector), args[0], args[1], ip);
				default:
					Output::Warning("Maniac: Expression function {} is not implemented", selector);
					return 0;
			}
		}
		default:
			Output::Warning("Maniac: Expression contains unsupported operation {}", static_cast<int>(op));
			return 0;
	}
}

bool PrepareNumericRange(int low, int high, bool self, const Game_BaseInterpreterContext& ip) {
	if (low < 0 || (!Player::IsPatchManiac() && high > 1000000)) return false;
	const bool maniac = Player::IsPatchManiac();
	const size_t size = static_cast<size_t>(high) + (maniac ? 2 : 1);
	if (!self) {
		if (!maniac) return Main_Data::game_variables->GetManiacWritable(high) != nullptr;
		Main_Data::game_variables->EnsureSize(size - 1); // GLOBAL physical zero is separate.
		return true;
	}
	const auto& frame = ip.GetFrame();
	auto* state = Main_Data::game_maniac_self_variables.get();
	auto* bank = state ? state->Ensure(frame.easyrpg_self_scope,
		frame.easyrpg_self_map_id, frame.easyrpg_self_event_id) : nullptr;
	if (!bank) return false;
	bank->numbers.resize(std::max(bank->numbers.size(), size));
	return true;
}

int EvaluateNumericArrayFunction(Fn selector, int32_t base, int32_t raw_count,
		const Game_BaseInterpreterContext& ip) {
	const int64_t first = std::abs(static_cast<int64_t>(base));
	const int64_t count = std::max<int64_t>(raw_count, selector == Fn::ArraySum ? 0 : 1);
	const int64_t prepared_high = first + count;
	if (!Player::IsPatchManiac() && prepared_high > 1000000) {
		Output::Warning("Maniac: Expression array function exceeds safe storage limit");
		return 0;
	}
	if (prepared_high >= INT32_MAX) {
		Output::Error("Maniac: Expression array length exceeds signed index range");
	}
	if (!PrepareNumericRange(static_cast<int>(first), static_cast<int>(prepared_high), base < 0, ip)) return 0;
	uint32_t sum = 0;
	int32_t extremum = selector == Fn::ArrayMin ? INT32_MAX : INT32_MIN;
	for (int64_t offset = 0; offset < count; ++offset) {
		const int32_t id = base < 0 ? NegativeAbs32(static_cast<int32_t>(first + offset))
			: static_cast<int32_t>(first + offset);
		const int32_t value = id == 0 && Player::IsPatchManiac()
			? Main_Data::game_variables->GetManiacRawSlot(0) : ip.ReadVariable(id);
		if (selector == Fn::ArraySum) sum += static_cast<uint32_t>(value);
		else if (selector == Fn::ArrayMin) extremum = std::min(extremum, value);
		else extremum = std::max(extremum, value);
	}
	return selector == Fn::ArraySum ? FromBits(sum) : extremum;
}

int EvaluateAssignment(int token, std::vector<int32_t>::iterator& it,
		std::vector<int32_t>::iterator end, const Game_BaseInterpreterContext& ip) try {
	const auto selector = static_cast<Op>(it == end ? 0 : *it++);
	const bool numeric = selector == Op::Var || selector == Op::SelfVar ||
		selector == Op::VarIndirect || selector == Op::SelfVarIndirect;
	const bool boolean = selector == Op::Switch || selector == Op::SelfSwitch ||
		selector == Op::SwitchIndirect || selector == Op::SelfSwitchIndirect;
	if (!numeric && !boolean) {
		Process(it, end, ip);
		Process(it, end, ip);
		return 0;
	}
	const bool direct = selector == Op::Var || selector == Op::Switch ||
		selector == Op::SelfVar || selector == Op::SelfSwitch;
	const auto address = ProcessPair(it, end, ip);
	if (!direct) {
		const int32_t rhs = Process(it, end, ip);
		return ProcessAssignmentRet{selector, address.first, &ip}.apply(token, rhs);
	}
	const int32_t first = Abs32(address.first);
	const int32_t second = Abs32(address.second);
	const bool self = selector == Op::SelfVar || selector == Op::SelfSwitch;
	if (first == second) {
		const int32_t rhs = Process(it, end, ip);
		const int32_t id = self ? NegativeAbs32(first) : first;
		return ProcessAssignmentRet{selector, id, &ip}.apply(token, rhs);
	}
	const int32_t low = std::min(first, second);
	const int32_t high = std::max(first, second);
	const bool maniac = Player::IsPatchManiac();
	if (low < 0 || (!maniac && high > 1000000)) {
		Output::Warning("Maniac: Expression assignment range exceeds safe storage limit");
		return 0;
	}
	if (maniac && !numeric && high == INT32_MAX) {
		Output::Error("Maniac: Expression assignment range exceeds signed index range");
	}
	const int rhs_token = it == end ? 0 : *it;
	int last = 0;
	bool range_prepared = false;
	const auto destination = [&](int index, bool in_self) {
		return ProcessAssignmentRet{selector, in_self ? NegativeAbs32(index) : index, &ip, range_prepared};
	};
	if (rhs_token == static_cast<int>(Op::Array)) {
		++it;
		const int32_t count = ReadExpressionCount(it, end);
		int32_t consumed = 0;
		for (int64_t index = low; index <= high; ++index) {
			int32_t rhs = 0;
			if (consumed < count) {
				rhs = Process(it, end, ip);
				++consumed;
			}
			last = destination(static_cast<int>(index), self).apply(token, rhs);
		}
		while (consumed < count && it != end) {
			Process(it, end, ip);
			++consumed;
		}
		return last;
	}
	const bool source_range = it + (it == end ? 0 : 1) < end &&
		((numeric && (rhs_token == static_cast<int>(Op::Var) || rhs_token == static_cast<int>(Op::SelfVar))) ||
		 (boolean && (rhs_token == static_cast<int>(Op::Switch) || rhs_token == static_cast<int>(Op::SelfSwitch)))) &&
		*(it + 1) == static_cast<int>(Op::Range);
	if (source_range) {
		++it; // The optimized path skips the reference token and evaluates its range child.
		const auto source = ProcessPair(it, end, ip);
		const int32_t source_low = std::min(Abs32(source.first), Abs32(source.second));
		const int32_t source_high = std::max(Abs32(source.first), Abs32(source.second));
		const bool source_self = rhs_token == static_cast<int>(Op::SelfVar) ||
			rhs_token == static_cast<int>(Op::SelfSwitch);
		if (numeric && !PrepareNumericRange(low, high, self, ip)) return 0;
		range_prepared = maniac && numeric;
		for (int64_t index = low; index <= high; ++index) {
			const int64_t source_index = static_cast<int64_t>(source_low) + index - low;
			int32_t rhs = 0;
			if (source_low >= 0 && source_index <= source_high && (maniac || source_index <= 1000000)) {
				const int source_id = source_self ? NegativeAbs32(static_cast<int32_t>(source_index))
					: static_cast<int>(source_index);
				rhs = ip.ReadVariable(source_id); // Native switch range copy also reads numeric storage.
			}
			last = destination(static_cast<int>(index), self).apply(token, rhs);
		}
		return last;
	}
	const int32_t rhs = Process(it, end, ip);
	if (numeric) {
		const bool broadcast_self = rhs_token == static_cast<int>(Op::SelfVar);
		if (!PrepareNumericRange(low, high, broadcast_self, ip)) return 0;
		range_prepared = maniac;
		for (int64_t index = low; index <= high; ++index) {
			last = destination(low, broadcast_self).apply(token, rhs); // Native repeats the first pointer.
		}
	} else {
		const bool broadcast_self = rhs_token == static_cast<int>(Op::SelfSwitch);
		for (int index = low; index < high; ++index) {
			last = destination(index, broadcast_self).apply(token, rhs); // Native excludes the last endpoint.
		}
	}
	return last;
} catch (const std::bad_alloc&) {
	Output::Error("Maniac: bad allocation.");
	return 0;
} catch (const std::length_error&) {
	Output::Error("Maniac: bad allocation.");
	return 0;
}

int32_t ManiacPatch::ParseExpression(Span<const int32_t> op_codes, const Game_BaseInterpreterContext& interpreter) {
	std::vector<int32_t> ops;
	for (auto &o: op_codes) {
		auto uo = static_cast<uint32_t>(o);
		ops.push_back(static_cast<int32_t>(uo & 0x000000FF));
		ops.push_back(static_cast<int32_t>((uo & 0x0000FF00) >> 8));
		ops.push_back(static_cast<int32_t>((uo & 0x00FF0000) >> 16));
		ops.push_back(static_cast<int32_t>((uo & 0xFF000000) >> 24));
	}
	auto beg = ops.begin();
	return Process(beg, ops.end(), interpreter);
}

std::pair<int32_t, int32_t> ProcessPair(std::vector<int32_t>::iterator& it,
		std::vector<int32_t>::iterator end, const Game_BaseInterpreterContext& ip) {
	if (it == end) return {0, 0};
	if (*it == static_cast<int32_t>(Op::Range)) {
		++it;
		const int32_t first = Process(it, end, ip);
		const int32_t second = Process(it, end, ip);
		return {first, second};
	}
	if (*it == static_cast<int32_t>(Op::Subscript)) {
		++it;
		if (it == end || *it != static_cast<int32_t>(Op::Array)) {
			Process(it, end, ip);
			Process(it, end, ip);
			return {0, 0};
		}
		++it;
		const int32_t count = ReadExpressionCount(it, end);
		std::vector<std::pair<int32_t, int32_t>> elements;
		for (int32_t i = 0; i < count && it != end; ++i) {
			elements.push_back(ProcessPair(it, end, ip));
		}
		const int32_t index = Process(it, end, ip);
		return index >= 0 && static_cast<size_t>(index) < elements.size() ? elements[index]
			: std::pair<int32_t, int32_t>{0, 0};
	}
	const int32_t value = Process(it, end, ip);
	return {value, value};
}

std::pair<int32_t, int32_t> ManiacPatch::ParseExpressionPair(Span<const int32_t> op_codes, const Game_BaseInterpreterContext& interpreter) {
	std::vector<int32_t> ops;
	for (auto word : op_codes) {
		const auto bits = static_cast<uint32_t>(word);
		for (unsigned shift = 0; shift < 32; shift += 8) {
			ops.push_back(static_cast<int32_t>((bits >> shift) & 0xff));
		}
	}
	if (ops.empty()) return {0, 0};
	auto it = ops.begin();
	return ProcessPair(it, ops.end(), interpreter);
}

std::vector<int32_t> ManiacPatch::ParseExpressions(Span<const int32_t> op_codes, const Game_BaseInterpreterContext& interpreter) {
	std::vector<int32_t> ops;
	for (auto& o : op_codes) {
		auto uo = static_cast<uint32_t>(o);
		ops.push_back(static_cast<int32_t>(uo & 0x000000FF));
		ops.push_back(static_cast<int32_t>((uo & 0x0000FF00) >> 8));
		ops.push_back(static_cast<int32_t>((uo & 0x00FF0000) >> 16));
		ops.push_back(static_cast<int32_t>((uo & 0xFF000000) >> 24));
	}

	if (ops.empty()) {
		return {};
	}

	auto it = ops.begin();

	std::vector<int32_t> results;

	while (it != ops.end()) {
		results.push_back(Process(it, ops.end(), interpreter));
	}

	return results;
}

int32_t ManiacPatch::GetGeneratedCommonNameCache() {
	return generated_common_name_cache;
}

void ManiacPatch::SetGeneratedCommonNameCache(int32_t value) {
	generated_common_name_cache = value;
}

std::array<bool, 50> ManiacPatch::GetKeyRange() {
	std::array<Input::Keys::InputKey, 50> keys = {
		Input::Keys::A,
		Input::Keys::B,
		Input::Keys::C,
		Input::Keys::D,
		Input::Keys::E,
		Input::Keys::F,
		Input::Keys::G,
		Input::Keys::H,
		Input::Keys::I,
		Input::Keys::J,
		Input::Keys::K,
		Input::Keys::L,
		Input::Keys::M,
		Input::Keys::N,
		Input::Keys::O,
		Input::Keys::P,
		Input::Keys::Q,
		Input::Keys::R,
		Input::Keys::S,
		Input::Keys::T,
		Input::Keys::U,
		Input::Keys::V,
		Input::Keys::W,
		Input::Keys::X,
		Input::Keys::Y,
		Input::Keys::Z,
		Input::Keys::N0,
		Input::Keys::N1,
		Input::Keys::N2,
		Input::Keys::N3,
		Input::Keys::N4,
		Input::Keys::N5,
		Input::Keys::N6,
		Input::Keys::N7,
		Input::Keys::N8,
		Input::Keys::N9,
		Input::Keys::LEFT,
		Input::Keys::UP,
		Input::Keys::RIGHT,
		Input::Keys::DOWN,
		Input::Keys::RETURN,
		Input::Keys::SHIFT,
		Input::Keys::CTRL,
		Input::Keys::ALT,
		Input::Keys::SPACE,
#if defined(USE_MOUSE) && defined(SUPPORT_MOUSE)
		Input::Keys::MOUSE_LEFT,
		Input::Keys::MOUSE_RIGHT,
		Input::Keys::MOUSE_MIDDLE,
		Input::Keys::MOUSE_SCROLLUP,
		Input::Keys::MOUSE_SCROLLDOWN
#else
		Input::Keys::NONE,
		Input::Keys::NONE,
		Input::Keys::NONE,
		Input::Keys::NONE,
		Input::Keys::NONE
#endif
	};

	std::array<bool, 50> pressed = {};

	for (size_t i = 0; i < pressed.size(); ++i) {
		pressed[i] = Input::IsRawKeyPressed(keys[i]);
	}

	if (!pressed[41]) {
		pressed[41] = Input::IsRawKeyPressed(Input::Keys::LSHIFT) || Input::IsRawKeyPressed(Input::Keys::RSHIFT);
	}
	if (!pressed[42]) {
		pressed[42] = Input::IsRawKeyPressed(Input::Keys::LCTRL) || Input::IsRawKeyPressed(Input::Keys::RCTRL);
	}
	if (!pressed[43]) {
		pressed[43] = Input::IsRawKeyPressed(Input::Keys::LALT) || Input::IsRawKeyPressed(Input::Keys::RALT);
	}

	return pressed;
}

bool ManiacPatch::GetKeyState(uint32_t key_id) {
	Input::Keys::InputKey key;

	// see https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
	switch (key_id) {
#if defined(USE_MOUSE) && defined(SUPPORT_MOUSE)
		case 0x1: key = Input::Keys::MOUSE_LEFT; break;
		case 0x2: key = Input::Keys::MOUSE_RIGHT; break;
		case 0x4: key = Input::Keys::MOUSE_MIDDLE; break;
		case 0x5: key = Input::Keys::MOUSE_XBUTTON1; break;
		case 0x6: key = Input::Keys::MOUSE_XBUTTON2; break;
#endif
		case 0x8: key = Input::Keys::BACKSPACE; break;
		case 0x9: key = Input::Keys::TAB; break;
		case 0xD: key = Input::Keys::RETURN; break;
		case 0x10: key = Input::Keys::SHIFT; break;
		case 0x11: key = Input::Keys::CTRL; break;
		case 0x12: key = Input::Keys::ALT; break;
		case 0x13: key = Input::Keys::PAUSE; break;
		case 0x14: key = Input::Keys::CAPS_LOCK; break;
		case 0x1B: key = Input::Keys::ESCAPE; break;
		case 0x20: key = Input::Keys::SPACE; break;
		case 0x21: key = Input::Keys::PGUP; break;
		case 0x22: key = Input::Keys::PGDN; break;
		case 0x23: key = Input::Keys::ENDS; break;
		case 0x24: key = Input::Keys::HOME; break;
		case 0x25: key = Input::Keys::LEFT; break;
		case 0x26: key = Input::Keys::UP; break;
		case 0x27: key = Input::Keys::RIGHT; break;
		case 0x28: key = Input::Keys::DOWN; break;
		case 0x2D: key = Input::Keys::INSERT; break;
		case 0x2E: key = Input::Keys::DEL; break;
		case 0x30: key = Input::Keys::N0; break;
		case 0x31: key = Input::Keys::N1; break;
		case 0x32: key = Input::Keys::N2; break;
		case 0x33: key = Input::Keys::N3; break;
		case 0x34: key = Input::Keys::N4; break;
		case 0x35: key = Input::Keys::N5; break;
		case 0x36: key = Input::Keys::N6; break;
		case 0x37: key = Input::Keys::N7; break;
		case 0x38: key = Input::Keys::N8; break;
		case 0x39: key = Input::Keys::N9; break;
		case 0x41: key = Input::Keys::A; break;
		case 0x42: key = Input::Keys::B; break;
		case 0x43: key = Input::Keys::C; break;
		case 0x44: key = Input::Keys::D; break;
		case 0x45: key = Input::Keys::E; break;
		case 0x46: key = Input::Keys::F; break;
		case 0x47: key = Input::Keys::G; break;
		case 0x48: key = Input::Keys::H; break;
		case 0x49: key = Input::Keys::I; break;
		case 0x4A: key = Input::Keys::J; break;
		case 0x4B: key = Input::Keys::K; break;
		case 0x4C: key = Input::Keys::L; break;
		case 0x4D: key = Input::Keys::M; break;
		case 0x4E: key = Input::Keys::N; break;
		case 0x4F: key = Input::Keys::O; break;
		case 0x50: key = Input::Keys::P; break;
		case 0x51: key = Input::Keys::Q; break;
		case 0x52: key = Input::Keys::R; break;
		case 0x53: key = Input::Keys::S; break;
		case 0x54: key = Input::Keys::T; break;
		case 0x55: key = Input::Keys::U; break;
		case 0x56: key = Input::Keys::V; break;
		case 0x57: key = Input::Keys::W; break;
		case 0x58: key = Input::Keys::X; break;
		case 0x59: key = Input::Keys::Y; break;
		case 0x5A: key = Input::Keys::Z; break;
		case 0x60: key = Input::Keys::KP0; break;
		case 0x61: key = Input::Keys::KP1; break;
		case 0x62: key = Input::Keys::KP2; break;
		case 0x63: key = Input::Keys::KP3; break;
		case 0x64: key = Input::Keys::KP4; break;
		case 0x65: key = Input::Keys::KP5; break;
		case 0x66: key = Input::Keys::KP6; break;
		case 0x67: key = Input::Keys::KP7; break;
		case 0x68: key = Input::Keys::KP8; break;
		case 0x69: key = Input::Keys::KP9; break;
		case 0x6A: key = Input::Keys::KP_MULTIPLY; break;
		case 0x6B: key = Input::Keys::KP_ADD; break;
		case 0x6D: key = Input::Keys::KP_SUBTRACT; break;
		case 0x6E: key = Input::Keys::KP_PERIOD; break;
		case 0x6F: key = Input::Keys::KP_DIVIDE; break;
		case 0x70: key = Input::Keys::F1; break;
		case 0x71: key = Input::Keys::F2; break;
		case 0x72: key = Input::Keys::F3; break;
		case 0x73: key = Input::Keys::F4; break;
		case 0x74: key = Input::Keys::F5; break;
		case 0x75: key = Input::Keys::F6; break;
		case 0x76: key = Input::Keys::F7; break;
		case 0x77: key = Input::Keys::F8; break;
		case 0x78: key = Input::Keys::F9; break;
		case 0x79: key = Input::Keys::F10; break;
		case 0x7A: key = Input::Keys::F11; break;
		case 0x7B: key = Input::Keys::F12; break;
		case 0x90: key = Input::Keys::NUM_LOCK; break;
		case 0x91: key = Input::Keys::SCROLL_LOCK; break;
		case 0xA0: key = Input::Keys::LSHIFT; break;
		case 0xA1: key = Input::Keys::RSHIFT; break;
		case 0xA2: key = Input::Keys::LCTRL; break;
		case 0xA3: key = Input::Keys::RCTRL; break;
		default:
			Output::Debug("Maniac KeyInputProcEx: Unsupported keycode {}", key_id);
			key = Input::Keys::NONE;
			break;
	}

	return Input::IsRawKeyPressed(key);



}

bool ManiacPatch::CheckString(std::string_view str_l, std::string_view str_r, int op, bool ignore_case) {
	auto check = [op](const auto& l, const auto& r) {
		switch (op) {
			case 0: // eq
				return l == r;
			case 2: // contains (l contains r)
				return l.find(r) != std::string::npos;
			case 1: // neq
				return l != r;
			case 3: // notContains (l does not contain r)
				return l.find(r) == std::string::npos;
			default:
				return false;
		}
	};

	if (ignore_case) {
		std::string str_l_lower = Utils::LowerCase(str_l);
		std::string str_r_lower = Utils::LowerCase(str_r);
		return check(str_l_lower, str_r_lower);
	}

	return check(str_l, str_r);
}

std::string_view ManiacPatch::GetLcfName(int data_type, int id, bool is_dynamic, bool* present) {
	if (present) *present = false;
	auto get_name = [&id](std::string_view type, const auto& vec) -> std::string_view {
		auto* data = FindLcfStringRecord(type, vec, id);
		if (!data) {
			Output::Warning("Unable to read {} name: {}", type, id);
			return {};
		}
		return data->name;
	};

	switch (data_type)
	{
	case 0:  //.actor[a].name
		if (is_dynamic) {
			auto actor = Main_Data::game_actors->GetActor(id);
			if (actor != nullptr) {
				if (present) *present = actor->IsManiacNamePresent();
				return actor->GetName();
			}
		}
		else {
			return get_name("Actor", lcf::Data::actors);
		}
		break;
	case 1:	 return get_name("Skill", lcf::Data::skills);   //.skill[a].name
	case 2:	 return get_name("Item", lcf::Data::items);   //.item[a].name
	case 3:	 return get_name("Enemy", lcf::Data::enemies);   //.enemy[a].name
	case 4:	 return get_name("Troop", lcf::Data::troops);   //.troop[a].name
	case 5:	 return get_name("Terrain", lcf::Data::terrains);   //.terrain[a].name
	case 6:	 return get_name("Attribute", lcf::Data::attributes);   //.element[a].name
	case 7:	 return get_name("State", lcf::Data::states);   //.state[a].name
	case 8:	 return get_name("Animation", lcf::Data::animations);   //.anim[a].name
	case 9:	 return get_name("Chipset", lcf::Data::chipsets);   //.tileset[a].name
	case 10: return Player::IsPatchManiac()
		? get_name("Switch", lcf::Data::switches) : Main_Data::game_switches->GetName(id); //.s[a].name
	case 11: return Player::IsPatchManiac()
		? get_name("Variable", lcf::Data::variables) : Main_Data::game_variables->GetName(id); //.v[a].name
	case 12: return {};  // FIXME: .t[a].name -- not sure how to get this for now
	case 13: //.cev[a].name
	{
		// The native getter indexes the common-event object table by id - 1.
		if (id > 0 && static_cast<int>(Game_Map::GetCommonEvents().size()) >= id) {
			return Game_Map::GetCommonEvents()[id - 1].GetName();
		}
		if (Player::IsPatchManiac()) {
			Output::Error("GetLcfName: Common Event {} was not found.", id);
		}
		break;
	}
	case 14: return get_name("Class", lcf::Data::classes);   //.class[a].name
	case 15: return get_name("BattlerAnimation", lcf::Data::battleranimations);   //.anim2[a].name
	case 16: return Player::IsPatchManiac()
		? get_name("Map", lcf::Data::treemap.maps) : Game_Map::GetMapName(id); //.map[a].name
	case 17:   //.mev[a].name
	{
		auto map = Game_Map::GetEvent(id);
		if (map != nullptr) {
			return map->GetName();
		}
		if (Player::IsPatchManiac()) {
			Output::Error("GetLcfData: Map Event {} was not found.", id);
		}
		break;
	}
	case 18: //.member[a].name, index starts from 0
	{
		auto actor = Main_Data::game_party->GetActor(id);
		if (actor != nullptr) {
			if (is_dynamic) {
				if (present) *present = actor->IsManiacNamePresent();
				return actor->GetName();
			}
			else {
				id = actor->GetId();
				return get_name("Actor", lcf::Data::actors);
			}
		}
		break;
	}
	}

	Output::Warning("GetLcfName: Unsupported data_type {} {}", data_type, id);
	return {};
}

std::string_view ManiacPatch::GetLcfDescription(int data_type, int id, bool is_dynamic, bool* present) {
	if (present) *present = false;
	auto get_desc = [id](std::string_view type, const auto& vec) -> std::string_view {
		auto* data = FindLcfStringRecord(type, vec, id);
		if (!data) {
			Output::Warning("Unable to read {} description: {}", type, id);
			return {};
		}
		if constexpr (std::is_same_v<typename std::decay_t<decltype(vec)>::value_type, lcf::rpg::Actor>) {
			return data->title;
		} else {
			return data->description;
		}
	};

	switch (data_type)
	{
	case 0:  //.actor[a].desc
		if (is_dynamic) {
			auto actor = Main_Data::game_actors->GetActor(id);
			if (actor != nullptr) {
				if (present) *present = actor->IsManiacTitlePresent();
				return actor->GetTitle();
			}
		}
		else {
			return get_desc("Actor", lcf::Data::actors);
		}
		break;
	case 1: return get_desc("Skill", lcf::Data::skills); //.skill[a].desc
	case 2: return get_desc("Item", lcf::Data::items); //.item[a].desc
	case 18: //.member[a].desc
	{
		auto actor = Main_Data::game_party->GetActor(id);
		if (actor != nullptr) {
			if (is_dynamic) {
				if (present) *present = actor->IsManiacTitlePresent();
				return actor->GetTitle();
			}
			else {
				id = actor->GetId();
				return get_desc("Actor", lcf::Data::actors);
			}
		}
		break;
	}
	}

	Output::Warning("GetLcfDescription: Unsupported data_type {} {}", data_type, id);
	return {};
}
