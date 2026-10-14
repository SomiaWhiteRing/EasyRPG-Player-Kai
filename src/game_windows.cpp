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
#include "maniac_text_picture.h"
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
#include "game_interpreter.h"

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
Game_Windows::Window_User::Window_User(lcf::rpg::SaveEasyRpgWindow save)
	: data(std::move(save))
{

}

void Game_Windows::SetSaveData(std::vector<lcf::rpg::SaveEasyRpgWindow> save) {
	for (auto& window : windows) window->Erase();
	windows.clear();

	int num_windows = static_cast<int>(save.size());
	windows.reserve(num_windows);
	for (int i = 0; i < num_windows; ++i) {
		windows.push_back(std::make_shared<Window_User>(std::move(save[i])));
		auto& win = windows.back();
		int id = win->data.ID;
		if (id > 0) {
			auto pic = Main_Data::game_pictures->GetPicturePtr(id);
			if (pic && pic->IsWindowAttached() && (pic->data.maniac_image_flags >> 24) != 1) {
				// no special async handling needed, loading will already yield
				bool async_wait;
				win->Refresh(async_wait);
			}
		}
	}
	if (Player::IsPatchManiac()) {
		for (int id = 1; id <= Main_Data::game_pictures->GetMaxPictureId(); ++id)
			if (auto* picture = Main_Data::game_pictures->GetPicturePtr(id)) RestoreManiacTextPicture(*picture);
	}
}

std::vector<lcf::rpg::SaveEasyRpgWindow> Game_Windows::GetSaveData() const {
	std::vector<lcf::rpg::SaveEasyRpgWindow> save;

	auto data_size = static_cast<int>(windows.size());
	save.reserve(data_size);

	for (auto& win: windows) {
		if (win->maniac_text_picture) {
			lcf::rpg::SaveEasyRpgWindow empty;
			empty.ID = win->data.ID;
			// Native canonical text cannot distinguish literal inserted escapes
			// from display tokens. Retain resolved tokens in Player's private save.
			lcf::rpg::SaveEasyRpgText text;
			text.text = lcf::DBString(ManiacTextPicture::SaveSnapshot(win->maniac_text_picture->parsed));
			empty.texts.push_back(std::move(text));
			save.push_back(std::move(empty));
		} else save.push_back(win->data);
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
	if (id <= 0) Output::Error("Invalid Window Access [{}]", id);
	if (EP_UNLIKELY(id > static_cast<int>(windows.size()))) {
		windows.reserve(id);
		while (static_cast<int>(windows.size()) < id) {
			windows.push_back(std::make_shared<Window_User>(static_cast<int>(windows.size()) + 1));
		}
	}
	return *windows[id - 1];
}

Game_Windows::Window_User* Game_Windows::GetWindowPtr(int id) {
	return id > 0 && id <= static_cast<int>(windows.size())
		? windows[id - 1].get() : nullptr;
}

bool Game_Windows::Window_User::Create(const WindowParams& params) {
	Erase();

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
	if (maniac_text_picture) maniac_text_picture->Cancel();
	maniac_text_picture.reset();
	window.reset();

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
		PendingMessage pm(CommandCodeInserter);
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
			x_max += 1;
			y_max += 1;
		}

		if (data.width == 0) {
			data.width = x_max;
		}

		if (data.height == 0) {
			data.height = y_max;
		}
	}

	window = std::make_unique<Window_Selectable>(0, 0, data.width, data.height);
	if (!data.flags.border_margin) {
		window->SetBorderX(0);
		// FIXME: Figure out why 0 does not work here (bug in Window class)
		window->SetBorderY(-3);
	}
	window->CreateContents();
	window->SetVisible(false);

	BitmapRef system;
	if (!data.system_name.empty()) {
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

	// Draw text
	for (size_t i = 0; i < data.texts.size(); ++i) {
		auto& font = fonts[i];
		const auto& pm = messages[i];
		const auto& text = data.texts[i];
		auto style_guard = apply_style(font, text);

		int x = text.position_x;
		int y = text.position_y + 2; // +2 to match the offset RPG_RT uses
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

void Game_Windows::Exchange(int source, int target) {
	GetWindow(std::max(source, target));
	std::swap(windows[source - 1], windows[target - 1]);
	windows[source - 1]->data.ID = source;
	windows[target - 1]->data.ID = target;
}

bool Game_Windows::HasPendingRequest(int id) const {
	return id > 0 && id <= static_cast<int>(windows.size()) &&
		(!windows[id - 1]->request_ids.empty() || (windows[id - 1]->maniac_text_picture &&
		!windows[id - 1]->maniac_text_picture->requests.empty()));
}

void Game_Windows::CreateManiacTextPicture(int id,
    const std::shared_ptr<ManiacTextPicture::Operation>& operation) {
    auto& window = GetWindow(id);
    window.Erase();
    window.maniac_text_picture = operation;
    operation->Start();
}

void Game_Windows::RestoreManiacTextPicture(Game_Pictures::Picture& picture) {
    if ((picture.data.maniac_image_flags >> 24) != 1) return;
    auto& window = GetWindow(picture.data.ID);
    if (window.maniac_text_picture && window.maniac_text_picture->picture.lock().get() == &picture) return;
    const bool snapshot = window.data.texts.size() == 1 && !window.data.texts[0].text.empty()
        && std::string_view(window.data.texts[0].text).front() == '\1';
    const std::string text(snapshot ? std::string_view(window.data.texts[0].text) : std::string_view(picture.data.name));
    window.Erase();
    auto operation = std::make_shared<ManiacTextPicture::Operation>();
    operation->restoring = true;
    operation->picture = Main_Data::game_pictures->GetPictureRef(picture.data.ID);
    operation->format.flags = picture.data.maniac_image_flags;
    operation->format.font = uint32_t(picture.data.finish_bot_trans);
    operation->format.spacing = picture.data.maniac_transparency_flags;
    // Native kind1 files carry blend/whole-flip in b9, without EasyRPG
    // extension chunks. Restore those consumers before preparing the bitmap.
    const uint32_t packed = operation->format.font >> 8;
    picture.data.easyrpg_blend_mode = int(Bitmap::BlendMode::Normal);
    switch (packed & 15) {
    case 1: picture.data.easyrpg_blend_mode = int(Bitmap::BlendMode::Multiply); break;
    case 2: picture.data.easyrpg_blend_mode = int(Bitmap::BlendMode::Additive); break;
    case 3: picture.data.easyrpg_blend_mode = int(Bitmap::BlendMode::Overlay); break;
    default: break;
    }
    const auto flip = (packed >> 4) & 15;
    picture.data.easyrpg_flip = 0;
    if (flip == 1 || flip == 3) picture.data.easyrpg_flip |= lcf::rpg::SavePicture::EasyRpgFlip_x;
    if (flip == 2 || flip == 3) picture.data.easyrpg_flip |= lcf::rpg::SavePicture::EasyRpgFlip_y;
    const auto& dimensions = picture.data.maniac_image_dimensions;
    if (dimensions.size() == 2) {
        operation->format.width = dimensions[0];
        operation->format.height = dimensions[1];
    }
    // Cached kind1 text does not expand V/T/N/SELF a second time.
    operation->parsed = ManiacTextPicture::Parse(Game_Interpreter::GetForegroundInterpreter(),
        Utils::DecodeUTF16(text, true), 0, true, snapshot);
    window.maniac_text_picture = operation;
    picture.data.easyrpg_type = lcf::rpg::SavePicture::EasyRpgType_window;
    picture.data.current_bot_trans = 0.0;
    operation->Start();
}
