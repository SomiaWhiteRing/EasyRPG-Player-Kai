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

#include "movie_player.h"

#ifdef _WIN32
#include "platform/windows/movie_player_dshow.h"
#endif
#ifdef EMSCRIPTEN
#include "platform/emscripten/movie_player_web.h"
#endif
#ifdef HAVE_ANDROID_MOVIE_PLAYER
#include "platform/android/movie_player_android.h"
#endif

bool MoviePlayer::HasError() const {
	return false;
}

std::unique_ptr<MoviePlayer> MoviePlayer::Create() {
#ifdef HAVE_DIRECTSHOW
	return std::make_unique<DirectShowMoviePlayer>();
#elif defined(HAVE_WEB_MOVIE_PLAYER)
	return std::make_unique<WebMoviePlayer>();
#elif defined(HAVE_ANDROID_MOVIE_PLAYER)
	return std::make_unique<AndroidMoviePlayer>();
#else
	return {};
#endif
}
