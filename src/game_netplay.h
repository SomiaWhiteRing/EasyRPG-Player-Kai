#ifndef EP_GAME_NETPLAY_H
#define EP_GAME_NETPLAY_H

#include <string_view>
#include <utility>

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
	std::pair<int, int> PictureTransform(int id);
	bool QueueAudio(const lcf::rpg::Music& music);
	bool QueueAudio(const lcf::rpg::Sound& sound);
	bool QueueAudio(int fadeout);
}

#endif
