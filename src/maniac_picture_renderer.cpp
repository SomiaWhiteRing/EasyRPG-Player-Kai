/* This file is part of EasyRPG Player. GPL-3.0-or-later. */
#include "maniac_picture_renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ManiacPicture {

int Truncate(double value) {
	if (!(value >= INT32_MIN && value < static_cast<double>(INT32_MAX) + 1.0)) return INT32_MIN;
	return static_cast<int>(value);
}

int Nearest(double value) { return Truncate(std::nearbyint(value)); }

std::optional<PrepareState> DescribeFrame(const Bitmap& source, const lcf::rpg::SavePicture& picture,
		const lcf::rpg::SaveScreen& screen, bool chromakey) {
	const int cols = picture.spritesheet_cols, rows = picture.spritesheet_rows;
	const auto index = static_cast<uint32_t>(picture.spritesheet_frame);
	if (cols <= 0 || rows <= 0 || index >= uint32_t(cols) * uint32_t(rows)) return {};
	const int width = source.GetWidth() / cols, height = source.GetHeight() / rows;
	if (width <= 0 || height <= 0) return {};
	const auto component = [&](double value, double tint) {
		return static_cast<int>(std::clamp(static_cast<int64_t>(Nearest(value))
			+ (picture.flags.affected_by_tint ? Truncate(std::round(tint)) : 100) - 100, int64_t{0}, int64_t{200}));
	};
	return PrepareState{width * int(index % cols), height * int(index / cols), width, height,
		component(picture.current_red, screen.tint_current_red), component(picture.current_green, screen.tint_current_green),
		component(picture.current_blue, screen.tint_current_blue), component(picture.current_sat, screen.tint_current_sat),
		picture.flags.affected_by_flash ? screen.flash_red : 0, picture.flags.affected_by_flash ? screen.flash_green : 0,
		picture.flags.affected_by_flash ? screen.flash_blue : 0,
		picture.flags.affected_by_flash ? Truncate(std::round(screen.flash_current_level)) : 0,
		static_cast<int>((static_cast<uint32_t>(picture.finish_bot_trans) >> 12) & 15), chromakey};
}

namespace {
int Round(double value) { return Truncate(std::round(value)); }

// Native 017400 clips source and destination in integer arithmetic. The
// original extents remain the accumulators' factors after clipping.
struct Axis {
	int source = 0, size = 0, destination = 0, length = 0;
	int original_source = 0, original_size = 0, original_destination = 0, original_length = 0;
	bool reversed = false;

	Axis(int src, int count, int first, int last)
		: source(src), size(count), destination(std::min(first, last)), length(std::abs(last - first)),
		original_source(src), original_size(count), original_destination(destination), original_length(length), reversed(last < first) {}

	bool Clip(int source_limit, int destination_limit) {
		if (!size || !length) return false;
		if (source < 0 && source + size > 0) {
			const int remaining = source + size;
			length = static_cast<int>(static_cast<int64_t>(length) * remaining / size);
			if (!remaining) return false;
			destination -= static_cast<int>(static_cast<int64_t>(length) * source / remaining);
			size = remaining; source = 0;
		}
		if (source + size > source_limit) {
			length = std::max(1, static_cast<int>(static_cast<int64_t>(length) * (source_limit - source) / size));
			size = source_limit - source;
		}
		if (destination < 0 && length + destination > 0) {
			const int remaining = length + destination;
			size = static_cast<int>(static_cast<int64_t>(remaining) * size / length);
			source -= static_cast<int>(static_cast<int64_t>(size) * destination / remaining);
			length = remaining; destination = 0;
		}
		if (destination + length > destination_limit) {
			size = std::max(1, static_cast<int>(static_cast<int64_t>(destination_limit - destination) * size / length));
			length = destination_limit - destination;
		}
		return size > 0 && length > 0 && destination >= 0 && source >= 0;
	}

	int Sample(int index) const {
		const int offset = reversed ? length - original_destination - 1 + destination - index
			: destination - original_destination + index;
		return original_source + static_cast<int>(static_cast<int64_t>(original_size) * offset / original_length);
	}
};

void Composite(Bitmap& destination, int x, int y, Color source, int blend, int opacity, bool key, bool reflect_x) {
	if (key && !source.alpha) return;
	if (reflect_x) x = destination.GetWidth() - 1 - x;
	destination.CompositeManiacPixel(x, y, source, blend, opacity);
}

void Scaled(Bitmap& destination, const Bitmap& source, int x, int y, double percent_x, double percent_y,
	int blend, int opacity, bool key, bool reflect_x) {
	const int width = source.GetWidth(), height = source.GetHeight();
	const double sx = percent_x * 0.01, sy = percent_y * 0.01;
	int left, right, top, bottom;
	if (Truncate(percent_x) == 100) { left = x - width / 2; right = left + width; }
	else if (sx >= 0) { left = Round(x - (width / 2) * sx); right = left + Truncate(width * sx); }
	else { right = Round(x + (width / 2) * sx); left = right - Truncate(width * sx); }
	if (Truncate(percent_y) == 100) { top = y - height / 2; bottom = top + height; }
	else if (sy >= 0) { top = Round(y - (height / 2) * sy); bottom = top + Truncate(height * sy); }
	else { bottom = Round(y + (height / 2) * sy); top = bottom - Truncate(height * sy); }
	Axis horizontal(0, width, left, right), vertical(0, height, top, bottom);
	if (!horizontal.Clip(width, destination.GetWidth()) || !vertical.Clip(height, destination.GetHeight())) return;
	// Clipping fixes the horizontal sample sequence for the whole rectangle.
	std::vector<int> columns(horizontal.length);
	for (int ix = 0; ix < horizontal.length; ++ix) columns[ix] = horizontal.Sample(ix);
	for (int iy = 0; iy < vertical.length; ++iy) {
		const int py = vertical.Sample(iy);
		if (static_cast<unsigned>(py) >= static_cast<unsigned>(height)) continue;
		destination.CompositeManiacRow(horizontal.destination, vertical.destination + iy, source, py,
			MakeSpan(columns), blend, opacity, key, reflect_x);
	}
}

void Rotated(Bitmap& destination, const Bitmap& source, int x, int y, double sx, double sy, double degrees,
	int blend, int opacity, bool key, bool reflect_x) {
	if (sx == 0.0 || sy == 0.0) return;
	const int width = source.GetWidth(), height = source.GetHeight();
	const double radians = degrees * 3.141592653589793 / 180.0;
	const double sine = std::sin(radians), cosine = std::cos(radians);
	const int scaled_width = Round(width * sx), scaled_height = Round(height * sy);
	const int extent_x = Round(std::abs(scaled_width * cosine) + std::abs(scaled_height * sine));
	const int extent_y = Round(std::abs(scaled_width * sine) + std::abs(scaled_height * cosine));
	const int half_x = Round((width / 2) * sx), half_y = Round((height / 2) * sx);
	// Native odd-height branch also uses the width scale for half_y.
	const int offset_x = (width & 1) && sx > 0 ? Round(std::abs(half_y * sine) + std::abs(half_x * cosine)) : extent_x / 2;
	const int offset_y = (height & 1) && sy > 0 ? Round(std::abs(half_x * sine) + std::abs(half_y * cosine)) : extent_y / 2;
	const int left = std::max(0, x - offset_x), top = std::max(0, y - offset_y);
	const int right = std::min(destination.GetWidth(), x + 1 + extent_x - offset_x);
	const int bottom = std::min(destination.GetHeight(), y + 1 + extent_y - offset_y);
	const double du = cosine / sx, dv = sine / sy, row_u = sine / sx, row_v = cosine / sy;
	double u0 = (du * (left - x) - row_u * (top - y) + width / 2) - 0.1;
	double v0 = (dv * (left - x) + row_v * (top - y) + height / 2) - 0.1;
	for (int iy = top; iy < bottom; ++iy) {
		double u = u0, v = v0;
		for (int ix = left; ix < right; ++ix) {
			const int px = Nearest(u), py = Nearest(v);
			if (static_cast<unsigned>(px) < static_cast<unsigned>(width) && static_cast<unsigned>(py) < static_cast<unsigned>(height))
				Composite(destination, ix, iy, source.GetManiacColorAt(px, py), blend, opacity, key, reflect_x);
			u += du; v += dv;
		}
		u0 -= row_u; v0 += row_v;
	}
}

void Wave(Bitmap& destination, const Bitmap& source, int x, int y, double sx, double sy, int amplitude, int phase,
	int blend, int opacity, bool key, bool reflect_x) {
	const int width = source.GetWidth(), height = source.GetHeight();
	const int half_x = Truncate((width / 2) * sx), half_y = Truncate((height / 2) * sy);
	const int length = Truncate(width * sx), rows = Truncate(height * sy);
	if (half_x <= 0 || half_y <= 0 || length <= 0 || rows <= 0) return;
	Axis vertical(0, height, y - half_y, y - half_y + rows);
	if (!vertical.Clip(height, destination.GetHeight())) return;
	static const auto table = [] {
		std::array<double, 256> values{};
		for (int i = 0; i < 256; ++i) values[i] = std::sin(static_cast<double>(i) * 3.14159265358979 * 0.0078125);
		return values;
	}();
	for (int row = 0; row < vertical.length; ++row) {
		const int index = phase + static_cast<int>((static_cast<int64_t>(row) * 256 * vertical.size / vertical.length) / 32);
		const int displacement = Truncate(table[static_cast<uint8_t>(index)] * amplitude);
		const int left = x - half_x + displacement;
		const int start = std::max(0, -left), end = std::min(length, destination.GetWidth() - left);
		const int py = std::min(vertical.source + vertical.size - 1,
			static_cast<int>(static_cast<int64_t>(vertical.destination - (y - half_y) + row) * height / (2 * half_y)));
		for (int column = start; column < end; ++column) {
			const int px = std::min(width - 1, static_cast<int>(static_cast<int64_t>(column) * width / (2 * half_x)));
			Composite(destination, left + column, vertical.destination + row, source.GetManiacColorAt(px, py), blend, opacity, key, reflect_x);
		}
	}
}
}

BitmapRef Prepare(const BitmapRef& source, const PrepareState& state, BitmapRef reuse, bool allow_view) {
	const Rect frame(state[0], state[1], state[2], state[3]);
	if (frame.width <= 0 || frame.height <= 0) return {};
	const std::array<int, 4> tone = {state[4], state[5], state[6], state[7]};
	const int flip = state[12];
	const bool sheet = frame.width != source->GetWidth() || frame.height != source->GetHeight();
	bool horizontal = false, vertical = false;
	if (flip == 1) horizontal = frame.width > 1;
	else if (flip == 2) vertical = frame.height >= (sheet ? 2 : 3);
	else if (flip == 3) {
		horizontal = frame.width > 1;
		vertical = frame.height > 1 && (horizontal || sheet || frame.height >= 3);
	}
	const int flash = state[11] * 8;
	if (allow_view && !horizontal && !vertical && tone == std::array<int, 4>{100, 100, 100, 100} && flash <= 0)
		return Bitmap::CreateReadOnlyView(source, frame);
	if (!reuse || reuse->IsReadOnly() || reuse->GetWidth() != frame.width || reuse->GetHeight() != frame.height)
		reuse = Bitmap::Create(frame.width, frame.height, true);
	reuse->PrepareManiacFrame(*source, frame, tone, horizontal, vertical, flash,
		{state[8] * 8, state[9] * 8, state[10] * 8}, state[13] != 0);
	return reuse;
}

void Draw(Bitmap& destination, const Bitmap& prepared, const lcf::rpg::SavePicture& picture,
		int x, int y, bool chromakey, bool reflect_x) {
	if (picture.current_top_trans == 100.0) return;
	const int opacity = Nearest(255.0 - picture.current_top_trans * 255.0 * 0.01);
	const int blend = (static_cast<uint32_t>(picture.finish_bot_trans) >> 8) & 3;
	const double sx = picture.current_magnify * 0.01, sy = picture.maniac_current_magnify_height * 0.01;
	if (picture.effect_mode == 0 && picture.current_rotation == 0.0
			&& Truncate(picture.current_magnify) == 100 && Truncate(picture.maniac_current_magnify_height) == 100
			&& blend == 0 && opacity >= 255 && !reflect_x) {
		destination.CopyManiacPixels(x - prepared.GetWidth() / 2, y - prepared.GetHeight() / 2,
			prepared, prepared.GetRect(), chromakey, true);
	} else if (picture.effect_mode == 0 && picture.current_rotation == 0.0) Scaled(destination, prepared, x, y, picture.current_magnify, picture.maniac_current_magnify_height, blend, opacity, chromakey, reflect_x);
	else if (picture.effect_mode == 2) Wave(destination, prepared, x, y, sx, sy, Nearest(picture.current_effect_power * 4.0), picture.current_waver, blend, opacity, chromakey, reflect_x);
	else Rotated(destination, prepared, x, y, sx, sy, -picture.current_rotation * (picture.effect_mode == 1 ? 1.40625 : 1.0), blend, opacity, chromakey, reflect_x);
}
}
