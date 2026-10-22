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

#include "pinyin_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>

#include "generated/pinyin_simp_dict.h"

namespace {
	constexpr size_t kMaxCompositeCandidates = 96;
	constexpr size_t kMaxSegmentations = 12;
	constexpr size_t kMaxSegments = 6;

	struct DictEntry {
		std::string key;
		std::string_view word;
		std::string_view spelling;
		int weight = 0;
	};

	struct CompositeCandidate {
		std::string text;
		double score = 0.0;
	};

	struct PinyinIndex {
		std::vector<DictEntry> entries;
		std::unordered_set<std::string> syllables;
	};

	std::string NormalizeKey(std::string_view text) {
		std::string normalized;
		normalized.reserve(text.size());

		for (auto c: text) {
			auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (lower >= 'a' && lower <= 'z') {
				normalized.push_back(lower);
			}
		}

		// The dictionary spells these syllables as lue/nue, while v is the
		// keyboard spelling of u-umlaut in lv/nv and lve/nve.
		for (size_t i = 1; i + 1 < normalized.size(); ++i) {
			if (normalized[i] == 'v' && normalized[i + 1] == 'e'
					&& (normalized[i - 1] == 'l' || normalized[i - 1] == 'n')) {
				normalized[i] = 'u';
			}
		}
		return normalized;
	}

	int ParseWeight(std::string_view text) {
		int result = 0;
		for (auto c: text) {
			if (c < '0' || c > '9') {
				break;
			}
			result = result * 10 + (c - '0');
		}
		return result;
	}

	void AddSyllables(PinyinIndex& index, std::string_view spelling) {
		size_t pos = 0;
		while (pos < spelling.size()) {
			while (pos < spelling.size() && spelling[pos] == ' ') {
				++pos;
			}

			const auto begin = pos;
			while (pos < spelling.size() && spelling[pos] != ' ') {
				++pos;
			}

			auto syllable = NormalizeKey(spelling.substr(begin, pos - begin));
			if (!syllable.empty()) {
				index.syllables.insert(std::move(syllable));
			}
		}
	}

	PinyinIndex BuildIndex() {
		PinyinIndex index;

		const auto dict = PinyinInputData::kPinyinSimpDict;
		size_t pos = 0;
		while (pos < dict.size()) {
			const auto end = dict.find('\n', pos);
			auto line = dict.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}

			const auto first_tab = line.find('\t');
			const auto second_tab = first_tab == std::string_view::npos ? std::string_view::npos : line.find('\t', first_tab + 1);
			if (second_tab != std::string_view::npos) {
				const auto word = line.substr(0, first_tab);
				const auto spelling = line.substr(first_tab + 1, second_tab - first_tab - 1);
				auto key = NormalizeKey(spelling);
				if (!key.empty()) {
					index.entries.push_back({ std::move(key), word, spelling, ParseWeight(line.substr(second_tab + 1)) });
					AddSyllables(index, spelling);
				}
			}

			if (end == std::string_view::npos) {
				break;
			}
			pos = end + 1;
		}

		std::sort(index.entries.begin(), index.entries.end(), [](const DictEntry& lhs, const DictEntry& rhs) {
			if (lhs.key != rhs.key) {
				return lhs.key < rhs.key;
			}
			if (lhs.weight != rhs.weight) {
				return lhs.weight > rhs.weight;
			}
			return lhs.word < rhs.word;
		});

		return index;
	}

	const PinyinIndex& GetIndex() {
		static const PinyinIndex index = BuildIndex();
		return index;
	}

	std::pair<std::vector<DictEntry>::const_iterator, std::vector<DictEntry>::const_iterator> FindRange(const PinyinIndex& index, const std::string& key) {
		auto lower = std::lower_bound(index.entries.begin(), index.entries.end(), key, [](const DictEntry& entry, const std::string& value) {
			return entry.key < value;
		});
		auto upper = std::upper_bound(lower, index.entries.end(), key, [](const std::string& value, const DictEntry& entry) {
			return value < entry.key;
		});
		return { lower, upper };
	}

	bool HasDelimiter(std::string_view query) {
		return query.find('\'') != std::string_view::npos;
	}

	bool MatchesBoundaries(std::string_view spelling, std::string_view query) {
		// An explicit separator must coincide with a dictionary syllable
		// boundary: xi'an must not also return the one-syllable xian entries.
		size_t pos = 0;
		for (auto c: spelling) {
			if (c == ' ') {
				if (pos < query.size() && query[pos] == '\'') ++pos;
			} else {
				if (pos >= query.size() || query[pos] == '\'') return false;
				++pos;
			}
		}
		return pos == query.size();
	}

	std::vector<std::string> SplitDelimitedQuery(std::string_view query) {
		std::vector<std::string> segments;

		size_t pos = 0;
		while (pos <= query.size()) {
			const auto end = query.find('\'', pos);
			auto segment = NormalizeKey(query.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
			if (segment.empty()) {
				return {};
			}
			segments.push_back(std::move(segment));
			if (end == std::string_view::npos) {
				break;
			}
			pos = end + 1;
		}

		return segments;
	}

	std::vector<std::vector<std::string>> SegmentQuery(std::string_view query, const PinyinIndex& index) {
		std::vector<std::vector<std::string>> result;

		if (HasDelimiter(query)) {
			auto segments = SplitDelimitedQuery(query);
			if (!segments.empty() && std::all_of(segments.begin(), segments.end(), [&index](const auto& segment) {
					return index.syllables.count(segment) > 0;
				})) {
				result.push_back(std::move(segments));
			}
			return result;
		}

		const auto normalized = NormalizeKey(query);
		std::vector<std::string> path;
		std::function<void(size_t)> dfs = [&](size_t pos) {
			if (result.size() >= kMaxSegmentations || path.size() > kMaxSegments) {
				return;
			}
			if (pos == normalized.size()) {
				if (path.size() > 1) {
					result.push_back(path);
				}
				return;
			}

			const auto remaining = normalized.size() - pos;
			const auto max_len = std::min<size_t>(6, remaining);
			for (size_t len = max_len; len >= 1; --len) {
				auto syllable = normalized.substr(pos, len);
				if (index.syllables.count(syllable) > 0) {
					path.push_back(std::move(syllable));
					dfs(pos + len);
					path.pop_back();
				}
				if (len == 1) {
					break;
				}
			}
		};
		dfs(0);

		return result;
	}

	void AddUnique(std::vector<std::string>& out, std::unordered_set<std::string>& seen, std::string candidate) {
		if (candidate.empty() || seen.count(candidate) > 0) {
			return;
		}
		seen.insert(candidate);
		out.push_back(std::move(candidate));
	}

	std::vector<const DictEntry*> GetComponentCandidates(const PinyinIndex& index, const std::string& syllable, size_t max_count) {
		std::vector<const DictEntry*> result;
		auto [begin, end] = FindRange(index, syllable);
		for (auto it = begin; it != end && result.size() < max_count; ++it) {
			if (it->spelling.find(' ') == std::string_view::npos) {
				result.push_back(&*it);
			}
		}
		return result;
	}

	void AddCompositeCandidates(
		const PinyinIndex& index,
		const std::vector<std::string>& segments,
		std::vector<std::string>& out,
		std::unordered_set<std::string>& seen,
		size_t max_candidates
	) {
		if (segments.size() < 2 || segments.size() > kMaxSegments) {
			return;
		}

		const size_t per_segment = segments.size() <= 3 ? 8 : 4;
		std::vector<std::vector<const DictEntry*>> components;
		components.reserve(segments.size());
		for (const auto& segment: segments) {
			auto candidates = GetComponentCandidates(index, segment, per_segment);
			if (candidates.empty()) {
				return;
			}
			components.push_back(std::move(candidates));
		}

		std::vector<CompositeCandidate> built;
		std::string current;
		std::function<void(size_t, double)> dfs = [&](size_t component_index, double score) {
			if (component_index == components.size()) {
				built.push_back({ current, score });
				return;
			}

			const auto old_size = current.size();
			for (const auto* entry: components[component_index]) {
				current.append(entry->word);
				dfs(component_index + 1, score + std::log(static_cast<double>(entry->weight) + 1.0));
				current.resize(old_size);
			}
		};
		dfs(0, 0.0);

		std::sort(built.begin(), built.end(), [](const CompositeCandidate& lhs, const CompositeCandidate& rhs) {
			if (lhs.score != rhs.score) {
				return lhs.score > rhs.score;
			}
			return lhs.text < rhs.text;
		});

		for (auto& candidate: built) {
			if (out.size() >= max_candidates) {
				return;
			}
			AddUnique(out, seen, std::move(candidate.text));
		}
	}
}

bool PinyinInput::IsValidInputChar(char c) {
	c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return (c >= 'a' && c <= 'z') || c == '\'';
}

std::vector<std::string> PinyinInput::Lookup(std::string_view query) {
	if (query.empty()) {
		return {};
	}
	if (query.size() > kMaxQueryLength || query.front() == '\'' || query.back() == '\''
			|| query.find("''") != std::string_view::npos
			|| !std::all_of(query.begin(), query.end(), IsValidInputChar)) {
		return {};
	}

	const auto& index = GetIndex();
	const auto key = NormalizeKey(query);
	if (key.empty()) {
		return {};
	}

	std::vector<std::string> result;
	std::unordered_set<std::string> seen;

	auto [begin, end] = FindRange(index, key);
	for (auto it = begin; it != end; ++it) {
		if (!HasDelimiter(query) || MatchesBoundaries(it->spelling, query)) {
			AddUnique(result, seen, std::string(it->word));
		}
	}

	const auto max_candidates = result.size() + kMaxCompositeCandidates;
	for (const auto& segments: SegmentQuery(query, index)) {
		if (result.size() >= max_candidates) break;
		AddCompositeCandidates(index, segments, result, seen, max_candidates);
	}

	return result;
}
