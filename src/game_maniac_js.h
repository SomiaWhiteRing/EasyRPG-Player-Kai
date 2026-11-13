#ifndef EP_GAME_MANIAC_JS_H
#define EP_GAME_MANIAC_JS_H

#include "string_view.h"
#include "span.h"
#include <cstdint>
#include <array>
#include <vector>

class Game_BaseInterpreterContext;
class Bitmap;

namespace Game_ManiacJs {
	struct InputFramePlan {
		bool restart = false;
		bool silent = false;
		bool close = false;
		std::array<int, 3> transfer{};
		std::vector<std::array<int, 2>> inputs;
	};
	void Execute(std::string_view source, Span<const int32_t> parameters,
		const Game_BaseInterpreterContext& caller);
	void DrawNetplayOverlay(Bitmap& destination, bool above_windows);
	void RunNetplayProjection(int seat, bool stopping);
	bool HasInputFrameDriver();
	InputFramePlan ReadInputFramePlan(int next, bool stopped);
	int TakeNumberInputInitialValue();
	void Reset();
}

#endif
