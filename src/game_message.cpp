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
#include "game_message.h"
#include "game_actor.h"
#include "game_actors.h"
#include "game_party.h"
#include "game_player.h"
#include "game_battle.h"
#include "game_system.h"
#include "game_strings.h"
#include "main_data.h"
#include "window_message.h"
#include "font.h"
#include "player.h"
#include "game_variables.h"
#include "game_switches.h"
#include <lcf/data.h>
#include <lcf/reader_util.h>
#include "output.h"
#include "feature.h"
#include "game_interpreter.h"
#include "game_maniac_self_variables.h"

#include <cctype>
#include <cstring>
#include <tuple>

namespace {
using HookCaller = Game_Message::ManiacHookCaller;

Game_ManiacSelfVariables::Bank* HookBank(HookCaller caller, bool create) {
	auto* state = Main_Data::game_maniac_self_variables.get();
	if (!state) return nullptr;
	return create ? state->Ensure(caller.scope, caller.map_id, caller.event_id)
		: state->FindMutable(caller.scope, caller.map_id, caller.event_id);
}

int32_t HookReadNumber(HookCaller caller, int32_t id) {
	return id < 0 ? Game_ManiacSelfVariables::ReadNumber(HookBank(caller, false), id)
		: Main_Data::game_variables->Get(id);
}

std::pair<std::string, bool> HookReadString(HookCaller caller, int32_t id) {
	if (id > 0) return {ToString(Main_Data::game_strings->Get(id)), Main_Data::game_strings->IsPresent(id)};
	if (id == 0) return {};
	const auto* bank = HookBank(caller, false);
	const auto slot = static_cast<size_t>(-static_cast<int64_t>(id));
	if (!bank || slot >= bank->strings.size()) return {};
	return {ToString(bank->strings[slot]), Game_ManiacSelfVariables::IsStringPresent(bank, slot)};
}

int32_t HookOffset(int32_t base, uint32_t offset) {
	const uint32_t bits = base < 0 ? static_cast<uint32_t>(base) - offset : static_cast<uint32_t>(base) + offset;
	int32_t result;
	std::memcpy(&result, &bits, sizeof(result));
	return result;
}

void HookWriteNumber(HookCaller caller, int32_t id, int32_t value) {
	if (id >= 0) {
		if (id < INT32_MAX) Main_Data::game_variables->GetManiacWritable(id + 1);
		if (auto* destination = Main_Data::game_variables->GetManiacWritable(id)) *destination = value;
		return;
	}
	auto* bank = HookBank(caller, true);
	if (!bank) return;
	const auto slot = static_cast<size_t>(-static_cast<int64_t>(id));
	bank->numbers.resize(std::max(bank->numbers.size(), slot + 2));
	bank->numbers[slot] = value;
}

void HookWriteString(HookCaller caller, int32_t id, const std::string& value, bool present) {
	if (id >= 0) {
		Main_Data::game_strings->SetManiacRawSlot(id, value, present);
		return;
	}
	auto* bank = HookBank(caller, true);
	if (!bank) return;
	const auto slot = static_cast<size_t>(-static_cast<int64_t>(id));
	bank->strings.resize(std::max(bank->strings.size(), slot + 1));
	bank->strings[slot] = lcf::DBString(std::string(value));
	Game_ManiacSelfVariables::SetStringPresence(bank, slot, present);
}

class UserHookParser {
public:
	UserHookParser(const char* iter, const char* end, HookCaller caller, bool evaluate = true,
		uint32_t grammar = static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax) | 4u)
		: pos(iter), end(end), caller(caller), evaluate(evaluate), grammar(grammar) {}
	const char* pos;
	const char* end;
	HookCaller caller;
	bool evaluate;
	uint32_t grammar;
	bool self_reference = false;

	bool IsSpace() const {
		return pos != end && (*pos == ' ' || (end - pos >= 3 && std::memcmp(pos, "\xe3\x80\x80", 3) == 0));
	}
	void SkipSpaces() {
		while (IsSpace()) pos += *pos == ' ' ? 1 : 3;
	}
	bool Prepare(bool& bracket) {
		bracket = false;
		if (grammar & 1) {
			if (pos == end || *pos != '[') return false;
			bracket = true; // The native strict path deliberately leaves '[' unconsumed.
		} else {
			if (pos == end || ((grammar & 4) && IsSpace())) return false;
			if (!(grammar & 4)) SkipSpaces();
			if (pos == end) return false;
			if (*pos == '[' || *pos == ':') { bracket = *pos == '['; ++pos; }
		}
		SkipSpaces();
		self_reference = self_reference && !bracket;
		return pos != end;
	}
	static int32_t Decimal(std::string_view value, size_t* used = nullptr) {
		size_t index = 0;
		while (index < value.size() && (value[index] == ' ' || value[index] == '\t' || value[index] == '\r' || value[index] == '\n')) ++index;
		bool negative = false;
		if (index < value.size() && (value[index] == '+' || value[index] == '-')) negative = value[index++] == '-';
		const size_t first_digit = index;
		uint32_t bits = 0;
		while (index < value.size() && value[index] >= '0' && value[index] <= '9') bits = bits * 10 + value[index++] - '0';
		if (used) *used = index == first_digit ? 0 : index;
		if (negative) bits = 0u - bits;
		int32_t result;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	}
	std::string ActorName(int id) const {
		if (id == 0) {
			if (!Main_Data::game_party || Main_Data::game_party->GetBattlerCount() == 0) return {};
			id = (*Main_Data::game_party)[0].GetId();
		}
		const auto* actor = Main_Data::game_actors->GetActor(id);
		if (!actor) Output::Error("Maniac message hook: Invalid actor ID {}", id);
		return ToString(actor->GetName());
	}
	char Prefix(bool& self) {
		const char* begin = pos;
		bool explicit_self = false;
		self = self_reference;
		if (pos == end) return 0;
		const auto escape = Utils::UTF8Next(pos, end);
		if (escape.ch == Player::escape_char || *pos == '\\') {
			pos = escape.next;
			self = self_reference = false;
		} else if (grammar & 2) return 0;
		if (pos == end) { pos = begin; return 0; }
		if (end - pos >= 4 && std::toupper(static_cast<unsigned char>(pos[0])) == 'S'
			&& std::toupper(static_cast<unsigned char>(pos[1])) == 'E'
			&& std::toupper(static_cast<unsigned char>(pos[2])) == 'L'
			&& std::toupper(static_cast<unsigned char>(pos[3])) == 'F') {
			self = self_reference = true;
			explicit_self = true;
			pos += 4;
		}
		if (pos != end) {
			const char type = std::toupper(static_cast<unsigned char>(*pos));
			if (type == 'V' || type == 'T' || (!explicit_self && type == 'N')) {
				if (type == 'N') self = false;
				++pos;
				return type;
			}
		}
		pos = begin;
		return 0;
	}
	int32_t Literal() {
		size_t used = 0;
		const auto value = Decimal(std::string_view(pos, end - pos), &used);
		pos += used;
		return value;
	}
	int32_t NumericToken() {
		bool self;
		const char type = Prefix(self);
		if (!type) return Literal();
		int32_t id = Number();
		self_reference = false;
		if (self) {
			const uint32_t bits = 0u - static_cast<uint32_t>(id);
			std::memcpy(&id, &bits, sizeof(id));
		}
		return !evaluate ? 0 : type == 'V' ? HookReadNumber(caller, id)
			: Decimal(type == 'T' ? HookReadString(caller, id).first : ActorName(id));
	}
	bool MixedToken(int32_t& number, std::string& value, bool& present) {
		if (pos != end && *pos == '"') {
			++pos;
			while (pos != end && *pos != '"') {
				if (*pos == '\\' && end - pos >= 2 && pos[1] == '"') ++pos;
				value += *pos++;
			}
			if (pos != end) ++pos;
			present = !value.empty();
			return true;
		}
		bool self;
		const char type = Prefix(self);
		if (!type) { number = Literal(); return false; }
		int32_t id = Number();
		self_reference = false;
		if (self) {
			const uint32_t bits = 0u - static_cast<uint32_t>(id);
			std::memcpy(&id, &bits, sizeof(id));
		}
		if (evaluate) {
			if (type == 'V') number = HookReadNumber(caller, id);
			else if (type == 'T') std::tie(value, present) = HookReadString(caller, id);
			else { value = ActorName(id); present = !value.empty(); }
		}
		return type != 'V';
	}
	int32_t Number() {
		bool bracket;
		if (!Prepare(bracket)) return 0;
		bool self;
		const char type = Prefix(self);
		int32_t value;
		if (type) {
			int32_t id = Number();
			self_reference = false;
			if (self) {
				const uint32_t bits = 0u - static_cast<uint32_t>(id);
				std::memcpy(&id, &bits, sizeof(id));
			}
			value = !evaluate ? 0 : type == 'V' ? HookReadNumber(caller, id)
				: Decimal(type == 'T' ? HookReadString(caller, id).first : ActorName(id));
			if (bracket && pos != end && *pos == ']') ++pos;
		} else {
			const char* begin = pos;
			size_t used = 0;
			value = Decimal(std::string_view(begin, end - begin), &used);
			if (bracket) {
				while (pos != end && *pos != ']') ++pos;
				if (pos != end) ++pos;
			} else {
				pos += used;
				SkipSpaces();
				if (pos != end && *pos == ';') ++pos;
			}
		}
		return value;
	}
};

const char* ParseColorNumbers(const char* iter, const char* end, HookCaller caller,
	std::array<int32_t, 2>& colors, bool evaluate) {
	colors = {-1, -1};
	UserHookParser parser(iter, end, caller, evaluate, static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax));
	bool bracket;
	if (parser.Prepare(bracket)) {
		uint32_t index = 0;
		while (parser.pos != end && *parser.pos != ']') {
			const auto value = parser.NumericToken();
			if (index < colors.size()) colors[index] = value;
			++index;
			parser.SkipSpaces();
			if (parser.pos == end || (*parser.pos != ',' && *parser.pos != ':')) break;
			++parser.pos;
			parser.SkipSpaces();
		}
		if (bracket) {
			while (parser.pos != end && *parser.pos != ']') ++parser.pos;
			if (parser.pos != end) ++parser.pos;
		} else if (parser.pos != end && *parser.pos == ';') ++parser.pos;
	}
	return parser.pos;
}

std::optional<std::string> PreserveManiacColor(char ch, const char** iter, const char* end, uint32_t escape_char) {
	if (!Player::IsPatchManiac() || (ch != 'C' && ch != 'c')) return std::nullopt;
	const char* begin = *iter;
	std::array<int32_t, 2> colors;
	*iter = ParseColorNumbers(begin, end, {}, colors, false);
	return Utils::EncodeUTF(std::u32string(1, escape_char)) + ch + std::string(begin, *iter);
}

// Preserve mixed-parameter spans before ordinary V/T/N message insertion.
// This lexical pass shares the cursor rules but neither reads banks nor invokes a hook.
std::optional<std::string> PreserveManiacMixedParameters(char ch, const char** iter, const char* end, uint32_t escape_char) {
	if (!Player::IsPatchManiac() || (ch != 'E' && ch != 'e' && ch != 'F' && ch != 'f')) return std::nullopt;
	const char* begin = *iter;
	const auto grammar = static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax)
		| ((ch == 'E' || ch == 'e') ? 4u : 0u);
	UserHookParser parser(begin, end, {}, false, grammar);
	bool bracket;
	if (parser.Prepare(bracket)) {
		while (parser.pos != end && *parser.pos != ']') {
			if (*parser.pos == '"') {
				++parser.pos;
				while (parser.pos != end && *parser.pos != '"') {
					if (*parser.pos == '\\' && end - parser.pos >= 2 && parser.pos[1] == '"') ++parser.pos;
					++parser.pos;
				}
				if (parser.pos != end) ++parser.pos;
			} else {
				parser.NumericToken();
			}
			parser.SkipSpaces();
			if (parser.pos == end || (*parser.pos != ',' && *parser.pos != ':')) break;
			++parser.pos;
			parser.SkipSpaces();
		}
		if (bracket) {
			while (parser.pos != end && *parser.pos != ']') ++parser.pos;
			if (parser.pos != end) ++parser.pos;
		} else if (parser.pos != end && *parser.pos == ';') ++parser.pos;
	}
	*iter = parser.pos;
	return Utils::EncodeUTF(std::u32string(1, escape_char)) + ch + std::string(begin, parser.pos);
}
} // namespace

Game_Message::ParseParamResult Game_Message::ParseManiacNumber(const char* iter, const char* end,
	ManiacHookCaller caller) {
	UserHookParser parser(iter, end, caller, true, static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax));
	const auto value = parser.Number();
	return {parser.pos, value};
}

const char* Game_Message::ParseManiacNumbers(const char* iter, const char* end, ManiacHookCaller caller,
	std::array<int32_t, 2>& numbers, bool evaluate) {
	return ParseColorNumbers(iter, end, caller, numbers, evaluate);
}

const char* Game_Message::ManiacIconEnd(const char* iter, const char* end) {
	if (iter == end || *iter != '$') return iter;
	++iter;
	if (iter != end && (*iter == '$' || (*iter >= 'A' && *iter <= 'Z') || (*iter >= 'a' && *iter <= 'z'))) {
		return iter + 1;
	}
	std::array<int32_t, 2> numbers;
	return ParseManiacNumbers(iter, end, {}, numbers, false);
}

const char* Game_Message::ParseManiacFace(const char* iter, const char* end, ManiacHookCaller caller,
	std::array<int32_t, 2>& numbers, std::optional<std::string>& name) {
	numbers = {0, 0};
	name.reset();
	UserHookParser parser(iter, end, caller, true, static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax));
	bool bracket;
	if (parser.Prepare(bracket)) {
		uint32_t number_cursor = 0, string_cursor = 0;
		while (parser.pos != end && *parser.pos != ']') {
			std::string value;
			int32_t number = 0;
			bool present = false;
			if (parser.MixedToken(number, value, present)) {
				if (string_cursor++ == 0 && present) name = std::move(value);
			} else {
				if (number_cursor < numbers.size()) numbers[number_cursor] = number;
				++number_cursor;
			}
			parser.SkipSpaces();
			if (parser.pos == end || (*parser.pos != ',' && *parser.pos != ':')) break;
			++parser.pos;
			parser.SkipSpaces();
		}
		if (bracket) {
			while (parser.pos != end && *parser.pos != ']') ++parser.pos;
			if (parser.pos != end) ++parser.pos;
		} else if (parser.pos != end && *parser.pos == ';') ++parser.pos;
	}
	return parser.pos;
}

bool Game_Message::RunManiacSystemHook(ManiacHookCaller caller, const int32_t* packet, size_t count,
	const std::string* text, AsyncOp& operation) {
	if (!Player::IsPatchManiac()) return false;
	const auto& config = Main_Data::game_system->GetSaveData();
	const auto& flags = config.maniac_message_hook_flags;
	const bool enabled = packet[0] == 1 ? flags.user_event : packet[0] == 2 ? flags.create_window
		: packet[0] == 4 ? flags.destroy_window : flags.text_rendering;
	const int event_id = config.maniac_message_hook_common_event_id;
	if (!enabled || event_id <= 0) return false;
	if (!lcf::ReaderUtil::GetElement(lcf::Data::commonevents, event_id)) {
		Output::Error("Maniac message hook: Invalid common event ID {}", event_id);
	}
	const int string_base = config.maniac_message_hook_callback_system_string_variable;
	if (text && string_base != 0) HookWriteString(caller, string_base, *text, !text->empty());
	const int number_base = config.maniac_message_hook_callback_system_variable;
	if (number_base != 0) {
		for (uint32_t index = 0; index < count; ++index) HookWriteNumber(caller, HookOffset(number_base, index), packet[index]);
	}
	operation = Game_Interpreter::RunManiacMessageHook(event_id);
	return true;
}

int32_t Game_Message::ReadManiacHookResult(ManiacHookCaller caller, uint32_t offset, int32_t fallback) {
	// A callback can reconfigure its banks. Read from the live base after restoring the caller.
	const int base = Main_Data::game_system->GetSaveData().maniac_message_hook_callback_system_variable;
	return base == 0 ? fallback : HookReadNumber(caller, HookOffset(base, offset));
}

const char* Game_Message::ProcessManiacUserHook(const char* iter, const char* end, ManiacHookCaller caller,
	std::array<int32_t, 8>& packet, AsyncOp& operation) {
	const auto& config = Main_Data::game_system->GetSaveData();
	const bool enabled = config.maniac_message_hook_flags.user_event && config.maniac_message_hook_common_event_id > 0;
	UserHookParser parser(iter, end, caller);
	bool bracket;
	if (parser.Prepare(bracket)) {
		uint32_t number_cursor = 0, string_cursor = 0;
		while (parser.pos != end && *parser.pos != ']') {
			bool present = false;
			std::string value;
			int32_t number = 0;
			const bool string_token = parser.MixedToken(number, value, present);
			// Assignment precedes the delimiter result, including the final token.
			if (string_token) {
				const int base = config.maniac_message_hook_callback_user_string_variable;
				if (enabled && base != 0) HookWriteString(caller, HookOffset(base, string_cursor), value, present);
				++string_cursor;
			} else {
				const int base = config.maniac_message_hook_callback_user_variable;
				if (enabled && base != 0) HookWriteNumber(caller, HookOffset(base, number_cursor), number);
				++number_cursor;
			}
			parser.SkipSpaces();
			if (parser.pos == end || (*parser.pos != ',' && *parser.pos != ':')) break;
			++parser.pos;
			parser.SkipSpaces();
		}
		if (bracket) {
			while (parser.pos != end && *parser.pos != ']') ++parser.pos;
			if (parser.pos != end) ++parser.pos;
		} else if (parser.pos != end && *parser.pos == ';') ++parser.pos;
	}
	if (RunManiacSystemHook(caller, packet.data(), packet.size(), nullptr, operation)) {
		for (uint32_t index = 1; index <= 4; ++index) packet[index] = ReadManiacHookResult(caller, index, packet[index]);
	}
	return parser.pos;
}

static MessageHistory message_history;
static Window_Message* window = nullptr;

void Game_Message::SetWindow(Window_Message* w) {
	window = w;
}

Window_Message* Game_Message::GetWindow() {
	return window;
}

int Game_Message::GetRealPosition() {
	if (Game_Battle::IsBattleRunning()) {
		if (Feature::HasRpg2kBattleSystem()) {
			return 2;
		}
		else {
			return 0;
		}
	}

	if (Main_Data::game_system->IsMessagePositionFixed()) {
		return Main_Data::game_system->GetMessagePosition();
	}
	else {
		// Move Message Box to prevent player hiding
		int disp = Main_Data::game_player->GetScreenY();

		switch (Main_Data::game_system->GetMessagePosition()) {
		case 0: // Up
			return disp > (16 * 7) ? 0 : 2;
		case 1: // Center
			if (disp <= 16 * 7) {
				return 2;
			}
			else if (disp >= 16 * 10) {
				return 0;
			}
			else {
				return 1;
			}
		default: // Down
			return disp >= (16 * 10) ? 0 : 2;
		};
	}
}

int Game_Message::WordWrap(std::string_view line, const int limit, const WordWrapCallback& callback) {
	return WordWrap(line, limit, callback, *Font::Default());
}

int Game_Message::WordWrap(std::string_view line, const int limit, const WordWrapCallback& callback, const Font& font) {
	int start = 0;
	int line_count = 0;

	do {
		int next = start;
		do {
			auto found = line.find(' ', next);
			if (found == std::string::npos) {
				found = line.size();
			}

			auto wrapped = line.substr(start, found - start);
			auto width = Text::GetSize(font, wrapped).width;
			if (width > limit) {
				if (next == start) {
					next = found + 1;
				}
				break;
			}

			next = found + 1;
		} while(next < static_cast<int>(line.size()));

		if (start == (next - 1)) {
			start = next;
			continue;
		}

		auto wrapped = line.substr(start, (next - 1) - start);

		callback(wrapped);
		line_count++;

		start = next;
	} while (start < static_cast<int>(line.size()));

	return line_count;
}

AsyncOp Game_Message::Update() {
	if (window) {
		window->Update();
		return window->GetAsyncOp();
	}
	return {};
}

AsyncOp Game_Message::SetPendingMessage(PendingMessage&& pm, const Game_BaseInterpreterContext* caller) {
	if (window) {
		ManiacHookCaller binding;
		if (caller && !caller->GetState().stack.empty()) {
			const auto& frame = caller->GetFrame();
			binding = {frame.easyrpg_self_scope, frame.easyrpg_self_map_id, frame.easyrpg_self_event_id};
		}
		window->SetManiacHookCaller(binding);
		// This flag has no known use, but RPG_RT sets it whenever an event message command
		// spawns a message. We replicate it for save game compatibility.
		Main_Data::game_system->SetMessageEventMessageActive(pm.IsEventMessage());

		window->StartMessageProcessing(std::move(pm));
		return window->GetAsyncOp();
	}
	return {};
}

bool Game_Message::IsMessagePending() {
	return window ? window->IsMessagePending() : false;
}

bool Game_Message::IsMessageActive() {
	return window ? !window->GetAllowNextMessage(false) : false;
}

bool Game_Message::CanShowMessage(bool foreground) {
	return window ? window->GetAllowNextMessage(foreground) : false;
}

static std::optional<std::string> ManiacCommandCodeInserter(char ch, const char** iter, const char* end,
	uint32_t escape_char, HookCaller caller, bool expand_inserted) {
	if (auto preserved = PreserveManiacMixedParameters(ch, iter, end, escape_char)) return preserved;
	if (auto preserved = PreserveManiacColor(ch, iter, end, escape_char)) return preserved;
	char type = std::toupper(static_cast<unsigned char>(ch));
	bool self = false;
	if (type == 'S' && end - *iter >= 4
		&& std::toupper(static_cast<unsigned char>((*iter)[0])) == 'E'
		&& std::toupper(static_cast<unsigned char>((*iter)[1])) == 'L'
		&& std::toupper(static_cast<unsigned char>((*iter)[2])) == 'F') {
		const char kind = std::toupper(static_cast<unsigned char>((*iter)[3]));
		if (kind == 'V' || kind == 'T' || kind == 'S') {
			type = kind;
			self = true;
			*iter += 4;
		}
	}
	if (!self && (type == 'S' || type == 'W')) {
		const char* begin = *iter;
		UserHookParser parser(begin, end, caller, false,
			static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax));
		parser.Number();
		*iter = parser.pos;
		return Utils::EncodeUTF(std::u32string(1, escape_char)) + ch + std::string(begin, parser.pos);
	}
	if (type != 'V' && type != 'T' && type != 'N' && !(self && type == 'S')) return std::nullopt;
	UserHookParser parser(*iter, end, caller, true,
		static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax));
	int32_t id = parser.Number();
	*iter = parser.pos;
	if (self) {
		const uint32_t bits = 0u - static_cast<uint32_t>(id);
		std::memcpy(&id, &bits, sizeof(id));
	}
	if (type == 'V') return std::to_string(HookReadNumber(caller, id));
	if (type == 'S') {
		const bool value = id < 0 ? Game_ManiacSelfVariables::ReadSwitch(HookBank(caller, false), id)
			: Main_Data::game_switches->Get(id);
		return value ? "ON" : "OFF";
	}
	std::string value;
	if (type == 'T') value = HookReadString(caller, id).first;
	else {
		if (id == 0 && Main_Data::game_party && Main_Data::game_party->GetBattlerCount() != 0) {
			id = (*Main_Data::game_party)[0].GetId();
		}
		if (const auto* actor = Main_Data::game_actors->GetActor(id)) value = ToString(actor->GetName());
	}
	// RPG_RT performs a second lexical pass on inserted text; a T/N inserted in
	// that pass is appended raw, without a third pass over its command codes.
	if (!expand_inserted) return value;
	return PendingMessage::ApplyTextInsertingCommands(std::move(value), escape_char,
		[caller](char code, const char** next, const char* limit, uint32_t escape) {
			return ManiacCommandCodeInserter(code, next, limit, escape, caller, false);
		});
}

std::optional<std::string> Game_Message::CommandCodeInserter(char ch, const char** iter, const char* end, uint32_t escape_char) {
	if (Player::IsPatchManiac()) return ManiacCommandCodeInserter(ch, iter, end, escape_char, {}, true);
	return PendingMessage::DefaultCommandInserter(ch, iter, end, escape_char);
}

PendingMessage::CommandInserter Game_Message::MakeCommandCodeInserter(const Game_BaseInterpreterContext& context) {
	if (!Player::IsPatchManiac()) return CommandCodeInserter;
	ManiacHookCaller caller;
	if (!context.GetState().stack.empty()) {
		const auto& frame = context.GetFrame();
		caller = {frame.easyrpg_self_scope, frame.easyrpg_self_map_id, frame.easyrpg_self_event_id};
	}
	return [caller](char ch, const char** iter, const char* end, uint32_t escape_char) {
		return ManiacCommandCodeInserter(ch, iter, end, escape_char, caller, true);
	};
}

Game_Message::ParseParamResult Game_Message::ParseParam(
		char upper,
		char lower,
		const char* iter,
		const char* end,
		uint32_t escape_char,
		bool skip_prefix,
		int max_recursion)
{
	if (!skip_prefix) {
		const auto begin = iter;
		if (iter == end) {
			return { begin, 0 };
		}
		auto ret = Utils::UTF8Next(iter, end);
		// Invalid commands
		if (ret.ch != escape_char) {
			return { begin, 0 };
		}
		iter = ret.next;
		if (iter == end || (*iter != upper && *iter != lower)) {
			return { begin, 0 };
		}
		++iter;
	}

	// If no bracket, RPG_RT will return 0.
	if (iter == end || *iter != '[') {
		return { iter, 0 };
	}

	int value = 0;
	++iter;
	bool stop_parsing = false;
	bool got_valid_number = false;

	while (iter != end && *iter != ']') {
		if (stop_parsing) {
			++iter;
			continue;
		}

		// Fast inline isdigit()
		if (*iter >= '0' && *iter <= '9') {
			value *= 10;
			value += (*iter - '0');
			++iter;
			got_valid_number = true;
			continue;
		}

		if (max_recursion > 0) {
			auto ret = Utils::UTF8Next(iter, end);
			auto ch = ret.ch;
			iter = ret.next;

			// Recursive variable case.
			if (ch == escape_char) {
				if (iter != end && (*iter == 'V' || *iter == 'v')) {
					++iter;

					auto ret = ParseParam('V', 'v', iter, end, escape_char, true, max_recursion - 1);
					iter = ret.next;
					int var_val = Main_Data::game_variables->Get(ret.value);

					got_valid_number = true;

					// RPG_RT concatenates the variable value.
					int m = 10;
					if (value != 0) {
						while (m < var_val) {
							m *= 10;
						}
					}
					value = value * m + var_val;
					continue;
				}
			}
		}

		// If we hit a non-digit, RPG_RT will stop parsing until the next closing bracket.
		stop_parsing = true;
	}

	if (iter != end) {
		++iter;
	}

	// Actor 0 references the first party member
	if (upper == 'N' && value == 0 && got_valid_number) {
		auto* party = Main_Data::game_party.get();
		if (party->GetBattlerCount() > 0) {
			value = (*party)[0].GetId();
		}
	}

	return { iter, value };
}

Game_Message::ParseParamStringResult Game_Message::ParseStringParam(
		char upper,
		char lower,
		const char* iter,
		const char* end,
		uint32_t escape_char,
		bool skip_prefix,
		int max_recursion)
{
	if (!skip_prefix) {
		const auto begin = iter;
		if (iter == end) {
			return { begin, "" };
		}
		auto ret = Utils::UTF8Next(iter, end);
		// Invalid commands
		if (ret.ch != escape_char) {
			return { begin, "" };
		}
		iter = ret.next;
		if (iter == end || (*iter != upper && *iter != lower)) {
			return { begin, "" };
		}
		++iter;
	}

	// If no bracket, return empty string
	if (iter == end || *iter != '[') {
		return { iter, "" };
	}

	std::string value;
	++iter;

	while (iter != end && *iter != ']') {
		// Fast inline isdigit()
		value += *iter;

		if (max_recursion > 0) {
			auto ret = Utils::UTF8Next(iter, end);
			auto ch = ret.ch;
			iter = ret.next;

			// Recursive variable case.
			if (ch == escape_char) {
				if (iter != end && (*iter == 'V' || *iter == 'v')) {
					++iter;

					auto ret = ParseParam('V', 'v', iter, end, escape_char, true, max_recursion - 1);
					iter = ret.next;
					int var_val = Main_Data::game_variables->Get(ret.value);

					value += std::to_string(var_val);
					continue;
				}
			}
		}
	}

	if (iter != end) {
		++iter;
	}

	return { iter, value };
}

Game_Message::ParseParamResult Game_Message::ParseVariable(const char* iter, const char* end, uint32_t escape_char, bool skip_prefix, int max_recursion) {
	return ParseParam('V', 'v', iter, end, escape_char, skip_prefix, max_recursion - 1);
}

Game_Message::ParseParamResult Game_Message::ParseString(const char* iter, const char* end, uint32_t escape_char, bool skip_prefix, int max_recursion) {
	return ParseParam('T', 't', iter, end, escape_char, skip_prefix, max_recursion);
}

Game_Message::ParseParamResult Game_Message::ParseColor(const char* iter, const char* end, uint32_t escape_char, bool skip_prefix, int max_recursion) {
	return ParseParam('C', 'c', iter, end, escape_char, skip_prefix, max_recursion);
}

Game_Message::ParseParamResult Game_Message::ParseSpeed(const char* iter, const char* end, uint32_t escape_char, bool skip_prefix, int max_recursion) {
	return ParseParam('S', 's', iter, end, escape_char, skip_prefix, max_recursion);
}

Game_Message::ParseParamResult Game_Message::ParseActor(const char* iter, const char* end, uint32_t escape_char, bool skip_prefix, int max_recursion) {
	return ParseParam('N', 'n', iter, end, escape_char, skip_prefix, max_recursion);
}

MessageHistory& Game_Message::GetMessageHistory() {
	return message_history;
}

void Game_Message::ClearMessageHistory() {
	message_history.Clear();
}

std::string Game_Message::ProcessTextForHistory(const std::string& text) {
	std::string result;
	result.reserve(text.size());

	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			char cmd = text[i + 1];

			// Remove timing control characters
			if (cmd == '$' || cmd == '.' || cmd == '|' || cmd == '!' ||
				cmd == '>' || cmd == '<' || cmd == '^') {
				i++;  // Skip '\' and command character
				continue;
			}

			// Process content display characters
			if (cmd == 'V' || cmd == 'v' || cmd == 'N' || cmd == 'n' || cmd == 'P' || cmd == 'p') {
				// Find [n]
				if (i + 2 < text.size() && text[i + 2] == '[') {
					size_t end = text.find(']', i + 3);
					if (end != std::string::npos) {
						std::string num_str = text.substr(i + 3, end - i - 3);
						int id = atoi(num_str.c_str());

						if (cmd == 'V' || cmd == 'v') {
							// \V[n] - Variable value
							int value = Main_Data::game_variables->Get(id);
							result += std::to_string(value);
						} else if (cmd == 'N' || cmd == 'n') {
							// \N[n] - Actor name
							auto* actor = Main_Data::game_actors->GetActor(id);
							if (actor) {
								result += ToString(actor->GetName());
							}
						} else if (cmd == 'P' || cmd == 'p') {
							// \P[n] - Party member name
							auto* actor = Main_Data::game_party->GetActor(id);
							if (actor) {
								result += ToString(actor->GetName());
							}
						}

						i = end;  // Jump to ']' position
						continue;
					}
				}
			}

			if (cmd == 'G' || cmd == 'g') {
				// \G - Currency unit
				result += ToString(lcf::Data::terms.gold);
				i++;
				continue;
			}

			// Preserve formatting control characters (color, font size, etc.)
			if (cmd == 'C' || cmd == 'c' || cmd == 'I' || cmd == 'i' ||
				cmd == '{' || cmd == '}' || cmd == '\\') {
				result += text[i];  // Preserve '\'
				result += text[i + 1];  // Preserve command character

				// If it's \C[n] or \I[n], preserve the complete [n] part
				if ((cmd == 'C' || cmd == 'c' || cmd == 'I' || cmd == 'i') &&
					i + 2 < text.size() && text[i + 2] == '[') {
					i += 2;  // Jump to '['
					result += text[i];  // Add '['
					while (i + 1 < text.size() && text[i + 1] != ']') {
						i++;
						result += text[i];
					}
					if (i + 1 < text.size() && text[i + 1] == ']') {
						i++;
						result += text[i];  // Add ']'
					}
				} else {
					i++;  // Skip command character
				}
				continue;
			}
		}

		result += text[i];
	}

	return result;
}
