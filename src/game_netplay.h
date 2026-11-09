#ifndef EP_GAME_NETPLAY_H
#define EP_GAME_NETPLAY_H

#include <string_view>

namespace lcf::rpg {
	class Music;
	class Sound;
}

namespace Game_Netplay {
	// Called only for an explicitly enabled project.
	void Command(std::string_view comment);
	void Close();
	bool UpdateMap();
	bool UsesConfirmedPicture(int id);
	bool QueueAudio(const lcf::rpg::Music& music);
	bool QueueAudio(const lcf::rpg::Sound& sound);
	bool QueueAudio(int fadeout);
}

#endif
