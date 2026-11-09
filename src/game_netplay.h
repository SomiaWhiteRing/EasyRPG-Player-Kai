#ifndef EP_GAME_NETPLAY_H
#define EP_GAME_NETPLAY_H

#include <string_view>

namespace Game_Netplay {
	// Called only for an explicitly enabled project.
	void Command(std::string_view comment);
	void Close();
}

#endif
