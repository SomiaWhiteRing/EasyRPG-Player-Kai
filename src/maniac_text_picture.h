/* EasyRPG Player, GPL v3 or later. Maniac 250921 text pictures. */
#ifndef EP_MANIAC_TEXT_PICTURE_H
#define EP_MANIAC_TEXT_PICTURE_H
#include "game_pictures.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

class Game_Interpreter;
namespace ManiacTextPicture {
struct Token {
    enum Kind { Glyph, ColorChange, Icon } kind = Glyph;
    char16_t unit = 0;
    std::array<int32_t, 2> values{{-1, -1}};
};
using Row = std::vector<Token>;
struct Parsed {
    std::vector<Row> rows;
    std::u16string canonical;
    std::string system;
    std::string font;
};
struct Format {
    uint32_t flags = 0;
    uint32_t font = 1;
    uint32_t spacing = 0;
    int32_t width = 0;
    int32_t height = 0;
    int FontSize() const { return font & 255; }
    int LetterSpacing() const { return (spacing >> 8) & 255; }
    int LineSpacing() const { return (spacing >> 16) & 255; }
    uint32_t Style() const { return (flags >> 8) & 0xffff; }
};
struct Operation : std::enable_shared_from_this<Operation> {
    std::weak_ptr<Game_Pictures::Picture> picture;
    Parsed parsed;
    Format format;
    BitmapRef bitmap;
    std::vector<FileRequestBinding> requests;
    // Keep the complete record until the yielding command finishes. This is
    // runtime state, never normalized back into its source EventCommand.
    std::vector<int32_t> arguments;
    bool ready = false;
    bool cancelled = false;
    bool restoring = false;
    void Start();
    void OnReady(FileRequestResult* result);
    void Render();
    void Cancel();
};
Parsed Parse(const Game_Interpreter& context, std::u16string text,
    int32_t body_reference, bool cached, bool resolved_tokens = false);
std::string SaveSnapshot(const Parsed& parsed);
BitmapRef Render(const Parsed& parsed, const Format& format);
std::array<int32_t, 2> OriginOffset(int32_t origin, int width, int height);
int32_t SignedBits(uint32_t bits);
}
#endif
