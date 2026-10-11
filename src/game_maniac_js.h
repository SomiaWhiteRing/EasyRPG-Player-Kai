#ifndef EP_GAME_MANIAC_JS_H
#define EP_GAME_MANIAC_JS_H

#include "string_view.h"
#include "span.h"
#include <cstdint>

class Game_BaseInterpreterContext;

namespace Game_ManiacJs {
	void Execute(std::string_view source, Span<const int32_t> parameters,
		const Game_BaseInterpreterContext& caller);
	void Reset();
}

#endif
