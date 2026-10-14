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

#ifndef EP_GAME_PICTURE_H
#define EP_GAME_PICTURE_H

// Headers
#include <string>
#include <deque>
#include <optional>
#include "async_handler.h"
#include <lcf/rpg/savepicture.h>
#include "sprite_picture.h"

class Sprite_Picture;
class Scene;
class Window_Base;

/**
 * Pictures class.
 */
class Game_Pictures {
public:
	Game_Pictures() = default;

	void SetSaveData(std::vector<lcf::rpg::SavePicture> save);
	std::vector<lcf::rpg::SavePicture> GetSaveData() const;

	void InitGraphics();

	static int GetDefaultNumberOfPictures();
	int GetMaxPictureId() const;
	void SetManiacPictureLimit(int32_t limit);

	struct Params {
		double position_x = 0;
		double position_y = 0;
		int top_trans = 0;
		int bottom_trans = 0;
		int red = 100;
		int green = 100;
		int blue = 100;
		int saturation = 100;
		int effect_mode = 0;
		int effect_power = 0;
		// Native fixed-angle pictures retain the integer numerator separately
		// from the double target angle (LSD chunks 46 and 16/52).
		double fixed_angle = 0.0;
		uint32_t maniac_style = 0;
		// Extensions
		bool flip_x = false;
		bool flip_y = false;
		int blend_mode = 0;
		int origin = 0;
		int magnify_width = 100; // RPG_RT supports magnify, but not independent for w/h
		int magnify_height = 100;
	};
	struct ShowParams : Params {
		std::string name;
		// RPG Maker 2k3 1.12
		int spritesheet_cols = 1;
		int spritesheet_rows = 1;
		int spritesheet_frame = 0;
		int spritesheet_speed = 0;
		int map_layer = 7;
		int battle_layer = 0;
		// erase_on_map_change | affected_by_flash | affected_by_shake
		int flags = 1 | 32 | 64;
		bool spritesheet_play_once = false;
		int maniac_spritesheet_start = 0;
		int maniac_spritesheet_end = 0;
		int maniac_spritesheet_finish = 0;
		uint32_t maniac_image_flags = 0;
		uint32_t maniac_transparency_flags = 0;
		bool position_scaled = false;
		bool use_transparent_color = false;
		bool fixed_to_map = false;
	};

	struct MoveParams : Params {
		int duration = 0;
		uint32_t maniac_style_mask = 0;
		uint32_t maniac_options = 0;
	};

	bool Show(int id, const ShowParams& params);
	void ChangeFile(int id, std::string name);
	void ChangeManiacIds(int32_t operation, int32_t source, int32_t count, int32_t target_or_offset, bool ignore_errors);
	void Move(int id, const MoveParams& params);
	void Erase(int id);
	void EraseAll();

	void Update(bool is_battle);

	void OnMapChange();
	void OnBattleEnd();
	void OnMapScrolled(int dx, int dy);

	struct Picture {
		explicit Picture(int id) { data.ID = id; }
		explicit Picture(lcf::rpg::SavePicture data);

		std::unique_ptr<Sprite_Picture> sprite;
		lcf::rpg::SavePicture data;
		FileRequestBinding request_id;
		bool needs_update = false;
		bool bitmap_modified = false;
		int origin = 0;
		bool position_scaled = false;
		// Resource sheets allocate processed pixels at frame size; mutable images
		// retain their original allocation when detached or flattened.
		int maniac_image_width = 0;
		int maniac_image_height = 0;
		int maniac_frame_width = 0;
		int maniac_frame_height = 0;
		bool maniac_resource_shared = false;
		BitmapRef maniac_processed;

		void Update(bool is_battle);
		// Filename reuse retains the backing image's key even if the command
		// replaces the saved transparency byte.
		std::optional<bool> maniac_image_chromakey;
		bool GetManiacImageChromakey() const { return maniac_image_chromakey.value_or(data.use_transparent_color); }

		bool IsOnMap() const;
		bool IsOnBattle() const;
		int NumSpriteSheetFrames() const;

		ShowParams GetShowParams() const;
		void SetNonEffectParams(const Params& params, bool set_positions);

		bool Show(const ShowParams& params, bool reuse_pixels = false);
		void Move(const MoveParams& params);
		void Erase();
		bool Exists() const;

		void CreateSprite();

		bool IsRequestPending() const;
		void MakeRequestImportant() const;

		void OnPictureSpriteReady(bool apply_origin = true);
		BitmapRef GetMutableBitmap(bool flatten_spritesheet = false);
		void InitManiacPixelLayout(bool resource_shared);
		void ResetManiacPixelLayout();
		void RecordManiacProcessedFrame(const BitmapRef& frame);
		BitmapRef GetManiacReadBitmap(bool processed) const;
		Color ReadManiacPixel(const Bitmap& source, bool processed, int x, int y) const;
		void ClearManiacTileRect(const Rect& rect);
		void CopyManiacTilePart(int x, int y, const Bitmap& source, const Rect& source_rect, bool skip_alpha_zero);
		void RestoreModifiedBitmap();
		void ApplyOrigin(bool is_move);
		void OnMapScrolled(int dx, int dy);

		void AttachWindow(const Window_Base& window);
		void AttachManiacTextBitmap(const BitmapRef& bitmap);
		bool IsWindowAttached() const;
	};

	Picture& GetPicture(int id);
	Picture* GetPicturePtr(int id);
	std::shared_ptr<Picture> GetPictureRef(int id);
	void ChangeFile(Picture& picture, std::string name);

private:
	void RequestPictureSprite(Picture& pic, bool apply_origin = true);
	void OnPictureSpriteReady(FileRequestResult*, bool apply_origin);

	// Slots exchange the same holders, including pending loads and JS references.
	std::vector<std::shared_ptr<Picture>> pictures;
	int frame_counter = 0;
};

inline bool Game_Pictures::Picture::IsOnMap() const {
	return data.map_layer > 0;
}

inline bool Game_Pictures::Picture::IsOnBattle() const {
	return data.battle_layer > 0;
}

#endif
