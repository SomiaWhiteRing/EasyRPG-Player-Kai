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
#include <cstring>
#include <cmath>
#include "tilemap_layer.h"
#include "output.h"
#include "player.h"
#include "map_data.h"
#include "main_data.h"
#include "bitmap.h"
#include "compiler.h"
#include "game_map.h"
#include "game_system.h"
#include "utils.h"
#include "drawable_mgr.h"
#include "baseui.h"

// Blocks subtiles IDs
// Mess with this code and you will die in 3 days...
// [tile-id][row][col]
static constexpr int8_t BlockA_Subtiles_IDS[47][2][2] = {
#define N -1
	{{N, N}, {N, N}},
	{{3, N}, {N, N}},
	{{N, 3}, {N, N}},
	{{3, 3}, {N, N}},
	{{N, N}, {N, 3}},
	{{3, N}, {N, 3}},
	{{N, 3}, {N, 3}},
	{{3, 3}, {N, 3}},
	{{N, N}, {3, N}},
	{{3, N}, {3, N}},
	{{N, 3}, {3, N}},
	{{3, 3}, {3, N}},
	{{N, N}, {3, 3}},
	{{3, N}, {3, 3}},
	{{N, 3}, {3, 3}},
	{{3, 3}, {3, 3}},
	{{1, N}, {1, N}},
	{{1, 3}, {1, N}},
	{{1, N}, {1, 3}},
	{{1, 3}, {1, 3}},
	{{2, 2}, {N, N}},
	{{2, 2}, {N, 3}},
	{{2, 2}, {3, N}},
	{{2, 2}, {3, 3}},
	{{N, 1}, {N, 1}},
	{{N, 1}, {3, 1}},
	{{3, 1}, {N, 1}},
	{{3, 1}, {3, 1}},
	{{N, N}, {2, 2}},
	{{3, N}, {2, 2}},
	{{N, 3}, {2, 2}},
	{{3, 3}, {2, 2}},
	{{1, 1}, {1, 1}},
	{{2, 2}, {2, 2}},
	{{0, 2}, {1, N}},
	{{0, 2}, {1, 3}},
	{{2, 0}, {N, 1}},
	{{2, 0}, {3, 1}},
	{{N, 1}, {2, 0}},
	{{3, 1}, {2, 0}},
	{{1, N}, {0, 2}},
	{{1, 3}, {0, 2}},
	{{0, 0}, {1, 1}},
	{{0, 2}, {0, 2}},
	{{1, 1}, {0, 0}},
	{{2, 0}, {2, 0}},
	{{0, 0}, {0, 0}}
#undef N
};

// [tile-id][row][col][x/y]
static constexpr uint8_t BlockD_Subtiles_IDS[50][2][2][2] = {
//     T-L     T-R       B-L     B-R
    {{{1, 2}, {1, 2}}, {{1, 2}, {1, 2}}},
    {{{2, 0}, {1, 2}}, {{1, 2}, {1, 2}}},
    {{{1, 2}, {2, 0}}, {{1, 2}, {1, 2}}},
    {{{2, 0}, {2, 0}}, {{1, 2}, {1, 2}}},
    {{{1, 2}, {1, 2}}, {{1, 2}, {2, 0}}},
    {{{2, 0}, {1, 2}}, {{1, 2}, {2, 0}}},
    {{{1, 2}, {2, 0}}, {{1, 2}, {2, 0}}},
    {{{2, 0}, {2, 0}}, {{1, 2}, {2, 0}}},
    {{{1, 2}, {1, 2}}, {{2, 0}, {1, 2}}},
    {{{2, 0}, {1, 2}}, {{2, 0}, {1, 2}}},
    {{{1, 2}, {2, 0}}, {{2, 0}, {1, 2}}},
    {{{2, 0}, {2, 0}}, {{2, 0}, {1, 2}}},
    {{{1, 2}, {1, 2}}, {{2, 0}, {2, 0}}},
    {{{2, 0}, {1, 2}}, {{2, 0}, {2, 0}}},
    {{{1, 2}, {2, 0}}, {{2, 0}, {2, 0}}},
    {{{2, 0}, {2, 0}}, {{2, 0}, {2, 0}}},
    {{{0, 2}, {0, 2}}, {{0, 2}, {0, 2}}},
    {{{0, 2}, {2, 0}}, {{0, 2}, {0, 2}}},
    {{{0, 2}, {0, 2}}, {{0, 2}, {2, 0}}},
    {{{0, 2}, {2, 0}}, {{0, 2}, {2, 0}}},
    {{{1, 1}, {1, 1}}, {{1, 1}, {1, 1}}},
    {{{1, 1}, {1, 1}}, {{1, 1}, {2, 0}}},
    {{{1, 1}, {1, 1}}, {{2, 0}, {1, 1}}},
    {{{1, 1}, {1, 1}}, {{2, 0}, {2, 0}}},
    {{{2, 2}, {2, 2}}, {{2, 2}, {2, 2}}},
    {{{2, 2}, {2, 2}}, {{2, 0}, {2, 2}}},
    {{{2, 0}, {2, 2}}, {{2, 2}, {2, 2}}},
    {{{2, 0}, {2, 2}}, {{2, 0}, {2, 2}}},
    {{{1, 3}, {1, 3}}, {{1, 3}, {1, 3}}},
    {{{2, 0}, {1, 3}}, {{1, 3}, {1, 3}}},
    {{{1, 3}, {2, 0}}, {{1, 3}, {1, 3}}},
    {{{2, 0}, {2, 0}}, {{1, 3}, {1, 3}}},
    {{{0, 2}, {2, 2}}, {{0, 2}, {2, 2}}},
    {{{1, 1}, {1, 1}}, {{1, 3}, {1, 3}}},
    {{{0, 1}, {0, 1}}, {{0, 1}, {0, 1}}},
    {{{0, 1}, {0, 1}}, {{0, 1}, {2, 0}}},
    {{{2, 1}, {2, 1}}, {{2, 1}, {2, 1}}},
    {{{2, 1}, {2, 1}}, {{2, 0}, {2, 1}}},
    {{{2, 3}, {2, 3}}, {{2, 3}, {2, 3}}},
    {{{2, 0}, {2, 3}}, {{2, 3}, {2, 3}}},
    {{{0, 3}, {0, 3}}, {{0, 3}, {0, 3}}},
    {{{0, 3}, {2, 0}}, {{0, 3}, {0, 3}}},
    {{{0, 1}, {2, 1}}, {{0, 1}, {2, 1}}},
    {{{0, 1}, {0, 1}}, {{0, 3}, {0, 3}}},
    {{{0, 3}, {2, 3}}, {{0, 3}, {2, 3}}},
    {{{2, 1}, {2, 1}}, {{2, 3}, {2, 3}}},
    {{{0, 1}, {2, 1}}, {{0, 3}, {2, 3}}},
    {{{1, 2}, {1, 2}}, {{1, 2}, {1, 2}}},
    {{{1, 2}, {1, 2}}, {{1, 2}, {1, 2}}},
    {{{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}}
};

// Set of neighboring autotiles -> autotile variant
// Each neighbor is represented by a single bit (1 - same autotile, 0 - any other case)
// The bits are ordered as follows (from most to least significant bit): NW N NE W E SW S SE
static const std::unordered_map<uint8_t, int> AUTOTILE_D_VARIANTS_MAP = { //it also works with A
	{0b11111111, 0},
	{0b01111111, 1},
	{0b11011111, 2},
	{0b01011111, 3},
	{0b11111110, 4},
	{0b01111110, 5},
	{0b11011110, 6},
	{0b01011110, 7},
	{0b11111011, 8},
	{0b01111011, 9},
	{0b11011011, 10},
	{0b01011011, 11},
	{0b11111010, 12},
	{0b01111010, 13},
	{0b11011010, 14},
	{0b01011010, 15},
	{0b01101011, 16},
	{0b01001011, 17},
	{0b01101010, 18},
	{0b01001010, 19},
	{0b00011111, 20},
	{0b00011110, 21},
	{0b00011011, 22},
	{0b00011010, 23},
	{0b11010110, 24},
	{0b11010010, 25},
	{0b01010110, 26},
	{0b01010010, 27},
	{0b11111000, 28},
	{0b01111000, 29},
	{0b11011000, 30},
	{0b01011000, 31},
	{0b01000010, 32},
	{0b00011000, 33},
	{0b00001011, 34},
	{0b00001010, 35},
	{0b00010110, 36},
	{0b00010010, 37},
	{0b11010000, 38},
	{0b01010000, 39},
	{0b01101000, 40},
	{0b01001000, 41},
	{0b00000010, 42},
	{0b00001000, 43},
	{0b01000000, 44},
	{0b00010000, 45},
	{0b00000000, 46}
};

TilemapLayer::TilemapLayer(int ilayer) :
	substitutions(Game_Map::GetTilesLayer(ilayer)),
	layer(ilayer),
	// SubLayer for the tiles without Wall or Above passability
	// Its z-value should be under z of the events in the lower layer
	lower_layer(this, Priority_TilesetBelow + TileBelow + layer),
	// SubLayer for the tiles with Wall or Above passability
	// Its z-value should be between the z of the events in the upper layer and the hero
	upper_layer(this, Priority_TilesetAbove + TileAbove + layer)
{
}

// This setup of having an always inlined DrawTile() which dispatches to DrawTileImpl()
// was created intentionally. Inlining the transparency check was measured and shown
// to provide a performance improvement
EP_ALWAYS_INLINE
void TilemapLayer::DrawTile(Bitmap& dst, Bitmap& tileset, Bitmap& tone_tileset, int x, int y, int row, int col, uint32_t tone_hash, bool allow_fast_blit) {
	auto op = tileset.GetTileOpacity(col, row);
	if (op != ImageOpacity::Transparent) {
		DrawTileImpl(dst, tileset, tone_tileset, x, y, row, col, tone_hash, op, allow_fast_blit);
	}
}

void TilemapLayer::DrawTileImpl(Bitmap& dst, Bitmap& tileset, Bitmap& tone_tileset, int x, int y, int row, int col, uint32_t tone_hash, ImageOpacity op, bool allow_fast_blit) {

	auto rect = Rect{ col * TILE_SIZE, row * TILE_SIZE, TILE_SIZE, TILE_SIZE };

	auto* src = &tileset;

	// Create tone changed tile
	if (tone != Tone()) {
		if (chipset_tone_tiles.insert(tone_hash).second) {
			tone_tileset.ToneBlit(col * TILE_SIZE, row * TILE_SIZE, tileset, rect, tone, Opacity::Opaque());
		}
		src = &tone_tileset;
	}

	bool use_fast_blit = fast_blit && allow_fast_blit;
	if (op == ImageOpacity::Opaque || use_fast_blit) {
		dst.BlitFast(x, y, *src, rect, 255);
	} else {
		dst.Blit(x, y, *src, rect, 255);
	}
}

static uint32_t MakeFTileHash(int id) {
	return static_cast<uint32_t>(id);
}

static uint32_t MakeETileHash(int id) {
	return static_cast<uint32_t>(id | (1 << 24));
}

static uint32_t MakeDTileHash(int id) {
	return static_cast<uint32_t>(id | (2 << 24));
}

static uint32_t MakeCTileHash(int id, int anim_step) {
	return static_cast<uint32_t>((id + (anim_step << 12)) | (3 << 24));
}

static uint32_t MakeAbTileHash(int id, int anim_step) {
	return static_cast<uint32_t>((id + (anim_step << 12)) | (4 << 24));
}

void TilemapLayer::Draw(Bitmap& dst, uint8_t z_order, int render_ox, int render_oy) {
	// Get the number of tiles that can be displayed on window
	int tiles_x = (int)ceil(Player::screen_width / (float)TILE_SIZE);
	int tiles_y = (int)ceil(Player::screen_height / (float)TILE_SIZE);

	// If ox or oy are not equal to the tile size draw the next tile too
	// to prevent black (empty) tiles at the borders
	if ((ox - render_ox) % TILE_SIZE != 0) {
		++tiles_x;
	}
	if ((oy - render_oy) % TILE_SIZE != 0) {
		++tiles_y;
	}

	const bool loop_h = Game_Map::LoopHorizontal();
	const bool loop_v = Game_Map::LoopVertical();

	auto div_rounding_down = [](int n, int m) {
		if (n >= 0) return n / m;
		return (n - m + 1) / m;
	};
	auto mod = [](int n, int m) {
		int rem = n % m;
		return rem >= 0 ? rem : m + rem;
	};

	// FIXME: When Game_Map singleton is made an object we can remove this null check
	const auto frames = Main_Data::game_system ? static_cast<uint32_t>(Main_Data::game_system->GetFrameCounter()) : 0u;
	auto animation_step_c = (frames / 6) % 4;
	auto animation_step_ab = frames / animation_speed;
	if (animation_type) {
		animation_step_ab %= 3;
	} else {
		animation_step_ab %= 4;
		if (animation_step_ab == 3) {
			animation_step_ab = 1;
		}
	}

	const int div_ox = div_rounding_down(ox - render_ox, TILE_SIZE);
	const int div_oy = div_rounding_down(oy - render_oy, TILE_SIZE);

	const int mod_ox = mod(ox - render_ox, TILE_SIZE);
	const int mod_oy = mod(oy - render_oy, TILE_SIZE);

	for (int y = 0; y < tiles_y; y++) {
		for (int x = 0; x < tiles_x; x++) {

			// Get the real maps tile coordinates
			int map_x = div_ox + x;
			int map_y = div_oy + y;
			if (loop_h) map_x = mod(map_x, width);
			if (loop_v) map_y = mod(map_y, height);

			bool out_of_bounds =
				map_x < 0 || map_x >= width ||
				map_y < 0 || map_y >= height;

			if (out_of_bounds) {
				continue;
			}

			int map_draw_x = x * TILE_SIZE - mod_ox;
			int map_draw_y = y * TILE_SIZE - mod_oy;

			// Get the tile data
			TileData &tile = GetDataCache(map_x, map_y);

			// Draw the sublayer if its z is being draw now
			if (z_order == tile.z) {
				if (layer == 0) {
					// If lower layer
					bool allow_fast_blit = (tile.z == TileBelow);

					if (tile.ID >= BLOCK_E && tile.ID < BLOCK_E + BLOCK_E_TILES) {
						int id = substitutions[tile.ID - BLOCK_E];
						// If Block E

						int row, col;

						// Get the tile coordinates from chipset
						if (id < 96) {
							// If from first column of the block
							col = 12 + id % 6;
							row = id / 6;
						} else {
							// If from second column of the block
							col = 18 + (id - 96) % 6;
							row = (id - 96) / 6;
						}

						auto tone_hash = MakeETileHash(id);
						DrawTile(dst, *chipset, *chipset_effect, map_draw_x, map_draw_y, row, col, tone_hash, allow_fast_blit);
					} else if (tile.ID >= BLOCK_C && tile.ID < BLOCK_D) {
						// If Block C

						// Get the tile coordinates from chipset
						int col = 3 + (tile.ID - BLOCK_C) / 50;
						int row = 4 + animation_step_c;

						auto tone_hash = MakeCTileHash(tile.ID, animation_step_c);
						DrawTile(dst, *chipset, *chipset_effect, map_draw_x, map_draw_y, row, col, tone_hash, allow_fast_blit);
					} else if (tile.ID < BLOCK_C) {
						// If Blocks A1, A2, B

						// Draw the tile from autotile cache
						TileXY pos = GetCachedAutotileAB(tile.ID, animation_step_ab);

						int col = pos.x;
						int row = pos.y;

						// Create tone changed tile
						auto tone_hash = MakeAbTileHash(tile.ID,  animation_step_ab);
						DrawTile(dst, *autotiles_ab_screen, *autotiles_ab_screen_effect, map_draw_x, map_draw_y, row, col, tone_hash, allow_fast_blit);
					} else {
						// If blocks D1-D12

						// Draw the tile from autotile cache
						TileXY pos = GetCachedAutotileD(tile.ID);

						int col = pos.x;
						int row = pos.y;

						auto tone_hash = MakeDTileHash(tile.ID);
						DrawTile(dst, *autotiles_d_screen, *autotiles_d_screen_effect, map_draw_x, map_draw_y, row, col, tone_hash, allow_fast_blit);
					}
				} else {
					// If upper layer

					// Check that block F is being drawn
					if (tile.ID >= BLOCK_F && tile.ID < BLOCK_F + BLOCK_F_TILES) {
						int id = substitutions[tile.ID - BLOCK_F];
						int row, col;

						// Get the tile coordinates from chipset
						if (id < 48) {
							// If from first column of the block
							col = 18 + id % 6;
							row = 8 + id / 6;
						} else {
							// If from second column of the block
							col = 24 + (id - 48) % 6;
							row = (id - 48) / 6;
						}

						auto tone_hash = MakeFTileHash(id);
						DrawTile(dst, *chipset, *chipset_effect, map_draw_x, map_draw_y, row, col, tone_hash);
					}
				}
			}
		}
	}
}

TilemapLayer::TileXY TilemapLayer::GetCachedAutotileAB(short ID, short animID) {
	short block = ID / 1000;
	short b_subtile = (ID - block * 1000) / 50;
	short a_subtile = ID - block * 1000 - b_subtile * 50;
	return autotiles_ab[animID][block][b_subtile][a_subtile];
}

TilemapLayer::TileXY TilemapLayer::GetCachedAutotileD(short ID) {
	short block = (ID - 4000) / 50;
	short subtile = ID - 4000 - block * 50;
	return autotiles_d[block][subtile];
}

void TilemapLayer::CreateTileCache(const std::vector<short>& nmap_data) {
	data_cache_vec.resize(width * height);
	for (int x = 0; x < width; x++) {
		for (int y = 0; y < height; y++) {
			auto tile_id = nmap_data[x + y * width];
			CreateTileCacheAt(x, y, tile_id);
		}
	}
}

void TilemapLayer::CreateTileCacheAt(int x, int y, int tile_id) {
	TileData tile;
	tile.ID = static_cast<short>(tile_id);
	tile.z = TileBelow;

	// Calculate the tile Z
	if (!passable.empty()) {
		if (tile.ID >= BLOCK_F) { // Upper layer
			if ((passable[substitutions[tile.ID - BLOCK_F]] & Passable::Above) != 0)
				tile.z = TileAbove + 1; // Upper sublayer
			else
				tile.z = TileBelow + 1; // Lower sublayer

		} else { // Lower layer
			int chip_index =
					tile.ID >= BLOCK_E ? substitutions[tile.ID - BLOCK_E] + 18 :
					tile.ID >= BLOCK_D ? (tile.ID - BLOCK_D) / 50 + 6 :
					tile.ID >= BLOCK_C ? (tile.ID - BLOCK_C) / 50 + 3 :
					tile.ID / 1000;
			if ((passable[chip_index] & (Passable::Wall | Passable::Above)) != 0)
				tile.z = TileAbove; // Upper sublayer
			else
				tile.z = TileBelow; // Lower sublayer

		}
	}
	GetDataCache(x, y) = tile;
}

void TilemapLayer::RecreateTileDataAt(int x, int y, int tile_id) {
	map_data[x + y * width] = static_cast<short>(tile_id);
	Game_Map::ReplaceTileAt(x, y, tile_id, layer);
	CreateTileCacheAt(x, y, tile_id);
}

void TilemapLayer::GenerateAutotileAB(short ID, short animID) {
	// Calculate the block to use
	//	1: A1 + Upper B (Grass + Coast)
	//	2: A2 + Upper B (Snow + Coast)
	//	3: A1 + Lower B (Grass + Ocean/Deep water)
	short block = ID / 1000;

	// Calculate the B block combination
	short b_subtile = (ID - block * 1000) / 50;
	if (b_subtile >= TILE_SIZE) {
		Output::Warning("Invalid AB autotile ID: {} (b_subtile = {})",
						ID, b_subtile);
		return;
	}

	// Calculate the A block combination
	short a_subtile = ID - block * 1000 - b_subtile * 50;
	if (a_subtile >= 47) {
		Output::Warning("Invalid AB autotile ID: {} (a_subtile = {})",
						ID, a_subtile);
		return;
	}

	if (autotiles_ab[animID][block][b_subtile][a_subtile].valid)
		return;

	uint8_t quarters[2][2][2];

	// Determine block B subtiles
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			// Skip the subtile if it will be used one from A block instead
			if (BlockA_Subtiles_IDS[a_subtile][j][i] != -1) continue;

			// Get the block B subtiles ids and get their coordinates on the chipset
			int t = (b_subtile >> (j * 2 + i)) & 1;
			if (block == 2) t ^= 3;

			quarters[j][i][0] = animID;
			quarters[j][i][1] = 4 + t;
		}
	}

	// Determine block A subtiles
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			// Skip the subtile if it was used one from B block
			if (BlockA_Subtiles_IDS[a_subtile][j][i] == -1) continue;

			// Get the block A subtiles ids and get their coordinates on the chipset
			quarters[j][i][0] = animID + (block == 1 ? 3 : 0);
			quarters[j][i][1] = BlockA_Subtiles_IDS[a_subtile][j][i];
		}
	}

	// Determine block B subtiles when combining A and B
	if (b_subtile != 0 && a_subtile != 0) {
		for (int j = 0; j < 2; j++) {
			for (int i = 0; i < 2; i++) {
				// calculate tile (row 0..3)
				int t = (b_subtile >> (j * 2 + i)) & 1;
				if (block == 2) t *= 2;

				// Skip the subtile if not used
				if (t == 0) continue;

				// Get the coordinates on the chipset
				quarters[j][i][0] = animID;
				quarters[j][i][1] = 4 + t;
			}
		}
	}

	// pack the quarters data into a word
	uint32_t quarters_hash = 0;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++)
			for (int k = 0; k < 2; k++) {
				quarters_hash <<= 4;
				quarters_hash |= quarters[j][i][k];
			}

	// check whether we have already generated this tile
	auto it = autotiles_ab_map.find(quarters_hash);
	if (it != autotiles_ab_map.end()) {
		autotiles_ab[animID][block][b_subtile][a_subtile] = it->second;
		return;
	}

	int id = autotiles_ab_next++;
	int dst_x = id % TILES_PER_ROW;
	int dst_y = id / TILES_PER_ROW;

	TileXY tile_xy(dst_x, dst_y);
	autotiles_ab_map[quarters_hash] = tile_xy;
	autotiles_ab[animID][block][b_subtile][a_subtile] = tile_xy;
}

void TilemapLayer::GenerateAutotileD(short ID) {
	// Calculate the D block id
	short block = (ID - 4000) / 50;

	// Calculate the D block combination
	short subtile = ID - 4000 - block * 50;

	if (block >= 12 || subtile >= 50 || block < 0 || subtile < 0) {
		Output::Warning("Tilemap index out of range: {} {}", block, subtile);
		return;
	}

	if (autotiles_d[block][subtile].valid)
		return;

	uint8_t quarters[2][2][2];

	// Get Block chipset coords
	short block_x, block_y;
	if (block < 4) {
		// If from first column
		block_x = (block % 2) * 3;
		block_y = 8 + (block / 2) * 4;
	} else {
		// If from second column
		block_x = 6 + (block % 2) * 3;
		block_y = ((block - 4) / 2) * 4;
	}

	// Calculate D block subtiles
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			// Get the block D subtiles ids and get their coordinates on the chipset
			quarters[j][i][0] = block_x + BlockD_Subtiles_IDS[subtile][j][i][0];
			quarters[j][i][1] = block_y + BlockD_Subtiles_IDS[subtile][j][i][1];
		}
	}

	// pack the quarters data into a word
	uint32_t quarters_hash = 0;
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++)
			for (int k = 0; k < 2; k++) {
				quarters_hash <<= 4;//multiply 16
				quarters_hash |= quarters[j][i][k];
			}

	// check whether we have already generated this tile
	auto it = autotiles_d_map.find(quarters_hash);
	if (it != autotiles_d_map.end()) {
		autotiles_d[block][subtile] = it->second;
		return;
	}

	int id = autotiles_d_next++;
	int dst_x = id % TILES_PER_ROW;
	int dst_y = id / TILES_PER_ROW;

	TileXY tile_xy(dst_x, dst_y);
	autotiles_d_map[quarters_hash] = tile_xy;
	autotiles_d[block][subtile] = tile_xy;
}

BitmapRef TilemapLayer::GenerateAutotiles(int count, const std::unordered_map<uint32_t, TileXY>& map) {
	int rows = (count + TILES_PER_ROW - 1) / TILES_PER_ROW;
	BitmapRef tiles = Bitmap::Create(TILES_PER_ROW * TILE_SIZE, rows * TILE_SIZE);
	tiles->Clear();
	Rect rect(0, 0, TILE_SIZE/2, TILE_SIZE/2);

	for (auto& p: map) {
		uint32_t quarters_hash = p.first;
		TileXY dst = p.second;

		// unpack the quarters data
		for (int j = 0; j < 2; j++) {
			for (int i = 0; i < 2; i++) {
				constexpr int mask = ~(0xFu << 28);

				int x = quarters_hash >> 28;
				quarters_hash &= mask;
				quarters_hash <<= 4;

				int y = quarters_hash >> 28;
				quarters_hash &= mask;
				quarters_hash <<= 4;

				rect.x = (x * 2 + i) * (TILE_SIZE/2);
				rect.y = (y * 2 + j) * (TILE_SIZE/2);

				tiles->BlitFast((dst.x * 2 + i) * (TILE_SIZE / 2), (dst.y * 2 + j) * (TILE_SIZE / 2), *chipset, rect, 255);
			}
		}
	}

	if (rows > 0) {
		tiles->CheckPixels(Bitmap::Flag_Chipset | Bitmap::Flag_ReadOnly);
	}

	return tiles;
}

namespace {
int ManiacTileType(int tile) {
	if (tile < 0) return -1;
	if (tile < 3000) return tile / 1000;
	if (tile < 4000) return 3 + (tile - 3000) / 50;
	if (tile < 5000) return 6 + (tile - 4000) / 50;
	return tile - 4982;
}

int ManiacTileBase(int type) {
	if (type < 3) return type * 1000;
	if (type < 6) return (type + 57) * 50;
	if (type < 18) return (type + 74) * 50;
	return type + 4982;
}

int ManiacAutotileLink(const Bitmap& chipset) {
	// 250921's tileset loader (0x7c270) compares each D block with this reference tile.
	if (chipset.GetWidth() < 480 || chipset.GetHeight() < 256) return -1;
	for (int type = 6; type < 18; ++type) {
		const int block = type - 6;
		const int sx = (block < 4 ? (block % 2) * 3 : 6 + (block % 2) * 3) * 16;
		const int sy = (block < 4 ? 8 + (block / 2) * 4 : ((block - 4) / 2) * 4) * 16;
		bool equal = true;
		for (int y = 0; y < 16 && equal; ++y) {
			for (int x = 0; x < 16; ++x) {
				if (chipset.GetColorAt(sx + x, sy + y) != chipset.GetColorAt(16 + x, 128 + y)) {
					equal = false;
					break;
				}
			}
		}
		if (equal) return type;
	}
	return -1;
}

template<typename T>
void UpdateManiacAutotile(std::vector<T>& tiles, int width, int height, int x, int y, uint8_t mask, int link,
	bool loop_horizontal = false, bool loop_vertical = false) {
	// Original mask/shape tables at 0x2251b8 / 0x214e20. The rectangle is built row by row.
	static constexpr uint8_t old_masks[50] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
		25, 27, 29, 31, 35, 39, 43, 47, 70, 78, 71, 79, 140, 141, 142, 143,
		95, 175, 59, 63, 103, 111, 206, 207, 157, 159, 127, 191, 223, 239, 255, 0,
		0, 0,
	};
	static constexpr uint8_t shapes[256] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
		16, 16, 17, 17, 18, 18, 19, 19, 16, 16, 17, 17, 18, 18, 19, 19,
		20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22, 22, 23, 23, 23, 23,
		34, 34, 34, 34, 35, 35, 35, 35, 34, 34, 34, 34, 35, 35, 35, 35,
		24, 26, 24, 26, 24, 26, 24, 26, 25, 27, 25, 27, 25, 27, 25, 27,
		32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
		36, 36, 36, 36, 36, 36, 36, 36, 37, 37, 37, 37, 37, 37, 37, 37,
		42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
		28, 29, 30, 31, 28, 29, 30, 31, 28, 29, 30, 31, 28, 29, 30, 31,
		40, 40, 41, 41, 40, 40, 41, 41, 40, 40, 41, 41, 40, 40, 41, 41,
		33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
		43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43,
		38, 39, 38, 39, 38, 39, 38, 39, 38, 39, 38, 39, 38, 39, 38, 39,
		44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44,
		45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45,
		46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46,
	};
	static constexpr int dx[8] = {-1, 1, 1, -1, -1, 0, 1, 0};
	static constexpr int dy[8] = {-1, -1, 1, 1, 0, -1, 0, 1};
	if (loop_horizontal) x = Utils::PositiveModulo(x, width);
	if (loop_vertical) y = Utils::PositiveModulo(y, height);
	if (x < 0 || y < 0 || x >= width || y >= height) return;
	auto& tile = tiles[static_cast<size_t>(y) * width + x];
	if (tile < 0 || tile >= 5000) return;
	const int type = ManiacTileType(tile);
	const int old_mask = tile % 50 == 49 ? 0 : old_masks[tile % 50];
	int neighbors[8];
	int new_mask = 0;
	for (int i = 0; i < 8; ++i) {
		int nx = x + dx[i], ny = y + dy[i];
		if (loop_horizontal) nx = Utils::PositiveModulo(nx, width);
		if (loop_vertical) ny = Utils::PositiveModulo(ny, height);
		const int other = nx < 0 || ny < 0 || nx >= width || ny >= height
			? -1 : ManiacTileType(tiles[static_cast<size_t>(ny) * width + nx]);
		neighbors[i] = other;
		const int mapped = other < 0 || other >= 18 ? -1 : other < 6 ? 0 : link;
		if (other != -1 && type != other && type != mapped && (type >= 6 || other >= 6))
			new_mask |= 1 << i;
	}
	const int merged = (new_mask & mask) | (old_mask & ~mask);
	int corners = 0;
	if (type < 3) {
		static constexpr int corner[4] = {0, 1, 3, 2};
		static constexpr int side_a[4] = {4, 5, 4, 7};
		static constexpr int side_b[4] = {5, 6, 7, 6};
		for (int i = 0; i < 4; ++i) {
			const bool water_corner = type < 2
				? neighbors[side_a[i]] == 2 && neighbors[side_b[i]] == 2 && !(merged & (1 << corner[i]))
				: neighbors[corner[i]] < 2 && neighbors[side_a[i]] < 2 && neighbors[side_b[i]] < 2;
			if (water_corner) corners |= 1 << i;
		}
	}
	tile = ManiacTileBase(type) + shapes[merged] + 50 * corners;
}

void DrawManiacChipsetTile(Bitmap& dst, const Bitmap& chipset, int x, int y, int tile, int time, int animation_type, int animation_speed) {
	if (tile < 0) return;
	const auto blit = [&](int dx, int dy, Rect source) {
		for (int iy = 0; iy < source.height; ++iy) for (int ix = 0; ix < source.width; ++ix) {
			const auto pixel = chipset.GetManiacColorAt(source.x + ix, source.y + iy);
			if (pixel.alpha) dst.SetManiacColorAt(dx + ix, dy + iy, pixel);
		}
	};
	if (tile < 3000) {
		const int block = tile / 1000;
		const int corners = (tile % 1000) / 50;
		const int shape = tile % 50;
		if (shape >= 47) return;
		int phase = time / (animation_speed == 0 ? 24 : 12);
		phase = animation_type == 0 ? phase & 3 : phase % 3;
		if (phase == 3) phase = 1;
		for (int row = 0; row < 2; ++row) {
			for (int col = 0; col < 2; ++col) {
				int bit = (corners >> (row * 2 + col)) & 1;
				int cx = block == 1 ? 6 : 0;
				int cy = BlockA_Subtiles_IDS[shape][row][col];
				if (cy < 0) {
					cx = 0;
					cy = 4 + (block == 2 ? bit ^ 3 : bit);
				}
				if (corners && shape && bit) {
					cx = 0;
					cy = 4 + (block == 2 ? bit * 2 : bit);
				}
				cx = static_cast<uint8_t>(cx + 2 * phase + col);
				cy = cy * 2 + row;
				blit(x + 8 * col, y + 8 * row, Rect(cx * 8, cy * 8, 8, 8));
			}
		}
	} else if (tile < 4000) {
		const int phase = (time / 6) & 3;
		blit(x, y, Rect((3 + (tile - 3000) / 50) * 16, (4 + phase) * 16, 16, 16));
	} else if (tile < 5000) {
		const int block = (tile - 4000) / 50;
		const int shape = tile % 50;
		const int bx = block < 4 ? (block % 2) * 3 : 6 + (block % 2) * 3;
		const int by = block < 4 ? 8 + (block / 2) * 4 : ((block - 4) / 2) * 4;
		for (int row = 0; row < 2; ++row) {
			for (int col = 0; col < 2; ++col) {
				const auto& q = BlockD_Subtiles_IDS[shape][row][col];
				const Rect src((bx + q[0]) * 16 + col * 8, (by + q[1]) * 16 + row * 8, 8, 8);
				blit(x + col * 8, y + row * 8, src);
			}
		}
	} else {
		const bool upper = tile >= 10000;
		const int index = tile - (upper ? 10000 : 5000);
		if (index < 0 || index >= 144) return;
		int col, row;
		if (upper) {
			col = index < 48 ? 18 + index % 6 : 24 + (index - 48) % 6;
			row = index < 48 ? 8 + index / 6 : (index - 48) / 6;
		} else {
			col = index < 96 ? 12 + index % 6 : 18 + (index - 96) % 6;
			row = index < 96 ? index / 6 : (index - 96) / 6;
		}
		blit(x, y, Rect(col * 16, row * 16, 16, 16));
	}
}
} // namespace

void TilemapLayer::DrawManiacTileRectangle(Bitmap& dst, const Bitmap& chipset, Rect rectangle,
	std::vector<int32_t> source, bool upper, int options, int time, int animation_type, int animation_speed) {
	static constexpr int lower_tiles[18] = {
		0, 1000, 2000, 3000, 3074, 3100, 4000, 4050, 4100,
		4150, 4200, 4250, 4300, 4350, 4400, 4450, 4500, 4550
	};
	const bool autotile = !upper && !(options & 1);
	const int link = autotile ? ManiacAutotileLink(chipset) : -1;
	std::vector<int32_t> tiles(source.size(), 0);
	for (int y = 0; y < rectangle.height; ++y) {
		for (int x = 0; x < rectangle.width; ++x) {
			const size_t index = static_cast<size_t>(y) * rectangle.width + x;
			const uint32_t value = source[index];
			tiles[index] = upper ? (value < 144 ? 10000 + value : 10000)
				: value < 18 ? lower_tiles[value] : value < 162 ? value + 4982 : 5143;
			if (autotile) {
				UpdateManiacAutotile(tiles, rectangle.width, rectangle.height, x, y, 255, link);
				static constexpr int dx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
				static constexpr int dy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
				static constexpr uint8_t mask[8] = {4, 140, 8, 25, 1, 35, 2, 70};
				for (int i = 0; i < 8; ++i)
					UpdateManiacAutotile(tiles, rectangle.width, rectangle.height, x + dx[i], y + dy[i], mask[i], link);
			}
		}
	}
	for (int y = 0; y < rectangle.height; ++y) {
		for (int x = 0; x < rectangle.width; ++x) {
			DrawManiacChipsetTile(dst, chipset, rectangle.x + x * 16, rectangle.y + y * 16,
				tiles[static_cast<size_t>(y) * rectangle.width + x], time, animation_type, animation_speed);
		}
	}
}

void TilemapLayer::SetChipset(BitmapRef const& nchipset) {
	chipset = nchipset;
	chipset_effect = Bitmap::Create(chipset->width(), chipset->height());
	chipset_tone_tiles.clear();

	if (autotiles_ab_next != 0 && autotiles_d_screen != nullptr && layer == 0) {
		autotiles_ab_screen = GenerateAutotiles(autotiles_ab_next, autotiles_ab_map);
		autotiles_d_screen = GenerateAutotiles(autotiles_d_next, autotiles_d_map);

		autotiles_ab_screen_effect = Bitmap::Create(autotiles_ab_screen->width(), autotiles_ab_screen->height());
		autotiles_d_screen_effect = Bitmap::Create(autotiles_d_screen->width(), autotiles_d_screen->height());
	}
}

void TilemapLayer::SetMapData(std::vector<short> nmap_data) {
	// Create the tiles data cache
	CreateTileCache(nmap_data);
	memset(autotiles_ab, 0, sizeof(autotiles_ab));
	memset(autotiles_d, 0, sizeof(autotiles_d));

	if (layer == 0) {
		autotiles_ab_map.clear();
		autotiles_d_map.clear();
		autotiles_ab_next = 0;
		autotiles_d_next = 0;
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {

				if (GetDataCache(x, y).ID < BLOCK_C) {
					// If blocks A and B

					GenerateAutotileAB(GetDataCache(x, y).ID, 0);
					GenerateAutotileAB(GetDataCache(x, y).ID, 1);
					GenerateAutotileAB(GetDataCache(x, y).ID, 2);
				} else if (GetDataCache(x, y).ID >= BLOCK_D && GetDataCache(x, y).ID < BLOCK_E) {
					// If block D

					GenerateAutotileD(GetDataCache(x, y).ID);
				}
			}
		}
		autotiles_ab_screen = GenerateAutotiles(autotiles_ab_next, autotiles_ab_map);
		autotiles_d_screen = GenerateAutotiles(autotiles_d_next, autotiles_d_map);

		autotiles_ab_screen_effect = Bitmap::Create(autotiles_ab_screen->width(), autotiles_ab_screen->height());
		autotiles_d_screen_effect = Bitmap::Create(autotiles_d_screen->width(), autotiles_d_screen->height());

		chipset_tone_tiles.clear();
	}

	map_data = std::move(nmap_data);
}

static inline bool IsTileFromBlock(int tile_id, int block) {
	switch (block) {
	case BLOCK_A: return tile_id >= BLOCK_A && tile_id < BLOCK_A_END;
	case BLOCK_B: return tile_id >= BLOCK_B && tile_id < BLOCK_B_END;
	case BLOCK_C: return tile_id >= BLOCK_C && tile_id < BLOCK_C_END;
	case BLOCK_D: return tile_id >= BLOCK_D && tile_id < BLOCK_D_END;
	case BLOCK_E: return tile_id >= BLOCK_E && tile_id < BLOCK_E_END;
	case BLOCK_F: return tile_id >= BLOCK_F && tile_id < BLOCK_F_END;
	default: return false;
	}
}

void TilemapLayer::SetMapTileDataAt(int x, int y, int tile_id, bool disable_autotile) {
	if(!IsInMapBounds(x, y))
		return;

	substitutions = Game_Map::GetTilesLayer(layer);

	bool is_autotile = IsTileFromBlock(tile_id, BLOCK_A) || IsTileFromBlock(tile_id, BLOCK_B) || IsTileFromBlock(tile_id, BLOCK_D);
	const bool maniac = Player::IsPatchManiac();

	if (maniac && !disable_autotile) {
		const bool loop_horizontal = Game_Map::LoopHorizontal();
		const bool loop_vertical = Game_Map::LoopVertical();
		const int link = chipset ? ManiacAutotileLink(*chipset) : -1;
		map_data[static_cast<size_t>(y) * width + x] = static_cast<short>(tile_id);
		UpdateManiacAutotile(map_data, width, height, x, y, 255, link, loop_horizontal, loop_vertical);
		RecreateTileDataAt(x, y, map_data[static_cast<size_t>(y) * width + x]);
		// Native updates only the affected mask bits of each existing neighbor.
		static constexpr int dx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
		static constexpr int dy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
		static constexpr uint8_t mask[8] = {4, 140, 8, 25, 1, 35, 2, 70};
		for (int i = 0; i < 8; ++i) {
			int nx = x + dx[i], ny = y + dy[i];
			if (loop_horizontal) nx = Utils::PositiveModulo(nx, width);
			if (loop_vertical) ny = Utils::PositiveModulo(ny, height);
			if (!IsInMapBounds(nx, ny)) continue;
			UpdateManiacAutotile(map_data, width, height, nx, ny, mask[i], link, loop_horizontal, loop_vertical);
			RecreateTileDataAt(nx, ny, map_data[static_cast<size_t>(ny) * width + nx]);
		}
	} else if (disable_autotile || !is_autotile) {
		RecreateTileDataAt(x, y, tile_id);
	} else {
		// Recalculate the replaced tile itself + every neighboring tile
		static constexpr struct { int dx; int dy; } adjacent[8] = {
				{-1, -1}, { 0, -1}, { 1, -1},
				{-1,  0}, { 1,  0},
				{-1,  1}, { 0,  1}, { 1,  1}
		};

		RecalculateAutotile(x, y, tile_id);

		for (const auto& adj : adjacent) {
			auto nx = x + adj.dx;
			auto ny = y + adj.dy;
			if (IsInMapBounds(nx, ny)) {
				RecalculateAutotile(nx, ny, GetDataCache(nx, ny).ID);
			}
		}
	}

	SetMapData(map_data);
}

static inline bool IsAutotileD(int tile_id) {
	return tile_id >= BLOCK_D && tile_id < BLOCK_E;
}

static inline bool IsSameAutotileAB(int current_tile_id, int neighbor_tile_id) {
	// Special case for water tiles - allow mixing of A and B blocks
	bool current_is_water = (IsTileFromBlock(current_tile_id, BLOCK_A) ||
		IsTileFromBlock(current_tile_id, BLOCK_B));
	bool neighbor_is_water = (IsTileFromBlock(neighbor_tile_id, BLOCK_A) ||
		IsTileFromBlock(neighbor_tile_id, BLOCK_B));

	if (current_is_water && neighbor_is_water) {
		return true;
	}

	// For non-water tiles, keep original behavior of requiring same block
	if (IsTileFromBlock(current_tile_id, BLOCK_A) && IsTileFromBlock(neighbor_tile_id, BLOCK_A)) {
		return true;
	}
	if (IsTileFromBlock(current_tile_id, BLOCK_B) && IsTileFromBlock(neighbor_tile_id, BLOCK_B)) {
		return true;
	}
	return false;
}

static inline bool IsSameAutotileD(int current_tile_id, int neighbor_tile_id) {
	return ChipIdToIndex(current_tile_id) == ChipIdToIndex(neighbor_tile_id);
}

static inline void ApplyCornerFixups(uint8_t& neighbors) {
	// Northwest corner
	if ((neighbors & NEIGHBOR_NW) && (neighbors & (NEIGHBOR_N | NEIGHBOR_W)) != (NEIGHBOR_N | NEIGHBOR_W)) {
		neighbors &= ~NEIGHBOR_NW;
	}

	// Northeast corner
	if ((neighbors & NEIGHBOR_NE) && (neighbors & (NEIGHBOR_N | NEIGHBOR_E)) != (NEIGHBOR_N | NEIGHBOR_E)) {
		neighbors &= ~NEIGHBOR_NE;
	}

	// Southwest corner
	if ((neighbors & NEIGHBOR_SW) && (neighbors & (NEIGHBOR_S | NEIGHBOR_W)) != (NEIGHBOR_S | NEIGHBOR_W)) {
		neighbors &= ~NEIGHBOR_SW;
	}

	// Southeast corner
	if ((neighbors & NEIGHBOR_SE) && (neighbors & (NEIGHBOR_S | NEIGHBOR_E)) != (NEIGHBOR_S | NEIGHBOR_E)) {
		neighbors &= ~NEIGHBOR_SE;
	}
}

void TilemapLayer::RecalculateAutotile(int x, int y, int tile_id) {
	static constexpr struct { int dx; int dy; uint8_t bit; } adjacent[8] = {
		{-1, -1, NEIGHBOR_NW}, { 0, -1, NEIGHBOR_N}, { 1, -1, NEIGHBOR_NE},
		{-1,  0, NEIGHBOR_W }, { 1,  0, NEIGHBOR_E},
		{-1,  1, NEIGHBOR_SW}, { 0,  1, NEIGHBOR_S}, { 1,  1, NEIGHBOR_SE}
	};

	auto calculateNeighbors = [&](auto isSameAutotileFn) {
		uint8_t neighbors = 0;
		for (const auto& adj : adjacent) {
			auto nx = x + adj.dx;
			auto ny = y + adj.dy;
			auto adj_tile_id = IsInMapBounds(nx, ny) ? GetDataCache(nx, ny).ID : tile_id;
			if (isSameAutotileFn(tile_id, adj_tile_id)) {
				neighbors |= adj.bit;
			}
		}
		ApplyCornerFixups(neighbors);
		return neighbors;
		};

	auto processBlock = [&](int /*blockType*/, int blockStride, int blockBase, auto isSameAutotileFn) {
		uint8_t neighbors = calculateNeighbors(isSameAutotileFn);
		int block = (tile_id - blockBase) / blockStride;
		int variant = AUTOTILE_D_VARIANTS_MAP.at(neighbors);
		int new_tile_id = blockBase + (block * blockStride) + variant;
		RecreateTileDataAt(x, y, new_tile_id);
		};

	if (IsTileFromBlock(tile_id, BLOCK_A)) {
		processBlock(BLOCK_A, BLOCK_A_STRIDE, BLOCK_A, IsSameAutotileAB);
	}
	if (IsTileFromBlock(tile_id, BLOCK_B)) {
		processBlock(BLOCK_B, BLOCK_B_STRIDE, BLOCK_B, IsSameAutotileAB);
	}
	if (IsTileFromBlock(tile_id, BLOCK_D)) {
		processBlock(BLOCK_D, BLOCK_D_STRIDE, BLOCK_D, IsSameAutotileD);
	}
}
void TilemapLayer::SetPassable(std::vector<unsigned char> npassable) {
	passable = std::move(npassable);

	// Recalculate z values of all tiles
	CreateTileCache(map_data);
}

void TilemapLayer::OnSubstitute() {
	substitutions = Game_Map::GetTilesLayer(layer);

	// Recalculate z values of all tiles
	CreateTileCache(map_data);
}

TilemapSubLayer::TilemapSubLayer(TilemapLayer* tilemap, Drawable::Z_t z) :
	Drawable(z),
	tilemap(tilemap),
	internal_z(static_cast<uint8_t>(z))
{
	DrawableMgr::Register(this);
}

void TilemapSubLayer::Draw(Bitmap& dst) {
	if (!tilemap->GetChipset()) {
		return;
	}

	tilemap->Draw(dst, internal_z, GetRenderOx(), GetRenderOy());
}

void TilemapLayer::SetTone(Tone tone) {
	if (tone == this->tone) {
		return;
	}

	this->tone = tone;

	if (autotiles_d_screen_effect) {
		autotiles_d_screen_effect->Clear();
	}
	if (autotiles_ab_screen_effect) {
		autotiles_ab_screen_effect->Clear();
	}
	if (chipset_effect) {
		chipset_effect->Clear();
	}
	chipset_tone_tiles.clear();
}
