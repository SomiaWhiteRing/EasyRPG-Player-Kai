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

#include "platform/emscripten/movie_player_web.h"

#include <algorithm>

#include <emscripten.h>

EM_JS(int, EasyRpgWebMovieOpen, (const char* path), {
 const filePath = UTF8ToString(path);
 try {
  // WORKERFS stores each resource as a Blob slice; keep videos out of WASM memory.
  const node = FS.lookupPath(filePath).node;
  const blob = Module.movieSource ? Module.movieSource(node.contents) : node.contents;
  if (!Module.movieSource && !(blob instanceof Blob)) throw new Error('Movie is not a local Blob');
  const id = Module.webMovieSequence = (Module.webMovieSequence || 0) + 1;
  Module.webMovie = {id, playing: true, error: "", width: 0, height: 0};
  postMessage({type: 'movie-open', id, blob, path: filePath});
  return 1;
 } catch (error) {
  Module.webMovie = {playing: false, error: String(error)};
  return 0;
 }
});
EM_JS(void, EasyRpgWebMovieUpdate, (int x, int y, int width, int height), {
 postMessage({type: 'movie-rect', id: Module.webMovie?.id, x, y, width, height});
});
EM_JS(void, EasyRpgWebMovieStop, (), {
 postMessage({type: 'movie-stop', id: Module.webMovie?.id});
 Module.webMovie = {playing: false, error: ""};
});
EM_JS(int, EasyRpgWebMovieIsPlaying, (), { return Module.webMovie?.playing ? 1 : 0; });
EM_JS(int, EasyRpgWebMovieHasError, (), { return Module.webMovie?.error ? 1 : 0; });
EM_JS(int, EasyRpgWebMovieGetNativeWidth, (), { return Module.webMovie?.width || 0; });
EM_JS(int, EasyRpgWebMovieGetNativeHeight, (), { return Module.webMovie?.height || 0; });

WebMoviePlayer::~WebMoviePlayer() {
	Stop();
}

bool WebMoviePlayer::Open(std::string_view path, std::string& error_message) {
	const auto path_string = std::string(path);
	opened = EasyRpgWebMovieOpen(path_string.c_str()) != 0;
	if (!opened) {
		error_message = "browser video playback failed";
	}
	return opened;
}

void WebMoviePlayer::Update(const Rect& dst_rect) {
	if (!opened) {
		return;
	}

	EasyRpgWebMovieUpdate(
		dst_rect.x,
		dst_rect.y,
		std::max(dst_rect.width, 1),
		std::max(dst_rect.height, 1));
}

void WebMoviePlayer::Stop() {
	if (!opened) {
		return;
	}

	EasyRpgWebMovieStop();
	opened = false;
}

bool WebMoviePlayer::IsPlaying() const {
	return opened && EasyRpgWebMovieIsPlaying() != 0;
}

bool WebMoviePlayer::HasError() const {
	return opened && EasyRpgWebMovieHasError() != 0;
}

int WebMoviePlayer::GetNativeWidth() const {
	return EasyRpgWebMovieGetNativeWidth();
}

int WebMoviePlayer::GetNativeHeight() const {
	return EasyRpgWebMovieGetNativeHeight();
}
