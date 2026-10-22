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

#include "window_pinyin.h"

#include <algorithm>
#include <utility>

#include "bitmap.h"
#include "font.h"
#include "utils.h"

namespace {
	constexpr int kCandidateGap = 8;

	std::string CandidateLabel(int slot, const std::string& candidate) {
		return std::to_string(slot + 1) + "." + candidate;
	}

	std::string FitText(std::string text, int max_width, bool keep_end = false) {
		if (Text::GetSize(*Font::Default(), text).width <= max_width) {
			return text;
		}
		auto chars = Utils::DecodeUTF32(text);
		while (!chars.empty()) {
			if (keep_end) chars.erase(chars.begin());
			else chars.pop_back();
			text = keep_end ? "..." + Utils::EncodeUTF(chars) : Utils::EncodeUTF(chars) + "...";
			if (Text::GetSize(*Font::Default(), text).width <= max_width) {
				return text;
			}
		}
		return {};
	}
}

Window_Pinyin::Window_Pinyin(int ix, int iy, int iwidth, int iheight)
	: Window_Base(ix, iy, iwidth, iheight) {
	SetContents(Bitmap::Create(width - 16, height - 16));
	Refresh();
}

void Window_Pinyin::SetInput(std::string nquery, std::vector<std::string> ncandidates) {
	query = std::move(nquery);
	candidates = std::move(ncandidates);
	index = 0;
	commit_error = false;
	Refresh();
}

void Window_Pinyin::SetCommitError(bool error) {
	if (commit_error == error) {
		return;
	}
	commit_error = error;
	Refresh();
}

void Window_Pinyin::Clear() {
	query.clear();
	candidates.clear();
	index = 0;
	commit_error = false;
	Refresh();
}

void Window_Pinyin::MovePage(int delta) {
	if (candidates.empty()) {
		return;
	}

	const auto page_count = GetPageCount();
	auto page = GetPage();
	page = (page + delta) % page_count;
	if (page < 0) {
		page += page_count;
	}
	index = page_starts[page];
	Refresh();
}

void Window_Pinyin::SetSelection(int nindex) {
	if (nindex < 0 || nindex >= static_cast<int>(candidates.size())) {
		return;
	}
	index = nindex;
	Refresh();
}

bool Window_Pinyin::SetVisibleSelection(int slot) {
	if (slot < 0 || slot >= kVisibleCandidates) {
		return false;
	}

	const auto candidate_index = GetFirstVisibleIndex() + slot;
	if (candidates.empty() || candidate_index >= page_starts[GetPage() + 1]) {
		return false;
	}

	SetSelection(candidate_index);
	return true;
}

int Window_Pinyin::GetSelection() const {
	return index;
}

bool Window_Pinyin::HasSelection() const {
	return !candidates.empty();
}

const std::string& Window_Pinyin::GetSelectedCandidate() const {
	static const std::string empty;
	if (!HasSelection()) {
		return empty;
	}
	return candidates[index];
}

int Window_Pinyin::GetFirstVisibleIndex() const {
	return candidates.empty() ? 0 : page_starts[GetPage()];
}

int Window_Pinyin::GetPage() const {
	return static_cast<int>(std::upper_bound(page_starts.begin(), page_starts.end(), index) - page_starts.begin()) - 1;
}

int Window_Pinyin::GetPageCount() const {
	if (candidates.empty()) {
		return 0;
	}
	return static_cast<int>(page_starts.size()) - 1;
}

void Window_Pinyin::RebuildPages() {
	page_starts.clear();
	int first = 0;
	while (first < static_cast<int>(candidates.size())) {
		page_starts.push_back(first);
		int used_width = 0;
		int count = 0;
		while (first + count < static_cast<int>(candidates.size()) && count < kVisibleCandidates) {
			const int item_width = Text::GetSize(*Font::Default(), CandidateLabel(count, candidates[first + count])).width;
			const int gap = count == 0 ? 0 : kCandidateGap;
			if (count > 0 && used_width + gap + item_width > contents->GetWidth()) {
				break;
			}
			used_width += gap + item_width;
			++count;
		}
		first += count;
	}
	page_starts.push_back(first);
}

void Window_Pinyin::Refresh() {
	RebuildPages();
	contents->Clear();
	SetCursorRect(Rect());

	std::string page;
	if (!candidates.empty()) {
		page = std::to_string(GetPage() + 1) + "/" + std::to_string(GetPageCount());
	}
	const int page_width = Text::GetSize(*Font::Default(), page).width;
	int query_right = contents->GetWidth() - page_width - kCandidateGap;
	if (commit_error) {
		const std::string error = "名字空间不足";
		const int error_width = Text::GetSize(*Font::Default(), error).width;
		query_right -= error_width;
		contents->TextDraw(query_right, 0, Font::ColorCritical, error);
		query_right -= kCandidateGap;
	}
	const std::string label = "拼音:";
	const int label_width = Text::GetSize(*Font::Default(), label).width;
	contents->TextDraw(0, 0, Font::ColorDefault, label);
	contents->TextDraw(label_width, 0, Font::ColorDefault,
		FitText(query.empty() ? "_" : query, query_right - label_width, true));
	contents->TextDraw(contents->GetWidth() - page_width, 0, Font::ColorDefault, page);

	int x = 0;
	const int first = GetFirstVisibleIndex();
	const int count = candidates.empty() ? 0 : page_starts[GetPage() + 1] - first;
	for (int i = 0; i < count; ++i) {
		const int candidate_index = first + i;
		const auto item = FitText(CandidateLabel(i, candidates[candidate_index]), contents->GetWidth() - x);

		const int item_width = Text::GetSize(*Font::Default(), item).width;
		if (candidate_index == index) {
			SetCursorRect(Rect(x, 14, item_width, 16));
		}
		contents->TextDraw(x, 16, Font::ColorDefault, item);
		x += item_width + kCandidateGap;
	}
	if (!query.empty() && candidates.empty()) {
		contents->TextDraw(0, 16, Font::ColorDisabled, "无候选，请修改拼音");
	}
}
