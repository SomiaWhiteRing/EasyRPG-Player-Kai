#ifdef HAVE_NLOHMANN_JSON

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "netplay_lan.h"
#include "json_helper.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr unsigned short discovery_port = 39219;
constexpr size_t message_limit = 192 * 1024;
constexpr char protocol[] = "EasyRPG-Kai-Netplay/1";

class Socket {
public:
	explicit Socket(SOCKET socket = INVALID_SOCKET) : socket(socket) {
		u_long nonblocking = 1;
		if (socket != INVALID_SOCKET && ioctlsocket(socket, FIONBIO, &nonblocking) != 0) Reset();
	}
	Socket(Socket&& other) noexcept : socket(std::exchange(other.socket, INVALID_SOCKET)) {}
	Socket& operator=(Socket&& other) noexcept {
		Reset();
		socket = std::exchange(other.socket, INVALID_SOCKET);
		return *this;
	}
	~Socket() { Reset(); }
	void Reset() {
		if (socket != INVALID_SOCKET) closesocket(std::exchange(socket, INVALID_SOCKET));
	}
	explicit operator bool() const { return socket != INVALID_SOCKET; }
	operator SOCKET() const { return socket; }
private:
	SOCKET socket;
};

bool InitSockets() {
	static const bool ready = [] {
		WSADATA data{};
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	return ready;
}

sockaddr_in Address(uint32_t address, uint16_t port) {
	sockaddr_in result{};
	result.sin_family = AF_INET;
	result.sin_addr.s_addr = address;
	result.sin_port = htons(port);
	return result;
}

bool Bind(SOCKET socket, uint16_t port) {
	const auto address = Address(INADDR_ANY, port);
	return bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
}

std::vector<sockaddr_in> BroadcastAddresses() {
	std::vector<sockaddr_in> result{Address(INADDR_BROADCAST, discovery_port)};
	ULONG length = 15000;
	std::vector<unsigned char> buffer(length);
	auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
	constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
	auto status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &length);
	if (status == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(length);
		adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
		status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &length);
	}
	if (status != NO_ERROR) return result;
	for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
		for (auto* ip = adapter->FirstUnicastAddress; ip; ip = ip->Next) {
			const auto prefix = ip->OnLinkPrefixLength;
			if (!prefix || prefix >= 32) continue;
			const auto address = reinterpret_cast<const sockaddr_in*>(ip->Address.lpSockaddr)->sin_addr.s_addr;
			result.push_back(Address(address | htonl(UINT32_MAX >> prefix), discovery_port));
		}
	}
	return result;
}

struct Room {
	sockaddr_in address;
	Clock::time_point seen;
	json summary;
};

struct Session {
	Socket query, discovery, listener, peer;
	std::map<int, Room> rooms;
	std::map<std::string, Clock::time_point> online;
	std::string directory_id, presence;
	std::vector<sockaddr_in> broadcasts;
	Clock::time_point next_query{}, deadline{}, ping_at{}, ping_sent{};
	std::string input, output, version, password = "0000", join_password;
	json profile = {{"name", "Player"}, {"avatar", ""}};
	json opponent;
	json recording;
	std::deque<int> messages;
	std::deque<std::array<int, 3>> frames;
	std::array<int, 3> terminal{}, peer_terminal{};
	int state = 0, room = 0, result = 0, latency = -1;
	bool locked = false, settings_dirty = false, route_ended = false;
	bool guest = false, connecting = false, reject = false;
	bool has_terminal = false, has_peer_terminal = false;
	std::string completed;

	static json ReadProfile(const json& value) {
		if (!value.is_object()) return {};
		const auto name = value.find("name");
		const auto avatar = value.find("avatar");
		if (name == value.end() || !name->is_string() || avatar == value.end() || !avatar->is_string()) return {};
		if (name->get_ref<const std::string&>().empty() || name->get_ref<const std::string&>().size() > 80 ||
				avatar->get_ref<const std::string&>().size() > 131072) return {};
		return {{"name", *name}, {"avatar", *avatar}};
	}

	json Summary(bool avatars) const {
		json players = json::array({profile});
		if (state >= 3) {
			if (guest) players.insert(players.begin(), opponent);
			else players.push_back(opponent);
		}
		if (!avatars) for (auto& player : players) player["avatar"] = "default";
		return {{"id", room}, {"players", std::move(players)}, {"locked", locked}};
	}

	void DropPeer() {
		const bool interrupted = state >= 3;
		peer.Reset();
		input.clear();
		output.clear();
		messages.clear();
		frames.clear();
		opponent = nullptr;
		recording = nullptr;
		has_terminal = has_peer_terminal = false;
		latency = -1;
		ping_sent = {};
		connecting = reject = false;
		route_ended |= interrupted;
		state = listener ? 2 : interrupted ? -1 : 0;
	}

	void Send(const json& value) {
		output += value.dump();
		output += '\n';
	}

	bool Handle(const json& value) {
		if (!value.is_array() || value.empty() || !value[0].is_string()) return false;
		const auto& kind = value[0].get_ref<const std::string&>();
		if (!guest && state == 2) {
			if (kind != "join" || value.size() != 6) return false;
			auto player = ReadProfile(value[5]);
			if (player.is_null() || !value[4].is_string()) return false;
			const int error = value[1] != protocol || value[2] != version || value[3] != room ? 2 :
				locked && value[4] != password ? (value[4] == "" ? 4 : 5) : 0;
			if (error) {
				Send({"reject", error});
				reject = true;
			} else {
				opponent = std::move(player);
				Send({"accept", profile, locked});
				state = 3;
			}
			return true;
		}
		if (guest && state == 1) {
			if (kind == "reject" && value.size() == 2 && value[1].is_number_integer()) {
				result = value[1].get<int>();
				return false;
			}
			if (kind != "accept" || value.size() != 3 || !value[2].is_boolean()) return false;
			locked = value[2].get<bool>();
			opponent = ReadProfile(value[1]);
			if (opponent.is_null()) return false;
			state = 4;
			return true;
		}
		if (state < 3) return false;
		if (kind == "ping" && value.size() == 1) Send({"pong"});
		else if (kind == "pong" && value.size() == 1 && ping_sent != Clock::time_point{}) {
			latency = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - ping_sent).count());
			ping_sent = {};
		} else if (kind == "lock" && guest && value.size() == 2 && value[1].is_boolean()) {
			locked = value[1].get<bool>();
		} else if (kind == "message" && value.size() == 2 && value[1].is_number_integer() && messages.size() < 256) {
			messages.push_back(value[1].get<int>());
		} else if ((kind == "input" || kind == "terminal") && value.size() == 4 &&
				value[1].is_number_integer() && value[2].is_number_integer() && value[3].is_number_integer()) {
			const std::array<int, 3> packet{value[1].get<int>(), value[2].get<int>(), value[3].get<int>()};
			if (kind == "terminal") {
				peer_terminal = packet;
				has_peer_terminal = true;
			} else {
				if (frames.size() >= 128) return false;
				if (packet[1] == -1) {
					has_peer_terminal = false;
					has_terminal = false;
				}
				frames.push_back(packet);
			}
		} else return false;
		return true;
	}

	void PumpPeer(Clock::time_point now) {
		if (listener && !peer) {
			peer = Socket(accept(listener, nullptr, nullptr));
			if (peer) {
				deadline = now + 5s;
				const int enabled = 1;
				setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
			}
		}
		if (!peer) return;
		if (connecting) {
			fd_set writable, failed;
			FD_ZERO(&writable);
			FD_ZERO(&failed);
			FD_SET(peer, &writable);
			FD_SET(peer, &failed);
			timeval timeout{};
			const auto ready = select(0, nullptr, &writable, &failed, &timeout);
			if (ready == SOCKET_ERROR || FD_ISSET(peer, &failed) || now >= deadline) {
				result = 3;
				DropPeer();
				return;
			}
			if (!FD_ISSET(peer, &writable)) return;
			connecting = false;
			Send({"join", protocol, version, room, join_password, profile});
		}
		if (state >= 3 && now >= ping_at) {
			ping_at = now + 1s;
			if (ping_sent == Clock::time_point{}) {
				ping_sent = now;
				Send({"ping"});
			} else if (now - ping_sent >= 10s) {
				DropPeer();
				return;
			}
		}
		if (output.size() > message_limit || (state < 3 && now >= deadline)) {
			if (guest) result = 3;
			DropPeer();
			return;
		}
		if (!output.empty()) {
			const int sent = send(peer, output.data(), static_cast<int>(output.size()), 0);
			if (sent > 0) output.erase(0, sent);
			else if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
				if (state == 1) result = 3;
				DropPeer();
				return;
			}
		}
		if (reject) {
			if (output.empty()) DropPeer();
			return;
		}
		char data[8192];
		const int count = recv(peer, data, sizeof(data), 0);
		if (count <= 0) {
			if (count == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
				if (state == 1) result = 3;
				DropPeer();
			}
			return;
		}
		input.append(data, count);
		size_t start = 0;
		for (size_t end; !reject && (end = input.find('\n', start)) != std::string::npos; start = end + 1) {
			const auto value = Json_Helper::Parse(std::string_view(input).substr(start, end - start));
			if (!value || !Handle(*value)) {
				if (state == 1 && !result) result = 3;
				DropPeer();
				return;
			}
		}
		input.erase(0, start);
		if (input.size() > message_limit) DropPeer();
	}

	bool StartDirectory() {
		if (query) return true;
		query = Socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
		discovery = Socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
		const int enabled = 1;
		setsockopt(query, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
		setsockopt(discovery, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
		if (!Bind(query, 0) || !Bind(discovery, discovery_port)) {
			query.Reset();
			discovery.Reset();
			return false;
		}
		if (directory_id.empty()) {
			std::random_device random;
			directory_id = std::to_string((static_cast<uint64_t>(random()) << 32) | random());
		}
		presence = json::array({protocol, version, directory_id}).dump();
		broadcasts = BroadcastAddresses();
		next_query = {};
		return true;
	}

	void PumpDiscovery(Clock::time_point now) {
		char data[2048];
		sockaddr_in sender{};
		int length = sizeof(sender);
		if (discovery) {
			for (int i = 0; i < 16; ++i) {
				const int count = recvfrom(discovery, data, sizeof(data), 0, reinterpret_cast<sockaddr*>(&sender), &length);
				if (count < 0) break;
				const auto query_data = Json_Helper::Parse(std::string_view(data, count));
				if (!query_data || !query_data->is_array() || query_data->size() != 3 ||
						(*query_data)[0] != protocol || (*query_data)[1] != version || !(*query_data)[2].is_string()) continue;
				const auto& id = (*query_data)[2].get_ref<const std::string&>();
				if (id.empty() || id.size() > 20 || id == directory_id) continue;
				online.insert_or_assign(id, now);
				if (!listener) continue;
				sockaddr_in address{};
				int size = sizeof(address);
				if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) continue;
				const auto response = json::array({protocol, version, ntohs(address.sin_port), Summary(false)}).dump();
				sendto(discovery, response.data(), static_cast<int>(response.size()), 0,
					reinterpret_cast<const sockaddr*>(&sender), sizeof(sender));
			}
		}
		if (!query) return;
		if (now >= next_query) {
			next_query = now + 1s;
			for (const auto& address : broadcasts) sendto(query, presence.data(), static_cast<int>(presence.size()), 0,
				reinterpret_cast<const sockaddr*>(&address), sizeof(address));
		}
		for (int i = 0; i < 32; ++i) {
			const int count = recvfrom(query, data, sizeof(data), 0, reinterpret_cast<sockaddr*>(&sender), &length);
			if (count < 0) break;
			auto value = Json_Helper::Parse(std::string_view(data, count));
			if (!value || !value->is_array() || value->size() != 4 || (*value)[0] != protocol || (*value)[1] != version ||
					!(*value)[2].is_number_integer() || !(*value)[3].is_object()) continue;
			auto& info = (*value)[3];
			const int port = (*value)[2].get<int>();
			const auto id = info.find("id"), players = info.find("players"), lock = info.find("locked");
			if (port <= 0 || port > 65535 || id == info.end() || !id->is_number_integer() ||
					players == info.end() || !players->is_array() || players->empty() || players->size() > 2 ||
					lock == info.end() || !lock->is_boolean()) continue;
			const int number = id->get<int>();
			if (number <= 0 || number > 999999 || std::any_of(players->begin(), players->end(),
					[](const json& player) { return ReadProfile(player).is_null(); })) continue;
			sender.sin_port = htons(static_cast<uint16_t>(port));
			rooms.insert_or_assign(number, Room{sender, now, std::move(info)});
		}
		for (auto it = rooms.begin(); it != rooms.end();) {
			if (now - it->second.seen > 4s) it = rooms.erase(it);
			else ++it;
		}
		for (auto it = online.begin(); it != online.end();) {
			if (now - it->second > 4s) it = online.erase(it);
			else ++it;
		}
	}

	void Pump() {
		const auto now = Clock::now();
		PumpPeer(now);
		PumpDiscovery(now);
	}
};

Session lan;
}

bool NetplayLan::Available() { return InitSockets(); }
int NetplayLan::State() { lan.Pump(); return lan.state; }
int NetplayLan::JoinResult() { return std::exchange(lan.result, 0); }
int NetplayLan::Online() { return lan.query ? 1 + static_cast<int>(lan.online.size()) : -1; }
int NetplayLan::Latency() { return lan.latency; }
bool NetplayLan::TakeRouteEnded() { return std::exchange(lan.route_ended, false); }

void NetplayLan::SetProfile(std::string version, std::string name, std::string avatar) {
	if (lan.version != version) StopDirectory();
	lan.version = std::move(version);
	lan.profile = {{"name", std::move(name)}, {"avatar", std::move(avatar)}};
}

std::string NetplayLan::Identity() { return lan.profile.dump(); }

std::string NetplayLan::Rooms() {
	if (!Available()) return {};
	if (!lan.StartDirectory()) return {};
	lan.Pump();
	auto rooms = json::array();
	for (const auto& [number, entry] : lan.rooms) {
		if (lan.state <= 0 || number != lan.room) rooms.push_back(entry.summary);
	}
	if (lan.state >= 2) rooms.push_back(lan.Summary(true));
	return rooms.dump();
}

std::string NetplayLan::RoomSettings() {
	if (!std::exchange(lan.settings_dirty, false)) return {};
	return json{{"enabled", lan.locked}, {"code", lan.password}}.dump();
}

void NetplayLan::StopDirectory() {
	lan.query.Reset();
	lan.discovery.Reset();
	lan.rooms.clear();
	lan.online.clear();
}

void NetplayLan::Open(int room, bool join, std::string_view password) {
	if (!Available() || lan.version.empty() || room <= 0 || room > 999999) {
		lan.result = 3;
		return;
	}
	const auto found = lan.rooms.find(room);
	if (join && found == lan.rooms.end()) {
		lan.result = 2;
		return;
	}
	Close();
	lan.room = room;
	lan.guest = join;
	lan.locked = false;
	lan.result = 0;
	if (!join) {
		lan.listener = Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
		if (!Bind(lan.listener, 0) || listen(lan.listener, 2) != 0 || !lan.StartDirectory()) {
			Close();
			lan.result = 3;
			return;
		}
		lan.state = 2;
		lan.password = std::to_string(1000 + Clock::now().time_since_epoch().count() % 9000);
		lan.settings_dirty = true;
		return;
	}
	lan.locked = found->second.summary["locked"] == true;
	lan.peer = Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
	const int enabled = 1;
	setsockopt(lan.peer, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
	const auto& address = found->second.address;
	const int status = connect(lan.peer, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
	if (status != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
		Close();
		lan.result = 3;
		return;
	}
	lan.state = 1;
	lan.deadline = Clock::now() + 5s;
	lan.join_password = password;
	lan.connecting = status != 0;
	if (!lan.connecting) lan.Send({"join", protocol, lan.version, lan.room, lan.join_password, lan.profile});
}

void NetplayLan::SetPassword(bool enabled, std::string_view password) {
	if (lan.state != 2 && lan.state != 3) return;
	if (password.size() != 4 || password.find_first_not_of("0123456789") != std::string_view::npos) return;
	lan.password = password;
	lan.locked = enabled;
	lan.settings_dirty = true;
	if (lan.state == 3) lan.Send({"lock", enabled});
}

void NetplayLan::Send(int value) {
	if (lan.state >= 3) lan.Send({"message", value});
}

std::optional<int> NetplayLan::Receive() {
	if (lan.messages.empty()) return {};
	const int value = lan.messages.front();
	lan.messages.pop_front();
	return value;
}

void NetplayLan::SendFrame(int match, int frame, int input) {
	if (lan.state >= 3) lan.Send({"input", match, frame, input});
}

bool NetplayLan::ReceiveFrame(std::array<int, 3>& frame) {
	if (lan.frames.empty()) return false;
	frame = lan.frames.front();
	lan.frames.pop_front();
	return true;
}

void NetplayLan::StartRecording(int match, int seed, int map, const std::array<int, 3>& rules,
		unsigned maniac_seed, std::string_view rng, bool blind, const std::vector<int32_t>& variables) {
	auto saved = json::array();
	for (size_t i = 0; i < variables.size(); ++i) {
		if (variables[i] != 0) saved.push_back({i + 1, variables[i]});
	}
	lan.recording = {{"version", 1}, {"gameVersion", lan.version}, {"match", match}, {"viewerSeat", lan.state - 2},
		{"startedAt", std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count()},
		{"players", lan.Summary(true)["players"]}, {"frames", json::array()},
		{"start", {{"seed", seed}, {"map", map}, {"fighters", {rules[0], rules[1]}}, {"rounds", rules[2]},
			{"maniacSeed", maniac_seed}, {"rng", rng}, {"blind", blind}, {"variables", std::move(saved)}}}};
}

void NetplayLan::RecordFrame(int index, const std::array<int, 2>& inputs) {
	if (!lan.recording.is_null() && lan.recording["frames"].size() == static_cast<size_t>(index))
		lan.recording["frames"].push_back(inputs);
}

void NetplayLan::Terminal(int frame, int first, int second) {
	if (lan.state < 3) return;
	lan.terminal = {frame, first, second};
	lan.has_terminal = true;
	lan.Send({"terminal", frame, first, second});
}

int NetplayLan::Finish() {
	if (State() < 3) return -1;
	if (!lan.has_terminal || !lan.has_peer_terminal) return 0;
	if (lan.terminal != lan.peer_terminal) return -1;
	if (!lan.recording.is_null()) {
		lan.recording["terminal"] = lan.terminal;
		lan.completed = lan.recording.dump();
		lan.recording = nullptr;
	}
	return 1;
}

std::string NetplayLan::TakeReplay() { return std::exchange(lan.completed, std::string{}); }

void NetplayLan::Close() {
	lan.listener.Reset();
	lan.DropPeer();
	lan.state = 0;
	lan.route_ended = false;
	lan.settings_dirty = false;
}

#endif
