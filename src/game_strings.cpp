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
#include <regex>
#include <lcf/encoder.h>
#include <lcf/reader_util.h>
#include "async_handler.h"
#include "game_map.h"
#include "game_message.h"
#include "game_strings.h"
#include "game_switches.h"
#include "game_interpreter_shared.h"
#include <cctype>
#include <cstring>
#include "game_variables.h"
#include "output.h"
#include "player.h"
#include "utils.h"

#ifdef HAVE_NLOHMANN_JSON
#include "json_helper.h"
#endif

void Game_Strings::WarnGet(int id) const {
	Output::Debug("Invalid read strvar[{}]!", id);
	--_warnings;
}

#ifdef HAVE_NLOHMANN_JSON
nlohmann::ordered_json* Game_Strings::ParseJson(int id) {
	auto it = _json_cache.find(id);
	if (it != _json_cache.end()) {
		return &(it->second);
	}

	auto str = ToString(Get(id));
	auto res = Json_Helper::Parse(str);

	if (!res) {
		return nullptr;
	} else {
		_json_cache[id] = *res;
		return &_json_cache[id];
	}
}
#endif

std::string_view Game_Strings::Asg(Str_Params params, std::string_view string) {
	Set(params, string);
	return Get(params.string_id);
}

std::string_view Game_Strings::Cat(Str_Params params, std::string_view string) {
	if (params.string_id <= 0) {
		return {};
	}

	if (!IsPresent(params.string_id)) {
		Set(params, string);
		return Get(params.string_id);
	}
	*_strings[params.string_id - 1] += ToString(string);
	return *_strings[params.string_id - 1];
}

int Game_Strings::ToNum(Str_Params params, int var_id, Game_Variables& variables) {
	if (params.string_id <= 0) {
		return -1;
	}

	if (!IsPresent(params.string_id)) {
		return 0;
	}

	int num;
	if (params.hex)
		num = static_cast<int>(std::strtol(_strings[params.string_id - 1]->c_str(), nullptr, 16));
	else
		num = static_cast<int>(std::strtol(_strings[params.string_id - 1]->c_str(), nullptr, 10));

	variables.Set(var_id, num);

	Game_Map::SetNeedRefreshForVarChange(var_id);

	return num;
}

int Game_Strings::GetLen(Str_Params params, int var_id, Game_Variables& variables) const {
	if (params.string_id <= 0) {
		return -1;
	}

	int len = Utils::UTF8Length(Get(params.string_id));
	variables.Set(var_id, len);

	Game_Map::SetNeedRefreshForVarChange(var_id);

	return len;
}

int Game_Strings::InStr(Str_Params params, std::string search, int var_id, int begin, Game_Variables& variables) const {
	if (params.string_id <= 0) {
		return -1;
	}

	if (params.extract) {
		search = Extract(search, params.hex);
	}

	auto search32 = Utils::DecodeUTF32(search);
	auto string32 = Utils::DecodeUTF32(Get(params.string_id));

	int index = string32.find(search32, begin);
	variables.Set(var_id, index);

	Game_Map::SetNeedRefreshForVarChange(var_id);

	return index;
}

int Game_Strings::Split(Str_Params params, const std::string& delimiter, int string_out_id, int var_id, Game_Variables& variables) {
	if (params.string_id <= 0) {
		return -1;
	}

	std::string str = ToString(Get(params.string_id));

	params.string_id = string_out_id;

	int components = 0;

	if (delimiter.empty()) {
		// Count the characters (or the codepoints in our case)
		components = 0;
		const char* iter = str.data();
		const auto end = str.data() + str.size();

		while (iter != end) {
			const char* start_copy = iter;
			auto ret = Utils::UTF8Next(iter, end);
			iter = ret.next;

			if (iter == end) {
				break;
			}

			Set(params, std::string(start_copy, iter - start_copy));

			params.string_id++;
			components++;
		}
	} else {
		components = 1;

		if (str.find(delimiter) == std::string::npos) {
			// token not found
		} else {
			// This works for UTF-8
			std::string token;
			for (auto index = str.find(delimiter); index != std::string::npos; index = str.find(delimiter)) {
				token = str.substr(0, index);
				Set(params, token);
				params.string_id++;
				components++;
				str.erase(0, index + delimiter.length());
			}
		}
	}

	// set the remaining string
	Set(params, str);
	variables.Set(var_id, components);

	Game_Map::SetNeedRefreshForVarChange(var_id);

	return components;
}

std::string Game_Strings::FromFile(std::string_view filename, int encoding, bool& do_yield) {
	do_yield = false;
	const bool native250921 = Player::IsPatchManiac()
		&& Player::game_config.maniac_version.Get() == 250921;
	if (native250921 && (encoding < 0 || encoding > 2)) {
		return {};
	}

	Filesystem_Stream::InputStream is = FileFinder::OpenText(filename);
	if (!is) {
		// Emscripten: Try to async fetch the file
		auto* request = AsyncHandler::RequestFile("Text", filename);
		request->SetImportantFile(true);
		request->Start();
		do_yield = !request->IsReady();

		return {};
	}

	auto vec = Utils::ReadStream(is);
	std::string file_content(vec.begin(), vec.end());
	const bool native_utf8 = native250921 && encoding == 1;
	if (native250921) {
		// Native filesystem text mode handles CRLF and DOS EOF before the
		// terminated ANSI/UTF8 conversion. Standalone CR and BOM are retained.
		const size_t zero = std::min(file_content.find('\0'), file_content.find('\x1a'));
		if (zero != std::string::npos) file_content.resize(zero);
		file_content = Utils::ReplaceAll(file_content, "\r\n", "\n");
	}
	if (native250921 && encoding == 2) {
		// The fixed runtime's ordinary wide stream uses the C locale facet:
		// each unsigned input byte becomes one wide unit after CRT text handling.
		std::u16string units;
		units.reserve(file_content.size());
		for (unsigned char byte : file_content) units.push_back(byte);
		return Utils::EncodeUTF(units, true);
	}

	if (encoding == 0) {
		lcf::Encoder enc(Player::encoding);
		enc.Encode(file_content);
	} else if (!native_utf8) {
		// UTF-8: Remove Byte Order Mask
		if (file_content.size() >= 3 && file_content[0] == '\xEF' && file_content[1] == '\xBB' && file_content[2] == '\xBF') {
			file_content.erase(0, 3);
		}
	}

	return file_content;
}

bool Game_Strings::ToFile(Str_Params params, std::string filename, int encoding) {
	std::string str = ToString(Get(params.string_id));

	if (params.extract) {
		filename = Extract(filename, params.hex);
	}
	return ToFile(str, std::move(filename), encoding);
}

bool Game_Strings::ToFile(std::string_view text, std::string filename, int encoding) {
	Output::BeginManiacFileOutput();
	const bool native250921 = Player::IsPatchManiac() && Player::game_config.maniac_version.Get() == 250921;
	std::string str = ToString(text);
	if (native250921 && encoding == 1) {
		// WideCharToMultiByte UTF8 consumes a terminated UTF16 string. This
		// also combines pairs formed across the collected input strings.
		str = Utils::EncodeUTF(Utils::DecodeUTF16(text.substr(0, text.find('\0')), true));
	} else if (native250921 && encoding == 2) {
		const auto units = Utils::DecodeUTF16(text, true);
		str.resize(units.size() * 2);
		for (size_t i = 0; i < units.size(); ++i) {
			str[i * 2] = static_cast<char>(units[i] & 0xFF);
			str[i * 2 + 1] = static_cast<char>(units[i] >> 8);
		}
	}

	// Maniacs forces the File in Text/ folder with .txt extension
	filename = "Text/" + filename;

	// EasyRPG Extension: When "*" is at the end of filename, ".txt" is not appended
	if (!native250921 && Player::HasEasyRpgExtensions() && filename.back() == '*') {
		filename.pop_back();
	} else {
		filename += ".txt";
	}

	auto txt_out = FileFinder::Save().OpenOutputStream(filename);
	auto txt_dir = FileFinder::GetPathAndFilename(filename).first;

	if (!txt_out) {
		// Android's app view can expose /data/user/0 as a directory link.
		// Native text output must create its parent under the selected save root.
		if (!FileFinder::Save().MakeDirectory(txt_dir, native250921)) {
			Output::Warning("Maniac String Op ToFile failed. Cannot create directory {}", txt_dir);
			return false;
		}

		txt_out = FileFinder::Save().OpenOutputStream(filename);
		if (!txt_out) {
			Output::Warning("Maniac String Op ToFile failed. Cannot write to {}", filename);
			return false;
		}
	}

	if (encoding == 0) {
		if (native250921) {
			str.resize(str.find('\0') == std::string::npos ? str.size() : str.find('\0'));
		}
		lcf::Encoder enc(Player::encoding);
		enc.Decode(str);
		if (native250921) {
			// Native writes twice the converted byte count, including its NUL.
			// Zero-fill safely instead of reproducing its uninitialized overread.
			str.resize((str.size() + 1) * 2, '\0');
		}
	}

	txt_out << str;
	txt_out.Close();

	AsyncHandler::SaveFilesystem();

	return true;
}

std::string_view Game_Strings::PopLine(Str_Params params, int offset, int string_out_id) {
	// FIXME: consideration needed around encoding -- what mode are files read in?
	if (params.string_id <= 0) {
		return {};
	}

	const std::string source = ToString(Get(params.string_id));
	std::string result;
	size_t position = 0;
	while (offset >= 0) {
		const size_t newline = source.find('\n', position);
		if (newline == std::string::npos) {
			result = source.substr(position);
			position = source.size();
			break;
		}
		result = source.substr(position, newline - position);
		position = newline + 1;
		--offset;
	}
	Set(params, source.substr(position));

	// the maniacs implementation is to always preserve the mutated base string
	// so in the case where the out_id matches the base string id, the popped line is discarded.
	if (string_out_id != params.string_id) {
		params.string_id = string_out_id;
		Set(params, result);
	}
	return Get(params.string_id);
}

std::string_view Game_Strings::ExMatch(Str_Params params, std::string expr, int var_id, int begin, int string_out_id, Game_Variables& variables) {
	// std::regex only works with char and wchar, not char32
	// For full Unicode support requires the w-API, even on non-Windows systems
	int var_result;
	std::string str_result;

	if (params.extract) {
		expr = Extract(expr, params.hex);
	}

	auto source = Get(params.string_id);
	std::string base = Substring(source, begin, Utils::UTF8Length(source));

	std::wsmatch match;
	auto wbase = Utils::ToWideString(base);
	auto wexpr = Utils::ToWideString(expr);

	std::wregex r(wexpr);

	std::regex_search(wbase, match, r);
	str_result = Utils::FromWideString(match.str());

	var_result = match.position() + begin;
	variables.Set(var_id, var_result);
	Game_Map::SetNeedRefreshForVarChange(var_id);

	if (string_out_id > 0) {
		params.string_id = string_out_id;
		Set(params, str_result);

		return Get(params.string_id);
	}
	return {};
}

void Game_Strings::RangeOp(Str_Params params, int string_id_1, std::string string, int op, int args[], Game_Variables& variables) {
	if (EP_UNLIKELY(ShouldWarn(params.string_id))) {
		WarnGet(params.string_id);
	}
	if (EP_UNLIKELY(ShouldWarn(string_id_1))) {
		WarnGet(string_id_1);
	}
	if (params.string_id <= 0 && string_id_1 <= 0) { return; }

	// maniacs just ignores if only one of the params is <= 0
	if (params.string_id <= 0) { params.string_id = 1; }
	if (string_id_1 <= 0) {	string_id_1 = 1; }

	// swap so that id_0 is < id_1
	if (params.string_id > string_id_1) {
		std::swap(params.string_id, string_id_1);
	}

	for (int start = params.string_id; params.string_id <= string_id_1; params.string_id++) {
		switch (op) {
		case 0:  Asg(params, string); break;
		case 1:  Cat(params, string); break;
		case 2:  ToNum(params, args[0] + (params.string_id - start), variables); break;
		case 3:  GetLen(params, args[0] + (params.string_id - start), variables); break;
		case 4:  InStr(params, string, args[1], args[2], variables); break;
		case 5:  params.string_id += Split(params, string, args[1], args[2], variables); break;
		case 8:  break; // range case not applicable for popLine; see case in game_interpreter.cpp
		case 9:  ExMatch(params, string, args[1] + (params.string_id - start), args[2], -1, variables); break;
		case 10: ExMatch(params, string, args[1] + (params.string_id - start), args[2], args[3], variables); break;
		}
	}
}

std::string Game_Strings::PrependMin(std::string_view string, int min_size, char c) {
	int len = Utils::UTF8Length(string);

	if (min_size < 0) {
		// Left adjust
		min_size = abs(min_size);
		if (len < min_size) {
			int s = min_size - len;
			return ToString(string) + std::string(s, c);
		}
	} else if (len < min_size) {
		// Right adjust
		int s = min_size - len;
		return std::string(s, c) + ToString(string);
	}
	return ToString(string);
}

std::string Game_Strings::Extract(std::string_view string, bool as_hex, const Game_BaseInterpreterContext* context) {
	PendingMessage::CommandInserter cmd_fn;
	if (context) {
		Game_Message::ManiacHookCaller caller;
		if (!context->GetState().stack.empty()) {
			const auto& frame = context->GetFrame();
			caller = {frame.easyrpg_self_scope, frame.easyrpg_self_map_id, frame.easyrpg_self_event_id};
		}
		auto fallback = Game_Message::MakeCommandCodeInserter(*context);
		const bool native250921 = Player::game_config.maniac_version.Get() == 250921;
		cmd_fn = [context, caller, as_hex, fallback, native250921](char ch, const char** iter, const char* end, uint32_t escape) {
			char type = std::toupper(static_cast<unsigned char>(ch));
			bool self = false;
			if (type == 'S' && end - *iter >= 4
				&& std::toupper(static_cast<unsigned char>((*iter)[0])) == 'E'
				&& std::toupper(static_cast<unsigned char>((*iter)[1])) == 'L'
				&& std::toupper(static_cast<unsigned char>((*iter)[2])) == 'F') {
				const char kind = std::toupper(static_cast<unsigned char>((*iter)[3]));
				if (kind == 'V' || kind == 'T' || kind == 'S') {
					type = kind;
					self = kind != 'S' || !native250921;
					*iter += 4;
					if (kind == 'S' && native250921) {
						// Native SELFS detection returns3, then the ordinary S branch
						// still advances two UTF16 units and does not negate the ID.
						for (int units = 2; units > 0 && *iter != end; ) {
							const auto next = Utils::UTF8Next(*iter, end);
							*iter = next.next;
							if (next.ch > 0xFFFF && units == 1) {
								// The cursor is left on the low surrogate: number parsing
								// yields0 and the outer scan copies that remaining unit.
								const char16_t low = 0xDC00 + ((next.ch - 0x10000) & 0x3FF);
								return std::optional<std::string>("OFF" + Utils::EncodeUTF(std::u16string(1, low), true));
							}
							units -= next.ch > 0xFFFF ? 2 : 1;
						}
					}
				}
			}
			if (type != 'V' && type != 'S' && type != 'T') return fallback(ch, iter, end, escape);
			const auto parsed = Game_Message::ParseManiacNumber(*iter, end, caller);
			*iter = parsed.next;
			int32_t id = parsed.value;
			if (self) {
				const uint32_t bits = 0u - static_cast<uint32_t>(id);
				std::memcpy(&id, &bits, sizeof(id));
			}
			if (type == 'S') return std::optional<std::string>(context->ReadSwitch(id) ? "ON" : "OFF");
			if (type == 'T') {
				const auto value = context->ReadString(id);
				return std::optional<std::string>(value.substr(0, value.find('\0')));
			}
			const int32_t value = context->ReadVariable(id);
			return std::optional<std::string>(as_hex ? fmt::format("{:x}", static_cast<uint32_t>(value)) : std::to_string(value));
		};
		return PendingMessage::ApplyTextInsertingCommands(ToString(string), Player::escape_char, cmd_fn);
	}

	if (as_hex) {
		cmd_fn = ManiacsCommandInserterHex;
	} else {
		cmd_fn = ManiacsCommandInserter;
	}

	return PendingMessage::ApplyTextInsertingCommands(ToString(string), Player::escape_char, cmd_fn);
}

std::string Game_Strings::Substring(std::string_view source, int begin, int length) {
	const char* iter = source.data();
	const auto end = source.data() + source.size();

	begin = AdjustIndex(source, begin);

	if (length < 0) {
		length = 0;
	}

	// Points at start of the substring
	auto left = Utils::UTF8Skip(iter, end, begin);

	// Points at end of the substring
	auto right = Utils::UTF8Skip(left.next, end, length);

	if (right.next == nullptr) {
		return std::string(left.next, end);
	} else {
		return std::string(left.next, right.next);
	}
}

std::string Game_Strings::Insert(std::string_view source, std::string_view what, int where) {
	const char* iter = source.data();
	const auto end = source.data() + source.size();

	where = AdjustIndex(source, where);

	// Points at insertion location
	auto ret = Utils::UTF8Skip(iter, end, where);

	return std::string(source.data(), ret.next) + ToString(what) + std::string(ret.next, end);
}

std::string Game_Strings::Erase(std::string_view source, int begin, int length) {
	const char* iter = source.data();
	const auto end = source.data() + source.size();

	begin = AdjustIndex(source, begin);

	if (length < 0) {
		length = 0;
	}

	// Points at start of deletion
	auto left = Utils::UTF8Skip(iter, end, begin);

	if (left.next == nullptr) {
		return ToString(source);
	}

	// Points at end of deletion
	auto right = Utils::UTF8Skip(left.next, end, length);

	std::string ret = std::string(source.data(), left.next);
	if (right.next != nullptr) {
		ret += std::string(right.next, end);
	}

	return ret;
}

namespace {
	std::wstring ManiacRegexUnits(std::string_view text) {
		const auto units = Utils::DecodeUTF16(text.substr(0, text.find('\0')), true);
		// Native regex consumes UTF16 units, including separate surrogate halves.
		// wchar_t is 32-bit on Web/Android, so ToWideString would change the units.
		return std::wstring(units.begin(), units.end());
	}

	std::string ManiacRegexText(std::wstring_view units) {
		return Utils::EncodeUTF(std::u16string(units.begin(), units.end()), true);
	}
}

std::string Game_Strings::ManiacRegExReplace(std::string_view str, std::string_view search, std::string_view replace, std::regex_constants::match_flag_type flags) {
	try {
		const std::wregex expression(ManiacRegexUnits(search), std::regex_constants::ECMAScript);
		const auto replacement = ManiacRegexUnits(replace);
		std::wstring format;
		for (size_t index = 0; index < replacement.size(); ++index) {
			format += replacement[index];
			if (replacement[index] != L'$' || index + 1 == replacement.size()) continue;
			const auto next = replacement[++index];
			// Native keeps $0/$00 literal; libc++ formats group0 instead.
			// Consume whole dollar tokens so an escaped $$0 is never rewritten.
			if (next == L'0' && (index + 1 == replacement.size() || replacement[index + 1] < L'1' || replacement[index + 1] > L'9')) format += L'$';
			format += next;
			if (next >= L'0' && next <= L'9' && index + 1 < replacement.size()
				&& replacement[index + 1] >= L'0' && replacement[index + 1] <= L'9') format += replacement[++index];
		}
		return ManiacRegexText(std::regex_replace(ManiacRegexUnits(str), expression, format, flags));
	} catch (const std::regex_error&) {
		return {};
	}
}

int Game_Strings::ManiacRegExSearch(std::string_view str, std::string_view search, int begin, std::vector<std::string>* captures) {
	try {
		const std::wregex expression(ManiacRegexUnits(search), std::regex_constants::ECMAScript);
		const auto units = ManiacRegexUnits(str);
		const auto offset = static_cast<size_t>(std::clamp<int64_t>(begin, 0, units.size()));
		const auto suffix = units.substr(offset);
		std::wsmatch match;
		if (!std::regex_search(suffix, match, expression)) {
			if (captures) captures->emplace_back();
			return -1;
		}
		if (captures) {
			for (const auto& capture : match) captures->push_back(ManiacRegexText(capture.str()));
		}
		return static_cast<int>(offset + match.position());
	} catch (const std::regex_error&) {
		// Unlike an ordinary miss, malformed regex contributes no capture slot.
		return -1;
	}
}

std::string Game_Strings::RegExReplace(std::string_view str, std::string_view search, std::string_view replace, std::regex_constants::match_flag_type flags) {
	// std::regex only works with char and wchar, not char32
	// For full Unicode support requires the w-API, even on non-Windows systems
	auto wstr = Utils::ToWideString(str);
	auto wsearch = Utils::ToWideString(search);
	auto wreplace = Utils::ToWideString(replace);

	std::wregex rexp(wsearch);

	auto result = std::regex_replace(wstr, rexp, wreplace, flags);

	return Utils::FromWideString(result);
}

int Game_Strings::AdjustIndex(std::string_view str, int index) {
	if (index >= 0) {
		return index;
	}

	return std::max(Utils::UTF8Length(str) - abs(index), 0);
}

std::optional<std::string> Game_Strings::ManiacsCommandInserter(char ch, const char** iter, const char* end, uint32_t escape_char) {
	if (ch == 'S' || ch == 's') {
		// \s in a normal message is the speed modifier
		// parsing a switch within an extracted string var command will parse \s[N] as a switch (ON/OFF)
		auto parse_ret = Game_Message::ParseSpeed(*iter, end, escape_char, true);
		*iter = parse_ret.next;
		int value = parse_ret.value;

		return Main_Data::game_switches->Get(value) ? "ON" : "OFF";
	} else if (ch == 'T' || ch == 't') {
		auto parse_ret = Game_Message::ParseString(*iter, end, escape_char, true);
		*iter = parse_ret.next;
		int value = parse_ret.value;

		// Contrary to Messages, the content of \t[]-strings is not evaluated
		return ToString(Main_Data::game_strings->Get(value));
	}

	return Game_Message::CommandCodeInserter(ch, iter, end, escape_char);
};

std::optional<std::string> Game_Strings::ManiacsCommandInserterHex(char ch, const char** iter, const char* end, uint32_t escape_char) {
	if (ch == 'V' || ch == 'v') {
		auto parse_ret = Game_Message::ParseVariable(*iter, end, escape_char, true);
		*iter = parse_ret.next;
		int value = parse_ret.value;

		int variable_value = Main_Data::game_variables->Get(value);
		std::ostringstream ss;
		ss << std::hex << variable_value;
		return ss.str();
	}

	return ManiacsCommandInserter(ch, iter, end, escape_char);
};

int Game_Strings::GetSizeWithLimit() {
	return static_cast<int>(std::min<size_t>(std::max(_strings.size(), lcf::Data::maniac_string_variables.size()), std::numeric_limits<int>::max()));
}

std::string_view Game_Strings::GetName(int id) const {
	const auto* strvar = lcf::ReaderUtil::GetElement(lcf::Data::maniac_string_variables, id);

	if (!strvar) {
		return {};
	} else {
		return strvar->name;
	}
}
