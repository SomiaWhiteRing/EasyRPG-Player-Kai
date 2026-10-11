/* Preserve Player string bytes independently of the legacy project codepage. */
#ifndef EP_MANIAC_STRING_STORAGE_H
#define EP_MANIAC_STRING_STORAGE_H

#include <lcf/dbstring.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ManiacStringStorage {
inline std::vector<uint8_t> Encode(const std::vector<lcf::DBString>& values) {
	std::vector<uint8_t> bytes;
	const auto put = [&](uint64_t value) {
		for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
	};
	put(values.size());
	for (const auto& value : values) {
		const std::string_view text = value;
		put(text.size());
		bytes.insert(bytes.end(), text.begin(), text.end());
	}
	return bytes;
}

inline bool Decode(const std::vector<uint8_t>& bytes, std::vector<lcf::DBString>& values) {
	size_t position = 0;
	const auto read = [&](uint64_t& value) {
		if (bytes.size() - position < 8) return false;
		value = 0;
		for (int i = 0; i < 8; ++i) value |= uint64_t(bytes[position++]) << (i * 8);
		return true;
	};
	uint64_t count;
	if (!read(count) || count > (bytes.size() - position) / 8) return false;
	std::vector<lcf::DBString> decoded;
	decoded.reserve(static_cast<size_t>(count));
	for (uint64_t i = 0; i < count; ++i) {
		uint64_t size;
		if (!read(size) || size > bytes.size() - position) return false;
		decoded.emplace_back(std::string(reinterpret_cast<const char*>(bytes.data() + position), static_cast<size_t>(size)));
		position += static_cast<size_t>(size);
	}
	if (position != bytes.size()) return false;
	values = std::move(decoded);
	return true;
}
}
#endif
