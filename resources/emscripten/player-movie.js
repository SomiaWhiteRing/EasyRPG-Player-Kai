// Loaded only when a game plays a movie. The larger decoder loads only on fallback.
export class MoviePlayback {
  constructor({canvas, context, output = context.destination, base, state}) {
    Object.assign(this, {canvas, context, output, base, state});
    this.alive = true;
    this.sources = new Set();
    this.frames = [];
    this.bytes = 0;
    this.endTime = 0;
    this.anchor = null;
    this.listeners = new AbortController();
  }

  start(blob, path) {
    this.blob = blob;
    this.path = path;
    const video = document.createElement('video');
    this.element = video;
    video.playsInline = true;
    this.nativeAudio = this.context.createMediaElementSource(video);
    this.nativeAudio.connect(this.output);
    this.attach();
    this.url = URL.createObjectURL(blob);
    video.src = this.url;
    video.onloadedmetadata = () => this.state({playing: true, width: video.videoWidth, height: video.videoHeight});
    video.onended = () => this.finish();
    video.onerror = () => this.fallback(`Native video error ${video.error?.code}: ${video.error?.message || ''}`);
    const play = () => {
      if (!this.alive || this.element !== video) return;
      video.play().then(() => {
        if (this.alive && this.element === video) clearTimeout(this.timer);
      }).catch(error => {
        if (!this.alive || this.element !== video) return;
        if (error.name === 'NotAllowedError') {
          // Autoplay rejection is not a codec failure. Retry on the next gesture.
          clearTimeout(this.timer);
        } else this.fallback(error.message);
      });
    };
    for (const target of new Set([document, this.canvas.ownerDocument.defaultView.parent.document])) {
      for (const event of ['pointerdown', 'keydown']) target.addEventListener(event, () => {
        void this.context.resume().catch(() => {});
        if (video.paused) play();
      }, {signal: this.listeners.signal});
    }
    this.timer = setTimeout(() => this.fallback('Native video load timed out'), 15000);
    play();
  }

  attach() {
    Object.assign(this.element.style, {position: 'absolute', pointerEvents: 'none', background: 'black', zIndex: '1'});
    this.canvas.parentNode.appendChild(this.element);
    if (this.rect) this.updateRect(this.rect);
  }

  updateRect(rect) {
    this.rect = rect;
    if (!this.element) return;
    const canvasRect = this.canvas.getBoundingClientRect(), parent = this.canvas.parentNode.getBoundingClientRect();
    Object.assign(this.element.style, {
      left: `${canvasRect.left - parent.left + rect.x * canvasRect.width / rect.screenWidth}px`,
      top: `${canvasRect.top - parent.top + rect.y * canvasRect.height / rect.screenHeight}px`,
      width: `${rect.width * canvasRect.width / rect.screenWidth}px`,
      height: `${rect.height * canvasRect.height / rect.screenHeight}px`,
    });
  }

  removeNative() {
    clearTimeout(this.timer);
    this.nativeAudio?.disconnect(); this.nativeAudio = undefined;
    if (this.element?.tagName === 'VIDEO') {
      this.element.onloadedmetadata = this.element.onended = this.element.onerror = null;
      this.element.pause();
      this.element.removeAttribute('src');
      this.element.load();
    }
    this.element?.remove();
    if (this.url) URL.revokeObjectURL(this.url);
    this.url = undefined;
  }

  fallback(reason) {
    if (!this.alive || this.worker) return;
    this.removeNative();
    console.info(`Movie decoder fallback: ${this.path}: ${reason}`);
    this.element = document.createElement('canvas');
    this.painter = this.element.getContext('2d', {alpha: false});
    if (!this.painter) { this.finish('Could not create movie canvas'); return; }
    this.attach();
    try {
      this.worker = new Worker(new URL('player-movie-worker.js', this.base));
      this.worker.onerror = event => this.finish(event.message || 'Movie worker failed');
      this.worker.onmessageerror = () => this.finish('Could not read decoded movie');
      this.worker.onmessage = ({data}) => {
        try { this.receive(data); } catch (error) { this.finish(error.message); }
      };
      this.worker.postMessage({type: 'open', blob: this.blob});
      this.timer = setTimeout(() => this.finish('Movie decoder load timed out'), 60000);
    } catch (error) { this.finish(error.message); }
  }

  receive(data) {
    if (!this.alive) return;
    clearTimeout(this.timer);
    this.pending = false;
    if (data.type === 'error') { this.finish(data.error); return; }
    if (data.type === 'ready') { this.pull(); return; }
    if (data.type === 'end') this.eof = true;
    else if (data.type === 'video' || data.type === 'audio') {
      if (!Number.isFinite(data.time) || !Number.isFinite(data.duration) || data.time < 0 || data.duration <= 0) {
        this.finish('Invalid movie timestamp'); return;
      }
      if (this.anchor === null) this.anchor = this.context.currentTime + 0.15;
      this.endTime = Math.max(this.endTime, data.time + data.duration);
      if (data.type === 'video') {
        this.frames.push(data);
        this.bytes += data.data.byteLength;
        this.state({playing: true, width: data.width, height: data.height});
      } else {
        const count = data.data.length / 2;
        const buffer = this.context.createBuffer(2, count, data.rate);
        for (let channel = 0; channel < 2; channel++) {
          const plane = buffer.getChannelData(channel);
          for (let i = 0; i < count; i++) plane[i] = data.data[i * 2 + channel];
        }
        const source = this.context.createBufferSource();
        source.buffer = buffer;
        source.connect(this.output);
        source.onended = () => {
          this.sources.delete(source);
          source.disconnect();
          // Audio completion also drives decoding when background tabs throttle rAF.
          if (this.alive) {
            try { this.tick(); } catch (error) { this.finish(error.message); }
          }
        };
        const start = this.anchor + data.time;
        const offset = Math.max(0, this.context.currentTime - start);
        if (offset < buffer.duration) {
          try {
            source.start(Math.max(start, this.context.currentTime), offset);
            this.sources.add(source);
          } catch (error) { source.disconnect(); throw error; }
        } else source.disconnect();
      }
    }
    this.tick();
  }

  pull() {
    if (!this.alive || this.eof || this.pending) return;
    const now = this.anchor === null ? 0 : this.context.currentTime - this.anchor;
    // Bound both decoded memory and scheduled audio. Never decode a whole clip ahead.
    if (this.bytes >= 32 * 1024 * 1024 || this.endTime > now + 0.5) return;
    this.pending = true;
    this.timer = setTimeout(() => this.finish('Movie decoding timed out'), 30000);
    this.worker.postMessage({type: 'pull'});
  }

  tick() {
    cancelAnimationFrame(this.animation);
    if (!this.alive) return;
    const now = this.anchor === null ? 0 : this.context.currentTime - this.anchor;
    let latest;
    while (this.frames.length && this.frames[0].time <= now) {
      latest = this.frames.shift();
      this.bytes -= latest.data.byteLength;
    }
    if (latest) {
      if (this.element.width !== latest.width) this.element.width = latest.width;
      if (this.element.height !== latest.height) this.element.height = latest.height;
      this.painter.putImageData(new ImageData(new Uint8ClampedArray(latest.data.buffer), latest.width, latest.height), 0, 0);
    }
    if (this.eof && now >= this.endTime && !this.frames.length && !this.sources.size) { this.finish(); return; }
    this.pull();
    this.animation = requestAnimationFrame(() => {
      try { this.tick(); } catch (error) { this.finish(error.message); }
    });
  }

  finish(error = '') {
    if (!this.alive) return;
    if (error) console.error(`Movie playback failed: ${this.path}: ${error}`);
    this.state({playing: false, error});
    this.stop();
  }

  stop() {
    this.alive = false;
    this.listeners.abort();
    clearTimeout(this.timer);
    cancelAnimationFrame(this.animation);
    this.removeNative();
    this.worker?.terminate();
    for (const source of this.sources) { source.stop(); source.disconnect(); }
    this.sources.clear();
    this.frames = [];
    this.blob = undefined;
  }
}
