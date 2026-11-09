#ifndef EP_GAME_NETPLAY_H
#define EP_GAME_NETPLAY_H

#include <string_view>
#include <utility>

class MapUpdateAsyncContext;

namespace lcf::rpg {
	class Music;
	class Sound;
}

namespace Game_Netplay {
	// Called only for an explicitly enabled project.
	void Command(std::string_view comment);
	void Close();
	struct TransferFade {
		int erase = 0;
		int show = 0;
		int delay = 0;
	};
	TransferFade TakeTransferFade();
	bool PreparesMap(int map_id);
	bool UpdatePreparation(MapUpdateAsyncContext& actx);
	bool UpdateMap();
	bool UsesConfirmedPicture(int id);
	bool HidesLivePicture(int id);
	struct PictureView {
		int reflection = 0;
		int offset_x = 0;
		int offset_y = 0;
		int confirmed_offset_x = 0;
		int confirmed_offset_y = 0;
		bool fallback = false;
	};
	PictureView PictureTransform(int id);
	void RecordPictureDraw(int id, int x, int y);
	bool QueueAudio(const lcf::rpg::Music& music);
	bool QueueAudio(const lcf::rpg::Sound& sound);
	bool QueueAudio(int fadeout);
}

#endif
