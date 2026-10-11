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
#include <fstream>
#include <functional>
#include "game_system.h"
#include "async_handler.h"
#include "game_battle.h"
#include "audio.h"
#include "baseui.h"
#include "bitmap.h"
#include "cache.h"
#include "output.h"
#include "game_ineluki.h"
#include "transition.h"
#include "main_data.h"
#include "player.h"
#include <lcf/reader_util.h>
#include "scene_save.h"
#include "scene_map.h"
#include "utils.h"
#include "audio_secache.h"
#include "feature.h"
#include "rand.h"

namespace {
	int32_t ManiacFaceDelay(int base, int range, bool positive) {
		const int32_t lower = positive ? 0 : static_cast<int32_t>(0u - static_cast<uint32_t>(range));
		const int32_t random = Player::game_config.maniac_version.Get() == 250921
			? Main_Data::ManiacRandomInclusive(lower, range)
			: Rand::GetRandomNumber(std::min(lower, range), std::max(lower, range));
		return static_cast<int32_t>(static_cast<uint32_t>(base) + static_cast<uint32_t>(random));
	}
}

Game_System::Game_System()
	: dbsys(&lcf::Data::system)
{ }

bool Game_System::AdvanceManiacMapStartup() {
	if (!HasManiacMapStartup()) return false;
	// 192ba0 allows one map update before the second starts the fade;
	// 3004 marks the load ended and bypasses that initial update delay.
	if (!maniac_load_ended && ++maniac_startup_updates <= 1) return false;
	maniac_load_ended = true;
	maniac_startup_complete = true;
	return true;
}

void Game_System::SetupFromSave(lcf::rpg::SaveSystem save) {
	data = std::move(save);
	loaded_frame_count = data.frame_count;
	maniac_loaded = true;
	maniac_load_ended = false;
	maniac_startup_complete = false;
	maniac_startup_updates = 0;
	if (Player::IsPatchManiac()) {
		data.maniac_run_when_inactive = static_cast<uint8_t>(data.maniac_run_when_inactive);
		ApplyManiacFpsRequest();
		Player::UpdateDebugFlag();
		ResetManiacFaceAnimation();
		if (DisplayUi) DisplayUi->RefreshFocusPolicy();
	}
}

void Game_System::SetMessageFaceIndex(int index) {
	data.face_id = index;
	if (Player::IsPatchManiac()) data.maniac_face_start = index;
}

int Game_System::GetMessageFaceWidth() const {
	return Player::IsPatchManiac() && data.maniac_face_width > 0 ? data.maniac_face_width : 48;
}

int Game_System::GetMessageFaceHeight() const {
	return Player::IsPatchManiac() && data.maniac_face_height > 0 ? data.maniac_face_height : 48;
}

void Game_System::SetManiacFaceSize(int32_t width, int32_t height) {
	data.maniac_face_width = width;
	data.maniac_face_height = height;
}

bool Game_System::IsManiacMessageMouseEnabled() const {
	return (data.maniac_message_mouse_flags & 1) == 0;
}

void Game_System::SetManiacMessageMouseFlags(int32_t flags) {
	data.maniac_message_mouse_flags = static_cast<uint8_t>(flags);
}

int32_t Game_System::GetManiacPictureLimit() const {
	return data.maniac_picture_limit;
}

void Game_System::SetManiacPictureLimit(int32_t limit) {
	data.maniac_picture_limit = limit;
}

void Game_System::ApplyManiacFpsRequest() {
	const int old_fps = Game_Clock::GetTargetGameFps();
	const int request = data.maniac_fps_request;
	if (request > 0) {
		Game_Clock::SetTargetGameFps(request);
		Player::SetTargetFps(request);
	}
	if (old_fps != request) Game_Clock::ResetFrame(Game_Clock::now(), true);
}

void Game_System::SetManiacFps(int32_t request, bool flags_present, int32_t flags) {
	data.maniac_fps_request = request;
	ApplyManiacFpsRequest();
	Player::maniac_testplay_selector = flags_present ? static_cast<uint32_t>(flags) & 15 : 0;
	Player::maniac_rshift_message = flags_present && ((static_cast<uint32_t>(flags) >> 4) & 1);
	if (Player::maniac_testplay_selector > 0) Player::debug_flag = Player::maniac_testplay_selector - 1;
}

void Game_System::SetManiacRunWhenInactive(int32_t value) {
	data.maniac_run_when_inactive = value != 0;
	if (DisplayUi) DisplayUi->RefreshFocusPolicy();
}

bool Game_System::IsManiacRunWhenInactive() const {
	return static_cast<uint8_t>(data.maniac_run_when_inactive) != 0;
}

void Game_System::SetManiacFrameSkip(int32_t value) {
	data.maniac_frameskip = static_cast<uint8_t>(value);
}

uint8_t Game_System::GetManiacFrameSkipMask() const {
	static constexpr uint8_t masks[] = {0, 4, 2, 1};
	return masks[data.maniac_frameskip & 3];
}

void Game_System::SetManiacBattleAlignment(int32_t alignment) {
	data.maniac_battle_alignment = static_cast<uint8_t>(alignment);
}

Point Game_System::GetBattleDisplayOffset() const {
	if (!Player::IsPatchManiac()) return {Player::menu_offset_x, Player::menu_offset_y};
	const int center_x = Player::screen_width / 2 - 160;
	const int center_y = Player::screen_height / 2 - 120;
	const int right = Player::screen_width - 320;
	const int bottom = Player::screen_height - 240;
	const Point offsets[] = {
		{center_x, center_y}, {0, 0}, {0, bottom}, {right, 0}, {right, bottom},
		{center_x, 0}, {center_x, bottom}, {0, center_y}, {right, center_y}
	};
	return data.maniac_battle_alignment < 9 ? offsets[data.maniac_battle_alignment] : Point{0, 0};
}

void Game_System::SetManiacFaceAnimation(int end, int interval, int interval_range, int frame_delay, int frame_delay_range) {
	data.maniac_face_end = end;
	data.maniac_face_interval = interval;
	data.maniac_face_interval_range = interval_range;
	data.maniac_face_frame_delay = frame_delay;
	data.maniac_face_frame_delay_range = frame_delay_range;
}

void Game_System::ResetManiacFaceInterval() {
	maniac_face_interval_elapsed = 0;
	maniac_face_interval_limit = ManiacFaceDelay(data.maniac_face_interval, data.maniac_face_interval_range,
		(data.maniac_face_animation_flags & 16) != 0);
}

void Game_System::ResetManiacFaceFrameDelay() {
	maniac_face_frame_elapsed = 0;
	maniac_face_frame_limit = ManiacFaceDelay(data.maniac_face_frame_delay, data.maniac_face_frame_delay_range,
		(data.maniac_face_animation_flags & 32) != 0);
}

void Game_System::ResetManiacFaceAnimation() {
	if (data.face_name.empty()) return;
	data.face_id = data.maniac_face_start;
	maniac_face_return_leg = false;
	ResetManiacFaceInterval();
	ResetManiacFaceFrameDelay();
}

void Game_System::UpdateManiacFaceAnimation() {
	const int flags = data.maniac_face_animation_flags;
	if ((flags & 2) == 0) return;
	if (maniac_face_interval_elapsed < maniac_face_interval_limit) {
		++maniac_face_interval_elapsed;
		return;
	}
	if (maniac_face_frame_elapsed < maniac_face_frame_limit) {
		++maniac_face_frame_elapsed;
		return;
	}
	maniac_face_frame_elapsed = 0;
	const int start = data.maniac_face_start;
	const int end = data.maniac_face_end;
	bool reached_end = false;
	if (start < end) {
		++data.face_id;
		reached_end = data.face_id >= end;
	} else if (start > end) {
		--data.face_id;
		reached_end = data.face_id <= end;
	}
	if (!reached_end) return;
	if ((flags & 8) != 0) {
		data.face_id = end;
	} else if ((flags & 4) != 0) {
		std::swap(data.maniac_face_start, data.maniac_face_end);
		maniac_face_return_leg = !maniac_face_return_leg;
	} else {
		data.face_id = start;
	}
	if ((flags & 64) != 0 || !maniac_face_return_leg) ResetManiacFaceInterval();
	ResetManiacFaceFrameDelay();
}

const lcf::rpg::SaveSystem& Game_System::GetSaveData() const {
	return data;
}

void Game_System::SetManiacMessageHook(int event_id, uint32_t mask, int system_variable,
	int system_string, int user_variable, int user_string) {
	data.maniac_message_hook_common_event_id = event_id;
	auto& flags = data.maniac_message_hook_flags;
	flags.user_event = (mask & 1) != 0;
	flags.create_window = (mask & 2) != 0;
	flags.destroy_window = (mask & 4) != 0;
	flags.text_rendering = (mask & 8) != 0;
	data.maniac_message_hook_callback_system_variable = system_variable;
	data.maniac_message_hook_callback_system_string_variable = system_string;
	data.maniac_message_hook_callback_user_variable = user_variable;
	data.maniac_message_hook_callback_user_string_variable = user_string;
}

bool Game_System::IsStopFilename(std::string_view name, Filesystem_Stream::InputStream (*find_func) (std::string_view), Filesystem_Stream::InputStream& found_stream) {
	if (name.empty() || name == "(OFF)") {
		found_stream = Filesystem_Stream::InputStream();
		return true;
	}

	found_stream = find_func(name);

	return !found_stream && (StartsWith(name, '(') && EndsWith(name, ')'));
}

bool Game_System::IsStopMusicFilename(std::string_view name, Filesystem_Stream::InputStream& found_stream) {
	return IsStopFilename(name, FileFinder::OpenMusic, found_stream);
}

bool Game_System::IsStopSoundFilename(std::string_view name, Filesystem_Stream::InputStream& found_stream) {
	return IsStopFilename(name, FileFinder::OpenSound, found_stream);
}

void Game_System::BgmPlay(lcf::rpg::Music const& bgm) {
	const lcf::rpg::Music requested_music = bgm;
	const bool native_250921 = Player::IsPatchManiac() && Player::game_config.maniac_version.Get() == 250921;
	lcf::rpg::Music previous_music = data.current_music;
	data.current_music = requested_music;
	if (native_250921) {
		previous_music.volume = Utils::Clamp<int32_t>(previous_music.volume, 0, 100);
		previous_music.tempo = Utils::Clamp<int32_t>(previous_music.tempo, 10, 400);
	}

	// Validate
	if (requested_music.volume < 0 || requested_music.volume > 100) {
		data.current_music.volume = Utils::Clamp<int32_t>(requested_music.volume, 0, 100);

		Output::Debug("BGM {} has invalid volume {}", requested_music.name, requested_music.volume);
	}

	if (requested_music.fadein < 0 || requested_music.fadein > 10000) {
		data.current_music.fadein = Utils::Clamp<int32_t>(requested_music.fadein, 0, 10000);

		Output::Debug("BGM {} has invalid fadein {}", requested_music.name, requested_music.fadein);
	}

	// Normal pitch is 50 to 200 but Yume2kki uses out of range values
	if (requested_music.tempo < 10 || requested_music.tempo > 400) {
		data.current_music.tempo = Utils::Clamp<int32_t>(requested_music.tempo, 10, 400);

		Output::Debug("BGM {} has invalid tempo {}", requested_music.name, requested_music.tempo);
	}

	// (OFF) means play nothing
	if (!requested_music.name.empty() && requested_music.name != "(OFF)") {
		// Same music: Only adjust volume and speed
		if (!data.music_stopping && previous_music.name == requested_music.name) {
			if (previous_music.volume != data.current_music.volume) {
				if (!bgm_pending) { // Delay if not ready
					Audio().BGM_Volume(data.current_music.volume);
				}
			}
			if (previous_music.tempo != data.current_music.tempo) {
				if (!bgm_pending) { // Delay if not ready
					Audio().BGM_Pitch(data.current_music.tempo);
				}
			}
		} else {
			Audio().BGM_Stop();
			bgm_pending = true;
			FileRequestAsync* request = AsyncHandler::RequestFile("Music", requested_music.name);
			music_request_id = request->Bind(&Game_System::OnBgmReady, this);
			request->Start();
		}
	} else {
		BgmStop();
	}

	data.music_stopping = false;
	// Native GetGameInfo and saves retain the request, separately from playback limits.
	if (native_250921) data.current_music = requested_music;
}

void Game_System::BgmStop() {
	music_request_id = FileRequestBinding();
	data.current_music.name = "(OFF)";
	Audio().BGM_Stop();
}

void Game_System::BgmFade(int duration, bool clear_current_music) {
	Audio().BGM_Fade(duration);
	if (clear_current_music) {
		data.current_music.name = "(OFF)";
	}
	data.music_stopping = true;
}

bool Game_System::BgmPlayedOnce() {
	if (Audio().BGM_PlayedOnce()) {
		if (Audio().BGM_GetType() == "wav") {
			// RPG_RT does not report looping for WAV
			return false;
		}
		return true;
	}

	return false;
}

void Game_System::SePlay(const lcf::rpg::Sound& se, bool stop_sounds) {
	if (se.name.empty()) {
		return;
	} else if (se.name == "(OFF)") {
		if (stop_sounds) {
			Audio().SE_Stop();
		}
		return;
	}

	// NOTE: Yume Nikki plays hundreds of sound effects at 0% volume on startup,
	// probably for caching. This avoids "No free channels" warnings.
	if (se.volume == 0)
		return;

	int32_t volume = se.volume;
	int32_t tempo = se.tempo;

	// Validate
	if (volume < 0 || volume > 100) {
		Output::Debug("SE {} has invalid volume {}", se.name, volume);
		volume = Utils::Clamp<int32_t>(volume, 0, 100);
	}

	// Normal pitch is 50 to 200 but Yume2kki uses out of range values
	if (tempo < 10 || tempo > 400) {
		Output::Debug("SE {} has invalid tempo {}", se.name, tempo);
		tempo = Utils::Clamp<int32_t>(se.tempo, 10, 400);
	}

	FileRequestAsync* request = AsyncHandler::RequestFile("Sound", se.name);
	lcf::rpg::Sound se_adj = se;
	se_adj.volume = volume;
	se_adj.tempo = tempo;
	se_request_ids[se.name] = request->Bind(&Game_System::OnSeReady, this, se_adj, stop_sounds);
	if (EndsWith(se.name, ".script")) {
		// Is a Ineluki Script File
		request->SetImportantFile(true);
	}
	request->Start();
}

void Game_System::SePlay(const lcf::rpg::Animation &animation) {
	Filesystem_Stream::InputStream stream;
	for (const auto& anim : animation.timings) {
		if (!IsStopSoundFilename(anim.se.name, stream)) {
			SePlay(anim.se);
			return;
		}
	}
}

std::string_view Game_System::GetSystemName() {
	return !data.graphics_name.empty() ?
		std::string_view(data.graphics_name) : std::string_view(lcf::Data::system.system_name);
}

void Game_System::OnChangeSystemGraphicReady(FileRequestResult* result) {
	Cache::SetSystemName(result->file);
	bg_color = Cache::SystemOrBlack()->GetBackgroundColor();

	Scene_Map* scene = (Scene_Map*)Scene::Find(Scene::Map).get();
	if (!scene || !scene->spriteset)
		return;
	scene->spriteset->SystemGraphicUpdated();
}

void Game_System::ReloadSystemGraphic() {
	FileRequestAsync* request = AsyncHandler::RequestFile("System", GetSystemName());
	system_request_id = request->Bind(&Game_System::OnChangeSystemGraphicReady, this);
	request->SetImportantFile(true);
	request->SetGraphicFile(true);
	request->Start();
}

void Game_System::SetSystemGraphic(const std::string& new_system_name,
		lcf::rpg::System::Stretch message_stretch,
		lcf::rpg::System::Font font) {

	bool changed = (GetSystemName() != new_system_name);

	data.graphics_name = new_system_name;
	data.message_stretch = message_stretch;
	data.font_id = font;

	if (changed) {
		ReloadSystemGraphic();
	}
}

void Game_System::ResetSystemGraphic() {
	data.graphics_name = "";
	data.message_stretch = (lcf::rpg::System::Stretch)0;
	data.font_id = (lcf::rpg::System::Font)0;

	ReloadSystemGraphic();
}

template <typename T>
static const T& GetAudio(const T& save, const T& db) {
	return save.name.empty() ? db : save;
}

template <typename T>
static void SetAudio(T& save, const T& db, T update) {
	if (update == db) {
		// RPG_RT only clears the name, but leaves the rest of the values alone
		save.name = {};
	} else {
		save = std::move(update);
	}
}

const lcf::rpg::Music& Game_System::GetSystemBGM(int which) {
	switch (which) {
		case BGM_Battle:
			return GetAudio(data.battle_music, dbsys->battle_music);
		case BGM_Victory:
			return GetAudio(data.battle_end_music, dbsys->battle_end_music);
		case BGM_Inn:
			return GetAudio(data.inn_music, dbsys->inn_music);
		case BGM_Boat:
			return GetAudio(data.boat_music, dbsys->boat_music);
		case BGM_Ship:
			return GetAudio(data.ship_music, dbsys->ship_music);
		case BGM_Airship:
			return GetAudio(data.airship_music, dbsys->airship_music);
		case BGM_GameOver:
			return GetAudio(data.gameover_music, dbsys->gameover_music);
	}

	static lcf::rpg::Music empty;
	return empty;
}

void Game_System::SetSystemBGM(int which, lcf::rpg::Music bgm) {
	switch (which) {
		case BGM_Battle:
			SetAudio(data.battle_music, dbsys->battle_music, std::move(bgm));
			break;
		case BGM_Victory:
			SetAudio(data.battle_end_music, dbsys->battle_end_music, std::move(bgm));
			break;
		case BGM_Inn:
			SetAudio(data.inn_music, dbsys->inn_music, std::move(bgm));
			break;
		case BGM_Boat:
			SetAudio(data.boat_music, dbsys->boat_music, std::move(bgm));
			break;
		case BGM_Ship:
			SetAudio(data.ship_music, dbsys->ship_music, std::move(bgm));
			break;
		case BGM_Airship:
			SetAudio(data.airship_music, dbsys->airship_music, std::move(bgm));
			break;
		case BGM_GameOver:
			SetAudio(data.gameover_music, dbsys->gameover_music, std::move(bgm));
			break;
	}
}

const lcf::rpg::Sound& Game_System::GetSystemSE(int which) {
	switch (which) {
		case SFX_Cursor:
			return GetAudio(data.cursor_se, dbsys->cursor_se);
		case SFX_Decision:
			return GetAudio(data.decision_se, dbsys->decision_se);
		case SFX_Cancel:
			return GetAudio(data.cancel_se, dbsys->cancel_se);
		case SFX_Buzzer:
			return GetAudio(data.buzzer_se, dbsys->buzzer_se);
		case SFX_BeginBattle:
			return GetAudio(data.battle_se, dbsys->battle_se);
		case SFX_Escape:
			return GetAudio(data.escape_se, dbsys->escape_se);
		case SFX_EnemyAttacks:
			return GetAudio(data.enemy_attack_se, dbsys->enemy_attack_se);
		case SFX_EnemyDamage:
			return GetAudio(data.enemy_damaged_se, dbsys->enemy_damaged_se);
		case SFX_AllyDamage:
			return GetAudio(data.actor_damaged_se, dbsys->actor_damaged_se);
		case SFX_Evasion:
			return GetAudio(data.dodge_se, dbsys->dodge_se);
		case SFX_EnemyKill:
			return GetAudio(data.enemy_death_se, dbsys->enemy_death_se);
		case SFX_UseItem:
			return GetAudio(data.item_se, dbsys->item_se);
	}

	static lcf::rpg::Sound empty;
	return empty;
}

void Game_System::SetSystemSE(int which, lcf::rpg::Sound sfx) {
	switch (which) {
		case SFX_Cursor:
			SetAudio(data.cursor_se, dbsys->cursor_se, std::move(sfx));
			break;
		case SFX_Decision:
			SetAudio(data.decision_se, dbsys->decision_se, std::move(sfx));
			break;
		case SFX_Cancel:
			SetAudio(data.cancel_se, dbsys->cancel_se, std::move(sfx));
			break;
		case SFX_Buzzer:
			SetAudio(data.buzzer_se, dbsys->buzzer_se, std::move(sfx));
			break;
		case SFX_BeginBattle:
			SetAudio(data.battle_se, dbsys->battle_se, std::move(sfx));
			break;
		case SFX_Escape:
			SetAudio(data.escape_se, dbsys->escape_se, std::move(sfx));
			break;
		case SFX_EnemyAttacks:
			SetAudio(data.enemy_attack_se, dbsys->enemy_attack_se, std::move(sfx));
			break;
		case SFX_EnemyDamage:
			SetAudio(data.enemy_damaged_se, dbsys->enemy_damaged_se, std::move(sfx));
			break;
		case SFX_AllyDamage:
			SetAudio(data.actor_damaged_se, dbsys->actor_damaged_se, std::move(sfx));
			break;
		case SFX_Evasion:
			SetAudio(data.dodge_se, dbsys->dodge_se, std::move(sfx));
			break;
		case SFX_EnemyKill:
			SetAudio(data.enemy_death_se, dbsys->enemy_death_se, std::move(sfx));
			break;
		case SFX_UseItem:
			SetAudio(data.item_se, dbsys->item_se, std::move(sfx));
			break;
	}
}

lcf::rpg::System::Stretch Game_System::GetMessageStretch() {
	return static_cast<lcf::rpg::System::Stretch>(!data.graphics_name.empty()
		? data.message_stretch
		: lcf::Data::system.message_stretch);
}

lcf::rpg::System::Font Game_System::GetFontId() {
	return static_cast<lcf::rpg::System::Font>(!data.graphics_name.empty()
		? data.font_id
		: lcf::Data::system.font_id);
}

Transition::Type Game_System::GetTransition(int which) {
	int transition = 0;

	auto get = [&](int local, int db) {
		return local >= 0 ? local : db;
	};

	switch (which) {
		case Transition_TeleportErase:
			transition = get(data.transition_out, lcf::Data::system.transition_out);
			break;
		case Transition_TeleportShow:
			transition = get(data.transition_in, lcf::Data::system.transition_in);
			break;
		case Transition_BeginBattleErase:
			transition = get(data.battle_start_fadeout, lcf::Data::system.battle_start_fadeout);
			break;
		case Transition_BeginBattleShow:
			transition = get(data.battle_start_fadein, lcf::Data::system.battle_start_fadein);
			break;
		case Transition_EndBattleErase:
			transition = get(data.battle_end_fadeout, lcf::Data::system.battle_end_fadeout);
			break;
		case Transition_EndBattleShow:
			transition = get(data.battle_end_fadein, lcf::Data::system.battle_end_fadein);
			break;
		default: assert(false && "Bad transition");
	}

	constexpr int num_types = 21;

	if (transition < 0 || transition >= num_types) {
		Output::Warning("Invalid transition value {}", transition);
		transition = Utils::Clamp(transition, 0, num_types - 1);
	}

	constexpr Transition::Type fades[2][num_types] = {
		{
			Transition::TransitionFadeOut,
			Transition::TransitionRandomBlocks,
			Transition::TransitionRandomBlocksDown,
			Transition::TransitionRandomBlocksUp,
			Transition::TransitionBlindClose,
			Transition::TransitionVerticalStripesOut,
			Transition::TransitionHorizontalStripesOut,
			Transition::TransitionBorderToCenterOut,
			Transition::TransitionCenterToBorderOut,
			Transition::TransitionScrollUpOut,
			Transition::TransitionScrollDownOut,
			Transition::TransitionScrollLeftOut,
			Transition::TransitionScrollRightOut,
			Transition::TransitionVerticalDivision,
			Transition::TransitionHorizontalDivision,
			Transition::TransitionCrossDivision,
			Transition::TransitionZoomIn,
			Transition::TransitionMosaicOut,
			Transition::TransitionWaveOut,
			Transition::TransitionCutOut,
			Transition::TransitionNone
		},
		{
			Transition::TransitionFadeIn,
			Transition::TransitionRandomBlocks,
			Transition::TransitionRandomBlocksDown,
			Transition::TransitionRandomBlocksUp,
			Transition::TransitionBlindOpen,
			Transition::TransitionVerticalStripesIn,
			Transition::TransitionHorizontalStripesIn,
			Transition::TransitionBorderToCenterIn,
			Transition::TransitionCenterToBorderIn,
			Transition::TransitionScrollUpIn,
			Transition::TransitionScrollDownIn,
			Transition::TransitionScrollLeftIn,
			Transition::TransitionScrollRightIn,
			Transition::TransitionVerticalCombine,
			Transition::TransitionHorizontalCombine,
			Transition::TransitionCrossCombine,
			Transition::TransitionZoomOut,
			Transition::TransitionMosaicIn,
			Transition::TransitionWaveIn,
			Transition::TransitionCutIn,
			Transition::TransitionNone,
		}
	};

	return fades[which % 2][transition];
}

void Game_System::SetTransition(int which, int transition) {
	auto set = [&](int t, int db) {
		return t != db ? t : -1;
	};
	switch (which) {
		case Transition_TeleportErase:
			data.transition_out = set(transition, lcf::Data::system.transition_out);
			break;
		case Transition_TeleportShow:
			data.transition_in = set(transition, lcf::Data::system.transition_in);
			break;
		case Transition_BeginBattleErase:
			data.battle_start_fadeout = set(transition, lcf::Data::system.battle_start_fadeout);
			break;
		case Transition_BeginBattleShow:
			data.battle_start_fadein = set(transition, lcf::Data::system.battle_start_fadein);
			break;
		case Transition_EndBattleErase:
			data.battle_end_fadeout = set(transition, lcf::Data::system.battle_end_fadeout);
			break;
		case Transition_EndBattleShow:
			data.battle_end_fadein = set(transition, lcf::Data::system.battle_end_fadein);
			break;
		default: assert(false && "Bad transition");
	}
}

std::string Game_System::InelukiReadLink(Filesystem_Stream::InputStream& stream) {
	// The first line contains the path to the actual audio file to play
	std::string line;
	if (!Utils::ReadLine(stream, line)) {
		Output::Warning("Ineluki MP3: Link file is empty: {}", stream.GetName());
		return {};
	}
	line = lcf::ReaderUtil::Recode(line, Player::encoding);

	Output::Debug("Ineluki MP3: Link file: {} -> {}", stream.GetName(), line);
	std::string line_canonical = FileFinder::MakeCanonical(line, 1);

	return line_canonical;
}

void Game_System::OnBgmReady(FileRequestResult* result) {
	// Take from current_music, params could have changed over time
	bgm_pending = false;

	Filesystem_Stream::InputStream stream;
	if (IsStopMusicFilename(result->file, stream)) {
		Audio().BGM_Stop();
		return;
	} else if (!stream) {
		Output::Debug("Music not found: {}", result->file);
		return;
	}

	if (Player::IsPatchKeyPatch() && EndsWith(result->file, ".link") && stream.GetSize() < 500) {
		// Handle Ineluki's MP3 patch
		std::string line = InelukiReadLink(stream);

		// Needs another Async roundtrip
		bgm_pending = true;
		FileRequestAsync *request = AsyncHandler::RequestFile(line);
		music_request_id = request->Bind(&Game_System::OnBgmInelukiReady, this);
		request->Start();
		return;
	}

	Audio().BGM_Play(std::move(stream), Utils::Clamp<int32_t>(data.current_music.volume, 0, 100),
		Utils::Clamp<int32_t>(data.current_music.tempo, 10, 400), Utils::Clamp<int32_t>(data.current_music.fadein, 0, 10000));
}

void Game_System::OnBgmInelukiReady(FileRequestResult* result) {
	bgm_pending = false;
	Audio().BGM_Play(FileFinder::Game().OpenFile(result->file), Utils::Clamp<int32_t>(data.current_music.volume, 0, 100),
		Utils::Clamp<int32_t>(data.current_music.tempo, 10, 400), Utils::Clamp<int32_t>(data.current_music.fadein, 0, 10000));
}

void Game_System::OnSeReady(FileRequestResult* result, lcf::rpg::Sound se, bool stop_sounds) {
	auto item = se_request_ids.find(result->file);
	if (item != se_request_ids.end()) {
		se_request_ids.erase(item);
	}

	if (Player::IsPatchKeyPatch() && EndsWith(result->file, ".script")) {
		// Is a Ineluki Script File
		Main_Data::game_ineluki->Execute(se);
		return;
	}

	auto se_cache = AudioSeCache::GetCachedSe(result->file);
	if (!se_cache) {
		Filesystem_Stream::InputStream stream;
		if (IsStopSoundFilename(result->file, stream)) {
			if (stop_sounds) {
				Audio().SE_Stop();
			}
			return;
		} else if (!stream) {
			Output::Debug("Sound not found: {}", result->file);
			return;
		}

		if (Player::IsPatchKeyPatch() && EndsWith(result->file, ".link") && stream.GetSize() < 500) {
			// Handle Ineluki's MP3 patch
			std::string line = InelukiReadLink(stream);

			// Needs another Async roundtrip
			FileRequestAsync* request = AsyncHandler::RequestFile(line);
			se_request_ids[line] = request->Bind(&Game_System::OnSeInelukiReady, this, se);
			request->Start();
			return;
		}

		se_cache = AudioSeCache::Create(std::move(stream), result->file);
	}

	if (!se_cache) {
		Output::Warning("Sound {}: Format not supported", result->file);
		return;
	}

	Audio().SE_Play(std::move(se_cache), se.volume, se.tempo);
}

void Game_System::OnSeInelukiReady(FileRequestResult* result, lcf::rpg::Sound se) {
	auto item = se_request_ids.find(result->file);
	if (item != se_request_ids.end()) {
		se_request_ids.erase(item);
	}

	auto se_cache = AudioSeCache::GetCachedSe(result->file);
	if (!se_cache) {
		auto stream = FileFinder::Game().OpenFile(result->file);
		se_cache = AudioSeCache::Create(std::move(stream), result->file);
	}

	if (!se_cache) {
		Output::Warning("Sound {}: Format not supported", result->file);
		return;
	}

	Audio().SE_Play(std::move(se_cache), se.volume, se.tempo);
}

bool Game_System::IsMessageTransparent() {
	if (Feature::HasRpg2kBattleSystem() && Game_Battle::IsBattleRunning()) {
		return false;
	}

	return data.message_transparent != 0;
}

