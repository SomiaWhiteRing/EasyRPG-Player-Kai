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
#include <cmath>
#include <cstring>
#include <iterator>
#include "bitmap.h"
#include <lcf/data.h>
#include "player.h"
#include "game_battle.h"
#include "game_battler.h"
#include "game_screen.h"
#include "game_system.h"
#include "game_variables.h"
#include "game_map.h"
#include "game_player.h"
#include "main_data.h"
#include "output.h"
#include "utils.h"
#include "options.h"
#include <lcf/reader_util.h>
#include "scene.h"
#include "weather.h"
#include "flash.h"
#include "shake.h"
#include "rand.h"
#include "baseui.h"
#include "filefinder.h"
#include "movie_player.h"
#ifdef EMSCRIPTEN
#  include "async_handler.h"
#endif

Game_Screen::Game_Screen()
{
}

Game_Screen::~Game_Screen() {
	StopMovie(false);
}

void Game_Screen::SetSaveData(lcf::rpg::SaveScreen screen)
{
	CancelBattleAnimation();

	data = std::move(screen);
	zoom_initialized = data.maniac_zoom_layer != 0;
}

void Game_Screen::InitGraphics() {
	weather = std::make_unique<Weather>();
	OnWeatherChanged();

	if (data.battleanim_active) {
		ShowBattleAnimation(data.battleanim_id,
				data.battleanim_target,
				data.battleanim_global,
				data.battleanim_frame);
	}
	RestoreManiacAnimations();
}

void Game_Screen::OnMapChange() {
	data.flash_red = 0;
	data.flash_green = 0;
	data.flash_blue = 0;
	data.flash_time_left = 0;
	data.flash_current_level = 0;
	flash_sat = 0;
	flash_period = 0;

	if (data.tint_current_red < 0 ||
		data.tint_current_green < 0 ||
		data.tint_current_blue < 0 ||
		data.tint_current_sat < 0) {
		data.tint_current_red = 100;
		data.tint_current_green = 100;
		data.tint_current_blue = 100;
		data.tint_current_sat = 100;
	}

	movie_filename = "";
	movie_pos_x = 0;
	movie_pos_y = 0;
	movie_res_x = 0;
	movie_res_y = 0;
	movie_finished = false;
#ifdef EMSCRIPTEN
	movie_pending = false;
#endif
	StopMovie(false);

	data.battleanim_active = false;
	animation.reset();
	maniac_animations.clear();
	data.easyrpg_maniac_animations.clear();
}

void Game_Screen::TintScreen(int r, int g, int b, int s, int tenths) {
	data.tint_finish_red = r;
	data.tint_finish_green = g;
	data.tint_finish_blue = b;
	data.tint_finish_sat = s;

	data.tint_time_left = tenths;

	if (data.tint_time_left == 0) {
		data.tint_current_red = data.tint_finish_red;
		data.tint_current_green = data.tint_finish_green;
		data.tint_current_blue = data.tint_finish_blue;
		data.tint_current_sat = data.tint_finish_sat;
	}
}

void Game_Screen::FlashOnce(int r, int g, int b, int s, int frames) {
	data.flash_red = r;
	data.flash_green = g;
	data.flash_blue = b;
	flash_sat = s;
	data.flash_current_level = s;
	data.flash_time_left = frames;
	data.flash_continuous = false;
	flash_period = 0;
}

void Game_Screen::FlashBegin(int r, int g, int b, int s, int frames) {
	FlashOnce(r, g, b, s, frames);

	flash_period = frames;
	data.flash_continuous = true;
}

void Game_Screen::FlashEnd() {
	data.flash_time_left = 0;
	data.flash_current_level = 0;
	flash_period = 0;
	data.flash_continuous = false;
}

void Game_Screen::FlashMapStepDamage() {
	FlashOnce(31, 10, 10, 20, 6);
}

void Game_Screen::ShakeOnce(int power, int speed, int tenths) {
	data.shake_strength = power;
	data.shake_speed = speed;
	data.shake_time_left = tenths;
	data.shake_continuous = false;
	// Shake position is not reset in RPG_RT, so that multiple shakes
	// which interrupt each other flow smoothly.
}

void Game_Screen::ShakeBegin(int power, int speed) {
	data.shake_strength = power;
	data.shake_speed = speed;
	data.shake_time_left = Shake::kShakeContinuousTimeStart;
	data.shake_continuous = true;
	// Shake position is not reset in RPG_RT, so that multiple shakes
	// which interrupt each other flow smoothly.
}

void Game_Screen::ShakeEnd() {
	data.shake_position = 0;
	data.shake_time_left = 0;
	// RPG_RT does not turn off the continuous shake flag when shake is disabled.
}

int Game_Screen::ZoomScreen(int x, int y, int scale, int frames, int layer) {
	const int width = Player::screen_width;
	const int height = Player::screen_height;
	if (layer < 1) {
		x = width / 2;
		y = height / 2;
		scale = 100;
	}
	const auto limit_center = [&](int value, int size) {
		// Native arithmetic wraps the screen-size product to signed 32 bits.
		const uint32_t product = static_cast<uint32_t>(size) * static_cast<uint32_t>(scale);
		int32_t signed_product;
		std::memcpy(&signed_product, &product, sizeof(product));
		const int quarter = (signed_product / 100 - size) / 4;
		const int low = size / 2 - quarter;
		const int high = size / 2 + quarter;
		// At scales below 100 the bounds are reversed in RPG_RT.
		return value < low ? low : value <= high ? value : high;
	};
	if (!zoom_initialized) {
		data.maniac_zoom_current_x = width / 2;
		data.maniac_zoom_current_y = height / 2;
		data.maniac_zoom_current_scale = 100.0;
		zoom_initialized = true;
	}
	data.maniac_zoom_finish_x = limit_center(x, width);
	data.maniac_zoom_finish_y = limit_center(y, height);
	data.maniac_zoom_finish_scale = scale;
	data.maniac_zoom_time_left = std::max(1, frames);
	data.maniac_zoom_layer = layer;
	return data.maniac_zoom_time_left;
}

void Game_Screen::DrawZoom(Bitmap& dst) {
	const double scale = std::max(100.0, data.maniac_zoom_current_scale);
	const double width = dst.GetWidth() * 100.0 / scale;
	const double height = dst.GetHeight() * 100.0 / scale;
	const double x = data.maniac_zoom_current_x < 0 && Main_Data::game_player
		? Main_Data::game_player->GetScreenX() : data.maniac_zoom_current_x;
	const double y = data.maniac_zoom_current_y < 0 && Main_Data::game_player
		? Main_Data::game_player->GetScreenY() : data.maniac_zoom_current_y;
	const Rect source_rect(
		std::clamp(static_cast<int>(x - width / 2), 0, static_cast<int>(dst.GetWidth() - width)),
		std::clamp(static_cast<int>(y - height / 2), 0, static_cast<int>(dst.GetHeight() - height)),
		static_cast<int>(width), static_cast<int>(height));
	if (source_rect.IsEmpty() || source_rect == dst.GetRect()) return;
	if (!zoom_buffer || zoom_buffer->GetRect() != dst.GetRect()) {
		zoom_buffer = Bitmap::Create(dst.GetWidth(), dst.GetHeight(), false);
	}
	zoom_buffer->Blit(0, 0, dst, dst.GetRect(), Opacity::Opaque(), Bitmap::BlendMode::NormalWithoutAlpha);
	dst.StretchBlit(dst.GetRect(), *zoom_buffer, source_rect, Opacity::Opaque(), Bitmap::BlendMode::NormalWithoutAlpha);
}

void Game_Screen::SetWeatherEffect(int type, int strength) {
	// Some games call weather effects in a parallel process
	// This causes issues in the rendering (weather rendered too fast)
	if (data.weather != type ||
		data.weather_strength != strength) {
		data.weather = type;
		data.weather_strength = strength;
		OnWeatherChanged();
	}
}

bool Game_Screen::PlayMovie(std::string filename,
							int pos_x, int pos_y, int res_x, int res_y) {
	if (IsMoviePlaying()) {
		return true;
	}

#ifdef EMSCRIPTEN
	const bool same_pending_movie = movie_pending && movie_filename == filename;
	if (!same_pending_movie) {
		movie_finished = false;
	}
#else
	movie_finished = false;
#endif

	movie_filename = std::move(filename);
	movie_pos_x = pos_x;
	movie_pos_y = pos_y;
	movie_res_x = res_x;
	movie_res_y = res_y;

#ifdef EMSCRIPTEN
	auto* movie_request = AsyncHandler::RequestFile("Movie", movie_filename);
	if (!movie_request->IsReady()) {
		movie_pending = true;
		movie_request->Start();
		return true;
	}

	movie_pending = false;

	if (!movie_request->IsSuccess()) {
		Output::Warning("Couldn't play movie: {}. Movie file could not be downloaded.", movie_filename);
		movie_filename.clear();
		return false;
	}
#endif

	auto movie_path = FileFinder::FindMovie(movie_filename);
	if (movie_path.empty()) {
		Output::Warning("Couldn't play movie: {}. Movie file not found.", movie_filename);
		movie_filename.clear();
		return false;
	}

	return OpenMovie(std::move(movie_path));
}

bool Game_Screen::OpenMovie(std::string movie_path) {
	auto full_path = FileFinder::MakePath(FileFinder::GetFullFilesystemPath(FileFinder::Game()), movie_path);

	if (!movie_player) {
		movie_player = MoviePlayer::Create();
	}

	if (!movie_player) {
		Output::Warning("Couldn't play movie: {}. Movie playback backend is unavailable.", movie_filename);
		movie_filename.clear();
		return false;
	}

	std::string error_message;
#ifdef HAVE_ANDROID_MOVIE_PLAYER
	// Document-provider resources need the game filesystem stream.
	full_path = movie_path;
#endif
	if (!movie_player->Open(full_path, error_message)) {
		Output::Warning("Couldn't play movie: {}. {}", movie_filename, error_message);
		StopMovie(false);
		return false;
	}

	Output::Debug("Playing movie: {} pos=({}, {}) size=({}x{}) source={}",
		movie_filename, movie_pos_x, movie_pos_y, movie_res_x, movie_res_y, movie_path);
	UpdateMovie();
	return movie_player->IsPlaying();
}

bool Game_Screen::IsMoviePlaying() const {
	return movie_player && movie_player->IsPlaying();
}

void Game_Screen::DrawMovie(Bitmap& dst) {
	if (!movie_player) return;
	const int width = movie_res_x > 0 ? movie_res_x : movie_player->GetNativeWidth();
	const int height = movie_res_y > 0 ? movie_res_y : movie_player->GetNativeHeight();
	movie_player->Draw(dst, {movie_pos_x, movie_pos_y, width, height});
}

bool Game_Screen::ConsumeMovieFinished() {
	if (!movie_finished) {
		return false;
	}

	movie_finished = false;
	return true;
}

static double interpolate(double d, double x0, double x1)
{
	return (x0 * (d - 1) + x1) / d;
}

void Game_Screen::StopWeather() {
	data.weather = Weather_None;
	OnWeatherChanged();
}

void Game_Screen::OnWeatherChanged() {
	int num_particles = Weather::GetMaxNumParticles(data.weather);

	InitParticles(num_particles);

	if (weather) {
		weather->OnWeatherChanged();
	}
}

void Game_Screen::InitParticles(int num_particles) {
	// RPG_RT initializes all particles on new game / load game.
	// We do it lazily instead. That way for games which don't use
	// weather effects, we never consume memory for those effects.
	auto sz = static_cast<int>(particles.size());

	if (num_particles <= sz) {
		return;
	}

	particles.resize(num_particles);

	for (int i = sz; i < num_particles; ++i) {
		auto& p = particles[i];
		// RPG_RT always initializes all particles to these values on startup.
		// This can cause minor visual glitches for the first few frames the
		// first time you start the sandstorm effect. We're bug compatible with RPG_RT.
		p.t = Rand::GetRandomNumber(0, 39);
		p.x = Rand::GetRandomNumber(0, GetPanLimitX() / 16 - 1);
		p.y = Rand::GetRandomNumber(0, GetPanLimitY() / 16 - 1);
	}
}

void Game_Screen::UpdateRain() {
	for (auto& p: particles) {
		if (p.t > 0) {
			--p.t;
			p.y += 4;
			p.x -= 1;
		} else if (Rand::PercentChance(10)) {
			p.t = 12;
			p.x = Rand::GetRandomNumber(0, GetPanLimitX() / 16 - 1);
			p.y = Rand::GetRandomNumber(0, GetPanLimitY() / 16 - 1);
		}
	}
}

void Game_Screen::UpdateSnow() {
	for (auto& p: particles) {
		if (p.t > 0) {
			--p.t;
			p.x -= Rand::GetRandomNumber(0, 1);
			p.y += Rand::GetRandomNumber(2, 3);
		} else if (Rand::PercentChance(5)) {
			p.t = 30;
			p.x = Rand::GetRandomNumber(0, GetPanLimitX() / 16 - 1);
			p.y = Rand::GetRandomNumber(0, GetPanLimitY() / 16 - 1);
		}
	}
}

void Game_Screen::UpdateFog() {
	++particles[0].x;
	++particles[1].x;
}

void Game_Screen::UpdateSandstorm() {
	// RPG_RT takes random numbers in the inclusive range [1, 127] and has a function
	// which takes [0, 255) -> [0, 2 * M_PI) and computes sin or cos. This epsilson
	// accounts for the range starting at 1 (not 0) and ending at 127 (not 128).

	constexpr auto epsilon = 1.0f / 128.0f;
	auto& rng = Rand::GetRNG();
	auto dist = std::uniform_real_distribution<float>(epsilon, M_PI - epsilon);

	UpdateFog();

	for (size_t i = 2; i < particles.size(); ++i) {
		auto& p = particles[i];
		if (p.t > 0) {
			--p.t;
			p.alpha += 2;
			p.x += static_cast<int>(p.vx);
			p.y += static_cast<int>(p.vy);
			p.vx += p.ax;
			p.vy += p.ay;
		} else if (Rand::PercentChance(10)) {
			p.t = 80;

			auto c = std::cos(dist(rng));
			auto s = std::sin(dist(rng));
			auto d = Rand::GetRandomNumber(16, 95);

			p.x = static_cast<int>(d * c * 2.0f) * Player::screen_width / 320 + Player::screen_width / 2;
			p.y = static_cast<int>(d * s) * Player::screen_height / 240;

			p.alpha = 180;
			p.vx = 0.0;
			p.vy = 0.0;
			p.ax = c * 2.0f * Player::screen_width / 320;
			p.ay = s * 2.0f * Player::screen_height / 240;
		}
	}
}

void Game_Screen::OnMapScrolled(int dx, int dy) {
	auto pan_limit_x = GetPanLimitX();
	auto pan_limit_y = GetPanLimitY();

	data.pan_x = (data.pan_x - dx + pan_limit_x) % pan_limit_x;
	data.pan_y = (data.pan_y - dy + pan_limit_y) % pan_limit_y;
}

void Game_Screen::UpdateScreenEffects() {
	if (data.tint_time_left > 0) {
		data.tint_current_red = interpolate(data.tint_time_left, data.tint_current_red, data.tint_finish_red);
		data.tint_current_green = interpolate(data.tint_time_left, data.tint_current_green, data.tint_finish_green);
		data.tint_current_blue = interpolate(data.tint_time_left, data.tint_current_blue, data.tint_finish_blue);
		data.tint_current_sat = interpolate(data.tint_time_left, data.tint_current_sat, data.tint_finish_sat);
		data.tint_time_left = data.tint_time_left - 1;
	}

	Flash::Update(data.flash_current_level,
			data.flash_time_left,
			data.flash_continuous,
			flash_period,
			flash_sat);

	Shake::Update(data.shake_position,
			data.shake_time_left,
			data.shake_strength,
			data.shake_speed,
			data.shake_continuous);
}

void Game_Screen::UpdateMovie() {
	if (movie_player) {
		movie_player->Update(GetMovieOutputRect());
		if (!movie_player->IsPlaying()) {
			if (movie_player->HasError()) {
				Output::Warning("Couldn't play movie: {}. Video playback backend failed.", movie_filename);
			}
			StopMovie(true);
		}
	}
}

void Game_Screen::StopMovie(bool finished) {
	if (movie_player) {
		movie_player->Stop();
		movie_player.reset();
	}

	movie_finished = finished;
#ifdef EMSCRIPTEN
	movie_pending = false;
#endif
	movie_filename.clear();
}

Rect Game_Screen::GetMovieOutputRect() const {
	int width = movie_res_x;
	int height = movie_res_y;

	if (movie_player) {
		if (width <= 0) {
			width = movie_player->GetNativeWidth();
		}
		if (height <= 0) {
			height = movie_player->GetNativeHeight();
		}
	}

	if (width <= 0) {
		width = Player::screen_width;
	}
	if (height <= 0) {
		height = Player::screen_height;
	}

	Rect movie_rect{movie_pos_x, movie_pos_y, width, height};
	if (!DisplayUi) {
		return movie_rect;
	}

	auto canvas = DisplayUi->GetPresentationRect();
	auto scale = [](int value, int src, int dst) {
		if (src == 0) {
			return value;
		}
		return static_cast<int>(std::llround(static_cast<double>(value) * dst / src));
	};

	return Rect{
		canvas.x + scale(movie_rect.x, static_cast<int>(DisplayUi->GetWidth()), canvas.width),
		canvas.y + scale(movie_rect.y, static_cast<int>(DisplayUi->GetHeight()), canvas.height),
		std::max(scale(movie_rect.width, static_cast<int>(DisplayUi->GetWidth()), canvas.width), 1),
		std::max(scale(movie_rect.height, static_cast<int>(DisplayUi->GetHeight()), canvas.height), 1)
	};
}

void Game_Screen::UpdateWeather() {
	switch (data.weather) {
		case Weather_None:
			break;
		case Weather_Rain:
			UpdateRain();
			break;
		case Weather_Snow:
			UpdateSnow();
			break;
		case Weather_Fog:
			UpdateFog();
			break;
		case Weather_Sandstorm:
			UpdateSandstorm();
			break;
	}
}

void Game_Screen::Update() {
	UpdateScreenEffects();
	if (data.maniac_zoom_time_left > 0) {
		const double step = 1.0 / data.maniac_zoom_time_left;
		data.maniac_zoom_current_x += (data.maniac_zoom_finish_x - data.maniac_zoom_current_x) * step;
		data.maniac_zoom_current_y += (data.maniac_zoom_finish_y - data.maniac_zoom_current_y) * step;
		data.maniac_zoom_current_scale += (data.maniac_zoom_finish_scale - data.maniac_zoom_current_scale) * step;
		--data.maniac_zoom_time_left;
	}
	UpdateMovie();
	UpdateWeather();
	UpdateBattleAnimation();
}

int Game_Screen::ShowBattleAnimation(int animation_id, int target_id, bool global, int start_frame, bool invert) {
	const lcf::rpg::Animation* anim = lcf::ReaderUtil::GetElement(lcf::Data::animations, animation_id);
	if (!anim) {
		Output::Warning("ShowBattleAnimation: Invalid battle animation ID {}", animation_id);
		return 0;
	}

	auto* chara = Game_Character::GetCharacter(target_id, target_id);
	if (!chara) {
		Output::Warning("ShowBattleAnimation: Invalid target event ID {}", target_id);
		CancelBattleAnimation();
		return 0;
	}

	data.battleanim_id = animation_id;
	data.battleanim_target = target_id;
	data.battleanim_global = global;
	data.battleanim_active = true;
	data.battleanim_frame = start_frame;

	animation.reset(new BattleAnimationMap(*anim, *chara, global));
	animation->SetInvert(invert);

	if (start_frame) {
		animation->SetFrame(start_frame);
	}

	return animation->GetFrames();
}

void Game_Screen::SetManiacAnimationBufferCount(int count) {
	count = std::clamp(count, 1, 1000);
	// Empty slots are implicit; growing the native pool starts no animation.
	while (!maniac_animations.empty()) {
		auto last = std::prev(maniac_animations.end());
		if (last->first < count) break;
		maniac_animations.erase(last);
	}
	SaveManiacAnimations();
}

int Game_Screen::ShowManiacBattleAnimation(int buffer, const ManiacAnimationParams& params, int start_frame) {
	buffer = std::clamp(buffer, 0, 999);
	if (params.mode < 0 || params.mode > 4 || start_frame < 0) {
		Output::Warning("ShowBattleAnimation: Invalid Maniac mode or frame");
		return 0;
	}
	if (params.animation_id == 0) {
		maniac_animations.erase(buffer);
		SaveManiacAnimations();
		return 0;
	}
	const auto* anim = lcf::ReaderUtil::GetElement(lcf::Data::animations, params.animation_id);
	if (!anim) {
		Output::Warning("ShowBattleAnimation: Invalid battle animation ID {}", params.animation_id);
		return 0;
	}
	auto effect = std::make_unique<BattleAnimationMap>(*anim, params);
	if (!effect->RefreshTarget()) {
		Output::Warning("ShowBattleAnimation: Invalid target event ID {}", params.target_id);
		return 0;
	}
	if (start_frame > 0) {
		effect->SetFrame(std::min(start_frame, effect->GetFrames()));
	}
	auto previous = maniac_animations.find(buffer);
	if (previous != maniac_animations.end()
			&& previous->second->GetManiacParams().keep
			&& previous->second->GetManiacParams().animation_id == params.animation_id) {
		effect->SetBitmap(previous->second->GetBitmap());
	}
	const int frames = effect->GetFrames() - effect->GetFrame();
	maniac_animations[buffer] = std::move(effect);
	SaveManiacAnimations();
	return frames;
}

void Game_Screen::SaveManiacAnimations() {
	// Version 2 adds the SELF bank's logical identity to each saved animation.
	auto& out = data.easyrpg_maniac_animations;
	out.clear();
	if (maniac_animations.empty()) {
		return;
	}
	out.push_back(2);
	for (const auto& entry : maniac_animations) {
		const auto& effect = *entry.second;
		const auto& p = effect.GetManiacParams();
		out.insert(out.end(), {entry.first, p.animation_id, p.mode, p.target_id,
			p.x, p.y, p.x_mode, p.y_mode, p.invert ? 1 : 0, p.keep ? 1 : 0,
			effect.GetFrame(), effect.GetPositionX(), effect.GetPositionY(), 0,
			p.self_scope, p.self_map_id, p.self_event_id});
	}
}

void Game_Screen::RestoreManiacAnimations() {
	const auto saved = data.easyrpg_maniac_animations;
	maniac_animations.clear();
	if (saved.empty()) {
		return;
	}
	const size_t record_size = saved[0] == 1 ? 14 : 17;
	if ((saved[0] != 1 && saved[0] != 2) || (saved.size() - 1) % record_size != 0) {
		Output::Warning("Invalid saved Maniac animation data");
		data.easyrpg_maniac_animations.clear();
		return;
	}
	for (size_t i = 1; i < saved.size(); i += record_size) {
		ManiacAnimationParams p;
		p.animation_id = saved[i + 1]; p.mode = saved[i + 2]; p.target_id = saved[i + 3];
		p.x = saved[i + 4]; p.y = saved[i + 5];
		p.x_mode = saved[i + 6]; p.y_mode = saved[i + 7];
		p.invert = saved[i + 8] != 0; p.keep = saved[i + 9] != 0;
		if (saved[0] == 2) {
			p.self_scope = saved[i + 14];
			p.self_map_id = saved[i + 15];
			p.self_event_id = saved[i + 16];
		} else if (p.mode == 4) {
			if (p.x_mode < 0 || p.x_mode > 1 || p.y_mode < 0 || p.y_mode > 1) {
				Output::Warning("Invalid saved Maniac animation binding mode");
				continue;
			}
			// Version 1 has no SELF identity. Preserve its valid global bindings;
			// do not infer a bank for previously unsupported SELF animation data.
			if (p.x_mode == 1) p.x = Main_Data::game_variables->Get(p.x);
			if (p.y_mode == 1) p.y = Main_Data::game_variables->Get(p.y);
		}
		p.x_mode = 0; p.y_mode = 0;
		const int buffer = std::clamp(saved[i], 0, 999);
		ShowManiacBattleAnimation(buffer, p, saved[i + 10]);
		auto it = maniac_animations.find(buffer);
		if (it != maniac_animations.end()) {
			it->second->SetPosition(saved[i + 11], saved[i + 12]);
		}
	}
	SaveManiacAnimations();
}

void Game_Screen::UpdateBattleAnimation() {
	for (auto it = maniac_animations.begin(); it != maniac_animations.end();) {
		auto& effect = *it->second;
		if (!effect.RefreshTarget()) {
			it = maniac_animations.erase(it);
			continue;
		}
		if (!effect.IsDone()) {
			effect.Update();
		}
		// Cached completed instances retain the bitmap, but do not play or wait.
		if (effect.IsDone() && !effect.GetManiacParams().keep && !Game_Battle::IsBattleRunning()) {
			it = maniac_animations.erase(it);
		} else {
			++it;
		}
	}
	SaveManiacAnimations();
	if (animation) {
		if (!animation->IsDone()) {
			animation->Update();
			data.battleanim_frame = animation->GetFrame();
		}

		if (animation->IsDone() && !Game_Battle::IsBattleRunning()) {
			// FIXME: Lifetime is flawed but we need the animation in battle for
			// SE and flash. Delay destruction until back on the map.
			data.battleanim_active = false;
			animation.reset();
		}
	}
}

void Game_Screen::CancelBattleAnimation() {
	maniac_animations.clear();
	data.easyrpg_maniac_animations.clear();
	data.battleanim_frame = animation ?
		animation->GetFrames() : 0;
	data.battleanim_active = false;
	animation.reset();
}

void Game_Screen::UpdateUnderlyingEventReferences() {
	for (auto it = maniac_animations.begin(); it != maniac_animations.end();) {
		if (!it->second->RefreshTarget()) {
			it = maniac_animations.erase(it);
		} else {
			++it;
		}
	}
	SaveManiacAnimations();
	if (!animation || !animation->HasTarget()) {
		return;
	}

	auto* chara = Game_Character::GetCharacter(data.battleanim_target, data.battleanim_target);
	if (!chara) {
		// Event was deleted
		data.battleanim_active = false;
		animation.reset();
	} else {
		animation->SetTarget(*chara);
	}
}
