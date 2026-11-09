#include "game_netplay.h"
#include "game_clock.h"
#include "game_map.h"
#include "game_maniac_self_variables.h"
#include "game_pictures.h"
#include "game_screen.h"
#include "game_strings.h"
#include "game_switches.h"
#include "game_system.h"
#include "game_variables.h"
#include "input.h"
#include "main_data.h"
#include "maniac_picture_renderer.h"
#include "netplay_rollback.h"
#include "player.h"
#include "rand.h"

#include <lcf/rpg/save.h>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <variant>

#ifdef PLAYER_WEB
#include <emscripten.h>
#endif

namespace {

using Audio = std::variant<lcf::rpg::Music, lcf::rpg::Sound, int>;

struct Effects {
	std::vector<Audio> audio;
	std::map<int, int> samples;
	std::map<int, int> picture_retirements;
	bool stop = false;
};

struct FrameContext {
	int index;
	int seat;
	std::array<int, 2> inputs;
	bool known;
	Effects& effects;
	bool& needs_confirmation;
	std::set<int>& confirmed_pictures;
	std::map<int, Game_Netplay::PictureView>& picture_transforms;
	std::map<int, int>& picture_births;
	const std::map<int, int>& presented_samples;
	const std::map<int, std::pair<int, int>>& drawn_pictures;
};

FrameContext* frame = nullptr;

#ifdef PLAYER_WEB

struct MapWorld {
	explicit MapWorld(int seat) : seat(seat) {}

	struct Snapshot {
		lcf::rpg::Save map;
		Game_Pictures::NetplaySnapshot pictures;
		std::vector<int32_t> raw_switches;
		Game_Strings::Strings_t strings;
		std::vector<lcf::DBString> string_zero;
		size_t string_size;
		Game_ManiacSelfVariables::Banks self_variables;
		std::set<int> confirmed_pictures;
		std::map<int, Game_Netplay::PictureView> picture_transforms;
		std::map<int, int> picture_births;
		Rand::RNG rng;
		uint32_t maniac_seed;
		int32_t variable_zero;
		bool need_refresh;
	};

	struct Effects : ::Effects {
		std::vector<Sprite_Picture::NetplayPose> pictures;
		std::unique_ptr<Snapshot> terminal;
	};

	Snapshot Capture() const {
		auto& strings = *Main_Data::game_strings;
		Snapshot state{{}, Main_Data::game_pictures->CaptureNetplay(),
			Main_Data::game_switches->GetManiacRawSaveData(), strings.GetData(), strings.GetManiacZeroSaveData(),
			strings.GetManiacVectorSize(),
			Main_Data::game_maniac_self_variables ? Main_Data::game_maniac_self_variables->GetSaveData()
				: Game_ManiacSelfVariables::Banks{},
			confirmed_pictures, picture_transforms, picture_births, Rand::GetRNG(),
			Main_Data::maniac_random_seed, Main_Data::game_variables->GetManiacRawSlot(0), Game_Map::GetNeedRefresh()};
		Game_Map::CaptureNetplay(state.map);
		state.map.screen = Main_Data::game_screen->GetSaveData();
		state.map.system.variables = Main_Data::game_variables->GetData();
		state.map.system.switches = Main_Data::game_switches->GetData();
		return state;
	}

	void Restore(Snapshot state) {
		Main_Data::game_variables->SetData(std::move(state.map.system.variables));
		*Main_Data::game_variables->GetManiacWritable(0) = state.variable_zero;
		Main_Data::game_switches->SetData(std::move(state.map.system.switches));
		Main_Data::game_switches->SetManiacRawSaveData(state.raw_switches);
		auto& strings = *Main_Data::game_strings;
		strings.SetData(std::move(state.strings));
		strings.EnsureSize(state.string_size);
		strings.SetManiacZeroSaveData(state.string_zero);
		if (!state.self_variables.empty() && !Main_Data::game_maniac_self_variables)
			Main_Data::game_maniac_self_variables = std::make_unique<Game_ManiacSelfVariables>();
		if (Main_Data::game_maniac_self_variables)
			Main_Data::game_maniac_self_variables->MutableBanks() = std::move(state.self_variables);
		confirmed_pictures = std::move(state.confirmed_pictures);
		picture_transforms = std::move(state.picture_transforms);
		picture_births = std::move(state.picture_births);
		Main_Data::game_screen->SetSaveData(std::move(state.map.screen));
		Main_Data::game_pictures->RestoreNetplay(std::move(state.pictures));
		Game_Map::RestoreNetplay(state.map);
		Game_Map::SetNeedRefresh(state.need_refresh);
		Rand::GetRNG() = state.rng;
		Main_Data::maniac_random_seed = state.maniac_seed;
	}

	bool Step(int index, std::array<int, 2> inputs, bool known, Effects& effects, bool& needs_confirmation) {
		FrameContext context{index, seat, inputs, known, effects, needs_confirmation,
			confirmed_pictures, picture_transforms, picture_births, presented_samples, drawn_pictures};
		frame = &context;
		const bool complete = Game_Map::UpdateNetplay();
		frame = nullptr;
		if (!complete) return false;
		if (effects.stop) effects.terminal = std::make_unique<Snapshot>(Capture());
		auto& screen = *Main_Data::game_screen;
		for (int id : confirmed_pictures) {
			const auto* picture = Main_Data::game_pictures->GetPicturePtr(id);
			if (!picture || !picture->sprite || !picture->sprite->IsVisible() || !picture->IsOnMap() ||
					picture->bitmap_modified || picture->IsWindowAttached()) continue;
			const auto& bitmap = picture->sprite->GetBitmap();
			if (!bitmap) continue;
			auto pixels = Player::IsPatchManiac()
				? ManiacPicture::DescribeFrame(*bitmap, picture->data, screen.GetSaveData(), picture->GetManiacImageChromakey())
				: std::nullopt;
			effects.pictures.push_back({picture->data, bitmap, screen.GetTone(), screen.GetFlashColor(),
				screen.GetShakeOffsetX(), screen.GetShakeOffsetY(), pixels});
		}
		return true;
	}

	bool Publish(int, Effects effects) {
		stopping = effects.stop;
		pending.push_back(std::move(effects));
		return stopping;
	}

	bool Present() {
		if (pending.empty()) return false;
		auto effects = std::move(pending.front());
		pending.pop_front();
		presented_samples = std::move(effects.samples);
		for (const auto& [id, birth] : effects.picture_retirements)
			presented_retirements.insert_or_assign(id, birth);
		for (auto& item : displayed) item.second->SetVisible(false);
		for (auto& pose : effects.pictures) {
			const int id = pose.data.ID;
			auto& sprite = displayed[id];
			if (!sprite) sprite = std::make_unique<Sprite_Picture>(id);
			sprite->PresentNetplay(std::move(pose));
		}
		auto& system = *Main_Data::game_system;
		for (const auto& effect : effects.audio) {
			if (auto* music = std::get_if<lcf::rpg::Music>(&effect)) system.BgmPlay(*music);
			else if (auto* sound = std::get_if<lcf::rpg::Sound>(&effect)) system.SePlay(*sound, true);
			else system.BgmFade(std::get<int>(effect));
		}
		if (effects.terminal) Restore(std::move(*effects.terminal));
		return effects.stop;
	}

	std::set<int> confirmed_pictures;
	std::map<int, Game_Netplay::PictureView> picture_transforms;
	std::map<int, int> picture_births;
	std::map<int, int> presented_retirements;
	std::map<int, int> presented_samples;
	std::map<int, std::pair<int, int>> drawn_pictures;
	std::map<int, std::unique_ptr<Sprite_Picture>> displayed;
	std::deque<Effects> pending;
	bool stopping = false;
	const int seat;
};

struct Match {
	Match(int id, int seed, int seat)
		: id(id), seed(seed), seat(seat), map_id(Game_Map::GetMapId()), world(seat), timeline(world, seat) {}

	int id;
	int seed;
	int seat;
	int map_id;
	bool announced = false;
	bool peer_ready = false;
	bool started = false;
	MapWorld world;
	Game_Netplay::Rollback<MapWorld> timeline;
};

std::unique_ptr<Match> match;

int ReadTransportState() {
	return EM_ASM_INT({ return Module.netplay?.state() ?? 0; });
}

#endif

}

void Game_Netplay::Command(std::string_view comment) {
	std::istringstream args{std::string(comment)};
	std::string prefix, command;
	args >> prefix >> command;
	if (command == "close" && !frame) {
		Close();
		return;
	}
	if (command == "stop" && frame) {
		frame->effects.stop = true;
		return;
	}
	if (command == "rooms" && !frame) {
#ifdef PLAYER_WEB
		int string_id;
		if (!(args >> string_id) || string_id <= 0) return;
		auto* data = reinterpret_cast<char*>(EM_ASM_PTR({
			const text = Module.netplay?.rooms();
			if (text === undefined) return 0;
			const size = lengthBytesUTF8(text) + 1;
			const buffer = _malloc(size);
			if (buffer) stringToUTF8(text, buffer, size);
			return buffer;
		}));
		if (data) {
			Main_Data::game_strings->SetManiacRawSlot(string_id, data);
			std::free(data);
		}
#endif
		return;
	}

	int variable;
	auto& vars = *Main_Data::game_variables;
	if (!(args >> variable) || !vars.IsValid(variable)) return;
	const auto write = [&](int id, int value) {
		if (vars.Get(id) == value) return;
		vars.Set(id, value);
		Game_Map::SetNeedRefreshForVarChange(id);
	};
	if (command == "active") {
		write(variable, frame != nullptr);
		return;
	}
	if (command == "seat") {
#ifdef PLAYER_WEB
		write(variable, frame ? frame->seat + 1 : std::max(0, ReadTransportState() - 2));
#else
		write(variable, 0);
#endif
		return;
	}
	if (frame) {
		if (command == "input") {
			int guest;
			if (!(args >> guest) || guest == variable || !vars.IsValid(guest)) return;
			write(variable, frame->inputs[0]);
			write(guest, frame->inputs[1]);
		} else if (command == "frame") {
			write(variable, frame->index);
		} else if (command == "known") {
			frame->needs_confirmation = !frame->known;
			write(variable, frame->known);
		} else if (command == "sample") {
			int count;
			if (!(args >> count) || count < 1 || count > vars.GetSizeWithLimit() - variable + 1) return;
			for (int i = 0; i < count; ++i)
				frame->effects.samples.insert_or_assign(variable + i, vars.Get(variable + i));
		} else if (command == "shown") {
			int destination, count;
			if (!(args >> destination >> count) || !vars.IsValid(destination) || count < 1 ||
					count > vars.GetSizeWithLimit() - std::max(variable, destination) + 1) return;
			for (int i = 0; i < count; ++i) {
				const auto found = frame->presented_samples.find(variable + i);
				write(destination + i, found == frame->presented_samples.end() ? 0 : found->second);
			}
		} else if (command == "drawn") {
			int x, y;
			if (!(args >> x >> y) || !vars.IsValid(x) || !vars.IsValid(y) || x == y) return;
			const auto found = frame->drawn_pictures.find(vars.Get(variable));
			if (found == frame->drawn_pictures.end()) {
				write(x, 0);
				write(y, 0);
			} else {
				write(x, found->second.first);
				write(y, found->second.second);
			}
		} else if (command == "present") {
			const int picture = vars.Get(variable);
			if (picture > 0 && picture <= Main_Data::game_pictures->GetMaxPictureId())
				frame->confirmed_pictures.insert(picture);
		} else if (command == "live") {
			frame->confirmed_pictures.erase(vars.Get(variable));
		} else if (command == "birth" || command == "retire") {
			const int picture = vars.Get(variable);
			if (picture <= 0 || picture > Main_Data::game_pictures->GetMaxPictureId()) return;
			if (command == "birth") {
				frame->picture_births.insert_or_assign(picture, frame->index);
			} else if (const auto birth = frame->picture_births.find(picture); birth != frame->picture_births.end()) {
				frame->effects.picture_retirements.insert_or_assign(picture, birth->second);
			}
		} else if (command == "track") {
			int last_var, flags;
			if (!(args >> last_var >> flags) || !vars.IsValid(last_var) || flags < 0 || flags > 13 ||
					(flags & 6) == 6 || (flags & 9) == 9) return;
			const int first = vars.Get(variable), last = vars.Get(last_var);
			if (first <= 0 || first > last || last > Main_Data::game_pictures->GetMaxPictureId()) return;
			PictureView view;
			view.reflection = (flags & 6) >> 1;
			view.fallback = (flags & 8) != 0;
			for (int* offset : {&view.offset_x, &view.offset_y, &view.confirmed_offset_x, &view.confirmed_offset_y}) {
				if (!view.fallback && offset == &view.confirmed_offset_x) break;
				int offset_var;
				if (!(args >> offset_var)) break;
				if (!vars.IsValid(offset_var)) return;
				*offset = std::clamp(vars.Get(offset_var), -4096, 4096);
			}
			for (int id = first; id <= last; ++id) {
				if ((flags & 1) || view.fallback) frame->confirmed_pictures.insert(id);
				else frame->confirmed_pictures.erase(id);
				if (view.reflection || view.offset_x || view.offset_y || view.fallback)
					frame->picture_transforms.insert_or_assign(id, view);
				else frame->picture_transforms.erase(id);
			}
		}
		return;
	}
	if (command == "available") {
#ifdef PLAYER_WEB
		write(variable, EM_ASM_INT({ return !!Module.netplay; }));
#else
		write(variable, 0);
#endif
	} else if (command == "state") {
#ifdef PLAYER_WEB
		write(variable, ReadTransportState());
#else
		write(variable, 0);
#endif
	} else if (command == "receive") {
		int received;
		if (!(args >> received) || !vars.IsValid(received) || received == variable) return;
#ifdef PLAYER_WEB
		int value = 0;
		const bool pending = EM_ASM_INT({
			const value = Module.netplay?.receive();
			if (value === undefined) return 0;
			HEAP32[$0 >> 2] = value;
			return 1;
		}, &value);
		if (pending) write(variable, value);
		write(received, pending);
#else
		write(received, 0);
#endif
	}
#ifdef PLAYER_WEB
	else if (command == "open") {
		EM_ASM({ Module.netplay?.open($0); }, vars.Get(variable));
	} else if (command == "create") {
		EM_ASM({ Module.netplay?.open($0, 'create'); }, vars.Get(variable));
	} else if (command == "join") {
		EM_ASM({ Module.netplay?.open($0, 'join'); }, vars.Get(variable));
	} else if (command == "send") {
		EM_ASM({ Module.netplay?.send($0); }, vars.Get(variable));
	} else if (command == "seed" && ReadTransportState() >= 3) {
		const auto seed = static_cast<uint32_t>(vars.Get(variable));
		Main_Data::maniac_random_seed = seed;
		Rand::GetRNG().seed(seed);
	} else if (command == "start" && !match) {
		int seed;
		if (!(args >> seed) || !vars.IsValid(seed)) return;
		const int state = ReadTransportState();
		if (state >= 3 && vars.Get(variable) > 0) {
			match = std::make_unique<Match>(vars.Get(variable), vars.Get(seed), state - 3);
		}
	}
#endif
}

void Game_Netplay::Close() {
#ifdef PLAYER_WEB
	match.reset();
	EM_ASM({ Module.netplay?.close(); });
#endif
}

bool Game_Netplay::UpdateMap() {
#ifdef PLAYER_WEB
	if (!match) return false;
	if (match->map_id != Game_Map::GetMapId()) {
		Close();
		return false;
	}
	if (match->world.stopping) {
		if (match->world.Present()) match.reset();
		return true;
	}
	if (ReadTransportState() != match->seat + 3) {
		match.reset();
		return true;
	}
	if (!match->announced) {
		EM_ASM({ Module.netplay.send($0, -1, $1); }, match->id, match->seed);
		match->announced = true;
	}
	std::array<int32_t, 3> packet;
	while (EM_ASM_INT({
		const packet = Module.netplay.receiveFrame();
		if (!packet) return 0;
		const offset = $0 >> 2;
		HEAP32[offset] = packet[0];
		HEAP32[offset + 1] = packet[1];
		HEAP32[offset + 2] = packet[2];
		return 1;
	}, packet.data())) {
		if (packet[0] != match->id) continue;
		const bool accepted = packet[1] == -1
			? !match->peer_ready && packet[2] == match->seed
			: match->peer_ready && packet[2] >= 0 && packet[2] <= 127 &&
				match->timeline.Receive(packet[1], packet[2]);
		if (!accepted) {
			Close();
			return true;
		}
		match->peer_ready = true;
	}
	if (!match->peer_ready) return true;
	if (!match->started) {
		Main_Data::maniac_random_seed = static_cast<uint32_t>(match->seed);
		Rand::GetRNG().seed(static_cast<uint32_t>(match->seed));
		match->started = true;
	}
	const int input = (Input::IsPressed(Input::LEFT) ? 1 : 0) |
		(Input::IsPressed(Input::RIGHT) ? 2 : 0) |
		(Input::IsPressed(Input::UP) ? 4 : 0) |
		(Input::IsPressed(Input::DOWN) ? 8 : 0) |
		(Input::IsPressed(Input::DECISION) ? 16 : 0) |
		(Input::IsPressed(Input::CANCEL) ? 32 : 0) |
		(Input::IsPressed(Input::SHIFT) ? 64 : 0);
	if (auto index = match->timeline.QueueLocal(input)) {
		EM_ASM({ Module.netplay.send($0, $1, $2); }, match->id, *index, input);
	}
	// Event projections read the confirmed pose that will be drawn on this tick.
	match->world.Present();
	if (!match->timeline.Update(Game_Clock::GetFrame())) Close();
	return true;
#else
	return false;
#endif
}

bool Game_Netplay::UsesConfirmedPicture(int id) {
#ifdef PLAYER_WEB
	if (!match || match->world.confirmed_pictures.count(id) == 0) return false;
	const auto view = match->world.picture_transforms.find(id);
	if (view == match->world.picture_transforms.end() || !view->second.fallback) return true;
	const auto* live = Main_Data::game_pictures->GetPicturePtr(id);
	if (!live || !live->IsOnMap() || !live->sprite || !live->sprite->IsVisible() || !live->sprite->GetBitmap())
		return true;
	const bool separate_bottom = !Player::IsPatchManiac() && Player::IsRPG2k3() && !Player::IsRPG2k3E();
	return live->data.current_top_trans == 100 && (!separate_bottom || live->data.current_bot_trans == 100);
#else
	(void)id;
	return false;
#endif
}

Game_Netplay::PictureView Game_Netplay::PictureTransform(int id) {
#ifdef PLAYER_WEB
	if (match) {
		const auto it = match->world.picture_transforms.find(id);
		if (it != match->world.picture_transforms.end()) return it->second;
	}
#else
	(void)id;
#endif
	return {};
}

bool Game_Netplay::HidesLivePicture(int id) {
#ifdef PLAYER_WEB
	if (match) {
		const auto& world = match->world;
		const auto retired = world.presented_retirements.find(id);
		if (retired != world.presented_retirements.end()) {
			const auto birth = world.picture_births.find(id);
			return birth != world.picture_births.end() && birth->second <= retired->second;
		}
	}
#else
	(void)id;
#endif
	return false;
}

void Game_Netplay::RecordPictureDraw(int id, int x, int y) {
#ifdef PLAYER_WEB
	if (match) match->world.drawn_pictures.insert_or_assign(id, std::pair{x, y});
#else
	(void)id;
	(void)x;
	(void)y;
#endif
}

bool Game_Netplay::QueueAudio(const lcf::rpg::Music& music) {
	if (!frame) return false;
	frame->effects.audio.emplace_back(music);
	return true;
}

bool Game_Netplay::QueueAudio(const lcf::rpg::Sound& sound) {
	if (!frame) return false;
	frame->effects.audio.emplace_back(sound);
	return true;
}

bool Game_Netplay::QueueAudio(int fadeout) {
	if (!frame) return false;
	frame->effects.audio.emplace_back(fadeout);
	return true;
}
