/* EasyRPG Player, GPL v3 or later. Shared Maniac UTF-16 numeric cursor.
 * Numeric consumption follows the retained native cursor contracts.
 */
#ifndef EP_MANIAC_TEXT_CURSOR_H
#define EP_MANIAC_TEXT_CURSOR_H
#include "game_interpreter_shared.h"
#include "game_actor.h"
#include "game_actors.h"
#include "game_party.h"
#include "main_data.h"
#include "output.h"
#include "utils.h"
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
class ManiacTextCursor {
public:
    ManiacTextCursor(const Game_BaseInterpreterContext& context, std::u16string_view text, uint8_t flags)
        : context(context), text(text), flags(flags) {}
static bool Space(char16_t unit) {
	// Shipped CRT table below 256; current reference Windows GetStringTypeW
	// above it. Keep the recorded Windows Unicode-version boundary explicit.
	return (unit >= u'\t' && unit <= u'\r') || unit == u' ' || unit == 0x85 || unit == 0xa0 ||
		unit == 0x1680 || unit == 0x180e || (unit >= 0x2000 && unit <= 0x200a) ||
		unit == 0x2028 || unit == 0x2029 || unit == 0x202f || unit == 0x205f || unit == 0x3000;
}
static std::pair<int32_t, size_t> ParseInteger(std::u16string_view text, bool hex = false) {
	size_t position = 0;
	while (position < text.size() && Space(text[position])) ++position;
	const bool negative = position < text.size() && text[position] == u'-';
	if (position < text.size() && (text[position] == u'-' || text[position] == u'+')) ++position;
	const auto digit = [&](char16_t unit) -> unsigned {
		// The shipped CRT explicitly accepts these BMP decimal digit blocks,
		// including fullwidth digits, independently of the selected radix.
		for (const char16_t first : {char16_t(0x30), char16_t(0x660), char16_t(0x6f0),
			char16_t(0x966), char16_t(0x9e6), char16_t(0xa66), char16_t(0xae6),
			char16_t(0xb66), char16_t(0xc66), char16_t(0xce6), char16_t(0xd66),
			char16_t(0xe50), char16_t(0xed0), char16_t(0xf20), char16_t(0x1040),
			char16_t(0x17e0), char16_t(0x1810), char16_t(0xff10)})
			if (unit >= first && unit - first < 10) return unit - first;
		if (hex && unit >= u'a' && unit <= u'f') return unit - u'a' + 10;
		if (hex && unit >= u'A' && unit <= u'F') return unit - u'A' + 10;
		return 16;
	};
	const unsigned radix = hex ? 16 : 10;
	if (hex && position + 1 < text.size() && digit(text[position]) == 0 &&
		(text[position + 1] == u'x' || text[position + 1] == u'X'))
		position += 2;
	const size_t first = position;
	const uint64_t limit = uint64_t(std::numeric_limits<int32_t>::max()) + negative;
	uint64_t value = 0;
	while (position < text.size() && digit(text[position]) < radix) {
		value = std::min(limit, value * radix + digit(text[position++]));
	}
	if (position == first) return {0, 0};
	return {static_cast<int32_t>(negative ? -int64_t(value) : int64_t(value)), position};
}
	char16_t At(int64_t index) const {
		return index >= 0 && index < int64_t(text.size()) ? text[size_t(index)] : char16_t(0);
	}
	static char16_t Upper(char16_t unit) {
		return unit >= u'a' && unit <= u'z' ? unit - 32 : unit;
	}
	static int32_t Negate(int32_t value) { return static_cast<int32_t>(uint32_t(0) - uint32_t(value)); }
	static bool ParameterSpace(char16_t unit) { return unit == u' ' || unit == 0x3000; }
	void SkipSpace() {
		while (position < int64_t(text.size()) && ParameterSpace(At(position))) ++position;
	}
	std::u16string ReadString(int32_t id) const {
		const auto value = Utils::DecodeUTF16(context.ReadString(id), true);
		return value.substr(0, value.find(char16_t(0)));
	}
	std::u16string ReadActor(int32_t id) const {
		Game_Actor* actor = nullptr;
		if (id == 0) actor = Main_Data::game_party ? Main_Data::game_party->GetActor(0) : nullptr;
		else {
			actor = Main_Data::game_actors ? Main_Data::game_actors->GetActor(id) : nullptr;
			if (!actor) Output::Error("Invalid Maniac text actor ID {}", id);
		}
		const auto value = actor ? Utils::DecodeUTF16(actor->GetName(), true) : std::u16string{};
		return value.substr(0, value.find(char16_t(0)));
	}
	bool Prepare(bool& bracket) {
		bracket = false;
		if (flags & 1) {
			if (position >= int64_t(text.size()) || At(position) != u'[') return false;
			bracket = true; // The native bit0 path deliberately does not consume '['.
		} else {
			if (flags & 4) {
				if (position >= int64_t(text.size()) || ParameterSpace(At(position))) return false;
			} else SkipSpace();
			if (position >= int64_t(text.size())) return false;
			bracket = At(position) == u'[';
			if (bracket || At(position) == u':') ++position;
		}
		SkipSpace();
		if (bracket) self_parameter = false;
		return position < int64_t(text.size());
	}
	int SelfPrefix() {
		if (position + 5 < int64_t(text.size()) && Upper(At(position + 2)) == u'E' &&
			Upper(At(position + 3)) == u'L' && Upper(At(position + 4)) == u'F') {
			const char16_t suffix = Upper(At(position + 5));
			if (suffix == u'V' || suffix == u'T' || suffix == u'S') {
				position += 6;
				return suffix == u'V' ? 1 : suffix == u'T' ? 2 : 3;
			}
		}
		return 0;
	}
	int32_t Constant(bool bracket) {
		if (position < 0 || position > int64_t(text.size())) {
			Output::Error("Maniac text parameter cursor exceeds its stored input");
			return 0;
		}
		const size_t start = size_t(position);
		if (bracket) {
			while (position < int64_t(text.size())) if (At(position++) == u']') break;
			return ParseInteger(std::u16string_view(text).substr(start)).first;
		}
		const auto parsed = ParseInteger(std::u16string_view(text).substr(start));
		position += parsed.second;
		SkipSpace();
		if (At(position) == u';') ++position;
		return parsed.first;
	}
	int32_t ReadNumber(unsigned depth = 0) {
		bool bracket;
		if (!Prepare(bracket)) return 0;
		if (depth >= 1024) {
			Output::Error("Maniac text parameter nesting exceeds the safe parser depth");
			return 0;
		}
		char16_t code = Upper(At(position));
		if (code == u'\\') {
			self_parameter = false;
			code = Upper(At(position + 1));
			if (code == u'S') {
				const int kind = SelfPrefix();
				if (kind != 1 && kind != 2) return Constant(bracket);
				code = kind == 1 ? u'V' : u'T';
				self_parameter = true;
			} else if (code == u'V' || code == u'T' || code == u'N') position += 2;
			else return Constant(bracket);
		} else if (!(flags & 2) && (code == u'V' || code == u'T' || code == u'N' || code == u'S')) {
			if (code == u'S') {
				--position; // Bare SELF uses the same detector from the previous unit.
				const int kind = SelfPrefix();
				if (kind != 1 && kind != 2) {
					++position;
					return Constant(bracket);
				}
				code = kind == 1 ? u'V' : u'T';
				self_parameter = true;
			}
			++position; // Bare SELF also skips the first unit after its prefix.
		} else return Constant(bracket);
		const bool self = self_parameter;
		int32_t id = ReadNumber(depth + 1);
		self_parameter = false;
		if (bracket && At(position) == u']') ++position;
		if (code != u'N' && self) id = Negate(id);
		if (code == u'V') return context.ReadVariable(id);
		return ParseInteger(code == u'T' ? ReadString(id) : ReadActor(id)).first;
	}

    // d2730 reads one list atom without d20d0's outer bracket preparation.
    // Even discarded third/subsequent list items perform nested reads.
    int32_t ListAtom(bool retain = true) {
        SkipSpace();
        char16_t code = Upper(At(position));
        bool self = false;
        if (code == u'\\') {
            code = Upper(At(position + 1));
            if (code == u'S') {
                const int kind = SelfPrefix();
                if (kind != 1 && kind != 2) return 0;
                self = true;
                code = kind == 1 ? u'V' : u'T';
            } else if (code == u'V' || code == u'T' || code == u'N') position += 2;
            else { ++position; return 0; }
        } else if (!(flags & 2) && (code == u'V' || code == u'T' || code == u'N' || code == u'S')) {
            if (code == u'S') {
                --position;
                const int kind = SelfPrefix();
                if (kind != 1 && kind != 2) { ++position; return 0; }
                self = true;
                code = kind == 1 ? u'V' : u'T';
            } else ++position;
        } else {
            const auto parsed = ParseInteger(text.substr(static_cast<size_t>(position)));
            position += parsed.second;
            SkipSpace();
            return parsed.first;
        }
        int32_t id = ReadNumber();
        if (!retain) return 0; // Native nullptr output skips the final read.
        if (self && code != u'N') id = Negate(id);
        if (code == u'V') return context.ReadVariable(id);
        return ParseInteger(code == u'T' ? ReadString(id) : ReadActor(id)).first;
    }
    bool ListSeparator() {
        SkipSpace();
        if (At(position) != u',' && At(position) != u':') return false;
        ++position;
        SkipSpace();
        return true;
    }
    std::array<int32_t, 2> ReadList() {
        std::array<int32_t, 2> result{{-1, -1}};
        bool bracket;
        if (!Prepare(bracket)) return result;
        size_t item = 0;
        do {
            const int32_t value = ListAtom(item < 2);
            if (item < 2) result[item] = value;
            ++item;
        } while (position < static_cast<int64_t>(text.size()) && ListSeparator());
        if (bracket) {
            // Delimiters 1/2 are ordinary units inside a numeric bracket.
            while (position < static_cast<int64_t>(text.size())) if (At(position++) == u']') break;
        } else if (At(position) == u';') ++position;
        return result;
    }
    const Game_BaseInterpreterContext& context;
    std::u16string_view text;
    uint8_t flags;
    int64_t position = 0;
    bool self_parameter = false;
};
#endif
