#ifndef EP_NETPLAY_ROLLBACK_H
#define EP_NETPLAY_ROLLBACK_H

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace Game_Netplay {

// The world owns simulation and presentation. Only this timeline chooses
// which inputs to simulate and which effects are ready to publish.
template<class World>
class Rollback {
public:
	explicit Rollback(World& world, int seat) : world(world), seat(seat) {
		frames[0].actual = 0;
	}

	int VisibleFrame() const { return next - 1; }
	bool WaitingForInput() const { return next - confirmed >= window && remote_next == confirmed; }

	bool Receive(int frame, int input) {
		if (frame != remote_next || frame > next + window + input_delay) return false;
		frames[frame].actual = input;
		++remote_next;
		return true;
	}

	std::optional<int> QueueLocal(int input) {
		if (send > next + input_delay) return {};
		frames[send].local = input;
		return send++;
	}

	bool Update(int display_frame) {
		if (CommitReady()) return true;
		if (next - confirmed < window) {
			auto& frame = frames.at(next);
			const bool known = !replay && confirmed == next && frame.actual.has_value();
			if (!Step(next, known)) return false;
			++next;
		}
		if (last_replay_display != display_frame && !Reconcile(display_frame)) return false;
		CommitReady();
		return true;
	}

	bool CommitReady() {
		const int simulated = replay ? replay->next : next;
		while (confirmed < std::min(simulated, remote_next)) {
			auto& frame = frames.at(confirmed);
			if (NeedsReplay(frame)) break;
			if (world.Publish(confirmed++, std::move(frame.effects))) return true;
		}
		const int oldest = confirmed - confirmed % checkpoint_interval;
		frames.erase(frames.begin(), frames.lower_bound(oldest));
		return false;
	}

private:
	static constexpr int input_delay = 1;
	static constexpr int window = 60;
	static constexpr int checkpoint_interval = 4;

	struct Frame {
		int local = 0;
		int used = 0;
		std::optional<int> actual;
		std::unique_ptr<typename World::Snapshot> before;
		typename World::Effects effects;
		bool needs_confirmation = false;
	};

	struct Replay {
		int next;
		int last_visible;
		typename World::Snapshot state;
	};

	bool Step(int index, bool known) {
		auto& frame = frames.at(index);
		if (index % checkpoint_interval == 0) {
			frame.before = std::make_unique<typename World::Snapshot>(world.Capture());
		}
		frame.used = known ? *frame.actual : 0;
		frame.effects = {};
		frame.needs_confirmation = false;
		std::array<int, 2> inputs{};
		inputs[seat] = frame.local;
		inputs[1 - seat] = frame.used;
		return world.Step(index, inputs, known, frame.effects, frame.needs_confirmation);
	}

	static bool NeedsReplay(const Frame& frame) {
		return frame.needs_confirmation || frame.used != *frame.actual;
	}

	bool Reconcile(int display_frame) {
		const int simulated = replay ? replay->next : next;
		if (confirmed < std::min(simulated, remote_next) && NeedsReplay(frames.at(confirmed))) {
			const int start = confirmed - confirmed % checkpoint_interval;
			replay.emplace(Replay{start, next - 1, std::move(*frames.at(start).before)});
		}
		if (!replay) return true;

		last_replay_display = display_frame;
		const int minimum_steps = std::clamp(next - replay->last_visible + 1, 2, 8);
		replay->last_visible = next;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
		auto visible = world.Capture();
		world.Restore(std::move(replay->state));
		int steps = 0;
		while (replay->next < next && steps < 8 &&
				(steps < minimum_steps || std::chrono::steady_clock::now() < deadline)) {
			if (!Step(replay->next, replay->next < remote_next)) {
				world.Restore(std::move(visible));
				return false;
			}
			++replay->next;
			++steps;
		}
		if (replay->next == next) {
			replay.reset();
		} else {
			replay->state = world.Capture();
			world.Restore(std::move(visible));
		}
		return true;
	}

	World& world;
	int seat;
	int send = input_delay;
	int remote_next = input_delay;
	int next = 0;
	int confirmed = 0;
	int last_replay_display = -1;
	std::map<int, Frame> frames;
	std::optional<Replay> replay;
};

}

#endif
