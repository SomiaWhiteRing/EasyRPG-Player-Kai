#include "game_netplay.h"
#include "game_clock.h"
#include "game_map.h"
#include "game_maniac_js.h"
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
#include "transition.h"

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
constexpr int confirmed_signal_bit = 128;

struct Effects {
	std::vector<Audio> audio;
	std::map<int, int> samples;
	std::map<int, int> picture_retirements;
	bool input_signal = false;
	bool stop = false;
	std::optional<std::array<int, 2>> score;
	std::optional<uint32_t> checkpoint;
};

struct PictureContinuationUpdate {
	Game_Netplay::PictureContinuation action;
	int x;
	int y;
	int angle;
};

struct FrameContext {
	int index;
	int seat;
	std::array<int, 2> inputs;
	bool known;
	Effects& effects;
	bool& needs_confirmation;
	std::set<int>& captured_pictures;
	std::map<int, Game_Netplay::PictureView>& picture_transforms;
	std::map<int, int>& picture_births;
	const std::map<int, int>& presented_samples;
	const std::map<int, std::pair<int, int>>& drawn_pictures;
	std::map<int, PictureContinuationUpdate> continuations;
};

FrameContext* frame = nullptr;

#ifdef PLAYER_WEB

bool HasVisibleLivePicture(int id) {
	const auto* live = Main_Data::game_pictures->GetPicturePtr(id);
	if (!live || !live->IsOnMap() || !live->sprite || !live->sprite->IsVisible() || !live->sprite->GetBitmap())
		return false;
	const bool separate_bottom = !Player::IsPatchManiac() && Player::IsRPG2k3() && !Player::IsRPG2k3E();
	return live->data.current_top_trans != 100 || (separate_bottom && live->data.current_bot_trans != 100);
}

std::pair<int, int> PicturePosition(const lcf::rpg::SavePicture& data, int shake_x, int shake_y) {
	int x = ManiacPicture::Nearest(data.current_x);
	int y = ManiacPicture::Nearest(data.current_y);
	if (data.flags.affected_by_shake) { x += shake_x; y += shake_y; }
	if (Player::game_config.fake_resolution.Get()) { x += Player::menu_offset_x; y += Player::menu_offset_y; }
	return {x, y};
}

struct MapWorld {
	MapWorld(int seat, int presentation_buffer) : seat(seat), presentation_buffer(presentation_buffer) {}

	struct ContinuedPicture {
		Sprite_Picture::NetplayPose pose;
		int birth;
	};
	struct Projection {
		int source;
		int x;
		int y;
		int shown_x;
		int shown_y;
	};

	struct Snapshot {
		lcf::rpg::Save map;
		Game_Pictures::NetplaySnapshot pictures;
		std::vector<int32_t> raw_switches;
		Game_Strings::Strings_t strings;
		std::vector<lcf::DBString> string_zero;
		size_t string_size;
		Game_ManiacSelfVariables::Banks self_variables;
		std::set<int> captured_pictures;
		std::map<int, Game_Netplay::PictureView> picture_transforms;
		std::map<int, int> picture_births;
		std::map<int, ContinuedPicture> continued_pictures;
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
			captured_pictures, picture_transforms, picture_births, continued_pictures, Rand::GetRNG(),
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
		captured_pictures = std::move(state.captured_pictures);
		picture_transforms = std::move(state.picture_transforms);
		picture_births = std::move(state.picture_births);
		continued_pictures = std::move(state.continued_pictures);
		Main_Data::game_screen->SetSaveData(std::move(state.map.screen));
		Main_Data::game_pictures->RestoreNetplay(std::move(state.pictures));
		Game_Map::RestoreNetplay(state.map);
		Game_Map::SetNeedRefresh(state.need_refresh);
		Rand::GetRNG() = state.rng;
		Main_Data::maniac_random_seed = state.maniac_seed;
	}

	std::optional<Sprite_Picture::NetplayPose> CapturePicture(int id) const {
		const auto* picture = Main_Data::game_pictures->GetPicturePtr(id);
		if (!picture || !picture->sprite || !picture->sprite->IsVisible() || !picture->IsOnMap() ||
				picture->bitmap_modified || (picture->IsWindowAttached() && (picture->data.maniac_image_flags >> 24) != 1)) {
			return {};
		}
		const auto& bitmap = picture->sprite->GetBitmap();
		if (!bitmap) return {};
		auto& screen = *Main_Data::game_screen;
		auto pixels = Player::IsPatchManiac()
			? ManiacPicture::DescribeFrame(*bitmap, picture->data, screen.GetSaveData(), picture->GetManiacImageChromakey())
			: std::nullopt;
		const auto view = picture_transforms.find(id);
		std::optional<Game_Netplay::PictureView> captured_view;
		if (view != picture_transforms.end() && view->second.per_pose) captured_view = view->second;
		const auto born = picture_births.find(id);
		return Sprite_Picture::NetplayPose{picture->data, bitmap, screen.GetTone(), screen.GetFlashColor(),
			screen.GetShakeOffsetX(), screen.GetShakeOffsetY(), pixels, false, captured_view,
			born == picture_births.end() ? -1 : born->second};
	}

	void RememberOpeningPictures() {
		for (int id = 1; id <= Main_Data::game_pictures->GetMaxPictureId(); ++id) {
			if (auto pose = CapturePicture(id)) opening_pictures.push_back(std::move(*pose));
		}
	}

	void PresentPicture(Sprite_Picture::NetplayPose pose) {
		const int id = pose.data.ID;
		auto& sprite = displayed[id];
		if (!sprite) sprite = std::make_unique<Sprite_Picture>(id);
		sprite->PresentNetplay(std::move(pose));
	}

	bool IsRetired(int id, int birth) const {
		const auto retired = presented_retirements.find(id);
		return retired != presented_retirements.end() && birth <= retired->second;
	}

	void PresentContinuations() {
		for (auto& item : displayed_continuations) item.second->SetVisible(false);
		if (stopping) return;
		for (const auto& [id, continuation] : continued_pictures) {
			if (IsRetired(id, continuation.birth) || HasVisibleLivePicture(id)) continue;
			const auto born = picture_births.find(id);
			if (born != picture_births.end() && born->second > continuation.birth) continue;
			auto pose = continuation.pose;
			auto& screen = *Main_Data::game_screen;
			pose.shake_x = screen.GetShakeOffsetX();
			pose.shake_y = screen.GetShakeOffsetY();
			pose.immediate_preview = true;
			auto& sprite = displayed_continuations[id];
			if (!sprite) sprite = std::make_unique<Sprite_Picture>(id);
			sprite->PresentNetplay(std::move(pose));
		}
	}

	void Project(size_t pending_count) {
		projections.clear();
		projection_pending = pending_count;
		Game_ManiacJs::RunNetplayProjection(seat, stopping);
	}

	bool Step(int index, std::array<int, 2> inputs, bool known, Effects& effects, bool& needs_confirmation) {
		FrameContext context{index, seat, inputs, known, effects, needs_confirmation,
			captured_pictures, picture_transforms, picture_births, presented_samples, drawn_pictures, {}};
		frame = &context;
		const bool complete = Game_Map::UpdateNetplay();
		frame = nullptr;
		if (!complete) return false;
		for (const auto& [id, update] : context.continuations) {
			switch (update.action) {
			case Game_Netplay::PictureContinuation::Capture:
				if (auto pose = CapturePicture(id)) {
					const auto birth = picture_births.find(id);
					continued_pictures.insert_or_assign(id, ContinuedPicture{std::move(*pose),
						birth == picture_births.end() ? index : birth->second});
				}
				break;
			case Game_Netplay::PictureContinuation::Move:
				if (auto picture = continued_pictures.find(id); picture != continued_pictures.end()) {
					auto& data = picture->second.pose.data;
					data.current_x = update.x;
					data.current_y = update.y;
					data.current_rotation = update.angle;
				}
				break;
			case Game_Netplay::PictureContinuation::Remove:
				continued_pictures.erase(id);
				break;
			}
		}
		if (effects.stop) effects.terminal = std::make_unique<Snapshot>(Capture());
		for (auto& pose : opening_pictures) {
			if (captured_pictures.count(pose.data.ID)) PresentPicture(std::move(pose));
		}
		opening_pictures.clear();
		for (int id : captured_pictures) {
			if (auto pose = CapturePicture(id)) effects.pictures.push_back(std::move(*pose));
		}
		return true;
	}

	bool Publish(int index, Effects effects) {
		if (effects.checkpoint) {
			EM_ASM({ Module.netplay?.recordCheckpoint?.($0, $1 >>> 0); }, index, *effects.checkpoint);
		}
		if (effects.stop) {
			const auto score = effects.score.value_or(std::array<int, 2>{-1, -1});
			EM_ASM({ Module.netplay?.recordTerminal?.($0, $1, $2); }, index, score[0], score[1]);
		}
		stopping = effects.stop;
		if (stopping) PresentContinuations();
		pending_signal |= effects.input_signal;
		pending.push_back(std::move(effects));
		return stopping;
	}

	bool Present() {
		if (pending.empty()) {
			presentation_playing = false;
			return false;
		}
		if (!presentation_playing && !stopping && pending.size() < static_cast<size_t>(presentation_buffer)) return false;
		presentation_playing = true;
		auto effects = std::move(pending.front());
		pending.pop_front();
		presented_samples = std::move(effects.samples);
		for (const auto& [id, birth] : effects.picture_retirements)
			presented_retirements.insert_or_assign(id, birth);
		for (auto& item : displayed) item.second->SetVisible(false);
		for (auto& pose : effects.pictures) PresentPicture(std::move(pose));
		auto& system = *Main_Data::game_system;
		for (const auto& effect : effects.audio) {
			if (auto* music = std::get_if<lcf::rpg::Music>(&effect)) system.BgmPlay(*music);
			else if (auto* sound = std::get_if<lcf::rpg::Sound>(&effect)) system.SePlay(*sound, true);
			else system.BgmFade(std::get<int>(effect));
		}
		if (effects.terminal) Restore(std::move(*effects.terminal));
		return effects.stop;
	}

	std::set<int> captured_pictures;
	std::map<int, Game_Netplay::PictureView> picture_transforms;
	std::map<int, int> picture_births;
	std::map<int, ContinuedPicture> continued_pictures;
	std::map<int, int> presented_retirements;
	std::map<int, int> presented_samples;
	std::map<int, std::pair<int, int>> drawn_pictures;
	std::map<int, int> immediate_draws;
	std::map<int, std::unique_ptr<Sprite_Picture>> displayed;
	std::map<int, std::unique_ptr<Sprite_Picture>> displayed_continuations;
	std::map<int, Projection> projections;
	std::deque<Effects> pending;
	std::vector<Sprite_Picture::NetplayPose> opening_pictures;
	bool stopping = false;
	bool pending_signal = false;
	const int seat;
	const int presentation_buffer;
	bool presentation_playing = false;
	size_t projection_pending = 0;
};

struct Match {
	Match(int id, int seed, int seat, bool continue_rng, int presentation_buffer)
		: id(id), seed(seed), seat(seat), map_id(Game_Map::GetMapId()), continue_rng(continue_rng),
		  world(seat, presentation_buffer), timeline(world, seat) {}

	~Match() {
		EM_ASM({ Module.netplay?.endMatch?.('match_end'); });
	}

	int id;
	int seed;
	int seat;
	int map_id;
	bool continue_rng;
	bool announced = false;
	bool peer_ready = false;
	bool started = false;
	bool stall_exit = false;
	std::array<int, 3> rules{};
	MapWorld world;
	Game_Netplay::Rollback<MapWorld> timeline;
};

std::unique_ptr<Match> match;
int prepare_map = 0;
int prepare_fade = 0;
std::optional<uint32_t> prepare_seed;
std::map<int, Game_Netplay::PictureView> prepare_picture_transforms;
Game_Netplay::TransferFade transfer_fade;

void ClearPreparation() {
	prepare_map = 0;
	prepare_seed.reset();
	prepare_picture_transforms.clear();
}

void SeedRng(uint32_t seed) {
	Main_Data::maniac_random_seed = seed;
	Rand::GetRNG().seed(seed);
}

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
		int first, second;
		if (args >> first >> second) {
			auto& vars = *Main_Data::game_variables;
			if (vars.IsValid(first) && vars.IsValid(second)) frame->effects.score = std::array{vars.Get(first), vars.Get(second)};
		}
		return;
	}
	if (command == "checkpoint" && frame && frame->index % 60 == 0) {
		auto& vars = *Main_Data::game_variables;
		uint32_t digest = 2166136261u;
		int id;
		while (args >> id) {
			if (vars.IsValid(id)) digest = (digest ^ static_cast<uint32_t>(vars.Get(id))) * 16777619u;
		}
		auto rng = Rand::GetRNG();
		digest = (digest ^ rng()) * 16777619u;
		frame->effects.checkpoint = (digest ^ Main_Data::maniac_random_seed) * 16777619u;
		return;
	}
	if (command == "result_record" && !frame) {
#ifdef PLAYER_WEB
		EM_ASM({ Module.netplay?.recordResult?.(); });
#endif
		return;
	}
	if (command == "stop_directory" && !frame) {
#ifdef PLAYER_WEB
		EM_ASM({ Module.netplay?.stopDirectory?.(); });
#endif
		return;
	}
	if (command == "fade_transfer" && !frame) {
#ifdef PLAYER_WEB
		int erase, show;
		if (args >> erase >> show && erase > 0 && show > 0) {
			int delay = 0;
			args >> delay;
			transfer_fade = {erase, show, std::max(0, delay)};
		}
#endif
		return;
	}
	if ((command == "rooms" || command == "identity" || command == "room_settings") && !frame) {
#ifdef PLAYER_WEB
		int string_id;
		if (!(args >> string_id) || string_id <= 0) return;
		auto* data = reinterpret_cast<char*>(EM_ASM_PTR({
			const text = $0 === 2 ? Module.netplay?.roomSettings?.() : $0 === 1 ? Module.netplay?.identity?.() : Module.netplay?.rooms();
			if (text === undefined) return 0;
			const size = lengthBytesUTF8(text) + 1;
			const buffer = _malloc(size);
			if (buffer) stringToUTF8(text, buffer, size);
			return buffer;
		}, command == "room_settings" ? 2 : command == "identity" ? 1 : 0));
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
#ifdef PLAYER_WEB
	if (command == "track" && (frame || prepare_map > 0)) {
		constexpr int confirmed = 1, fallback = 8, record = 16, per_pose = 32, unseen = 64;
		int last_var, flags;
		if (!(args >> last_var >> flags) || !vars.IsValid(last_var) || flags < 0 || flags > 127 ||
				(flags & 6) == 6 || ((flags & confirmed) && (flags & (fallback | unseen)))) return;
		const int first = vars.Get(variable), last = vars.Get(last_var);
		if (first <= 0 || first > last || last > Main_Data::game_pictures->GetMaxPictureId()) return;
		PictureView view;
		view.reflection = (flags & 6) >> 1;
		view.fallback = (flags & (fallback | unseen)) != 0;
		view.draw_live = (flags & record) && !(flags & (confirmed | fallback | unseen));
		view.per_pose = (flags & (per_pose | unseen)) != 0;
		view.unseen_fallback = (flags & unseen) != 0;
		for (int* offset : {&view.offset_x, &view.offset_y, &view.confirmed_offset_x, &view.confirmed_offset_y}) {
			if (!view.fallback && offset == &view.confirmed_offset_x) break;
			int offset_var;
			if (!(args >> offset_var)) break;
			if (!vars.IsValid(offset_var)) return;
			*offset = std::clamp(vars.Get(offset_var), -4096, 4096);
		}
		auto& transforms = frame ? frame->picture_transforms : prepare_picture_transforms;
		for (int id = first; id <= last; ++id) {
			if (frame) {
				if (flags & (confirmed | fallback | record | unseen)) frame->captured_pictures.insert(id);
				else frame->captured_pictures.erase(id);
			}
			if (view.reflection || view.offset_x || view.offset_y || view.fallback || view.draw_live || view.per_pose)
				transforms.insert_or_assign(id, view);
			else transforms.erase(id);
		}
		return;
	}
#endif
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
		} else if (command == "signal") {
			frame->effects.input_signal |= vars.Get(variable) != 0;
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
		} else if (command == "upcoming") {
#ifdef PLAYER_WEB
			int delay, related, value;
			if (!(args >> delay >> related >> value) || !vars.IsValid(delay) || !vars.IsValid(related) ||
					!vars.IsValid(value) || delay == value) return;
			int distance = 0;
			for (const auto& queued : match->world.pending) {
				++distance;
				const auto watched = queued.samples.find(variable);
				if (watched == queued.samples.end() || watched->second == 0) continue;
				const auto other = queued.samples.find(related);
				write(delay, distance);
				write(value, other == queued.samples.end() ? 0 : other->second);
				return;
			}
			write(delay, 0);
			write(value, 0);
#endif
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
			if (picture > 0 && picture <= Main_Data::game_pictures->GetMaxPictureId()) {
				frame->captured_pictures.insert(picture);
				const auto view = frame->picture_transforms.find(picture);
				if (view != frame->picture_transforms.end()) view->second.draw_live = false;
			}
		} else if (command == "live") {
			frame->captured_pictures.erase(vars.Get(variable));
		} else if (command == "birth" || command == "retire") {
			const int picture = vars.Get(variable);
			if (picture <= 0 || picture > Main_Data::game_pictures->GetMaxPictureId()) return;
			if (command == "birth") {
				frame->picture_births.insert_or_assign(picture, frame->index);
			} else if (const auto birth = frame->picture_births.find(picture); birth != frame->picture_births.end()) {
				frame->effects.picture_retirements.insert_or_assign(picture, birth->second);
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
	} else if (command == "join_result") {
#ifdef PLAYER_WEB
		write(variable, EM_ASM_INT({ return Module.netplay?.joinResult?.() ?? 0; }));
#else
		write(variable, 0);
#endif
	} else if (command == "online") {
#ifdef PLAYER_WEB
		write(variable, EM_ASM_INT({ return Module.netplay?.online?.() ?? -1; }));
#else
		write(variable, -1);
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
		int string_id = 0;
		args >> string_id;
		const auto code = string_id > 0 ? Main_Data::game_strings->GetManiacRawSlot(string_id) : std::string_view();
		EM_ASM({ Module.netplay?.open($0, 'join', $3 ? UTF8ToString($1, $2) : null); },
			vars.Get(variable), code.data(), code.size(), string_id > 0);
	} else if (command == "password") {
		int string_id;
		if (!(args >> string_id) || string_id <= 0) return;
		const auto code = Main_Data::game_strings->GetManiacRawSlot(string_id);
		EM_ASM({ Module.netplay?.setPassword($0 !== 0, UTF8ToString($1, $2)); }, vars.Get(variable), code.data(), code.size());
	} else if (command == "send") {
		EM_ASM({ Module.netplay?.send($0); }, vars.Get(variable));
	} else if (command == "seed" && ReadTransportState() >= 3) {
		SeedRng(static_cast<uint32_t>(vars.Get(variable)));
	} else if (command == "prepare" && !match && ReadTransportState() >= 3) {
		ClearPreparation();
		prepare_map = std::max(0, vars.Get(variable));
		int frames = 0;
		args >> frames;
		prepare_fade = std::max(0, frames);
		int seed;
		if (args >> seed && vars.IsValid(seed)) prepare_seed = static_cast<uint32_t>(vars.Get(seed));
	} else if (command == "start" && !match) {
		int seed;
		if (!(args >> seed) || !vars.IsValid(seed)) return;
		std::string rng_mode;
		args >> rng_mode;
		int buffer_variable;
		const int buffer = args >> buffer_variable && vars.IsValid(buffer_variable)
			? std::clamp(vars.Get(buffer_variable), 1, 60) : 1;
		std::string stall_mode;
		args >> stall_mode;
		const int state = ReadTransportState();
		if (state >= 3 && vars.Get(variable) > 0) {
			match = std::make_unique<Match>(vars.Get(variable), vars.Get(seed), state - 3, rng_mode == "continue_rng", buffer);
			match->stall_exit = stall_mode == "stall_exit";
			for (int& rule : match->rules) {
				int id;
				if (args >> id && vars.IsValid(id)) rule = vars.Get(id);
			}
			if (PreparesMap(match->map_id)) match->world.picture_transforms = std::move(prepare_picture_transforms);
			ClearPreparation();
		}
	}
#endif
}

void Game_Netplay::Close() {
#ifdef PLAYER_WEB
	EM_ASM({ Module.netplay?.endMatch?.('closed'); });
	match.reset();
	ClearPreparation();
	transfer_fade = {};
	EM_ASM({ Module.netplay?.close(); });
#endif
}

bool Game_Netplay::InMatch() {
#ifdef PLAYER_WEB
	return match != nullptr;
#else
	return false;
#endif
}

bool Game_Netplay::WaitingForInput() {
#ifdef PLAYER_WEB
	return match && match->stall_exit && !match->world.stopping && match->timeline.WaitingForInput();
#else
	return false;
#endif
}

Game_Netplay::TransferFade Game_Netplay::TakeTransferFade() {
#ifdef PLAYER_WEB
	return std::exchange(transfer_fade, {});
#else
	return {};
#endif
}

bool Game_Netplay::PreparesMap(int map_id) {
#ifdef PLAYER_WEB
	return prepare_map > 0 && prepare_map == map_id;
#else
	return false;
#endif
}

bool Game_Netplay::UpdatePreparation(MapUpdateAsyncContext& actx) {
#ifdef PLAYER_WEB
	if (!PreparesMap(Game_Map::GetMapId())) return false;
	if (ReadTransportState() < 3) {
		ClearPreparation();
		return false;
	}
	if (prepare_seed) {
		SeedRng(*prepare_seed);
		prepare_seed.reset();
	}
	if (!Game_Map::UpdateNetplay(actx)) return true;
	if (match) match->world.RememberOpeningPictures();
	auto& transition = Transition::instance();
	if (prepare_fade > 0 && transition.IsErasedNotActive()) {
		transition.InitShow(Transition::TransitionFadeIn, Scene::instance.get(), prepare_fade);
	}
	return true;
#else
	(void)actx;
	return false;
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
		else match->world.Project(match->world.pending.size());
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
			: match->peer_ready && packet[2] >= 0 && packet[2] <= (127 | confirmed_signal_bit) &&
				match->timeline.Receive(packet[1], packet[2]);
		if (!accepted) {
			Close();
			return true;
		}
		match->peer_ready = true;
	}
	EM_ASM({
		Module.netplay?.progress?.($0, $1, $2, $3, $4, $5, $6, $7);
	}, match->id, match->timeline.VisibleFrame(), match->timeline.ConfirmedFrame(),
		match->timeline.ReceivedFrame(), match->timeline.WaitingForInput(),
		match->world.pending.size(), match->peer_ready, match->timeline.ReplayFrame());
	if (!match->peer_ready) return true;
	if (WaitingForInput() && Input::IsTriggered(Input::CANCEL)) {
		EM_ASM({ Module.netplay?.endMatch?.('user_cancel'); });
		Close();
		return true;
	}
	if (!match->started) {
		if (!match->continue_rng) SeedRng(static_cast<uint32_t>(match->seed));
		if (EM_ASM_INT({ return Number(Module.netplay?.recording?.() ?? false); })) {
			std::ostringstream rng;
			rng << Rand::GetRNG();
			const auto state = rng.str();
			const auto& rules = match->rules;
			EM_ASM({
				Module.netplay.recordStart($0, $1, $2, $3, $4, $5, $6, UTF8ToString($7));
			}, match->id, match->seed, match->map_id, rules[0], rules[1], rules[2],
				Main_Data::maniac_random_seed, state.c_str());
		}
	}
	match->started = true;
	if (match->world.presentation_buffer > 1 && match->timeline.CommitReady()) {
		if (match->world.Present()) match.reset();
		else match->world.Project(match->world.pending.size());
		return true;
	}
	const int input = (Input::IsPressed(Input::LEFT) ? 1 : 0) |
		(Input::IsPressed(Input::RIGHT) ? 2 : 0) |
		(Input::IsPressed(Input::UP) ? 4 : 0) |
		(Input::IsPressed(Input::DOWN) ? 8 : 0) |
		(Input::IsPressed(Input::DECISION) ? 16 : 0) |
		(Input::IsPressed(Input::CANCEL) ? 32 : 0) |
		(Input::IsPressed(Input::SHIFT) ? 64 : 0) |
		(match->world.pending_signal ? confirmed_signal_bit : 0);
	if (auto index = match->timeline.QueueLocal(input)) {
		match->world.pending_signal = false;
		EM_ASM({ Module.netplay.send($0, $1, $2); }, match->id, *index, input);
	}
	// Event projections read the confirmed pose that will be drawn on this tick.
	match->world.Present();
	const auto pending_count = match->world.pending.size();
	if (!match->timeline.Update(Game_Clock::GetFrame())) Close();
	else {
		match->world.Project(pending_count);
		match->world.PresentContinuations();
	}
	return true;
#else
	return false;
#endif
}

bool Game_Netplay::UsesConfirmedPicture(int id, const PictureView* captured_view, int birth) {
#ifdef PLAYER_WEB
	if (!match) return false;
	if (!captured_view && match->world.captured_pictures.count(id) == 0) return false;
	const auto projection = match->world.projections.find(id);
	if (projection != match->world.projections.end() && projection->second.source)
		return projection->second.source == 2;
	if (captured_view && captured_view->unseen_fallback) {
		const auto& world = match->world;
		const auto drawn = world.immediate_draws.find(id);
		if (birth >= 0 && (world.IsRetired(id, birth) ||
				(drawn != world.immediate_draws.end() && drawn->second >= birth))) return false;
		const auto view = world.picture_transforms.find(id);
		return world.stopping || view == world.picture_transforms.end() ||
			!view->second.unseen_fallback || !HasVisibleLivePicture(id);
	}
	if (captured_view && !captured_view->fallback) return !captured_view->draw_live;
	if (!captured_view) {
		const auto view = match->world.picture_transforms.find(id);
		if (view == match->world.picture_transforms.end()) return true;
		if (view->second.draw_live) return false;
		if (!view->second.fallback) return true;
		if (view->second.unseen_fallback && match->world.stopping) return true;
	}
	return !HasVisibleLivePicture(id);
#else
	(void)id;
	(void)captured_view;
	(void)birth;
	return false;
#endif
}

Game_Netplay::PictureView Game_Netplay::PictureTransform(int id, const PictureView* captured_view) {
	PictureView view = captured_view ? *captured_view : PictureView{};
#ifdef PLAYER_WEB
	const auto& transforms = match ? match->world.picture_transforms : prepare_picture_transforms;
	if (!captured_view && !transforms.empty() && (match || PreparesMap(Game_Map::GetMapId()))) {
		const auto it = transforms.find(id);
		if (it != transforms.end()) view = it->second;
	}
	if (match) {
		const auto projection = match->world.projections.find(id);
		if (projection != match->world.projections.end()) {
			const auto& value = projection->second;
			view.offset_x = value.x;
			view.offset_y = value.y;
			view.confirmed_offset_x = value.shown_x;
			view.confirmed_offset_y = value.shown_y;
		}
	}
#else
	(void)id;
#endif
	return view;
}

bool Game_Netplay::HidesLivePicture(int id) {
#ifdef PLAYER_WEB
	if (match) {
		const auto& world = match->world;
		const auto birth = world.picture_births.find(id);
		return birth != world.picture_births.end() && world.IsRetired(id, birth->second);
	}
#else
	(void)id;
#endif
	return false;
}

void Game_Netplay::ContinuePicture(int id, PictureContinuation action, int x, int y, int angle) {
	if (frame && id > 0 && id <= Main_Data::game_pictures->GetMaxPictureId()) {
		frame->continuations.insert_or_assign(id, PictureContinuationUpdate{action, x, y, angle});
	}
}

int Game_Netplay::ProjectionSample(int id, bool shown) {
#ifdef PLAYER_WEB
	if (match) {
		if (!shown) return Main_Data::game_variables->Get(id);
		const auto& samples = match->world.presented_samples;
		const auto sample = samples.find(id);
		if (sample != samples.end()) return sample->second;
	}
#else
	(void)id;
	(void)shown;
#endif
	return 0;
}

std::pair<int, int> Game_Netplay::UpcomingSample(int id, int related) {
#ifdef PLAYER_WEB
	if (match) {
		size_t distance = 0;
		for (const auto& queued : match->world.pending) {
			if (++distance > match->world.projection_pending) break;
			const auto sample = queued.samples.find(id);
			if (sample == queued.samples.end() || sample->second == 0) continue;
			const auto other = queued.samples.find(related);
			return {static_cast<int>(distance), other == queued.samples.end() ? 0 : other->second};
		}
	}
#else
	(void)id;
	(void)related;
#endif
	return {};
}

std::optional<std::pair<int, int>> Game_Netplay::ProjectionPosition(int id, int source) {
#ifdef PLAYER_WEB
	if (match) {
		const auto& world = match->world;
		if (source == 0) {
			if (const auto* picture = Main_Data::game_pictures->GetPicturePtr(id)) {
				const auto& screen = *Main_Data::game_screen;
				return PicturePosition(picture->data, screen.GetShakeOffsetX(), screen.GetShakeOffsetY());
			}
		} else if (source == 1) {
			const auto shown = world.displayed.find(id);
			if (shown != world.displayed.end() && shown->second->IsVisible()) {
				const auto& pose = *shown->second->GetNetplayPose();
				if (pose.data.current_top_trans != 100) return PicturePosition(pose.data, pose.shake_x, pose.shake_y);
			}
		} else if (source == 2) {
			const auto drawn = world.drawn_pictures.find(id);
			if (drawn != world.drawn_pictures.end()) return drawn->second;
		}
	}
#else
	(void)id;
	(void)source;
#endif
	return {};
}

void Game_Netplay::ProjectPictures(int first, int last, int source, int x, int y, int shown_x, int shown_y) {
#ifdef PLAYER_WEB
	if (match && first > 0 && first <= last && last <= Main_Data::game_pictures->GetMaxPictureId() &&
			source >= 0 && source <= 2) {
		for (int id = first; id <= last; ++id)
			match->world.projections.insert_or_assign(id, MapWorld::Projection{source, x, y, shown_x, shown_y});
	}
#else
	(void)first;
	(void)last;
	(void)source;
	(void)x;
	(void)y;
	(void)shown_x;
	(void)shown_y;
#endif
}

void Game_Netplay::RecordPictureDraw(int id, int x, int y, bool immediate) {
#ifdef PLAYER_WEB
	if (match) {
		auto& world = match->world;
		world.drawn_pictures.insert_or_assign(id, std::pair{x, y});
		const auto view = world.picture_transforms.find(id);
		if (immediate && view != world.picture_transforms.end() && view->second.unseen_fallback)
			world.immediate_draws.insert_or_assign(id, match->timeline.VisibleFrame());
	}
#else
	(void)id;
	(void)x;
	(void)y;
	(void)immediate;
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
