/* global Mp4Muxer, VideoEncoder, AudioEncoder, AudioData, VideoFrame */
(() => {
  const sampleRate = 48000;
  const samplesPerFrame = 800;
  const audioBatchFrames = 8;
  const timestamp = frame => Math.round(frame * 1000000 / 60);
  async function selectAudioConfig() {
    if (typeof AudioEncoder !== 'function') return;
    for (const codec of ['mp4a.40.2', 'opus']) {
      const config = {codec, sampleRate, numberOfChannels: 2, bitrate: 128000};
      if ((await AudioEncoder.isConfigSupported(config)).supported) return config;
    }
  }

  // Native WebViews can pass planar frames straight to the encoder. At 2x,
  // one original pixel is exactly one chroma block in the output.
  function createPlanarFrames(width, height) {
    const stride = width * 2;
    const ySize = stride * height * 2;
    const chromaSize = width * height;
    const pixels = new Uint8Array(ySize + chromaSize * 2);
    const colorSpace = {matrix: 'bt709', primaries: 'bt709', transfer: 'bt709', fullRange: false};
    return {
      draw(buffer, pitch) {
        const words = new Uint32Array(buffer.buffer, buffer.byteOffset, buffer.byteLength / 4);
        const sourceStride = pitch / 4;
        for (let y = 0; y < height; ++y) {
          const row = y * stride * 2;
          for (let x = 0; x < width; ++x) {
            const word = words[y * sourceStride + x];
            const r = word >>> 16 & 255, g = word >>> 8 & 255, b = word & 255;
            const luma = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
            pixels[row + x * 2] = pixels[row + x * 2 + 1] = luma;
            pixels[ySize + y * width + x] = ((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128;
            pixels[ySize + chromaSize + y * width + x] = ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
          }
          pixels.copyWithin(row + stride, row, row + stride);
        }
      },
      frame(timing) {
        return new VideoFrame(pixels, {format: 'I420', codedWidth: stride, codedHeight: height * 2, colorSpace, ...timing});
      },
    };
  }

  self.prepareVideoExport = async (options, runtimeBase, createPixelRenderer, preferPlanarFrames, report) => {
    const audioConfig = await selectAudioConfig();
    if (!audioConfig) throw new Error('H.264 or audio encoding unavailable');
    importScripts(new URL('mp4-muxer.js', runtimeBase).href);
    const mixer = await createEasyRpgEngine({...options, audioOnly: true, noInitialRun: true, sampleRate, videoExportTick: 0});
    mixer._web_audio_init(sampleRate);
    return scale => createVideoExport(mixer, audioConfig, scale, createPixelRenderer, preferPlanarFrames, report);
  };

  async function createVideoExport(mixer, audioConfig, scale, createPixelRenderer, preferPlanarFrames, report) {
    if (!Number.isInteger(scale) || scale < 1 || scale > 9) throw new Error('Invalid video scale');
    // AVC levels for 320×240 at 60 FPS, scaled by each integer from 1 to 9.
    const level = ['1f', '1f', '20', '2a', '2a', '33', '33', '34', '34'][scale - 1];
    const videoConfig = {
      codec: 'avc1.4200' + level, width: 320 * scale, height: 240 * scale, framerate: 60,
      bitrate: Math.max(5000000, 2500000 * scale), latencyMode: 'realtime', avc: {format: 'avc'},
    };
    if (typeof OffscreenCanvas !== 'function' || typeof VideoEncoder !== 'function' ||
        !(await VideoEncoder.isConfigSupported(videoConfig)).supported)
      throw new Error('H.264 encoding unavailable at the selected scale');
    const muxer = new Mp4Muxer.Muxer({
      target: new Mp4Muxer.ArrayBufferTarget(),
      video: {codec: 'avc', width: videoConfig.width, height: videoConfig.height},
      audio: {codec: audioConfig.codec === 'opus' ? 'opus' : 'aac', sampleRate, numberOfChannels: 2},
      fastStart: 'in-memory', firstTimestampBehavior: 'cross-track-offset',
    });
    const video = new VideoEncoder({output: (chunk, meta) => muxer.addVideoChunk(chunk, meta), error: report});
    const audio = new AudioEncoder({output: (chunk, meta) => muxer.addAudioChunk(chunk, meta), error: report});
    video.configure(videoConfig);
    audio.configure(audioConfig);

    const pcm = mixer._malloc(samplesPerFrame * audioBatchFrames * 4);
    let audioFrames = 0;
    const textEncoder = new TextEncoder();
    const {width: outputWidth, height: outputHeight} = videoConfig;
    const planar = preferPlanarFrames && scale === 2 && createPlanarFrames(outputWidth / 2, outputHeight / 2);
    const canvas = new OffscreenCanvas(outputWidth, outputHeight);
    let drawCanvas, usePlanar;
    function createCanvasDraw() {
      const draw = createPixelRenderer(canvas, scale, true);
      if (draw) return draw;
      const context = canvas.getContext('2d', {alpha: false});
      context.imageSmoothingEnabled = false;
      return (buffer, width, height, pitch) => {
        const source = new VideoFrame(buffer, {
          format: 'BGRX', codedWidth: width, codedHeight: height,
          layout: [{offset: 0, stride: pitch}], timestamp: 0,
        });
        context.drawImage(source, 0, 0, width * scale, height * scale);
        source.close();
      };
    }
    // Yield to encoder callbacks in bounded batches without the nested timer delay.
    const tasks = new MessageChannel();
    let scheduled = 0;
    let rendered, tick = 0, capturing = false, finish = false, complete = false, lastProgress = -1;

    const send = (progress) => {
      if (progress === lastProgress) return;
      lastProgress = progress;
      postMessage({type: 'video-export-progress', progress});
    };
    const encodeAudio = (endFrame) => {
      const sample = new AudioData({
        format: 's16', sampleRate, numberOfFrames: samplesPerFrame * audioFrames, numberOfChannels: 2,
        timestamp: timestamp(endFrame - audioFrames),
        data: mixer.HEAPU8.subarray(pcm, pcm + samplesPerFrame * audioFrames * 4),
      });
      audio.encode(sample);
      sample.close();
      audioFrames = 0;
    };
    return {
      capabilities: mixer.audioCapabilities,
      command(command) {
        const bytes = textEncoder.encode(JSON.stringify(command) + '\0');
        const address = mixer._malloc(bytes.length);
        try {
          mixer.HEAPU8.set(bytes, address);
          mixer._web_audio_command(address);
        } finally {
          mixer._free(address);
        }
      },
      service(name, data) {
        if (name !== 'video.capture') return 0;
        const {operation, progress} = JSON.parse(data);
        if (operation === 'begin') capturing = true;
        else if (operation === 'progress') send(Math.min(98, Math.floor(progress * 98)));
        else if (operation === 'finish') finish = true;
        else if (operation === 'fail') report(new Error('Replay did not reach its recorded result'));
        return 0;
      },
      present(buffer, width, height, pitch) {
        if (!capturing) return;
        usePlanar = planar && width * 2 === outputWidth && height * 2 === outputHeight;
        if (usePlanar) planar.draw(buffer, pitch);
        else (drawCanvas ??= createCanvasDraw())(buffer, width, height, pitch);
        rendered = true;
      },
      capture(engine) {
        mixer.videoExportTick = engine.videoExportTick;
        const address = pcm + audioFrames * samplesPerFrame * 4;
        mixer.HEAPU8.fill(0, address, address + samplesPerFrame * 4);
        mixer._web_audio_render(address, samplesPerFrame);
        engine.audioState = mixer.audioState;
        if (!capturing || complete) return;
        if (!rendered && tick === 0) return;
        const time = timestamp(tick);
        const timing = {timestamp: time, duration: timestamp(tick + 1) - time};
        const frame = usePlanar ? planar.frame(timing) : new VideoFrame(canvas, timing);
        video.encode(frame, {keyFrame: tick % 120 === 0});
        frame.close();
        ++tick;
        ++audioFrames;
        if (audioFrames === audioBatchFrames || finish) encodeAudio(tick);
        if (finish) {
          complete = true;
          send(99);
          Promise.all([video.flush(), audio.flush()]).then(() => {
            muxer.finalize();
            const bytes = muxer.target.buffer;
            postMessage({type: 'video-export-ready', bytes}, [bytes]);
          }).catch(report);
        }
      },
      schedule(callback) {
        if (complete) return;
        const next = () => {
          if (complete) return;
          const encoder = video.encodeQueueSize > 8 ? video : audio.encodeQueueSize > 12 ? audio : null;
          if (encoder) encoder.addEventListener('dequeue', next, {once: true});
          else callback(performance.now());
        };
        if (++scheduled % 16 === 0) {
          tasks.port1.onmessage = next;
          tasks.port2.postMessage(null);
        } else queueMicrotask(next);
      },
    };
  }
})();
