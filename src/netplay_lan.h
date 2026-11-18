#ifndef EP_NETPLAY_LAN_H
#define EP_NETPLAY_LAN_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NetplayLan {
	bool Available();
	int State();
	int JoinResult();
	int Online();
	int Finish();
	bool TakeRouteEnded();
	int Latency();
	std::string Rooms();
	std::string Identity();
	std::string RoomSettings();
	void StopDirectory();
	void SetProfile(std::string version, std::string name, std::string avatar);
	void Open(int room, bool join, std::string_view password = {});
	void SetPassword(bool enabled, std::string_view password);
	void Send(int value);
	std::optional<int> Receive();
	void SendFrame(int match, int frame, int input);
	bool ReceiveFrame(std::array<int, 3>& frame);
	void StartRecording(int match, int seed, int map, const std::array<int, 3>& rules,
		unsigned maniac_seed, std::string_view rng, bool blind, const std::vector<int32_t>& variables);
	void RecordFrame(int index, const std::array<int, 2>& inputs);
	void Terminal(int frame, int first, int second);
	std::string TakeReplay();
	void Close();
}

#endif
