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
#include <png.h>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <vector>
#include <fstream>
#include <array>

#include "output.h"
#include "image_png.h"
#include "player.h"

static void read_data(png_structp png_ptr, png_bytep data, png_size_t length) {
	png_bytep* bufp = (png_bytep*) png_get_io_ptr(png_ptr);
	memcpy(data, *bufp, length);
	*bufp += length;
}

static void read_data_istream(png_structp png_ptr, png_bytep data, png_size_t length) {
	auto* bufp = reinterpret_cast<Filesystem_Stream::InputStream*>(png_get_io_ptr(png_ptr));
	if (bufp != nullptr && *bufp) {
		bufp->read(reinterpret_cast<char*>(data), length);
	}
}

static void on_png_warning(png_structp, png_const_charp warn_msg) {
	Output::Debug("libpng: {}", warn_msg);
}

static void on_png_error(png_structp, png_const_charp error_msg) {
	Output::Warning("libpng: {}", error_msg);
}

struct ImagePNG::PictureLoader::Data {
	Filesystem_Stream::InputStream stream;
	png_structp png = nullptr;
	png_infop info = nullptr;
	BitmapRef bitmap;
	std::array<uint32_t, 256> palette{};
	std::array<Color, 256> straight_palette{};
	std::vector<uint8_t> row;
	int next_row = 0;
	int palette_size = 0;
	bool transparent = false;
	bool has_transparent = false;
	bool has_opaque = false;
	bool complete = false;
	bool failed = false;
	~Data() {
		if (png) {
			png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
		}
	}
};

ImagePNG::PictureLoader::PictureLoader() : data(std::make_unique<Data>()) {}
ImagePNG::PictureLoader::~PictureLoader() = default;

std::unique_ptr<ImagePNG::PictureLoader> ImagePNG::PictureLoader::Create(Filesystem_Stream::InputStream stream, bool transparent) {
	auto loader = std::unique_ptr<PictureLoader>(new PictureLoader());
	if (!loader->Init(std::move(stream), transparent)) {
		return {};
	}
	return loader;
}

bool ImagePNG::PictureLoader::Init(Filesystem_Stream::InputStream stream, bool transparent) {
	if (!stream || Bitmap::pixel_format.bits != 32) {
		return false;
	}
	uint8_t signature[8];
	stream.read(reinterpret_cast<char*>(signature), sizeof(signature));
	if (stream.gcount() != sizeof(signature) || png_sig_cmp(signature, 0, sizeof(signature))) {
		return false;
	}
	stream.seekg(0);
	data->stream = std::move(stream);
	data->transparent = transparent;
	data->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, on_png_error, on_png_warning);
	if (!data->png) {
		return false;
	}
	data->info = png_create_info_struct(data->png);
	if (!data->info) {
		return false;
	}
	if (setjmp(png_jmpbuf(data->png))) {
		return false;
	}
	png_set_read_fn(data->png, &data->stream, [](png_structp png, png_bytep bytes, png_size_t length) {
		auto& input = *static_cast<Filesystem_Stream::InputStream*>(png_get_io_ptr(png));
		input.read(reinterpret_cast<char*>(bytes), length);
		if (input.gcount() != static_cast<std::streamsize>(length)) {
			png_error(png, "Truncated picture preload");
		}
	});
	png_read_info(data->png, data->info);
	png_uint_32 width, height;
	int depth, type, interlace;
	png_get_IHDR(data->png, data->info, &width, &height, &depth, &type, &interlace, nullptr, nullptr);
	// Bound speculative memory and preserve the regular decoder for other PNG types.
	const auto pixels = static_cast<uint64_t>(width) * height;
	if (depth != 8 || type != PNG_COLOR_TYPE_PALETTE || interlace != PNG_INTERLACE_NONE
			|| pixels < 4 * 1024 * 1024 || pixels > 128 * 1024 * 1024 / 4) {
		return false;
	}
	png_colorp palette = nullptr;
	if (!png_get_PLTE(data->png, data->info, &palette, &data->palette_size)) {
		return false;
	}
	png_read_update_info(data->png, data->info);
	// Every pixel is written before publication; avoid clearing a whole sheet here.
	auto storage = std::unique_ptr<void, decltype(&std::free)>(std::malloc(pixels * 4), &std::free);
	if (!storage) {
		return false;
	}
	data->bitmap = Bitmap::Create(storage.get(), width, height, width * 4,
		transparent ? Bitmap::pixel_format : Bitmap::opaque_pixel_format);
	pixman_image_set_destroy_function(data->bitmap->bitmap.get(),
		[](pixman_image_t*, void* buffer) { std::free(buffer); }, storage.get());
	storage.release();
	data->bitmap->original_bpp = 8;
	if (Player::IsPatchManiac()) data->bitmap->maniac_pixels.resize(pixels);
	data->bitmap->SetId(ToString(data->stream.GetName()));
	data->row.resize(width);
	for (int i = 0; i < data->palette_size; ++i) {
		const auto& color = palette[i];
		data->straight_palette[i] = Color(color.red, color.green, color.blue, (transparent && i == 0) ? 0 : 255);
		// RPG pictures use palette index zero as the color key, ignoring PNG tRNS.
		data->palette[i] = (transparent && i == 0) ? 0
			: data->bitmap->format.rgba_to_uint32_t(color.red, color.green, color.blue, 255);
	}
	return true;
}

bool ImagePNG::PictureLoader::ReadRows(int count) {
	if (data->failed || data->complete) {
		return !data->failed;
	}
	if (setjmp(png_jmpbuf(data->png))) {
		data->failed = true;
		data->bitmap.reset();
		return false;
	}
	const int end = std::min(data->bitmap->GetHeight(), data->next_row + count);
	for (; data->next_row < end; ++data->next_row) {
		png_read_row(data->png, data->row.data(), nullptr);
		auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data->bitmap->pixels())
			+ static_cast<size_t>(data->next_row) * data->bitmap->pitch());
		for (size_t x = 0; x < data->row.size(); ++x) {
			auto index = data->row[x];
			if (index >= data->palette_size) {
				png_error(data->png, "Invalid picture palette index");
			}
			dst[x] = data->palette[index];
			if (!data->bitmap->maniac_pixels.empty()) data->bitmap->maniac_pixels[static_cast<size_t>(data->next_row) * data->row.size() + x] = data->straight_palette[index];
			bool transparent = data->transparent && index == 0;
			data->has_transparent |= transparent;
			data->has_opaque |= !transparent;
		}
	}
	if (data->next_row == data->bitmap->GetHeight()) {
		png_read_end(data->png, nullptr);
		data->bitmap->image_opacity = !data->has_opaque ? ImageOpacity::Transparent
			: data->has_transparent ? ImageOpacity::Alpha_1Bit : ImageOpacity::Opaque;
		data->bitmap->read_only = true;
		data->complete = true;
	}
	return true;
}

bool ImagePNG::PictureLoader::IsComplete() const { return data->complete; }
BitmapRef ImagePNG::PictureLoader::GetBitmap() const { return data->complete ? data->bitmap : BitmapRef{}; }

static bool ReadPNGWithReadFunction(png_voidp,png_rw_ptr, bool, ImageOut&);
static void ReadPalettedData(png_struct*, png_info*, png_uint_32, png_uint_32, bool, uint32_t*);
static void ReadGrayData(png_struct*, png_info*, png_uint_32, png_uint_32, bool, uint32_t*);
static void ReadGrayAlphaData(png_struct*, png_info*, png_uint_32, png_uint_32, uint32_t*);
static void ReadRGBData(png_struct*, png_info*, png_uint_32, png_uint_32, uint32_t*);
static void ReadRGBAData(png_struct*, png_info*, png_uint_32, png_uint_32, uint32_t*);

bool ImagePNG::Read(const void* buffer, bool transparent, ImageOut& output) {
	return ReadPNGWithReadFunction((png_voidp)&buffer, read_data, transparent, output);
}

bool ImagePNG::Read(Filesystem_Stream::InputStream& stream, bool transparent, ImageOut& output) {
	return ReadPNGWithReadFunction(&stream, read_data_istream, transparent, output);
}

static bool ReadPNGWithReadFunction(png_voidp user_data, png_rw_ptr fn, bool transparent, ImageOut& output) {
	output.pixels = nullptr;

	png_struct *png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, on_png_error, on_png_warning);
	if (png_ptr == NULL) {
		Output::Warning("Couldn't allocate PNG structure");
		return false;
	}

	png_info *info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		Output::Warning("Couldn't allocate PNG info structure");
		return false;
	}

	if (setjmp(png_jmpbuf(png_ptr))) {
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return false;
	}

	png_set_read_fn(png_ptr, user_data, fn);

	png_read_info(png_ptr, info_ptr);

	png_uint_32 w, h;
	int bit_depth, color_type;
	png_get_IHDR(png_ptr, info_ptr, &w, &h,
				 &bit_depth, &color_type, NULL, NULL, NULL);

	output.pixels = malloc(w * h * 4);
	if (!output.pixels) {
		Output::Warning("Error allocating PNG pixel buffer.");
		return false;
	}

	switch (color_type) {
		case PNG_COLOR_TYPE_PALETTE:
			ReadPalettedData(png_ptr, info_ptr, w, h, transparent, (uint32_t*)output.pixels);
			output.bpp = 8;
			break;
		case PNG_COLOR_TYPE_GRAY:
			ReadGrayData(png_ptr, info_ptr, w, h, transparent, (uint32_t*)output.pixels);
			output.bpp = 8;
			break;
		case PNG_COLOR_TYPE_GRAY_ALPHA:
			ReadGrayAlphaData(png_ptr, info_ptr, w, h, (uint32_t*)output.pixels);
			output.bpp = 8;
			break;
		case PNG_COLOR_TYPE_RGB:
			ReadRGBData(png_ptr, info_ptr, w, h, (uint32_t*)output.pixels);
			output.bpp = 24;
			break;
		case PNG_COLOR_TYPE_RGB_ALPHA:
			ReadRGBAData(png_ptr, info_ptr, w, h, (uint32_t*)output.pixels);
			output.bpp = 32;
			break;
	}

	png_read_end(png_ptr, NULL);
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

	output.width = w;
	output.height = h;
	return true;
}

static void ReadPalettedData(
	png_struct* png_ptr, png_info* info_ptr,
	png_uint_32 w, png_uint_32 h,
	bool transparent,
	uint32_t* pixels
) {
	// For transparent images, all the colors are opaque, except the
	// color with index 0. So we'll need to do index->RGB conversion
	// on our own.
	png_set_packing(png_ptr);
	png_read_update_info(png_ptr, info_ptr);

	if (!png_get_valid(png_ptr, info_ptr, PNG_INFO_PLTE)) {
		Output::Warning("Palette PNG without PLTE block");
		return;
	}

	png_colorp palette;
	int num_palette;
	png_get_PLTE(png_ptr, info_ptr, &palette, &num_palette);

	for (png_uint_32 y = 0; y < h; y++) {
		// We read the indices (w bytes) into the end of the pixel
		// data for this row (4w bytes), then scan over them
		// converting them into RGBA values. Putting them at the end
		// gives us enough room that we don't overwrite an index
		// we'll need later with an RGBA value.

		uint32_t* beginning_of_row = pixels + y * w;

		uint8_t* indices = (uint8_t*)beginning_of_row + w * 3;
		png_read_row(png_ptr, (png_bytep)indices, NULL);

		uint32_t* dst = beginning_of_row;
		for (png_uint_32 x = 0; x < w; x++, dst++) {
			uint8_t idx = indices[x];
			png_color& color = palette[idx];
			uint8_t alpha = (idx == 0 && transparent) ? 0 : 255;
			uint8_t rgba[4] = { color.red, color.green, color.blue, alpha };
			*dst = *(uint32_t*)rgba;
		}
	}
}

static void ReadGrayData(
	png_struct* png_ptr, png_info* info_ptr,
	png_uint_32 w, png_uint_32 h,
	bool transparent,
	uint32_t* pixels
) {
	png_set_strip_16(png_ptr);
	png_set_expand(png_ptr);
	png_set_gray_to_rgb(png_ptr);
	png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
	png_read_update_info(png_ptr, info_ptr);

	for (png_uint_32 y = 0; y < h; y++) {
		png_bytep dst = (png_bytep) pixels + y * w * 4;
		png_read_row(png_ptr, dst, NULL);
	}

	// Black pixels are transparent
	if (transparent) {
		uint8_t ck1[4] = {0, 0, 0, 255};
		uint8_t ck2[4] = {0, 0, 0,   0};
		uint32_t srckey = *(uint32_t*)ck1;
		uint32_t dstkey = *(uint32_t*)ck2;
		uint32_t* p = (uint32_t*) pixels;
		for (unsigned i = 0; i < w * h; i++, p++)
			if (*p == srckey)
				*p = dstkey;
	}
}

static void ReadGrayAlphaData(
	png_struct* png_ptr, png_info* info_ptr,
	png_uint_32 w, png_uint_32 h,
	uint32_t* pixels
) {
	png_set_strip_16(png_ptr);
	png_set_gray_to_rgb(png_ptr);
	png_read_update_info(png_ptr, info_ptr);

	for (png_uint_32 y = 0; y < h; y++) {
		png_bytep dst = (png_bytep) pixels + y * w * 4;
		png_read_row(png_ptr, dst, NULL);
	}
}

static void ReadRGBData(
	png_struct* png_ptr, png_info* info_ptr,
	png_uint_32 w, png_uint_32 h,
	uint32_t* pixels
) {
	png_set_strip_16(png_ptr);
	png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
	png_read_update_info(png_ptr, info_ptr);

	for (png_uint_32 y = 0; y < h; y++) {
		png_bytep dst = (png_bytep) pixels + y * w * 4;
		png_read_row(png_ptr, dst, NULL);
	}
}

static void ReadRGBAData(
	png_struct* png_ptr, png_info* info_ptr,
	png_uint_32 w, png_uint_32 h,
	uint32_t* pixels
) {
	png_set_strip_16(png_ptr);
	png_read_update_info(png_ptr, info_ptr);

	for (png_uint_32 y = 0; y < h; y++) {
		png_bytep dst = (png_bytep) pixels + y * w * 4;
		png_read_row(png_ptr, dst, NULL);
	}
}

static void write_data(png_structp out_ptr, png_bytep data, png_size_t len) {
	static_cast<std::ostream*>(png_get_io_ptr(out_ptr))->write(reinterpret_cast<char const*>(data), len);
}
static void flush_stream(png_structp out_ptr) {
	static_cast<std::ostream*>(png_get_io_ptr(out_ptr))->flush();
}

bool ImagePNG::Write(std::ostream& os, uint32_t width, uint32_t height, uint32_t* data, bool include_alpha) {
	png_structp write = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!write) {
		Output::Warning("Bitmap::WritePNG: error in png_create_write");
		return false;
	}

	png_infop info = png_create_info_struct(write);
	if (!info) {
		png_destroy_write_struct(&write, &info);
		Output::Warning("ImagePNG::WritePNG: error in png_create_info_struct");
		return false;
	}

	png_bytep* ptrs = new png_bytep[height];
	for (size_t i = 0; i < height; ++i) {
		ptrs[i] = reinterpret_cast<png_bytep>(&data[width*i]);
	}

	if (setjmp(png_jmpbuf(write))) {
		png_destroy_write_struct(&write, &info);
		delete [] ptrs;
		Output::Warning("ImagePNG::WritePNG: error writing PNG file");
		return false;
	}

	png_set_write_fn(write, &os, &write_data, &flush_stream);
	if (include_alpha) {
		png_set_compression_level(write, 9);
		png_set_filter(write, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
	}

	png_set_IHDR(write, info, width, height, 8,
				 include_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
				 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	png_write_info(write, info);
	png_write_image(write, ptrs);
	png_write_end(write, NULL);

	png_destroy_write_struct(&write, &info);
	delete [] ptrs;

	return true;
}
