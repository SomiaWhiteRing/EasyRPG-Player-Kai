/* This file is part of EasyRPG Player. GPL-3.0-or-later. */
#ifndef EP_MANIAC_PICTURE_RENDERER_H
#define EP_MANIAC_PICTURE_RENDERER_H

#include "bitmap.h"
#include <lcf/rpg/savepicture.h>
#include <lcf/rpg/savescreen.h>
#include <optional>

/** Maniac 250921 picture contracts; see docs/research/maniac-250921/REPORT.md section 7. */
namespace ManiacPicture {
int Truncate(double value);
int Nearest(double value);
// frame xywh, tone rgb/saturation, flash rgb/level, flip nibble, chromakey.
using PrepareState = std::array<int, 14>;
std::optional<PrepareState> DescribeFrame(const Bitmap& source, const lcf::rpg::SavePicture& picture,
	const lcf::rpg::SaveScreen& screen, bool chromakey);
BitmapRef Prepare(const BitmapRef& source, const PrepareState& state, BitmapRef reuse, bool allow_view);
void Draw(Bitmap& destination, const Bitmap& prepared, const lcf::rpg::SavePicture& picture, int x, int y, bool chromakey);
}
#endif
