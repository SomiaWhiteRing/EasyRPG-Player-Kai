/* EasyRPG Player, GPL v3 or later.
 * 33ee0/34c60/35360/35d80 from the fixed 250921 reference.
 * Font rasterization is a named portable boundary; this is not a GDI pixel
 * equivalence claim. UTF-16 consumption and snapshot ownership are separate.
 */
#include "maniac_text_picture.h"
#include "game_interpreter.h"
#include "player.h"
#include <lcf/reader_util.h>
#include <optional>
#include "maniac_text_cursor.h"
#include "bitmap.h"
#include "cache.h"
#include "filefinder.h"
#include "font.h"
#include "game_system.h"
#include "main_data.h"
#include "output.h"
#include "utils.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <lcf/data.h>

namespace ManiacTextPicture {
int32_t SignedBits(uint32_t bits) {
    int32_t result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
namespace {
std::u16string Terminated(std::u16string value) {
    value.resize(std::min(value.size(), value.find(char16_t(0))));
    return value;
}
std::u16string Decimal(int32_t value, int padding = 0) {
    char buffer[16];
    const auto end = std::to_chars(buffer, buffer + sizeof(buffer), value).ptr;
    std::string result(buffer, end);
    if (result.size() < static_cast<size_t>(padding))
        result.insert(value < 0 ? 1 : 0, padding - result.size(), '0');
    return Utils::DecodeUTF16(result);
}
std::u16string BodyActor(int32_t id) {
    auto* actor = id == 0
        ? (Main_Data::game_party ? Main_Data::game_party->GetActor(0) : nullptr)
        : (Main_Data::game_actors ? Main_Data::game_actors->GetActor(id) : nullptr);
    if (!actor) {
        // Unlike Console's numeric/name fallback, the body helper errors for
        // missing actor zero as well as a bad explicit actor ID.
        Output::Error("Invalid Maniac text picture actor ID {}", id);
        return {};
    }
    return Terminated(Utils::DecodeUTF16(actor->GetName(), true));
}
std::optional<std::u16string> Config(ManiacTextCursor& cursor, const Game_Interpreter& context, bool resolved_tokens) {
    if (cursor.position >= static_cast<int64_t>(cursor.text.size())) return {};
    const char16_t mode = cursor.At(cursor.position++);
    if (mode == 2 || mode == 3) {
        uint32_t id = 0;
        const auto alphabet = Utils::DecodeUTF16(lcf::ReaderUtil::Recode("\\~\x7f", Player::encoding), true);
        while (cursor.position < static_cast<int64_t>(cursor.text.size())) {
            uint16_t unit = cursor.At(cursor.position);
            if (alphabet.size() == 3) {
                if (unit == alphabet[0]) unit = 0x5c;
                else if (unit == alphabet[1]) unit = 0x7e;
                else if (unit == alphabet[2]) unit = 0x7f;
            }
            if (!(unit & 0x40)) break; // Invalid unit is not consumed.
            ++cursor.position;
            id = (id << 5) | (unit & 31);
            if (!(unit & 0x20)) break;
        }
        const int32_t slot = mode == 2 ? SignedBits(id)
            : cursor.context.ResolveVariableIndirectAddress(SignedBits(id));
        if (!context.IsManiacStringPresent(slot)) return std::nullopt;
        return cursor.ReadString(slot);
    }
    const auto first = cursor.position;
    while (cursor.position < static_cast<int64_t>(cursor.text.size()) && cursor.At(cursor.position) >= 3)
        ++cursor.position;
    if (cursor.position == first && !resolved_tokens) return std::nullopt;
    return std::u16string(cursor.text.substr(static_cast<size_t>(first), static_cast<size_t>(cursor.position - first)));
}
}

Parsed Parse(const Game_Interpreter& context, std::u16string text,
    int32_t body_reference, bool cached, bool resolved_tokens) {
    text = Terminated(std::move(text));
    ManiacTextCursor lookup(context, text, static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax));
    if (!cached && !text.empty() && (text[0] == 2 || text[0] == 3)) {
        const int32_t id = text[0] == 2 ? body_reference
            : context.ResolveVariableIndirectAddress(body_reference);
        const auto body = lookup.ReadString(id);
        text = Terminated(std::u16string(1, char16_t(1)) + body + text.substr(1));
    }
    ManiacTextCursor cursor(context, text, resolved_tokens ? 0 : static_cast<uint8_t>(lcf::Data::system.maniac_text_syntax));
    Parsed result;
    Row row;
    std::u16string processed;
    int padding = 0;
    const auto glyph = [&](char16_t unit) {
        Token token;
        token.unit = unit;
        row.push_back(token);
    };
    const auto flush = [&] {
        // Native's temporary row is copied with lstrlenW after inline T can
        // have inserted embedded zero units; canonical retains those units.
        const auto nul = std::find_if(row.begin(), row.end(), [](const auto& token) {
            return token.kind == Token::Glyph && token.unit == 0;
        });
        row.erase(nul, row.end());
        result.rows.push_back(std::move(row));
        row = {};
    };
    const auto newline = [&] {
        flush();
        processed += char16_t(10);
    };
    const auto insert = [&](std::u16string_view value, bool split_lines) {
        // Inserted escapes/control units do not reenter this dispatch loop.
        for (const char16_t unit : value) {
            if (split_lines && unit == 10) newline();
            else { glyph(unit); processed += unit; }
        }
    };
    if (!text.empty()) ++cursor.position; // Unconditional marker skip.
    while (cursor.position < static_cast<int64_t>(text.size())) {
        const auto source = cursor.position;
        char16_t unit = cursor.At(cursor.position);
        if (unit < 3) break;
        ++cursor.position;
        if (unit < 32) {
            if (unit == 10) {
                newline();
                if (cursor.At(cursor.position) < 3) result.rows.emplace_back();
            }
            continue;
        }
        if (unit == u'$') {
            const char16_t next = cursor.At(cursor.position);
            if (next == u'$') {
                ++cursor.position;
                glyph(u'$');
                processed += u'$'; // Native canonical retains one literal $.
                continue;
            }
            Token token;
            token.kind = Token::Icon;
            if ((next >= u'A' && next <= u'Z') || (next >= u'a' && next <= u'z')) {
                ++cursor.position;
                const int index = next >= u'a' ? next - u'a' + 26 : next - u'A';
                // Named icons use the native fixed 13-column coordinates.
                // Numeric lists retain Cell's actual-resource-width mapping.
                token.values = {{index % 13, index / 13}};
            } else token.values = cursor.ReadList();
            row.push_back(token);
            processed += text.substr(static_cast<size_t>(source), static_cast<size_t>(cursor.position - source));
            continue;
        }
        if (unit != u'\\') { glyph(unit); processed += unit; continue; }
        const char16_t code = ManiacTextCursor::Upper(cursor.At(cursor.position));
        if (code == 1 || code == 2 || code == 3) break; // Cursor remains at delimiter.
        if (code == 0 || code == u'\\') {
            if (code) ++cursor.position;
            glyph(u'\\');
            processed += u"\\\\";
            continue;
        }
        ++cursor.position;
        if (resolved_tokens && code == u'G') {
            // Player's private snapshot escapes literal control units so they
            // cannot become a body/config delimiter or a row separator.
            glyph(static_cast<char16_t>(cursor.ReadNumber()));
            continue;
        }
        if (code == u'C') {
            Token token;
            token.kind = Token::ColorChange;
            token.values = cursor.ReadList();
            row.push_back(token);
            processed += u"\\C[" + Decimal(token.values[0]) + u"," + Decimal(token.values[1]) + u"]";
            continue;
        }
        if (code == u'D') { padding = std::clamp(cursor.ReadNumber(), 1, 10); continue; }
        if (cached && (code == u'V' || code == u'T' || code == u'N')) {
            cursor.position = source + 1;
            glyph(u'\\');
            processed += u'\\';
            continue;
        }
        if (code == u'V' || code == u'T' || code == u'N') {
            const int32_t id = cursor.ReadNumber();
            if (code == u'V') insert(Decimal(context.ReadVariable(id), padding), false);
            else if (code == u'T') insert(Utils::DecodeUTF16(context.ReadString(id), true), true);
            else insert(BodyActor(id), false);
            continue;
        }
        if (code == u'S') {
            cursor.position = source;
            const int kind = cached ? 0 : cursor.SelfPrefix();
            if (kind == 1 || kind == 2) {
                const int32_t id = ManiacTextCursor::Negate(cursor.ReadNumber());
                if (kind == 1) insert(Decimal(context.ReadVariable(id), padding), false);
                else insert(Utils::DecodeUTF16(context.ReadString(id), true), true);
                continue;
            }
            // Failed/cached S copies its original slash. SELFS advances six
            // units and then copies the unit at that cursor, even a control.
            const char16_t literal = cursor.At(cursor.position++);
            glyph(literal);
            processed += literal;
            continue;
        }
        // Unknown ordinary escapes copy the following unit directly. In
        // particular \$ must not reenter the dollar/icon dispatch.
        const char16_t literal = cursor.At(cursor.position - 1);
        glyph(literal);
        processed += literal;
    }
    if (!row.empty()) flush();
    auto system = Config(cursor, context, resolved_tokens).value_or(Main_Data::game_system
        ? Utils::DecodeUTF16(Main_Data::game_system->GetSystemName(), true) : std::u16string{});
    auto font = Config(cursor, context, resolved_tokens).value_or(Main_Data::game_system
        && Main_Data::game_system->GetFontId() == lcf::rpg::System::Font_mincho ? u"RPG2000" : u"RPG2000G");
    result.system = Utils::EncodeUTF(system, true);
    result.font = Utils::EncodeUTF(font, true);
    result.canonical = std::u16string(1, char16_t(1)) + processed + char16_t(1) + system + char16_t(1) + font;
    return result;
}

std::string SaveSnapshot(const Parsed& parsed) {
    std::u16string body(1, char16_t(1));
    bool newline = false;
    for (const auto& row : parsed.rows) {
        if (newline) body += char16_t(10);
        newline = true;
        if (row.empty()) body += u"\\G[0]"; // Flush preserves one empty row.
        for (const auto& token : row) {
            if (token.kind == Token::Glyph) {
                if (token.unit < 32) {
                    body += u"\\G[" + Decimal(token.unit) + u"]";
                    continue;
                }
                if (token.unit == u'\\' || token.unit == u'$') body += token.unit;
                body += token.unit;
            } else {
                body += token.kind == Token::ColorChange ? u"\\C[" : u"$[";
                body += Decimal(token.values[0]) + u"," + Decimal(token.values[1]) + u"]";
            }
        }
    }
    body += char16_t(1);
    body += Utils::DecodeUTF16(parsed.system, true);
    body += char16_t(1);
    body += Utils::DecodeUTF16(parsed.font, true);
    return Utils::EncodeUTF(body, true);
}

std::array<int32_t, 2> OriginOffset(int32_t origin, int width, int height) {
    static constexpr uint8_t table[] = {0, 5, 7, 13, 15, 1, 3, 4, 12, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t mode = table[uint32_t(origin) & 15];
    const auto half = [](int size, bool negative) { return negative ? -size / 2 - (size & 1) : size / 2; };
    return {{mode & 4 ? half(width, mode & 8) : 0, mode & 1 ? half(height, mode & 2) : 0}};
}

namespace {
FontRef LoadFont(const Parsed& parsed, const Format& format) {
    const std::string& name = parsed.font;
    Filesystem_Stream::InputStream stream;
    const bool bold = format.Style() & 0x80;
    if (bold) stream = FileFinder::OpenFont(name + "-Bold");
    if (!stream) stream = FileFinder::OpenFont(name + "-Regular");
    if (!stream) stream = FileFinder::OpenFont(name);
    auto font = stream ? Font::CreateFtFont(std::move(stream), format.FontSize(), bold, false) : FontRef{};
    if (!font) font = Font::CreateSystemFont(name, format.FontSize(), bold, false);
    return font ? font : Font::Default();
}

bool Cell(const Bitmap& bitmap, std::array<int32_t, 2> values, int size, bool system, Point& cell) {
    int64_t x = values[0], y = values[1];
    if (y < 0) {
        x = std::max<int64_t>(x, 0);
        const int columns = bitmap.GetWidth() / size;
        if (columns == 0) {
            Output::Error("Maniac text picture resource is narrower than one {}-pixel cell", size);
            return false; // Named host safety boundary: native divides by zero.
        }
        y = x / columns;
        x %= columns;
    }
    x *= size;
    y *= size;
    if (x < 0 || y < 0 || x + size > bitmap.GetWidth() || y + size + (system ? 48 : 0) > bitmap.GetHeight()) return false;
    cell = Point{static_cast<int>(x) + (system ? 2 : 0), static_cast<int>(y) + (system ? 50 : 0)};
    return true;
}


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

BitmapRef Render(const Parsed& parsed, const Format& format) {
    const int size = format.FontSize();
    if (size <= 0) { Output::Error("Maniac text picture has a zero font size in saved data"); return {}; }
    auto font = LoadFont(parsed, format);
    auto style = font->GetCurrentStyle();
    style.size = size;
    style.bold = format.Style() & 0x80;
    style.italic = false;
    style.draw_shadow = !(format.Style() & 0x40);
    style.draw_gradient = !(format.Style() & 0x20);
    style.letter_spacing = 0;
    auto guard = font->ApplyStyle(style);
    const bool shadow = !(format.Style() & 0x40);
    const bool margin = !(format.Style() & 0x100);
    const int margin_x = margin ? 8 : 0, margin_y = margin ? 10 : 0;
    int64_t width = margin_x * 2;
    int64_t height = margin_y * 2 + size;
    for (const auto& row : parsed.rows) {
        int64_t measured = 0, icons = 0, colors = 0;
        for (const auto& token : row) {
            if (token.kind == Token::Icon) ++icons;
            else if (token.kind == Token::ColorChange) ++colors;
            else measured += font->GetSize(static_cast<char32_t>(token.unit)).width;
        }
        const int64_t spacing_units = std::max<int64_t>(static_cast<int64_t>(row.size()) - colors - 1, 0);
        width = std::max(width, margin_x * 2 + measured + icons * size + spacing_units * format.LetterSpacing());
    }
    if (!parsed.rows.empty()) {
        height = margin_y * 2 + static_cast<int64_t>(parsed.rows.size()) * (size + format.LineSpacing()) - format.LineSpacing();
        if (shadow && !margin) { ++width; ++height; }
    }
    if (format.width != 0) width = format.width;
    if (format.height != 0) height = format.height;
    if (width < 0 || height < 0 || width > INT32_MAX || height > INT32_MAX ||
        width * height > INT32_MAX / 4) {
        Output::Error("Maniac text picture dimensions exceed safe bitmap storage: {} x {}", width, height);
        return {}; // Named allocation boundary, not a native clamp.
    }
    auto system = Cache::System(parsed.system);
    if (system->GetWidth() < 64 || system->GetHeight() < 64) {
        Output::Error("Maniac text picture system resource is too small for frame/colors");
        return {};
    }
    const auto background = format.Style() & 15;
    auto bitmap = CreateManiacWindow(static_cast<int>(width), static_cast<int>(height), *system,
        background == 1 ? lcf::rpg::System::Stretch_stretch
            : background == 2 ? lcf::rpg::System::Stretch_tiled : lcf::rpg::System::Stretch_easyrpg_none,
        !(format.Style() & 0x10));
    Point color{2, 50};
    int64_t y = margin_y;
    const auto icons = Cache::Exfont();
    for (const auto& row : parsed.rows) {
        int64_t x = margin_x;
        for (const auto& token : row) {
            if (token.kind == Token::ColorChange) {
                Cell(*system, token.values, 16, true, color); // Bad coordinates retain color.
                continue;
            }
            if (token.kind == Token::Icon) {
                Point cell;
                if (Cell(*icons, token.values, 12, false, cell)) {
                    const Rect icon(cell.x, cell.y, 12, 12);
                    font->RenderManiacTextGlyph(*bitmap, static_cast<int>(x), static_cast<int>(y), *system, color, 0, &icon);
                }
                x += size + format.LetterSpacing();
                continue;
            }
            const auto unit = static_cast<char32_t>(token.unit);
            const int advance = font->GetSize(unit).width;
            font->RenderManiacTextGlyph(*bitmap, static_cast<int>(x), static_cast<int>(y), *system, color, unit);
            x += advance + format.LetterSpacing();
        }
        y += size + format.LineSpacing();
    }
    return bitmap;
}

void Operation::Start() {
    std::vector<FileRequestAsync*> pending;
    auto weak = weak_from_this();
    const auto request = [&](std::string_view directory, const std::string& name, bool graphic) {
        if (name.empty()) return;
        auto* file = AsyncHandler::RequestFile(directory, name);
        if (file->IsReady()) return;
        file->SetImportantFile(true);
        file->SetGraphicFile(graphic);
        requests.push_back(file->Bind([weak](FileRequestResult* result) {
            if (auto operation = weak.lock()) operation->OnReady(result);
        }));
        pending.push_back(file);
    };
    const auto& name = parsed.font;
    if (format.Style() & 0x80) request("Font", name + "-Bold", false);
    request("Font", name + "-Regular", false);
    request("Font", name, false);
    request("System", parsed.system, true);
    if (pending.empty()) { Render(); return; }
    for (auto* file : pending) file->Start();
}
void Operation::OnReady(FileRequestResult* result) {
    if (cancelled) return;
    const auto found = std::find_if(requests.begin(), requests.end(), [&](const auto& value) { return *value == result->request_id; });
    if (found == requests.end()) return;
    requests.erase(found);
    if (requests.empty()) Render();
}
void Operation::Render() {
    if (cancelled) return;
    bitmap = ManiacTextPicture::Render(parsed, format);
    ready = true;
    if (restoring && bitmap) if (auto pic = picture.lock()) pic->AttachManiacTextBitmap(bitmap);
}
void Operation::Cancel() {
    cancelled = true;
    requests.clear();
}
}
