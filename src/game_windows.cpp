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

#include "game_windows.h"
#include "game_message.h"
#include "game_strings.h"
#include "main_data.h"
#include "compiler.h"
#include "text.h"
#include "cache.h"
#include "pending_message.h"
#include "filefinder.h"
#include "output.h"
#include "player.h"
#include "game_battle.h"
#include "game_map.h"
#include "game_interpreter_map.h"

static std::optional<std::string> CommandCodeInserter(char ch, const char **iter, const char *end, uint32_t escape_char) {
	if ((ch == 'T' || ch == 't') && Player::IsPatchManiac()) {
		auto parse_ret = Game_Message::ParseString(*iter, end, escape_char, true);
		*iter = parse_ret.next;
		int value = parse_ret.value;

		// Contrary to Messages, the content of \t[]-strings is not evaluated
		return ToString(Main_Data::game_strings->Get(value));
	}

	return PendingMessage::DefaultCommandInserter(ch, iter, end, escape_char);
}

static Rect ProcessManiacText(std::string_view text, const Font& font, int line_spacing, std::optional<std::array<int, 3>> context, const Bitmap* system = nullptr, Bitmap* dest = nullptr, int origin_x = 0, int origin_y = 0) {
	const auto style = font.GetCurrentStyle();
	const auto units = Utils::DecodeUTF16(text, true);
	size_t cursor = 0;
	int x = 0, y = 0, width = 0, count = 0;
	Point color_cell{2, 50};
	Game_Message::ManiacHookCaller caller;
	const auto& interpreter = Game_Battle::IsBattleRunning() ? Game_Battle::GetInterpreter() : Game_Map::GetInterpreter();
	const auto& stack = interpreter.GetState().stack;
	if (!stack.empty()) {
		const auto& frame = stack.back();
		caller = {frame.easyrpg_self_scope, frame.easyrpg_self_map_id, frame.easyrpg_self_event_id};
	}
	if (context) caller = {(*context)[0], (*context)[1], (*context)[2]};
	const auto numbers = [&]() {
		const auto tail = Utils::EncodeUTF(units.substr(cursor), true);
		std::array<int32_t, 2> values;
		const auto* next = Game_Message::ParseManiacNumbers(tail.data(), tail.data() + tail.size(), caller, values);
		cursor += Utils::DecodeUTF16(std::string_view(tail.data(), next - tail.data()), true).size();
		return values;
	};
	const auto end_line = [&]() {
		width = std::max(width, x - (count ? style.letter_spacing : 0));
		x = count = 0;
	};
	while (cursor < units.size()) {
		char16_t ch = units[cursor++];
		std::optional<Rect> icon;
		if (ch == '\n') { end_line(); y += style.size + line_spacing; continue; }
		if (ch == '\\' && cursor < units.size()) {
			ch = units[cursor++];
			if (ch == 'C' || ch == 'c') {
				const auto values = numbers();
				if (system) {
					const int columns = system->GetWidth() / 16;
					const int index = std::max(0, values[0]);
					const int cx = values[1] < 0 ? index % columns : index;
					const int cy = values[1] < 0 ? index / columns : values[1];
					if (cx >= 0 && cy >= 0 && static_cast<int64_t>(cx) * 16 + 16 <= system->GetWidth() && static_cast<int64_t>(cy) * 16 + 64 <= system->GetHeight()) color_cell = {cx * 16 + 2, cy * 16 + 50};
				}
				continue;
			}
		} else if (ch == '$' && cursor < units.size()) {
			const auto next = units[cursor];
			if (next == '$') { ++cursor; }
			else {
				std::array<int32_t, 2> cell;
				if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z')) {
					++cursor;
					const int index = next >= 'a' ? next - 'a' + 26 : next - 'A';
					cell = {index % 13, index / 13};
				} else cell = numbers();
				int cx = std::max(0, cell[0]), cy = cell[1];
				if (cy < 0) {
					const int columns = Cache::Exfont()->GetWidth() / 12;
					cy = cx / columns; cx %= columns;
				}
				icon = Rect(cx <= INT32_MAX / 12 ? cx * 12 : INT32_MAX,
					cy <= INT32_MAX / 12 ? cy * 12 : INT32_MAX, 12, 12);
			}
		}
		if (Utils::IsControlCharacter(ch)) continue;
		const int advance = icon ? style.size + style.letter_spacing : font.GetSize(ch).width;
		if (dest && system) font.RenderManiacTextGlyph(*dest, origin_x + x, origin_y + y, *system, color_cell, ch, icon ? &*icon : nullptr);
		x += advance;
		++count;
	}
	end_line();
	return {0, 0, width, y + style.size};
}

static BitmapRef CreateManiacWindow(int width, int height, const Bitmap& system, int background, bool frame) {
	auto bitmap = Bitmap::Create(width, height, true);
	bitmap->EnableManiacPixels();
	const auto copy = [&](int x, int y, int sx, int sy) {
		const auto color = system.GetColorAt(sx, sy);
		if (color.alpha) bitmap->SetManiacColorAt(x, y, color);
	};
	if (background == lcf::rpg::System::Stretch_stretch || background == lcf::rpg::System::Stretch_tiled) {
		for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
			copy(x, y, background == lcf::rpg::System::Stretch_stretch ? x * 32 / width : x % 32,
				background == lcf::rpg::System::Stretch_stretch ? y * 32 / height : y % 32);
	}
	if (frame && width >= 8 && height >= 8) {
		// Draw complete corners in order; 8..15 pixel windows overlap them.
		for (int corner = 0; corner < 4; ++corner) {
			for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
				copy((corner & 1 ? width - 8 : 0) + x, (corner & 2 ? height - 8 : 0) + y,
					(corner & 1 ? 56 : 32) + x, (corner & 2 ? 24 : 0) + y);
		}
		if (width > 16) for (int x = 8; x < width - 8; ++x) for (int y = 0; y < 8; ++y) {
			copy(x, y, 40 + (x - 8) % 16, y);
			copy(x, height - 8 + y, 40 + (x - 8) % 16, 24 + y);
		}
		if (height > 16) for (int y = 8; y < height - 8; ++y) for (int x = 0; x < 8; ++x) {
			copy(x, y, 32 + x, 8 + (y - 8) % 16);
			copy(width - 8 + x, y, 56 + x, 8 + (y - 8) % 16);
		}
	}
	return bitmap;
}

Game_Windows::Window_User::Window_User(lcf::rpg::SaveEasyRpgWindow save)
	: data(std::move(save))
{

}

void Game_Windows::SetSaveData(std::vector<lcf::rpg::SaveEasyRpgWindow> save) {
	windows.clear();

	int num_windows = static_cast<int>(save.size());
	windows.reserve(num_windows);
	for (int i = 0; i < num_windows; ++i) {
		windows.emplace_back(std::move(save[i]));
		auto& win = windows.back();
		int id = win.data.ID;
		if (id > 0) {
			auto pic = Main_Data::game_pictures->GetPicturePtr(id);
			if (pic && pic->IsWindowAttached() && (!Player::IsPatchManiac() || pic->data.maniac_image_flags >> 24 != 1)) {
				// no special async handling needed, loading will already yield
				bool async_wait;
				win.Refresh(async_wait);
			}
		}
	}
	if (Player::IsPatchManiac()) {
		// RPG_RT 0x6b370 restores type-1 pictures from chunk 1. Private windows
		// are retained for old EasyRPG saves, but native records are authoritative.
		for (int id = 1; id <= Main_Data::game_pictures->GetMaxPictureId(); ++id) {
			auto* pic = Main_Data::game_pictures->GetPicturePtr(id);
			if (!pic || pic->data.maniac_image_flags >> 24 != 1) continue;
			const auto& picture = pic->data;
			auto serialized = Utils::DecodeUTF16(picture.name, true);
			serialized.resize(std::find(serialized.begin(), serialized.end(), 0) - serialized.begin());
			size_t cursor = serialized.empty() ? 0 : 1;
			const auto field = [&]() {
				const auto first = cursor;
				while (cursor < serialized.size() && serialized[cursor] >= 3) ++cursor;
				auto result = Utils::EncodeUTF(serialized.substr(first, cursor - first), true);
				if (cursor < serialized.size()) ++cursor;
				return result;
			};
			WindowParams params;
			WindowText text;
			text.text = field();
			params.system_name = field();
			text.font_name = field();
			const auto flags = picture.maniac_image_flags;
			const uint32_t style = static_cast<uint32_t>(picture.finish_bot_trans);
			text.font_size = style & 255;
			text.letter_spacing = (picture.maniac_transparency_flags >> 8) & 255;
			text.line_spacing = (picture.maniac_transparency_flags >> 16) & 255;
			text.draw_gradient = !(flags & (1 << 13));
			text.draw_shadow = !(flags & (1 << 14));
			text.bold = (flags & (1 << 15)) != 0;
			params.draw_frame = !(flags & (1 << 12));
			params.border_margin = !(flags & (1 << 16));
			const auto stretch = (flags >> 8) & 15;
			params.message_stretch = stretch == 1 ? lcf::rpg::System::Stretch_stretch
				: stretch == 2 ? lcf::rpg::System::Stretch_tiled : lcf::rpg::System::Stretch_easyrpg_none;
			if (picture.maniac_image_dimensions.size() == 2) {
				params.width = picture.maniac_image_dimensions[0];
				params.height = picture.maniac_image_dimensions[1];
			}
			params.texts = {std::move(text)};
			auto& win = GetWindow(id);
			win.Create(params);
			pic->data.easyrpg_type = lcf::rpg::SavePicture::EasyRpgType_window;
			pic->data.current_bot_trans = 0.0;
			pic->data.easyrpg_flip = ((style & (16 << 8)) ? lcf::rpg::SavePicture::EasyRpgFlip_x : 0)
				| ((style & (32 << 8)) ? lcf::rpg::SavePicture::EasyRpgFlip_y : 0);
			pic->data.easyrpg_blend_mode = (style & 0xf00) == 0x100 ? static_cast<int>(Bitmap::BlendMode::Multiply)
				: (style & 0xf00) == 0x200 ? static_cast<int>(Bitmap::BlendMode::Additive)
				: (style & 0xf00) == 0x300 ? static_cast<int>(Bitmap::BlendMode::Overlay) : 0;
			bool async_wait;
			win.Refresh(async_wait);
		}
	}
}

std::vector<lcf::rpg::SaveEasyRpgWindow> Game_Windows::GetSaveData() const {
	std::vector<lcf::rpg::SaveEasyRpgWindow> save;

	auto data_size = static_cast<int>(windows.size());
	save.reserve(data_size);

	for (auto& win: windows) {
		save.push_back(win.data);
	}

	// RPG_RT Save game data always has a constant number of pictures
	// depending on the engine version. We replicate this, unless we have even
	// more pictures than that.
	while (data_size > static_cast<int>(save.size())) {
		lcf::rpg::SaveEasyRpgWindow data;
		data.ID = static_cast<int>(save.size()) + 1;
		save.push_back(std::move(data));
	}

	return save;
}

Game_Windows::Window_User& Game_Windows::GetWindow(int id) {
	if (EP_UNLIKELY(id > static_cast<int>(windows.size()))) {
		windows.reserve(id);
		while (static_cast<int>(windows.size()) < id) {
			windows.emplace_back(static_cast<int>(windows.size()) + 1);
		}
	}
	return windows[id - 1];
}

Game_Windows::Window_User* Game_Windows::GetWindowPtr(int id) {
	return id <= static_cast<int>(windows.size())
		? &windows[id - 1] : nullptr;
}

bool Game_Windows::Window_User::Create(const WindowParams& params) {
	Erase();
	maniac_caller = params.maniac_caller;

	data.width = params.width;
	data.height = params.height;
	data.system_name = lcf::DBString(params.system_name);
	data.message_stretch = params.message_stretch;
	data.flags.draw_frame = params.draw_frame;
	data.flags.border_margin = params.border_margin;

	for (const auto& text: params.texts) {
		lcf::rpg::SaveEasyRpgText data_text;

		data_text.text = lcf::DBString(text.text);
		data_text.position_x = text.position_x;
		data_text.position_y = text.position_y;
		data_text.font_name = lcf::DBString(text.font_name);
		data_text.font_size = text.font_size;
		data_text.letter_spacing = text.letter_spacing;
		data_text.line_spacing = text.line_spacing;
		data_text.flags.draw_gradient = text.draw_gradient;
		data_text.flags.draw_shadow = text.draw_shadow;
		data_text.flags.bold = text.bold;
		data_text.flags.italic = text.italic;

		data.texts.push_back(data_text);
	}

	return true;
}

bool Game_Windows::Create(int id, const WindowParams& params, bool& async_wait) {
	auto& window = GetWindow(id);

	if (window.Create(params)) {
		if (Main_Data::game_pictures->Show(id, params)) {
			if (Player::IsPatchManiac() && !params.texts.empty()) {
				auto& picture = Main_Data::game_pictures->GetPicture(id).data;
				const auto& text = params.texts.front();
				picture.name = "\x01" + text.text + "\x01" + params.system_name + "\x01" + text.font_name;
				picture.maniac_image_flags = params.maniac_text_flags | 0x01000000;
				picture.maniac_transparency_flags = params.maniac_transparency_flags;
				picture.finish_bot_trans = params.maniac_text_style;
				picture.easyrpg_type = lcf::rpg::SavePicture::EasyRpgType_window;
			}
			window.Refresh(async_wait);
			return true;
		} else {
			window.Erase();
			return false;
		}
	}

	return false;
}

void Game_Windows::Window_User::Erase() {
	int id = data.ID;
	data = {};
	data.ID = id;
	maniac_caller.reset();
	maniac_bitmap.reset();

	request_ids.clear();
}

void Game_Windows::Window_User::Refresh(bool& async_wait) {
	async_wait = !Request();

	if (async_wait) {
		// Create fake window to prevent crashes
		if (!window) {
			window = std::make_unique<Window_Selectable>(0, 0, 0, 0);
		}
		return;
	}

	std::vector<FontRef> fonts;
	std::vector<PendingMessage> messages;
	const char32_t escape = Player::IsPatchManiac() ? U'\\' : Player::escape_char;

	// Preprocessing
	for (const auto& text: data.texts) {
		FontRef font;

		Filesystem_Stream::InputStream font_file;
		std::string font_name = ToString(text.font_name);

		if (!font_name.empty()) {
			// Try to find best fitting font
			if (text.flags.bold && text.flags.italic) {
				font_file = FileFinder::OpenFont(font_name + "-BoldItalic");
			}

			if (!font_file && text.flags.bold) {
				font_file = FileFinder::OpenFont(font_name + "-Bold");
			}

			if (!font_file && text.flags.italic) {
				font_file = FileFinder::OpenFont(font_name + "-Italic");
			}

			if (!font_file) {
				font_file = FileFinder::OpenFont(font_name+ "-Regular");
			}

			if (!font_file) {
				font_file = FileFinder::OpenFont(font_name);
			}

			if (!font_file) {
				font = Font::CreateSystemFont(font_name, text.font_size, text.flags.bold, text.flags.italic);
				if (!font) {
					Output::Warning("Font not found: {}", text.font_name);
					font = Font::Default();
				}
			} else {
				font = Font::CreateFtFont(std::move(font_file), text.font_size, text.flags.bold, text.flags.italic);
				if (!font) {
					Output::Warning("Error loading font: {}", text.font_name);
					font = Font::Default();
				}
			}
		} else {
			font = Font::Default();
		}

		fonts.emplace_back(font);

		std::stringstream ss(ToString(text.text));
		std::string out;
		// 250921 resolves V/T/N when 3007 creates the picture. The saved text
		// must not be resolved again against later variables or a load's caller.
		PendingMessage pm(Player::IsPatchManiac()
			? PendingMessage::CommandInserter([](char, const char**, const char*, uint32_t) -> std::optional<std::string> { return std::nullopt; })
			: PendingMessage::CommandInserter(CommandCodeInserter));
		while (Utils::ReadLine(ss, out)) {
			pm.PushLine(out);
		}
		messages.emplace_back(pm);
	}

	auto apply_style = [](auto& font, const auto& text) {
		Font::Style style = font->GetCurrentStyle();
		style.size = text.font_size;
		style.bold = text.flags.bold;
		style.italic = text.flags.italic;
		style.draw_shadow = text.flags.draw_shadow;
		style.draw_gradient = text.flags.draw_gradient;
		style.color_offset = {};
		style.letter_spacing = text.letter_spacing;
		return font->ApplyStyle(style);
	};

	if (data.width == 0 || data.height == 0) {
		// Automatic window size
		int x_max = 0;
		int y_max = 0;

		for (size_t i = 0; i < data.texts.size(); ++i) {
			// Lots of duplication with the rendering code below but cannot be easily reduced more
			auto& font = fonts[i];
			const auto& pm = messages[i];
			const auto& text = data.texts[i];
			auto style_guard = apply_style(font, text);

			int x = text.position_x;
			int y = text.position_y;
			if (Player::IsPatchManiac()) {
				const auto size = ProcessManiacText(text.text, *font, text.line_spacing, maniac_caller);
				x_max = std::max(x_max, x + size.width);
				y_max = std::max(y_max, y + size.height);
				continue;
			}
			for (const auto& line: pm.GetLines()) {
				std::u32string line32;
				auto* text_index = line.data();
				const auto* end = line.data() + line.size();

				while (text_index != end) {
					auto tret = Utils::TextNext(text_index, end, escape);
					text_index = tret.next;

					if (EP_UNLIKELY(!tret)) {
						continue;
					}

					const auto ch = tret.ch;

					if (ch == '\n') {
						if (!line32.empty()) {
							x += Text::GetSize(*font, Utils::EncodeUTF(line32)).width;
							line32.clear();
						}

						x_max = std::max(x, x_max);
						x = 0;
						y += text.font_size + text.line_spacing;

						continue;
					}

					if (Utils::IsControlCharacter(ch)) {
						// control characters not handled
						continue;
					}

					if (tret.is_exfont) {
						// exfont processed later
						line32 += '$';
					}

					if (tret.is_escape && ch != escape) {
						if (!line32.empty()) {
							x += Text::GetSize(*font, Utils::EncodeUTF(line32)).width;
							line32.clear();
						}

						// Special message codes
						switch (ch) {
							case 'c':
							case 'C':
							{
								// Color
								text_index = Game_Message::ParseColor(text_index, end, escape, true).next;
							}
							break;
						}
						continue;
					}

					line32 += static_cast<char32_t>(ch);
				}

				if (!line32.empty()) {
					x += Text::GetSize(*font, Utils::EncodeUTF(line32)).width;
				}

				x_max = std::max(x, x_max);

				x = 0;
				y += text.font_size + text.line_spacing;
			}

			y -= text.line_spacing;

			y_max = std::max(y, y_max);
		}

		// Border size
		if (data.flags.border_margin) {
			x_max += 16;
			y_max += 20; // 16 looks better but this matches better with Maniac Patch
		} else {
			const bool shadow = !Player::IsPatchManiac()
				|| (!data.texts.empty() && !data.texts.front().text.empty() && data.texts.front().flags.draw_shadow);
			x_max += shadow;
			y_max += shadow;
		}

		if (data.width == 0) {
			data.width = x_max;
		}

		if (data.height == 0) {
			data.height = y_max;
		}
	}

	window = std::make_unique<Window_Selectable>(0, 0, data.width, data.height);
	const bool native_text = Player::IsPatchManiac();
	if (native_text) {
		// Native clipping is against the full picture, including its margins.
		window->SetBorderX(0);
		window->SetBorderY(0);
	} else if (!data.flags.border_margin) {
		window->SetBorderX(0);
		// FIXME: Figure out why 0 does not work here (bug in Window class)
		window->SetBorderY(-3);
	}
	window->CreateContents();
	window->SetVisible(false);

	BitmapRef system;
	if (native_text || !data.system_name.empty()) {
		system = Cache::System(data.system_name);
	} else {
		system = Cache::SystemOrBlack();
	}

	auto& pic = Main_Data::game_pictures->GetPicture(data.ID);

	window->SetWindowskin(system, pic.data.use_transparent_color);
	window->SetStretch(data.message_stretch == lcf::rpg::System::Stretch_stretch);

	if (data.message_stretch == lcf::rpg::System::Stretch_easyrpg_none) {
		window->SetBackOpacity(0);
	}

	if (!data.flags.draw_frame) {
		window->SetFrameOpacity(0);
	}
	if (native_text) maniac_bitmap = CreateManiacWindow(data.width, data.height, *system, data.message_stretch, data.flags.draw_frame);

	// Draw text
	for (size_t i = 0; i < data.texts.size(); ++i) {
		auto& font = fonts[i];
		const auto& pm = messages[i];
		const auto& text = data.texts[i];
		auto style_guard = apply_style(font, text);

		int x = text.position_x;
		int y = text.position_y + 2; // +2 to match the offset RPG_RT uses
		if (Player::IsPatchManiac()) {
			ProcessManiacText(text.text, *font, text.line_spacing, maniac_caller, system.get(), maniac_bitmap.get(),
				x + (data.flags.border_margin ? 8 : 0), text.position_y + (data.flags.border_margin ? 10 : 0));
			continue;
		}
		int text_color = 0;
		for (const auto& line: pm.GetLines()) {
			std::u32string line32;
			auto* text_index = line.data();
			const auto* end = line.data() + line.size();

			while (text_index != end) {
				auto tret = Utils::TextNext(text_index, end, escape);
				text_index = tret.next;

				if (EP_UNLIKELY(!tret)) {
					continue;
				}

				const auto ch = tret.ch;

				if (ch == '\n') {
					if (!line32.empty()) {
						Text::Draw(*window->GetContents(), x, y, *font, *system, text_color, Utils::EncodeUTF(line32));
						line32.clear();
					}

					x = 0;
					y += text.font_size + text.line_spacing;
					continue;
				}

				if (Utils::IsControlCharacter(ch)) {
					// control characters not handled
					continue;
				}

				if (tret.is_exfont) {
					// exfont processed later
					line32 += '$';
				}

				if (tret.is_escape && ch != escape) {
					if (!line32.empty()) {
						x += Text::Draw(*window->GetContents(), x, y, *font, *system, text_color, Utils::EncodeUTF(line32)).x;
						line32.clear();
					}

					// Special message codes
					switch (ch) {
						case 'c':
						case 'C':
						{
							// Color
							auto pres = Game_Message::ParseColor(text_index, end, escape, true);
							auto value = pres.value;
							text_index = pres.next;
							text_color = value > 19 ? 0 : value;
						}
						break;
					}
					continue;
				}

				line32 += static_cast<char32_t>(ch);
			}

			if (!line32.empty()) {
				Text::Draw(*window->GetContents(), x, y, *font, *system, text_color, Utils::EncodeUTF(line32));
			}

			x = 0;
			y += text.font_size + text.line_spacing;
		}
	}

	// Add to picture
	pic.AttachWindow(*window);
	if (native_text) pic.sprite->SetBitmap(maniac_bitmap);
}

bool Game_Windows::Window_User::Request() {
	if (!request_ids.empty()) {
		return true;
	}

	std::vector<FileRequestAsync*> requests;

	for (const auto& text: data.texts) {
		// Create Async requests for all needed resources
		std::string font_name = ToString(text.font_name);
		if (!font_name.empty()) {
			// Request all possible candidates of fonts
			auto bind = [this, &font_name, &requests](const char* suffix) {
				std::string name = font_name + suffix;
				FileRequestAsync* request = AsyncHandler::RequestFile("Font",  name);
				if (!request->IsReady()) {
					request->SetImportantFile(true);
					request_ids.push_back(request->Bind(&Window_User::OnRequestReady, this));
					requests.push_back(request);
				}
			};

			if (text.flags.bold && text.flags.italic) {
				bind("-BoldItalic");
			}

			if (text.flags.bold) {
				bind("-Bold");
			}

			if (text.flags.italic) {
				bind("-Italic");
			}

			bind("-Regular");
			bind("");
		}
	}

	if (!data.system_name.empty()) {
		FileRequestAsync* request = AsyncHandler::RequestFile("System", data.system_name);
		if (!request->IsReady()) {
			request->SetImportantFile(true);
			request->SetGraphicFile(true);
			request_ids.push_back(request->Bind(&Window_User::OnRequestReady, this));
			requests.push_back(request);
		}
	}

	if (request_ids.empty()) {
		// Every request is already finished
		return true;
	}

	for (auto& request: requests) {
		request->Start();
	}

	return false;
}

void Game_Windows::Window_User::OnRequestReady(FileRequestResult* result) {
	auto it = std::find_if(request_ids.begin(), request_ids.end(), [&result](const auto& what) {
		return *what == result->request_id;
	});

	assert(it != request_ids.end());

	request_ids.erase(it);

	if (request_ids.empty()) {
		// All assets requested, repeat Refresh call
		bool async_wait;
		Refresh(async_wait);

		assert(!async_wait);
	}
}

void Game_Windows::Erase(int id) {
	auto* window = GetWindowPtr(id);
	if (EP_LIKELY(window)) {
		window->Erase();
	}
}
