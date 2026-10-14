/* This file is part of EasyRPG Player. GPL-3.0-or-later. */
#ifndef EP_MANIAC_PICTURE_RENDERER_H
#define EP_MANIAC_PICTURE_RENDERER_H

#include "bitmap.h"
#include <lcf/rpg/savepicture.h>
#include <lcf/rpg/savescreen.h>

/** Maniac 250921 picture contracts; see docs/research/maniac-250921/REPORT.md section 7. */
namespace ManiacPicture {
int Truncate(double value);
int Nearest(double value);
BitmapRef Prepare(const Bitmap& source, Rect frame, const lcf::rpg::SavePicture& picture,
	const lcf::rpg::SaveScreen& screen, bool chromakey);
void Draw(Bitmap& destination, const Bitmap& prepared, const lcf::rpg::SavePicture& picture, int x, int y, bool chromakey);
}
#endif
