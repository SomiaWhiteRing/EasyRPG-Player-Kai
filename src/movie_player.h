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

#ifndef EP_MOVIE_PLAYER_H
#define EP_MOVIE_PLAYER_H

#include <memory>
#include <string>
#include <string_view>

#include "rect.h"

class Bitmap;

class MoviePlayer {
public:
	virtual ~MoviePlayer() = default;

	virtual bool Open(std::string_view path, std::string& error_message) = 0;
	virtual void Update(const Rect& dst_rect) = 0;
	virtual void Stop() = 0;
	virtual bool IsPlaying() const = 0;
	virtual bool HasError() const;
	virtual void Draw(Bitmap&, const Rect&) {}
	virtual int GetNativeWidth() const = 0;
	virtual int GetNativeHeight() const = 0;

	static std::unique_ptr<MoviePlayer> Create();
};

#endif
