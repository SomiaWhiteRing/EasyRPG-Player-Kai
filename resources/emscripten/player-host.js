// The only page API for the site's private Worker runtime.
window.createEasyRpgPlayer = async function createEasyRpgPlayer(options) {
  options.signal?.throwIfAborted();
  const canvas = document.getElementById('canvas');
  if (!canvas?.transferControlToOffscreen || !window.AudioWorkletNode)
    throw new Error('当前浏览器不支持本站播放器所需的画面或音频功能。');
  const base = new URL(options.runtimeBase, location.href).href;
  const controller = new AbortController();
  const signal = controller.signal;
  const page = window.parent;
  const context = new AudioContext({latencyHint: 'interactive'});
  const worker = new Worker(new URL('player-worker.js', base));
  const audioWorker = new Worker(new URL('player-audio-worker.js', base));
  let audioReadyResolve, audioReadyReject;
  const audioReady = new Promise((resolve, reject) => { audioReadyResolve = resolve; audioReadyReject = reject; });
  void audioReady.catch(() => {});
  let node, keys = {}, width = 320, height = 240, gamepadFrame = 0;
  let readyResolve, readyReject, stoppedResolve, stoppedReject, screenshot;
  let closed = false, started = false, stopping = false, stopPromise;
  let movie, movieId, movieRect, movieLoadTimer;
  const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
  // Cancellation can arrive while the audio module is still being fetched.
  void ready.catch(() => {});
  const send = data => { if (!closed) worker.postMessage(data); };
  const report = message => {
    console.error(message);
    const error = new Error(message);
    readyReject(error); stoppedReject?.(error); screenshot?.reject(error);
    audioReadyReject(error);
    screenshot = undefined;
    options.onError?.(error);
  };
  const listen = (element, event, callback) => element.addEventListener(event, callback, {signal});
  if (options.signal) listen(options.signal, 'abort', () => {
    if (!started) readyReject(options.signal.reason);
  });
  const download = (blob, filename) => {
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  const stopMovie = () => {
    clearTimeout(movieLoadTimer);
    movieId = undefined;
    movie?.stop();
    movie = undefined;
    movieRect = undefined;
  };
  const openMovie = async data => {
    if (closed || stopping) return;
    stopMovie();
    movieId = data.id;
    const state = state => {
      if (!closed && movieId === data.id) send({type: 'movie-state', id: data.id, state});
    };
    movieLoadTimer = setTimeout(() => {
      if (movieId !== data.id) return;
      state({playing: false, error: 'Movie playback module load timed out'});
      stopMovie();
    }, 60000);
    try {
      const {MoviePlayback} = await import(new URL('player-movie.js', base).href);
      if (closed || stopping || movieId !== data.id) return;
      clearTimeout(movieLoadTimer);
      movie = new MoviePlayback({canvas, context, base, state});
      movie.start(data.blob, data.path);
      if (movieRect) movie.updateRect(movieRect);
    } catch (error) {
      if (movieId !== data.id) return;
      console.error(`Movie playback failed: ${data.path}: ${error}`);
      state({playing: false, error: String(error)});
      stopMovie();
    }
  };
  worker.onmessage = ({data}) => {
    if (data.type === 'ready') {
      keys = data.keys; width = data.width; height = data.height;
      started = true;
      node.port.postMessage({type: 'start'});
      readyResolve();
    } else if (data.type === 'size') { width = data.width; height = data.height; }
    else if (data.type === 'log') (console[data.level] || console.log)(data.message);
    else if (data.type === 'error') report(data.message);
    else if (data.type === 'save-error') {
      console.error(data.message);
      stoppedReject?.(new Error(data.message));
      options.onError?.(new Error(data.message));
    }
    else if (data.type === 'stopped') {
      stopping = true;
      stopMovie();
      if (stoppedResolve) stoppedResolve();
      else if (started) options.onExit?.();
      else readyReject(new Error('播放器在游戏启动前退出，请检查运行日志。'));
    }
    else if (data.type === 'screenshot' && screenshot) {
      screenshot.resolve({blob: new Blob([data.bytes], {type: 'image/png'}), width: data.width, height: data.height});
      screenshot = undefined;
    } else if (data.type === 'download') download(new Blob([data.bytes]), data.filename);
    else if (data.type === 'upload') {
      const input = document.createElement('input');
      input.type = 'file';
      input.accept = data.kind === 'save' ? '.lsd' : data.kind === 'font' ? '.ttf,.otf' : '.sf2';
      input.onchange = async () => {
        const file = input.files[0];
        if (file) send({type: 'upload', kind: data.kind, slot: data.slot, name: file.name, bytes: await file.arrayBuffer()});
      };
      input.click();
    } else if (data.type === 'cursor') canvas.style.cursor = data.visible ? 'default' : 'none';
    else if (data.type === 'fullscreen') options.onFullscreen?.();
    else if (data.type === 'url') {
      try {
        const url = new URL(data.url);
        if (url.protocol === 'https:' || url.protocol === 'http:') window.open(url.href, '_blank', 'noopener,noreferrer');
      } catch { /* Invalid game-supplied URL. */ }
    } else if (data.type === 'movie-open') {
      void openMovie(data);
    } else if (data.type === 'movie-stop' && data.id === movieId) stopMovie();
    else if (data.type === 'movie-rect' && data.id === movieId) {
      movieRect = {...data, screenWidth: width, screenHeight: height};
      movie?.updateRect(movieRect);
    }
  };
  audioWorker.onmessage = ({data}) => {
    if (data.type === 'ready') audioReadyResolve(data.capabilities);
    else if (data.type === 'log') (console[data.level] || console.log)(data.message);
    else if (data.type === 'error') report(data.message);
  };
  audioWorker.onerror = event => report(event.message || '音频组件启动失败');
  worker.onerror = event => report(event.message);
  worker.onmessageerror = () => report('播放器消息无法读取。');

  const special = {Enter: 'RETURN', NumpadEnter: 'RETURN', Escape: 'ESCAPE', Space: 'SPACE',
    ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT',
    ShiftLeft: 'LSHIFT', ShiftRight: 'RSHIFT', ControlLeft: 'LCTRL', ControlRight: 'RCTRL',
    AltLeft: 'LALT', AltRight: 'RALT', PageUp: 'PGUP', PageDown: 'PGDN', End: 'ENDS',
    Backspace: 'BACKSPACE', Tab: 'TAB', Home: 'HOME', Delete: 'DEL', Insert: 'INSERT',
    Backquote: 'BACKTICK', BracketLeft: 'LEFT_BRACKET', BracketRight: 'RIGHT_BRACKET',
    Backslash: 'BACKSLASH', Quote: 'APOSTROPH', Semicolon: 'SEMICOLON', Comma: 'COMMA',
    Period: 'PERIOD', Slash: 'SLASH', CapsLock: 'CAPS_LOCK', NumLock: 'NUM_LOCK', ScrollLock: 'SCROLL_LOCK',
    NumpadMultiply: 'KP_MULTIPLY', NumpadAdd: 'KP_ADD',
    NumpadSubtract: 'KP_SUBTRACT', NumpadDecimal: 'KP_PERIOD', NumpadDivide: 'KP_DIVIDE'};
  const keyEvent = event => {
    const name = special[event.code] || (/^Key[A-Z]$/.test(event.code) ? event.code.slice(3) :
      /^Digit\d$/.test(event.code) ? `N${event.code.slice(5)}` :
      /^Numpad\d$/.test(event.code) ? `KP${event.code.slice(6)}` : event.code);
    if (!keys[name]) return;
    event.preventDefault();
    send({type: 'key', key: keys[name], pressed: event.type === 'keydown'});
    if (event.type === 'keydown') void context.resume().catch(() => {});
  };
  listen(document, 'keydown', keyEvent); listen(document, 'keyup', keyEvent);
  // The site's touch controls live outside the iframe, but belong to the same player.
  const focus = () => send({type: 'focus', focused: !page.document.hidden && page.document.hasFocus(), fullscreen: !!page.document.fullscreenElement});
  // Firefox's blur precedes the settled focus state when entering the iframe.
  // Focus can also return directly to the iframe after switching tabs.
  const scheduleFocus = () => queueMicrotask(focus);
  for (const target of new Set([page, window])) {
    listen(target, 'focus', scheduleFocus); listen(target, 'blur', scheduleFocus);
  }
  listen(page.document, 'visibilitychange', focus);
  listen(page.document, 'fullscreenchange', focus);
  listen(canvas, 'pointerleave', () => send({type: 'mouse', x: -1, y: -1, focus: false}));
  listen(canvas, 'wheel', event => {
    const key = keys[event.deltaY < 0 ? 'MOUSE_SCROLLUP' : 'MOUSE_SCROLLDOWN'];
    if (!key) return;
    event.preventDefault();
    send({type: 'key', key, pressed: true});
    setTimeout(() => send({type: 'key', key, pressed: false}), 40);
  });
  const pointerPosition = event => {
    const rect = canvas.getBoundingClientRect();
    return {x: Math.floor((event.clientX-rect.left)*width/rect.width), y: Math.floor((event.clientY-rect.top)*height/rect.height)};
  };
  listen(canvas, 'pointermove', event => {
    send({type: 'mouse', ...pointerPosition(event), focus: true});
  });
  for (const type of ['pointerdown', 'pointerup', 'pointercancel', 'lostpointercapture']) listen(canvas, type, event => {
    const pressed = type === 'pointerdown';
    if (pressed) {
      event.preventDefault(); canvas.focus(); canvas.setPointerCapture(event.pointerId);
      void context.resume().catch(() => {});
    }
    const position = pointerPosition(event);
    send({type: 'mouse', ...position, focus: true});
    if (event.pointerType === 'touch') {
      send({type: 'touch', ...position, id: event.pointerId, pressed});
    } else if (type === 'pointercancel' || type === 'lostpointercapture') {
      for (const name of ['MOUSE_LEFT', 'MOUSE_MIDDLE', 'MOUSE_RIGHT'])
        if (keys[name]) send({type: 'key', key: keys[name], pressed: false});
    } else {
      const key = keys[['MOUSE_LEFT', 'MOUSE_MIDDLE', 'MOUSE_RIGHT'][event.button]];
      if (key) send({type: 'key', key, pressed});
    }
  });

  const gamepadButtons = ['JOY_A', 'JOY_B', 'JOY_X', 'JOY_Y', 'JOY_SHOULDER_LEFT',
    'JOY_SHOULDER_RIGHT', null, null, 'JOY_BACK', 'JOY_START', 'JOY_LSTICK', 'JOY_RSTICK',
    'JOY_DPAD_UP', 'JOY_DPAD_DOWN', 'JOY_DPAD_LEFT', 'JOY_DPAD_RIGHT', 'JOY_GUIDE'];
  let previousPad = '';
  const pollGamepad = () => {
    const pad = !page.document.hidden && page.document.hasFocus() ? [...navigator.getGamepads()].find(p => p?.mapping === 'standard') : null;
    const state = {axes: [0, 1, 2, 3].map(i => pad?.axes[i] || 0),
      triggers: [6, 7].map(i => pad?.buttons[i]?.value || 0),
      buttons: gamepadButtons.map((_, i) => !!pad?.buttons[i]?.pressed)};
    const serialized = JSON.stringify(state);
    if (serialized !== previousPad && Object.keys(keys).length) {
      previousPad = serialized;
      send({type: 'gamepad', axes: state.axes, triggers: state.triggers});
      gamepadButtons.forEach((name, i) => {
        if (name && keys[name]) send({type: 'key', key: keys[name], pressed: state.buttons[i]});
      });
    }
    gamepadFrame = requestAnimationFrame(pollGamepad);
  };

  const startupTimer = setTimeout(() => report('游戏启动超时，请检查运行日志。'), 45000);
  const shutdown = async () => {
    closed = true; controller.abort(); cancelAnimationFrame(gamepadFrame); stopMovie(); node?.disconnect();
    screenshot?.reject(new Error('游戏已停止。')); screenshot = undefined;
    try { await context.close(); } finally { worker.terminate(); audioWorker.terminate(); }
  };
  try {
    // Cancel/timeout must also end startup while the audio module is loading.
    await Promise.race([context.audioWorklet.addModule(new URL('player-audio.js', base)), ready]);
    node = new AudioWorkletNode(context, 'easyrpg-audio', {numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]});
    const audioChannel = new MessageChannel();
    const controlChannel = new MessageChannel();
    node.port.postMessage({type: 'connect', port: audioChannel.port1}, [audioChannel.port1]);
    node.connect(context.destination);
    audioWorker.postMessage({type: 'start', runtimeBase: base, packages: options.packages,
      workId: options.workId, sampleRate: context.sampleRate,
      audioPort: audioChannel.port2, controlPort: controlChannel.port1},
      [audioChannel.port2, controlChannel.port1]);
    const audioCapabilities = await Promise.race([audioReady, ready]);
    const offscreen = canvas.transferControlToOffscreen();
    worker.postMessage({type: 'start', canvas: offscreen, runtimeBase: base, packages: options.packages,
      workId: options.workId, sampleRate: context.sampleRate, arguments: options.arguments || [],
      audioPort: controlChannel.port2, audioCapabilities}, [offscreen, controlChannel.port2]);
    await ready;
    focus();
    gamepadFrame = requestAnimationFrame(pollGamepad);
    void context.resume().catch(() => {});
  } catch (error) {
    await shutdown(); throw error;
  } finally { clearTimeout(startupTimer); }
  return {
    captureScreenshot() {
      if (screenshot) return Promise.reject(new Error('正在截取图片，请稍后重试。'));
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => { screenshot = undefined; reject(new Error('截取图片超时。')); }, 5000);
        screenshot = {resolve: value => { clearTimeout(timer); resolve(value); },
          reject: error => { clearTimeout(timer); reject(error); }};
        send({type: 'capture'});
      });
    },
    stop() {
      return stopPromise ||= (async () => {
        stopping = true;
        stopMovie();
        const stopped = new Promise((resolve, reject) => { stoppedResolve = resolve; stoppedReject = reject; });
        send({type: 'stop'});
        const timeout = setTimeout(() => stoppedReject(new Error('等待存档写入超时。')), 15000);
        try { await stopped; await shutdown(); }
        catch (error) { stopPromise = undefined; throw error; }
        finally { clearTimeout(timeout); }
      })();
    },
  };
};
