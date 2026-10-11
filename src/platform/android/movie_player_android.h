/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EP_ANDROID_MOVIE_PLAYER_H
#define EP_ANDROID_MOVIE_PLAYER_H
#include "movie_player.h"

class AndroidMoviePlayer final : public MoviePlayer {
public:
	AndroidMoviePlayer();
	~AndroidMoviePlayer() override;
	bool Open(std::string_view path, std::string& error_message) override;
	void Update(const Rect& dst_rect) override;
	void Stop() override;
	bool IsPlaying() const override;
	bool HasError() const override;
	int GetNativeWidth() const override;
	int GetNativeHeight() const override;
	void Draw(Bitmap& dst, const Rect& rect) override;
private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
#endif
