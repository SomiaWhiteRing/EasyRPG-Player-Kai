/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/android/movie_player_android.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <vector>
#include <SDL3/SDL.h>
#include "bitmap.h"
#include "audio.h"
#include "filefinder.h"
#include "filesystem_stream.h"
#include "output.h"
#include "player.h"
#include "pixel_format.h"
extern "C" {
	int movie_open(const char* path);
	int movie_step(void);
	void movie_close(void);
	const char* movie_error(void);
}
namespace {
	using Clock = std::chrono::steady_clock;
	struct VideoFrame {
		std::vector<uint8_t> pixels;
		int width = 0, height = 0;
		double time = 0;
	};
	struct DecoderOutput {
		Filesystem_Stream::InputStream input;
		std::deque<VideoFrame> frames;
		SDL_AudioStream* audio = nullptr;
		int audio_rate = 0;
		size_t bytes = 0;
		double end_time = 0;
		bool failed = false;
		uint64_t video_frames = 0, audio_frames = 0;
	};
	// One active movie; decode callbacks run synchronously in Update.
	DecoderOutput* output = nullptr;
}
extern "C" int movie_read(void*, uint8_t* data, int size) {
	output->input.read(reinterpret_cast<char*>(data), size);
	const auto count = output->input.gcount();
	return count > 0 ? static_cast<int>(count) : -541478725; // AVERROR_EOF
}
extern "C" int64_t movie_seek(void*, int64_t offset, int whence) {
	if (whence & 0x10000) return output->input.GetSize(); // AVSEEK_SIZE
	output->input.clear();
	whence &= ~0x20000; // AVSEEK_FORCE
	const auto mode = whence == SEEK_SET ? std::ios_base::beg :
		whence == SEEK_CUR ? std::ios_base::cur : std::ios_base::end;
	output->input.seekg(offset, mode);
	return output->input ? static_cast<int64_t>(output->input.tellg()) : -1;
}
extern "C" void movie_video(const uint8_t* data, int size, int width, int height, double time, double duration) {
	output->frames.push_back({std::vector<uint8_t>(data, data + size), width, height, time});
	output->bytes += size;
	output->end_time = std::max(output->end_time, time + duration);
	++output->video_frames;
}
extern "C" void movie_audio(const uint8_t* data, int count, int rate, double time) {
	if (!Player::no_audio_flag) {
		if (!output->audio || output->audio_rate != rate) {
			SDL_DestroyAudioStream(output->audio);
			const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, rate};
			output->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
			output->audio_rate = rate;
			if (!output->audio || !SDL_ResumeAudioStreamDevice(output->audio)) output->failed = true;
		}
		if (output->audio && (!SDL_SetAudioStreamGain(output->audio, Audio().GetManiacOutputGain())
			|| !SDL_PutAudioStreamData(output->audio, data, count * 2 * sizeof(float)))) output->failed = true;
	}
	output->end_time = std::max(output->end_time, time + static_cast<double>(count) / rate);
	output->audio_frames += count;
}
struct AndroidMoviePlayer::Impl {
	DecoderOutput decoder;
	Clock::time_point start;
	VideoFrame visible;
	int last_audio_queue = -1;
	double audio_progress_time = 0;
	bool playing = false, eof = false, error = false;
};
AndroidMoviePlayer::AndroidMoviePlayer() : impl(std::make_unique<Impl>()) {}
AndroidMoviePlayer::~AndroidMoviePlayer() { Stop(); }
bool AndroidMoviePlayer::Open(std::string_view path, std::string& error_message) {
	Stop();
	impl->decoder.input = FileFinder::Game().OpenInputStream(path);
	if (!impl->decoder.input) { error_message = "Could not open movie stream"; return false; }
	output = &impl->decoder;
	if (movie_open(std::string(path).c_str()) < 0) {
		error_message = movie_error();
		Stop();
		return false;
	}
	impl->playing = true;
	impl->start = Clock::now();
	Update({});
	return impl->playing;
}
void AndroidMoviePlayer::Update(const Rect&) {
	if (!impl->playing) return;
	const double now = std::chrono::duration<double>(Clock::now() - impl->start).count();
	auto& decoder = impl->decoder;
	// Same bounded half-second/32 MiB lookahead as the existing Web decoder.
	while (!impl->eof && decoder.end_time <= now + .5 && decoder.bytes < 32 * 1024 * 1024) {
		const int result = movie_step();
		if (result <= 0) {
			impl->eof = true;
			Output::Debug("Android movie decoded end: video={} audio_frames={} audio_queued={} audio_available={}",
				decoder.video_frames, decoder.audio_frames,
				decoder.audio ? SDL_GetAudioStreamQueued(decoder.audio) : 0,
				decoder.audio ? SDL_GetAudioStreamAvailable(decoder.audio) : 0);
			if (result < 0) { Output::Warning("Android movie decoder: {}", movie_error()); impl->error = true; }
			break;
		}
	}
	while (!decoder.frames.empty() && decoder.frames.front().time <= now) {
		decoder.bytes -= decoder.frames.front().pixels.size();
		impl->visible = std::move(decoder.frames.front());
		decoder.frames.pop_front();
	}
	if (decoder.failed) { Output::Warning("Android movie audio: {}", SDL_GetError()); impl->error = true; }
	const bool audio_drained = !decoder.audio ||
		(SDL_GetAudioStreamQueued(decoder.audio) == 0 && SDL_GetAudioStreamAvailable(decoder.audio) == 0);
	const int queued = decoder.audio ? SDL_GetAudioStreamQueued(decoder.audio) : 0;
	if (queued != impl->last_audio_queue) {
		impl->last_audio_queue = queued;
		impl->audio_progress_time = now;
	}
	if (impl->eof && !audio_drained && now - impl->audio_progress_time > 2) {
		Output::Warning("Android movie audio output stalled with {} queued bytes", queued);
		impl->error = true;
	}
	if (impl->error || (impl->eof && now >= decoder.end_time && decoder.frames.empty() && audio_drained)) {
		Output::Debug("Android movie completed: decoded_video={} decoded_audio_frames={} error={}",
			decoder.video_frames, decoder.audio_frames, impl->error);
		impl->playing = false;
	}
}
void AndroidMoviePlayer::Stop() {
	if (output == &impl->decoder) { movie_close(); output = nullptr; }
	SDL_DestroyAudioStream(impl->decoder.audio);
	impl->decoder.audio = nullptr;
	impl->decoder.input.Close();
	impl->decoder.frames.clear();
	impl->decoder.bytes = 0;
	impl->decoder.end_time = 0;
	impl->decoder.failed = false;
	impl->decoder.video_frames = impl->decoder.audio_frames = 0;
	impl->visible = {};
	impl->last_audio_queue = -1;
	impl->audio_progress_time = 0;
	impl->playing = impl->eof = impl->error = false;
}
bool AndroidMoviePlayer::IsPlaying() const { return impl->playing; }
bool AndroidMoviePlayer::HasError() const { return impl->error; }
int AndroidMoviePlayer::GetNativeWidth() const { return impl->visible.width; }
int AndroidMoviePlayer::GetNativeHeight() const { return impl->visible.height; }
void AndroidMoviePlayer::Draw(Bitmap& dst, const Rect& rect) {
	auto& frame = impl->visible;
	if (frame.pixels.empty()) return;
	auto bitmap = Bitmap::Create(frame.pixels.data(), frame.width, frame.height, frame.width * 4,
		format_R8G8B8A8_n().format());
	dst.StretchBlit(rect, *bitmap, bitmap->GetRect(), Opacity::Opaque());
}
