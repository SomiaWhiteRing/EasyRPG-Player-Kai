/*
 * Import the observed Maniac 250921 self-bank layout from LSD root chunk 0x74.
 * This is intentionally strict: an unrecognized field must not yield a
 * partially restored game state. Encoding covers these same observed fields;
 * connecting it to regular saves requires version and platform validation.
 */
#ifndef EP_MANIAC_ORIGINAL_SAVE_H
#define EP_MANIAC_ORIGINAL_SAVE_H

#include "game_maniac_self_variables.h"
#include "utils.h"

#include <lcf/rpg/save.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ManiacOriginalSave {

// Native slot IDs and run lengths are signed 32-bit values. A bank also
// contains its physical slot zero, which is not written to the native field.
constexpr uint64_t MaxPositiveSlots = std::numeric_limits<int32_t>::max();

class Cursor {
public:
	Cursor(const uint8_t* bytes, size_t length) : bytes(bytes), length(length) {}

	bool Done() const { return position == length; }
	size_t Remaining() const { return length - position; }

	bool Ber(uint64_t& value) {
		value = 0;
		for (int i = 0; i < 5; ++i) {
			if (Done()) return false;
			const uint8_t octet = bytes[position++];
			value = (value << 7) | (octet & 0x7f);
			if (!(octet & 0x80)) return true;
		}
		return false;
	}

	bool Slice(uint64_t size, Cursor& result) {
		if (size > Remaining()) return false;
		result = Cursor(bytes + position, static_cast<size_t>(size));
		position += static_cast<size_t>(size);
		return true;
	}

	bool Byte(uint8_t& value) {
		if (Done()) return false;
		value = bytes[position++];
		return true;
	}

	bool Int32(int32_t& value) {
		if (Remaining() < 4) return false;
		const uint32_t bits = uint32_t(bytes[position])
			| (uint32_t(bytes[position + 1]) << 8)
			| (uint32_t(bytes[position + 2]) << 16)
			| (uint32_t(bytes[position + 3]) << 24);
		std::memcpy(&value, &bits, sizeof(value));
		position += 4;
		return true;
	}

	bool Utf16(std::string& result, uint64_t units) {
		if (units > Remaining() / 2) return false;
		result.clear();
		for (uint64_t i = 0; i < units; ++i) {
			const uint32_t first = uint32_t(bytes[position]) | (uint32_t(bytes[position + 1]) << 8);
			position += 2;
			uint32_t codepoint = first;
			if (first >= 0xd800 && first <= 0xdbff && i + 1 < units) {
				const uint32_t second = uint32_t(bytes[position]) | (uint32_t(bytes[position + 1]) << 8);
				if (second >= 0xdc00 && second <= 0xdfff) {
					position += 2;
					++i;
					codepoint = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
				}
			}
			if (codepoint <= 0x7f) {
				result.push_back(static_cast<char>(codepoint));
			} else if (codepoint <= 0x7ff) {
				result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
				result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
			} else if (codepoint <= 0xffff) {
				result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
				result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
				result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
			} else {
				result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
				result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
				result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
				result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
			}
		}
		return true;
	}

private:
	const uint8_t* bytes;
	size_t length;
	size_t position = 0;
};

inline bool DecodeStringSlots(Cursor& field, Game_ManiacSelfVariables::Bank& bank) {
	bank.strings = {lcf::DBString{}}; // Slot zero is absent from the original payload.
	bank.empty_string_slots.clear();
	while (!field.Done()) {
		if (bank.strings.size() > MaxPositiveSlots) return false;
		uint64_t units = 0;
		if (!field.Ber(units)) return false;
		// The native loader treats zero as a zero-slot run. Even an allocated
		// empty string written by the native encoder does not survive this load.
		if (units == 0) continue;
		if (units >= (uint64_t(1) << 35) - MaxPositiveSlots) {
			const uint64_t count = (uint64_t(1) << 35) - units;
			if (count > MaxPositiveSlots + 1 - bank.strings.size()) return false;
			bank.strings.resize(bank.strings.size() + static_cast<size_t>(count));
			continue;
		}
		std::string text;
		if (!field.Utf16(text, units)) return false;
		bank.strings.emplace_back(std::string_view(text));
	}
	bank.string_vector_size = static_cast<int32_t>(bank.strings.size());
	return true;
}

inline bool ImportObservedGlobals(lcf::rpg::Save& save) {
	if (!save.system.maniac_strings_utf16) return true;
	Game_ManiacSelfVariables::Bank bank;
	const auto& bytes = save.system.maniac_string_payload;
	Cursor field(bytes.data(), bytes.size());
	if (!DecodeStringSlots(field, bank)) return false;
	save.system.maniac_strings.assign(bank.strings.begin() + 1, bank.strings.end());
	if (save.easyrpg_data.maniac_global_string_raw_bytes.empty()) {
		save.easyrpg_data.maniac_global_string_empty_slots = std::move(bank.empty_string_slots);
		save.easyrpg_data.maniac_global_string_vector_size = std::max(
			save.easyrpg_data.maniac_global_string_vector_size, bank.string_vector_size - 1);
	}
	return true;
}

inline bool Leaf(Cursor& record, Game_ManiacSelfVariables::Bank& bank) {
	bool seen[4] = {};
	for (;;) {
		uint64_t id = 0;
		if (!record.Ber(id)) return false;
		if (id == 0) return true;
		if (id > 3 || seen[id]) return false;
		seen[id] = true;
		uint64_t size = 0;
		if (!record.Ber(size)) return false;
		Cursor field(nullptr, 0);
		if (!record.Slice(size, field)) return false;
		if (id == 1) {
			bank.switches.emplace_back(0);
			bool value = true;
			bool first = true;
			while (!field.Done()) {
				uint64_t count = 0;
				if (!field.Ber(count)) return false;
				if (first && count >= (uint64_t(1) << 35) - MaxPositiveSlots) {
					count = (uint64_t(1) << 35) - count;
					value = false;
				}
				if (!count || count > MaxPositiveSlots + 1 - bank.switches.size()) return false;
				bank.switches.insert(bank.switches.end(), static_cast<size_t>(count), value ? 1 : 0);
				value = !value;
				first = false;
			}
		} else if (id == 2) {
			if (field.Remaining() % 4 || field.Remaining() / 4 > MaxPositiveSlots) return false;
			bank.numbers.resize(field.Remaining() / 4 + 1);
			for (size_t slot = 1; slot < bank.numbers.size(); ++slot) {
				if (!field.Int32(bank.numbers[slot])) return false;
			}
		} else {
			if (!DecodeStringSlots(field, bank)) return false;
		}
		if (!field.Done()) return false;
	}
}

inline bool Events(Cursor& list, Game_ManiacSelfVariables& state, int map_id) {
	uint64_t count = 0;
	if (!list.Ber(count) || count > 10000) return false;
	for (uint64_t index = 0; index < count; ++index) {
		uint64_t event_id = 0;
		if (!list.Ber(event_id) || event_id == 0 || event_id > std::numeric_limits<int32_t>::max()
			|| state.Find(map_id ? 2 : 1, map_id, static_cast<int>(event_id))) return false;
		auto* bank = state.Ensure(map_id ? 2 : 1, map_id, static_cast<int>(event_id));
		if (!bank || !Leaf(list, *bank)) return false;
	}
	return list.Done();
}

inline bool Maps(Cursor& list, Game_ManiacSelfVariables& state) {
	uint64_t count = 0;
	if (!list.Ber(count) || count > 10000) return false;
	for (uint64_t index = 0; index < count; ++index) {
		uint64_t map_id = 0, field_id = 0, size = 0;
		if (!list.Ber(map_id) || map_id == 0 || map_id > std::numeric_limits<int32_t>::max()) return false;
		if (!list.Ber(field_id) || field_id != 2 || !list.Ber(size)) return false;
		Cursor events(nullptr, 0);
		if (!list.Slice(size, events) || !Events(events, state, static_cast<int>(map_id))) return false;
		if (!list.Ber(field_id) || field_id != 0) return false;
	}
	return list.Done();
}

inline bool Decode(const std::vector<uint8_t>& bytes, Game_ManiacSelfVariables::Banks& banks) {
	if (bytes.empty()) return false;
	Game_ManiacSelfVariables state;
	Cursor root(bytes.data(), bytes.size());
	bool seen_common = false, seen_map = false;
	while (!root.Done()) {
		uint64_t id = 0, size = 0;
		if (!root.Ber(id)) return false;
		if (id == 0) {
			if (!root.Done()) return false;
			break;
		}
		if (!root.Ber(size) || id > 2) return false;
		Cursor child(nullptr, 0);
		if (!root.Slice(size, child)) return false;
		if (id == 1) {
			if (seen_common || !Events(child, state, 0)) return false;
			seen_common = true;
		} else {
			if (seen_map || !Maps(child, state)) return false;
			seen_map = true;
		}
	}
	if (!seen_common && !seen_map) return false;
	banks = std::move(state.MutableBanks());
	return true;
}

inline void RestoreManiacFrameIdentity(lcf::rpg::SaveEventExecState& state, int map_id) {
	for (auto& frame : state.stack) {
		if (frame.easyrpg_self_scope != 0) continue;
		if ((frame.maniac_event_info & 0xf0) == 0x20 && frame.maniac_event_id > 0) {
			frame.easyrpg_self_scope = 1;
			frame.easyrpg_self_map_id = 0;
			frame.easyrpg_self_event_id = frame.maniac_event_id;
		} else if (map_id > 0 && frame.event_id > 0) {
			// A zero map event_id can mean an event on a previous map.
			// Its native ID alone does not identify the owning map.
			frame.easyrpg_self_scope = 2;
			frame.easyrpg_self_map_id = map_id;
			frame.easyrpg_self_event_id = frame.event_id;
		}
	}
}

inline bool ImportObservedBanks(lcf::rpg::Save& save) {
	const int map_id = save.party_location.map_id;
	RestoreManiacFrameIdentity(save.foreground_event_execstate, map_id);
	for (auto& event : save.map_info.events) {
		RestoreManiacFrameIdentity(event.parallel_event_execstate, map_id);
	}
	for (auto& event : save.common_events) {
		RestoreManiacFrameIdentity(event.parallel_event_execstate, map_id);
	}
	if (save.maniac_original_bank_bytes.empty()
		|| !save.easyrpg_data.maniac_self_variable_banks.empty()) return true;
	Game_ManiacSelfVariables::Banks imported;
	if (!Decode(save.maniac_original_bank_bytes, imported)) return false;
	save.easyrpg_data.maniac_self_variable_banks = std::move(imported);
	return true;
}

inline void PutBer(std::vector<uint8_t>& out, uint64_t value) {
	uint8_t reversed[5];
	size_t length = 0;
	do {
		reversed[length++] = static_cast<uint8_t>(value & 0x7f);
		value >>= 7;
	} while (value);
	while (length > 1) out.push_back(static_cast<uint8_t>(reversed[--length] | 0x80));
	out.push_back(reversed[0]);
}

inline void PutField(std::vector<uint8_t>& out, uint64_t id, const std::vector<uint8_t>& payload) {
	PutBer(out, id);
	PutBer(out, payload.size());
	out.insert(out.end(), payload.begin(), payload.end());
}

inline bool EncodeStringSlots(const Game_ManiacSelfVariables::Bank& bank, std::vector<uint8_t>& out) {
	if (bank.strings.size() > MaxPositiveSlots + 1) return false;
	std::vector<uint8_t> result;
	size_t end = bank.strings.size();
	while (end > 1 && !Game_ManiacSelfVariables::IsStringPresent(&bank, end - 1)) --end;
	for (size_t i = 1; i < end; ++i) {
		if (!Game_ManiacSelfVariables::IsStringPresent(&bank, i)) {
			size_t next = i + 1;
			while (next < end && !Game_ManiacSelfVariables::IsStringPresent(&bank, next)) ++next;
			PutBer(result, (uint64_t(1) << 35) - (next - i));
			i = next - 1;
			continue;
		}
		bool valid;
		const auto units = Utils::DecodeUTF16(static_cast<std::string_view>(bank.strings[i]), true, &valid);
		if (!valid) return false;
		PutBer(result, units.size());
		for (char16_t unit : units) {
			result.push_back(static_cast<uint8_t>(unit));
			result.push_back(static_cast<uint8_t>(unit >> 8));
		}
	}
	out = std::move(result);
	return true;
}

inline bool ImportObservedGlobalScalars(lcf::rpg::Save& save) {
	const auto& variables = save.easyrpg_data.maniac_global_variables;
	const auto& switches = save.easyrpg_data.maniac_global_switches;
	if ((!variables.empty() && (variables.size() < save.system.variables.size()
		|| !std::equal(save.system.variables.begin(), save.system.variables.end(), variables.begin())))
		|| (!switches.empty() && (switches.size() < save.system.switches.size()
		|| !std::equal(save.system.switches.begin(), save.system.switches.end(), switches.begin())))) return false;
	if (!variables.empty()) save.system.variables = variables;
	if (!switches.empty()) save.system.switches = switches;
	if (save.easyrpg_data.maniac_global_switch_raw.empty() && save.system.maniac_switch_payload_present) {
		const auto& bytes = save.system.maniac_switch_payload;
		for (size_t i = 0; i < bytes.size(); ++i) {
			if (bytes[i] > 1) {
				save.easyrpg_data.maniac_global_switch_raw.push_back(static_cast<int32_t>(i + 1));
				save.easyrpg_data.maniac_global_switch_raw.push_back(bytes[i]);
			}
		}
	}
	return true;
}

inline bool EncodeObservedGlobalScalars(lcf::rpg::Save& save) {
	std::vector<uint8_t> switches;
	switches.reserve(save.system.switches.size());
	for (bool value : save.system.switches) switches.push_back(value ? 1 : 0);
	const auto& raw = save.easyrpg_data.maniac_global_switch_raw;
	if (raw.size() % 2) return false;
	for (size_t i = 0; i < raw.size(); i += 2) {
		if (raw[i] < 0 || raw[i + 1] < 0 || raw[i + 1] > UINT8_MAX) return false;
		if (raw[i] == 0) continue; // Physical slot zero belongs to Player private state.
		if (static_cast<size_t>(raw[i]) > switches.size()) return false;
		if (raw[i + 1] <= 1 || !save.system.switches[raw[i] - 1]) return false;
		switches[raw[i] - 1] = static_cast<uint8_t>(raw[i + 1]);
	}
	save.easyrpg_data.maniac_global_variables = save.system.variables;
	save.easyrpg_data.maniac_global_switches = save.system.switches;
	while (!save.system.variables.empty() && save.system.variables.back() == 0) save.system.variables.pop_back();
	while (!switches.empty() && switches.back() == 0) switches.pop_back();
	save.system.switches.resize(switches.size());
	save.system.maniac_switch_payload = std::move(switches);
	save.system.maniac_switch_payload_present = true;
	save.system.maniac_global_scalars_active = true;
	return true;
}

inline bool EncodeObservedGlobals(lcf::rpg::Save& save) {
	Game_ManiacSelfVariables::Bank bank;
	bank.strings = {lcf::DBString{}};
	bank.strings.insert(bank.strings.end(), save.system.maniac_strings.begin(), save.system.maniac_strings.end());
	const int32_t size = save.easyrpg_data.maniac_global_string_vector_size;
	if (size < 0) return false;
	if (size_t(size) + 1 > bank.strings.size()) bank.strings.resize(size_t(size) + 1);
	bank.empty_string_slots = save.easyrpg_data.maniac_global_string_empty_slots;
	std::vector<uint8_t> bytes;
	if (!EncodeStringSlots(bank, bytes)) return false;
	save.system.maniac_string_payload = std::move(bytes);
	save.system.maniac_string_payload_present = true;
	save.system.maniac_strings_utf16 = true;
	return true;
}

inline bool EncodeLeaf(const Game_ManiacSelfVariables::Bank& bank, std::vector<uint8_t>& out) {
	if (bank.strings.size() > MaxPositiveSlots + 1) return false;
	size_t switch_end = bank.switches.size();
	while (switch_end > 1 && bank.switches[switch_end - 1] == 0) --switch_end;
	if (switch_end > MaxPositiveSlots + 1) return false;
	if (switch_end > 1) {
		std::vector<uint8_t> payload;
		for (size_t i = 1; i < switch_end;) {
			const bool value = bank.switches[i] != 0;
			size_t next = i + 1;
			while (next < switch_end && (bank.switches[next] != 0) == value) ++next;
			const uint64_t count = next - i;
			PutBer(payload, i == 1 && !value ? (uint64_t(1) << 35) - count : count);
			i = next;
		}
		PutField(out, 1, payload);
	}
	size_t number_end = bank.numbers.size();
	while (number_end > 1 && bank.numbers[number_end - 1] == 0) --number_end;
	if (number_end > MaxPositiveSlots + 1) return false;
	if (number_end > 1) {
		std::vector<uint8_t> payload;
		payload.reserve((number_end - 1) * 4);
		for (size_t i = 1; i < number_end; ++i) {
			uint32_t bits = 0;
			std::memcpy(&bits, &bank.numbers[i], sizeof(bits));
			for (int byte = 0; byte < 4; ++byte) {
				payload.push_back(static_cast<uint8_t>(bits >> (byte * 8)));
			}
		}
		PutField(out, 2, payload);
	}
	if (bank.strings.size() > 1) {
		std::vector<uint8_t> payload;
		if (!EncodeStringSlots(bank, payload)) return false;
		if (!payload.empty()) PutField(out, 3, payload);
	}
	PutBer(out, 0);
	return true;
}

inline bool EncodeObservedBanks(const Game_ManiacSelfVariables::Banks& banks,
		std::vector<uint8_t>& bytes) {
	std::map<int, const Game_ManiacSelfVariables::Bank*> common;
	std::map<int, std::map<int, const Game_ManiacSelfVariables::Bank*>> maps;
	for (const auto& bank : banks) {
		if (bank.event_id <= 0 || bank.event_id > 1000000
			|| bank.strings.size() > MaxPositiveSlots + 1) return false;
		if (bank.scope == 1) {
			if (bank.map_id != 0 || !common.emplace(bank.event_id, &bank).second) return false;
		} else if (bank.scope == 2) {
			if (bank.map_id <= 0 || bank.map_id > 1000000
				|| !maps[bank.map_id].emplace(bank.event_id, &bank).second) return false;
		} else {
			return false;
		}
	}
	if (common.size() > 10000 || maps.size() > 10000) return false;
	std::vector<uint8_t> result;
	if (!common.empty()) {
		std::vector<uint8_t> events;
		PutBer(events, common.size());
		for (const auto& [id, bank] : common) {
			PutBer(events, id);
			if (!EncodeLeaf(*bank, events)) return false;
		}
		PutField(result, 1, events);
	}
	if (!maps.empty()) {
		std::vector<uint8_t> map_list;
		PutBer(map_list, maps.size());
		for (const auto& [map_id, event_banks] : maps) {
			if (event_banks.size() > 10000) return false;
			std::vector<uint8_t> events;
			PutBer(events, event_banks.size());
			for (const auto& [event_id, bank] : event_banks) {
				PutBer(events, event_id);
				if (!EncodeLeaf(*bank, events)) return false;
			}
			PutBer(map_list, map_id);
			PutField(map_list, 2, events);
			PutBer(map_list, 0);
		}
		PutField(result, 2, map_list);
	}
	if (!result.empty()) PutBer(result, 0);
	bytes = std::move(result);
	return true;
}

} // namespace ManiacOriginalSave

#endif
