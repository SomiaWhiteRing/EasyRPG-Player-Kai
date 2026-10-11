/* SPDX-License-Identifier: GPL-3.0-or-later
 * Small, decode-only FFmpeg bridge. Web uses a Worker; Android supplies
 * filesystem callbacks and bounded native audio/video consumers.
 */
#include <errno.h>
#include <math.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#define EM_JS(result, name, args, ...) result name args;
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

static AVFormatContext *format;
static AVCodecContext *decoders[2];
static int streams[2] = {-1, -1};
static AVPacket *packet;
static AVFrame *frame;
static struct SwsContext *scale;
static struct SwrContext *resample;
static AVChannelLayout audio_layout;
static int audio_rate, audio_format = -1;
static uint8_t *pixels, *samples;
static unsigned int pixel_capacity, sample_capacity;
static int active = -1, draining;
static double origin, next_time[2];
static char error_message[256];
#ifndef __EMSCRIPTEN__
static AVIOContext *custom_io;
extern int movie_read(void *opaque, uint8_t *data, int size);
extern int64_t movie_seek(void *opaque, int64_t offset, int whence);
#endif

EM_JS(void, movie_video, (const uint8_t *data, int size, int width, int height, double time, double duration), {
  Module.onVideo(HEAPU8.slice(data, data + size), width, height, time, duration);
});
EM_JS(void, movie_audio, (const uint8_t *data, int count, int rate, double time), {
  Module.onAudio(HEAPF32.slice(data / 4, data / 4 + count * 2), rate, time);
});

static int fail(int result, const char *operation) {
  char detail[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(result, detail, sizeof(detail));
  snprintf(error_message, sizeof(error_message), "%s: %s", operation, detail);
  return -1;
}

EMSCRIPTEN_KEEPALIVE const char *movie_error(void) { return error_message; }

EMSCRIPTEN_KEEPALIVE void movie_close(void) {
  av_packet_free(&packet);
  av_frame_free(&frame);
  for (int i = 0; i < 2; ++i) {
    avcodec_free_context(&decoders[i]);
    streams[i] = -1;
    next_time[i] = 0;
  }
  avformat_close_input(&format);
#ifndef __EMSCRIPTEN__
  if (custom_io) av_freep(&custom_io->buffer);
  avio_context_free(&custom_io);
#endif
  sws_freeContext(scale);
  scale = NULL;
  swr_free(&resample);
  av_channel_layout_uninit(&audio_layout);
  av_freep(&pixels);
  av_freep(&samples);
  pixel_capacity = sample_capacity = 0;
  active = -1;
  draining = audio_rate = 0;
  audio_format = -1;
}

EMSCRIPTEN_KEEPALIVE int movie_open(const char *path) {
  movie_close();
  error_message[0] = 0;
  av_log_set_level(AV_LOG_ERROR);
  format = avformat_alloc_context();
  if (!format) return fail(AVERROR(ENOMEM), "Allocate movie");
  format->probesize = 1024 * 1024;
  format->max_analyze_duration = 3 * AV_TIME_BASE;
#ifdef __EMSCRIPTEN__
  int result = avformat_open_input(&format, path, NULL, NULL);
#else
  uint8_t *io_buffer = av_malloc(32768);
  if (!io_buffer) return fail(AVERROR(ENOMEM), "Allocate movie input");
  custom_io = avio_alloc_context(io_buffer, 32768, 0, NULL, movie_read, NULL, movie_seek);
  if (!custom_io) { av_free(io_buffer); return fail(AVERROR(ENOMEM), "Allocate movie input context"); }
  format->pb = custom_io;
  format->flags |= AVFMT_FLAG_CUSTOM_IO;
  int result = avformat_open_input(&format, NULL, NULL, NULL);
#endif
  if (result < 0) return fail(result, "Open movie");
  result = avformat_find_stream_info(format, NULL);
  if (result < 0) return fail(result, "Read movie streams");
  origin = format->start_time == AV_NOPTS_VALUE ? 0 : (double)format->start_time / AV_TIME_BASE;
  for (int i = 0; i < 2; ++i) {
    enum AVMediaType type = i == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    streams[i] = av_find_best_stream(format, type, -1, -1, NULL, 0);
    if (i == 1 && streams[i] == AVERROR_STREAM_NOT_FOUND) continue;
    if (streams[i] < 0) return fail(streams[i], i ? "Find audio decoder" : "Find video decoder");
    const AVCodecParameters *parameters = format->streams[streams[i]]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) return fail(AVERROR_DECODER_NOT_FOUND, "Unsupported movie codec");
    decoders[i] = avcodec_alloc_context3(codec);
    if (!decoders[i]) return fail(AVERROR(ENOMEM), "Allocate decoder");
    result = avcodec_parameters_to_context(decoders[i], parameters);
    if (result < 0) return fail(result, "Configure decoder");
    decoders[i]->thread_count = 1;
    decoders[i]->max_pixels = 4096 * 2160;
    decoders[i]->pkt_timebase = format->streams[streams[i]]->time_base;
    result = avcodec_open2(decoders[i], codec, NULL);
    if (result < 0) return fail(result, "Start decoder");
  }
  packet = av_packet_alloc();
  frame = av_frame_alloc();
  if (!packet || !frame) return fail(AVERROR(ENOMEM), "Allocate movie buffers");
  return 0;
}

static int output_frame(int kind) {
  AVStream *stream = format->streams[streams[kind]];
  double time = next_time[kind];
  if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
    time = (double)frame->best_effort_timestamp * av_q2d(stream->time_base) - origin;
  if (!isfinite(time)) return fail(AVERROR_INVALIDDATA, "Invalid timestamp");
  time = fmax(0, time);
  if (kind == 0) {
    if (frame->width <= 0 || frame->height <= 0 ||
        (int64_t)frame->width * frame->height > 4096 * 2160)
      return fail(AVERROR_INVALIDDATA, "Movie exceeds 4096x2160 pixel budget");
    int size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, frame->width, frame->height, 1);
    if (size < 0) return fail(size, "Video size");
    av_fast_malloc(&pixels, &pixel_capacity, size);
    if (!pixels) return fail(AVERROR(ENOMEM), "Video buffer");
    scale = sws_getCachedContext(scale, frame->width, frame->height, frame->format,
      frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale) return fail(AVERROR(ENOMEM), "Video conversion");
    uint8_t *destination[4] = {pixels};
    int strides[4] = {frame->width * 4};
    int result = sws_scale(scale, (const uint8_t * const *)frame->data, frame->linesize,
      0, frame->height, destination, strides);
    if (result < 0) return fail(result, "Convert video");
    AVRational rate = av_guess_frame_rate(format, stream, frame);
    double duration = frame->duration > 0 ? frame->duration * av_q2d(stream->time_base) :
      (rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 1.0 / 30);
    next_time[0] = time + duration;
    movie_video(pixels, size, frame->width, frame->height, time, duration);
  } else {
    if (frame->sample_rate <= 0 || frame->sample_rate > 192000 ||
        frame->nb_samples <= 0 || frame->nb_samples > 192000)
      return fail(AVERROR_INVALIDDATA, "Invalid audio frame");
    if (!resample || audio_rate != frame->sample_rate || audio_format != frame->format ||
        av_channel_layout_compare(&audio_layout, &frame->ch_layout)) {
      swr_free(&resample);
      av_channel_layout_uninit(&audio_layout);
      AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
      int result = swr_alloc_set_opts2(&resample, &stereo, AV_SAMPLE_FMT_FLT, frame->sample_rate,
        &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
      if (result < 0) return fail(result, "Audio conversion setup");
      result = swr_init(resample);
      if (result < 0) return fail(result, "Audio conversion init");
      result = av_channel_layout_copy(&audio_layout, &frame->ch_layout);
      if (result < 0) return fail(result, "Audio layout");
      audio_rate = frame->sample_rate;
      audio_format = frame->format;
    }
    // Same sample rate: only convert sample format and downmix to stereo.
    av_fast_malloc(&samples, &sample_capacity, (size_t)frame->nb_samples * 2 * sizeof(float));
    if (!samples) return fail(AVERROR(ENOMEM), "Audio buffer");
    int count = swr_convert(resample, &samples, frame->nb_samples,
      (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (count < 0) return fail(count, "Convert audio");
    if (!count) return 0;
    next_time[1] = time + (double)count / frame->sample_rate;
    movie_audio(samples, count, frame->sample_rate, time);
  }
  return 1;
}

// At most one decoded frame per call. The host supplies backpressure.
EMSCRIPTEN_KEEPALIVE int movie_step(void) {
  for (;;) {
    if (active >= 0) {
      int result = avcodec_receive_frame(decoders[active], frame);
      if (result >= 0) {
        result = output_frame(active);
        av_frame_unref(frame);
        if (!result) continue;
        return result;
      }
      if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) return fail(result, "Decode frame");
      active = -1;
    }
    if (draining) {
      if (draining > 2) return 0;
      int kind = draining++ - 1;
      if (!decoders[kind]) continue;
      int result = avcodec_send_packet(decoders[kind], NULL);
      if (result < 0 && result != AVERROR_EOF) return fail(result, "Drain decoder");
      active = kind;
      continue;
    }
    int result = av_read_frame(format, packet);
    if (result == AVERROR_EOF) { draining = 1; continue; }
    if (result < 0) return fail(result, "Read movie packet");
    int kind = packet->stream_index == streams[0] ? 0 : packet->stream_index == streams[1] ? 1 : -1;
    if (kind >= 0) result = avcodec_send_packet(decoders[kind], packet);
    av_packet_unref(packet);
    if (kind < 0) continue;
    if (result < 0) return fail(result, "Decode packet");
    active = kind;
  }
}
