#include "game_netplay.h"
#include "game_map.h"
#include "game_variables.h"
#include "main_data.h"

#include <sstream>

#ifdef PLAYER_WEB
#include <emscripten.h>
#endif

void Game_Netplay::Command(std::string_view comment) {
	std::istringstream args{std::string(comment)};
	std::string prefix, command;
	args >> prefix >> command;
	if (command == "close") {
		Close();
		return;
	}

	int variable;
	auto& vars = *Main_Data::game_variables;
	if (!(args >> variable) || !vars.IsValid(variable)) return;
	const auto write = [&](int id, int value) {
		if (vars.Get(id) == value) return;
		vars.Set(id, value);
		Game_Map::SetNeedRefresh(true);
	};
	if (command == "available") {
#ifdef PLAYER_WEB
		write(variable, EM_ASM_INT({ return !!Module.netplay; }));
#else
		write(variable, 0);
#endif
	} else if (command == "state") {
#ifdef PLAYER_WEB
		write(variable, EM_ASM_INT({ return Module.netplay?.state() ?? 0; }));
#else
		write(variable, 0);
#endif
	} else if (command == "receive") {
		int received;
		if (!(args >> received) || !vars.IsValid(received) || received == variable) return;
#ifdef PLAYER_WEB
		int value = 0;
		const bool pending = EM_ASM_INT({
			const value = Module.netplay?.receive();
			if (value === undefined) return 0;
			HEAP32[$0 >> 2] = value;
			return 1;
		}, &value);
		if (pending) write(variable, value);
		write(received, pending);
#else
		write(received, 0);
#endif
	}
#ifdef PLAYER_WEB
	else if (command == "open") {
		EM_ASM({ Module.netplay?.open($0); }, vars.Get(variable));
	} else if (command == "send") {
		EM_ASM({ Module.netplay?.send($0); }, vars.Get(variable));
	}
#endif
}

void Game_Netplay::Close() {
#ifdef PLAYER_WEB
	EM_ASM({ Module.netplay?.close(); });
#endif
}
