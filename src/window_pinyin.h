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

#ifndef EP_WINDOW_PINYIN_H
#define EP_WINDOW_PINYIN_H

#include <string>
#include <vector>

#include "window_base.h"

class Window_Pinyin : public Window_Base {
public:
	Window_Pinyin(int ix, int iy, int iwidth = 256, int iheight = 48);

	void SetInput(std::string query, std::vector<std::string> candidates);
	void SetCommitError(bool error);
	void Clear();
	void MovePage(int delta);
	void SetSelection(int index);
	bool SetVisibleSelection(int slot);
	int GetSelection() const;
	bool HasSelection() const;
	const std::string& GetSelectedCandidate() const;
	void Refresh();

private:
	static constexpr int kVisibleCandidates = 4;

	int GetFirstVisibleIndex() const;
	int GetPage() const;
	int GetPageCount() const;
	void RebuildPages();

	std::string query;
	std::vector<std::string> candidates;
	std::vector<int> page_starts;
	int index = 0;
	bool commit_error = false;
};

#endif
