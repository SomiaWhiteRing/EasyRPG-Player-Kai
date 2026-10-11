/*
 * Maniac self-variable banks owned by the current game. The keys and values
 * are also the Player save representation; original RPG_RT interoperability
 * remains a separate format question.
 */
#ifndef EP_GAME_MANIAC_SELF_VARIABLES_H
#define EP_GAME_MANIAC_SELF_VARIABLES_H

#include <lcf/rpg/saveeasyrpgselfvariablebank.h>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "async_op.h"
#include "maniac_string_storage.h"

class Game_ManiacSelfVariables {
public:
	using Bank = lcf::rpg::SaveEasyRpgSelfVariableBank;
	using Banks = std::vector<Bank>;

	const Bank* Find(int scope, int map_id, int event_id) const {
		if (event_id <= 0 || (scope != 1 && scope != 2) || (scope == 2 && map_id <= 0)) {
			return nullptr;
		}
		auto it = std::find_if(banks.begin(), banks.end(), [&](const Bank& bank) {
			return bank.scope == scope && bank.map_id == (scope == 1 ? 0 : map_id)
				&& bank.event_id == event_id;
		});
		return it == banks.end() ? nullptr : &*it;
	}
	Bank* FindMutable(int scope, int map_id, int event_id) {
		return const_cast<Bank*>(static_cast<const Game_ManiacSelfVariables&>(*this).Find(scope, map_id, event_id));
	}

	Bank* Ensure(int scope, int map_id, int event_id) {
		if (event_id <= 0 || (scope != 1 && scope != 2) || (scope == 2 && map_id <= 0)) {
			return nullptr;
		}
		map_id = scope == 1 ? 0 : map_id;
		auto it = std::find_if(banks.begin(), banks.end(), [&](const Bank& bank) {
			return bank.scope == scope && bank.map_id == map_id && bank.event_id == event_id;
		});
		if (it != banks.end()) {
			return &*it;
		}
		banks.emplace_back();
		auto& bank = banks.back();
		bank.scope = scope;
		bank.map_id = map_id;
		bank.event_id = event_id;
		return &bank;
	}

	const Banks& GetSaveData() const { return banks; }
	Banks GetSerializableSaveData() const {
		Banks result = banks;
		for (auto& bank : result) {
			bank.string_vector_size = static_cast<int32_t>(bank.strings.size());
			bank.string_raw_bytes = ManiacStringStorage::Encode(bank.strings);
		}
		return result;
	}
	Banks& MutableBanks() { return banks; }
	bool SetSaveData(Banks value) {
		for (auto& bank : value) {
			if (!bank.string_raw_bytes.empty()) {
				if (!ManiacStringStorage::Decode(bank.string_raw_bytes, bank.strings)) return false;
				bank.string_raw_bytes.clear();
			}
		}
		banks = std::move(value);
		for (auto& bank : banks) {
			if (bank.string_vector_size >= 0
				&& bank.strings.size() < static_cast<size_t>(bank.string_vector_size)) {
				bank.strings.resize(static_cast<size_t>(bank.string_vector_size));
			}
			auto& ids = bank.empty_string_slots;
			ids.erase(std::remove_if(ids.begin(), ids.end(), [](int32_t id) {
				return id < 0;
			}), ids.end());
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			if (!ids.empty() && bank.strings.size() <= static_cast<size_t>(ids.back())) {
				bank.strings.resize(static_cast<size_t>(ids.back()) + 1);
			}
			ids.erase(std::remove_if(ids.begin(), ids.end(), [&](int32_t id) {
				return !bank.strings[static_cast<size_t>(id)].empty();
			}), ids.end());
		}
		return true;
	}

	static int32_t ReadNumber(const Bank* bank, int id) {
		if (!bank || id >= 0) {
			return 0;
		}
		const auto slot = -static_cast<int64_t>(id);
		return slot < static_cast<int64_t>(bank->numbers.size()) ? bank->numbers[static_cast<size_t>(slot)] : 0;
	}

	static int ReadSwitch(const Bank* bank, int id) {
		if (!bank || id >= 0) {
			return 0;
		}
		const auto slot = -static_cast<int64_t>(id);
		return slot < static_cast<int64_t>(bank->switches.size()) ? bank->switches[static_cast<size_t>(slot)] : 0;
	}

	static bool IsStringPresent(const Bank* bank, size_t slot) {
		if (!bank || slot >= bank->strings.size()) return false;
		if (!bank->strings[slot].empty()) return true;
		return std::binary_search(bank->empty_string_slots.begin(), bank->empty_string_slots.end(),
			static_cast<int32_t>(slot));
	}

	static void SetStringPresence(Bank* bank, size_t slot, bool present) {
		if (!bank || slot >= bank->strings.size()) return;
		auto& ids = bank->empty_string_slots;
		const int32_t id = static_cast<int32_t>(slot);
		auto it = std::lower_bound(ids.begin(), ids.end(), id);
		const bool listed = it != ids.end() && *it == id;
		if (present && bank->strings[slot].empty()) {
			if (!listed) ids.insert(it, id);
		} else if (listed) {
			ids.erase(it);
		}
	}

private:
	Banks banks;
};

inline bool ApplyManiacSaveResult(AsyncOp const& operation, bool success,
	Game_ManiacSelfVariables* state) {
	if (operation.GetType() != AsyncOp::eSave || operation.GetSaveResultVar() >= 0 || !state) {
		return false;
	}
	const auto slot = -static_cast<int64_t>(operation.GetSaveResultVar());
	auto* bank = state->Ensure(operation.GetSaveSelfScope(), operation.GetSaveSelfMapId(),
		operation.GetSaveSelfEventId());
	if (!bank) return false;
	bank->numbers.resize(std::max(bank->numbers.size(), static_cast<size_t>(slot + 1)));
	bank->numbers[static_cast<size_t>(slot)] = success ? 1 : 0;
	return true;
}

#endif
