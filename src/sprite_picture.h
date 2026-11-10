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

#ifndef EP_PICTURE_SPRITE_H
#define EP_PICTURE_SPRITE_H

#include "game_netplay.h"
#include "sprite.h"
#include <array>
#include <lcf/rpg/savepicture.h>
#include <optional>

class Bitmap;

/**
 * Sprite picture class.
 */
class Sprite_Picture : public Sprite {
public:
	/**
	 * Constructor.
	 *
	 * @param pic_id the picture id
	 */
	Sprite_Picture(int pic_id, Drawable::Flags flags = Drawable::Flags::Default);

	void Draw(Bitmap& dst) override;

	struct NetplayPose {
		lcf::rpg::SavePicture data;
		BitmapRef bitmap;
		Tone tone;
		Color flash;
		int shake_x;
		int shake_y;
		std::optional<std::array<int, 14>> maniac_pixels;
		bool immediate_preview = false;
		std::optional<Game_Netplay::PictureView> captured_view;
		int birth = -1;
	};
	void PresentNetplay(NetplayPose pose);
	const NetplayPose* GetNetplayPose() const { return netplay_pose.get(); }

	void OnPictureShow(bool reuse_pixels = false);
	void SetPictureId(int id);
	void UpdateManiacPixels();
	void MaterializeManiacPixels() const;
	void ResetManiacPixels();
	BitmapRef GetPreparedBitmap(Rect& rect) const;

	/** @return Width of a single spritesheet frame or the entire width if the picture has no spritesheet */
	int GetFrameWidth() const;

	/** @return Height of a single spritesheet frame or the entire width if the picture has no spritesheet */
	int GetFrameHeight() const;

private:
	void DrawStandard(Bitmap& dst, const lcf::rpg::SavePicture& data, int frames,
		const Tone& screen_tone, const Color& flash, int shake_x, int shake_y, int reflection, int offset_x, int offset_y);
	void UpdatePictureZ();
	std::unique_ptr<NetplayPose> netplay_pose;
	mutable BitmapRef maniac_source;
	mutable BitmapRef maniac_prepared;
	mutable std::array<int, 14> maniac_prepare_state{};
	mutable uint64_t maniac_source_revision = 0;
	mutable bool maniac_pixels_pending = false;
	int last_spritesheet_frame = -1;
	int pic_id = 0;
	const bool feature_spritesheet = false;
	const bool feature_priority_layers = false;
	const bool feature_bottom_trans = false;
};

#endif
