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
#include "game_switches.h"
#include "output.h"
#include <lcf/reader_util.h>
#include <lcf/data.h>

void Game_Switches::WarnGet(int variable_id) const {
	Output::Debug("Invalid read sw[{}]!", variable_id);
	--_warnings;
}

bool Game_Switches::Set(int switch_id, bool value) {
	if (EP_UNLIKELY(ShouldWarn(switch_id, switch_id))) {
		Output::Debug("Invalid write sw[{}] = {}!", switch_id, value);
		--_warnings;
	}
	if (switch_id <= 0) {
		return false;
	}
	auto& ss = _switches;
	if (static_cast<size_t>(switch_id) > ss.size()) {
		ss.resize(switch_id);
	}
	ss[switch_id - 1] = value;
	return value;
}

void Game_Switches::SetManiacRaw(int switch_id, uint8_t value) {
	if (switch_id == 0) {
		_maniac_zero_switch = value;
		return;
	}
	if (switch_id < 0) return;
	Set(switch_id, value != 0);
	_switches[switch_id - 1] = value;
}

uint8_t Game_Switches::GetManiacRawSlot(int switch_id) const {
	return switch_id == 0 ? _maniac_zero_switch : static_cast<uint8_t>(GetInt(switch_id));
}

std::vector<int32_t> Game_Switches::GetManiacRawSaveData() const {
	std::vector<int32_t> data;
	if (_maniac_zero_switch != 0) {
		data.push_back(0);
		data.push_back(_maniac_zero_switch);
	}
	for (size_t i = 0; i < _switches.size(); ++i) {
		if (_switches[i] > 1) {
			data.push_back(static_cast<int32_t>(i + 1));
			data.push_back(_switches[i]);
		}
	}
	return data;
}

void Game_Switches::SetManiacRawSaveData(const std::vector<int32_t>& data) {
	for (auto& value : _switches) value = value != 0;
	_maniac_zero_switch = 0;
	for (size_t i = 0; i + 1 < data.size(); i += 2) {
		const int32_t value = data[i + 1];
		if (value >= 0 && value <= UINT8_MAX && (data[i] == 0 || value > 1)) {
			SetManiacRaw(data[i], static_cast<uint8_t>(value));
		}
	}
}

void Game_Switches::SetRange(int first_id, int last_id, bool value) {
	if (EP_UNLIKELY(ShouldWarn(first_id, last_id))) {
		Output::Debug("Invalid write sw[{},{}] = {}!", first_id, last_id, value);
		--_warnings;
	}
	auto& ss = _switches;
	if (last_id > 0 && static_cast<size_t>(last_id) > ss.size()) {
		ss.resize(last_id, false);
	}
	for (int i = std::max(0, first_id - 1); i < last_id; ++i) {
		ss[i] = value;
	}
}

bool Game_Switches::Flip(int switch_id) {
	if (EP_UNLIKELY(ShouldWarn(switch_id, switch_id))) {
		Output::Debug("Invalid flip sw[{}]!", switch_id);
		--_warnings;
	}
	if (switch_id <= 0) {
		return false;
	}
	auto& ss = _switches;
	if (static_cast<size_t>(switch_id) > ss.size()) {
		ss.resize(switch_id);
	}
	ss[switch_id - 1] = !ss[switch_id - 1];
	return ss[switch_id - 1];
}

void Game_Switches::FlipRange(int first_id, int last_id) {
	if (EP_UNLIKELY(ShouldWarn(first_id, last_id))) {
		Output::Debug("Invalid flip sw[{},{}]!", first_id, last_id);
		--_warnings;
	}
	auto& ss = _switches;
	if (last_id > 0 && static_cast<size_t>(last_id) > ss.size()) {
		ss.resize(last_id);
	}
	for (int i = std::max(0, first_id - 1); i < last_id; ++i) {
		ss[i] = !ss[i];
	}
}

std::string_view Game_Switches::GetName(int _id) const {
	const auto* sw = lcf::ReaderUtil::GetElement(lcf::Data::switches, _id);

	if (!sw) {
		// No warning, is valid because the switch array resizes dynamic during runtime
		return {};
	} else {
		return sw->name;
	}
}

