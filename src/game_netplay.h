#ifndef EP_GAME_NETPLAY_H
#define EP_GAME_NETPLAY_H

#include <optional>
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
	bool HidesLivePicture(int id);
	struct PictureView {
		int reflection = 0;
		int offset_x = 0;
		int offset_y = 0;
		int confirmed_offset_x = 0;
		int confirmed_offset_y = 0;
		bool fallback = false;
		bool draw_live = false;
		bool per_pose = false;
		bool unseen_fallback = false;
	};
	bool UsesConfirmedPicture(int id, const PictureView* captured_view = nullptr, int birth = -1);
	PictureView PictureTransform(int id, const PictureView* captured_view = nullptr);
	enum class PictureContinuation { Capture, Move, Remove };
	void ContinuePicture(int id, PictureContinuation action, int x = 0, int y = 0, int angle = 0);
	int ProjectionSample(int id, bool shown);
	std::pair<int, int> UpcomingSample(int id, int related);
	std::optional<std::pair<int, int>> ProjectionPosition(int id, int source);
	void ProjectPictures(int first, int last, int source, int x, int y, int shown_x, int shown_y);
	void RecordPictureDraw(int id, int x, int y, bool immediate);
	bool QueueAudio(const lcf::rpg::Music& music);
	bool QueueAudio(const lcf::rpg::Sound& sound);
	bool QueueAudio(int fadeout);
}

#endif
