// The only page API for the site's private Worker runtime.
window.createEasyRpgPlayer = async function createEasyRpgPlayer(options) {
  options.signal?.throwIfAborted();
  const canvas = document.getElementById('canvas');
  if (!canvas?.transferControlToOffscreen || !window.AudioWorkletNode)
    throw new Error('当前浏览器不支持本站播放器所需的画面或音频功能。');
  if (!canvas.hasAttribute('tabindex')) canvas.tabIndex = 0;
  const base = new URL(options.runtimeBase, location.href).href;
  const controller = new AbortController();
  const signal = controller.signal;
  const page = window.parent;
  const context = new AudioContext({latencyHint: 'interactive'});
  const masterVolume = context.createGain();
  masterVolume.connect(context.destination);
  const worker = new Worker(new URL('player-worker.js', base));
  const audioWorker = new Worker(new URL('player-audio-worker.js', base));
  let audioReadyResolve, audioReadyReject;
  const audioReady = new Promise((resolve, reject) => { audioReadyResolve = resolve; audioReadyReject = reject; });
  void audioReady.catch(() => {});
  let node, keys = {}, width = 320, height = 240, gamepadFrame = 0;
  let readyResolve, readyReject, stoppedResolve, stoppedReject, screenshot;
  let closed = false, started = false, stopping = false, stopPromise;
  let movie, movieId, movieRect, movieLoadTimer;
  let consoleInput, consoleWindow, startupTimer;
  let fileOutputConfirmation;
  let windowInterpolation = false;
  let windowFit = true, windowZoom = 0, windowFitRequested = false;
  let screenMode, screenModeButton, screenModeRoot;
  const embeddedCanvasStyle = Object.fromEntries(['width', 'height', 'position', 'left', 'top']
    .map(property => [property, canvas.style[property]]));
  const applyWindowZoom = () => {
    Object.assign(canvas.style, embeddedCanvasStyle);
    if (!windowZoom) return;
    const factor = windowFit ? 1 : 1.5;
    const zoom = Math.max(1, Math.min(windowZoom,
      Math.floor(window.innerWidth * factor / width), Math.floor(window.innerHeight * factor / height)));
    canvas.style.width = `${width * zoom}px`;
    canvas.style.height = `${height * zoom}px`;
    if (windowFitRequested) {
      canvas.style.position = 'fixed';
      canvas.style.left = `${Math.trunc((window.innerWidth - width * zoom) / 2)}px`;
      canvas.style.top = `${Math.trunc((window.innerHeight - height * zoom) / 2)}px`;
    }
  };
  const applyWindowInterpolation = () => {
    canvas.style.imageRendering = windowInterpolation && !page.document.fullscreenElement ? 'auto' : 'pixelated';
  };
  const applyScreenMode = () => {
    if (!screenMode || !page.document.fullscreenElement) return;
    let w = window.innerWidth, h = window.innerHeight;
    if (!screenMode.layout) {
      const zoom = Math.max(1, Math.min(Math.floor(w / width), Math.floor(h / height)));
      w = width * zoom; h = height * zoom;
    } else if (!screenMode.stretch) {
      if (w * height > h * width) w = Math.floor(h * width / height);
      else h = Math.floor(w * height / width);
    }
    canvas.style.position = 'fixed';
    canvas.style.width = `${w}px`; canvas.style.height = `${h}px`;
    canvas.style.left = `${Math.trunc((window.innerWidth - w) / 2)}px`;
    canvas.style.top = `${Math.trunc((window.innerHeight - h) / 2)}px`;
    applyWindowInterpolation();
  };
  const applyVideoLayout = () => {
    if (screenMode && page.document.fullscreenElement) applyScreenMode();
    else {
      if (screenModeRoot) screenModeRoot.style.display = 'contents';
      applyWindowZoom();
    }
  };
  const requestScreenMode = async data => {
    if (closed || stopping) return;
    screenMode = data.fullscreen ? data : undefined;
    width = data.width; height = data.height;
    if (!data.fullscreen) {
      screenModeButton?.remove(); screenModeButton = undefined;
      if (page.document.fullscreenElement) await page.document.exitFullscreen();
      if (screenModeRoot) screenModeRoot.style.display = 'contents';
      windowZoom = data.zoom; windowFitRequested = true;
      applyWindowZoom(); applyWindowInterpolation();
      return;
    }
    if (page.document.fullscreenElement) { applyScreenMode(); return; }
    if (!screenModeRoot) {
      screenModeRoot = document.createElement('div');
      screenModeRoot.style.display = 'contents';
      canvas.before(screenModeRoot); screenModeRoot.append(canvas);
    }
    if (!screenModeButton) {
      screenModeButton = document.createElement('button');
      screenModeButton.textContent = '进入全屏';
      screenModeButton.style.cssText = 'position:fixed;left:50%;top:8px;z-index:2147483647';
      document.body.append(screenModeButton);
      screenModeButton.onclick = async () => {
        if (!screenMode || closed || stopping) return;
        try {
          screenModeRoot.style.cssText = 'display:block;width:100vw;height:100vh;background:black';
          await screenModeRoot.requestFullscreen();
          screenModeButton.remove(); screenModeButton = undefined;
          applyScreenMode(); canvas.focus();
        } catch (error) { console.error(error); }
      };
    }
  };
  const consolePalette = ['#000000', '#000080', '#008000', '#008080', '#800000', '#800080', '#808000', '#c0c0c0',
    '#808080', '#0000ff', '#00ff00', '#00ffff', '#ff0000', '#ff00ff', '#ffff00', '#ffffff'];
  const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
  // Cancellation can arrive while the audio module is still being fetched.
  void ready.catch(() => {});
  const send = data => { if (!closed) worker.postMessage(data); };
  const report = message => {
    closeConsoleWindow(true);
    console.error(message);
    const error = new Error(message);
    readyReject(error); stoppedReject?.(error); screenshot?.reject(error);
    audioReadyReject(error);
    screenshot = undefined;
    options.onError?.(error);
  };
  const listen = (element, event, callback) => element.addEventListener(event, callback, {signal});
  function cancelConsoleInput() { consoleInput?.controller.abort(); }

  function requestFileOutputConfirmation(data) {
    if (closed || stopping || fileOutputConfirmation) {
      send({type: 'file-output-confirmation-result', id: data.id, answer: false});
      return;
    }
    clearTimeout(startupTimer);
    const dialog = document.createElement('dialog');
    dialog.setAttribute('aria-label', '文件输出');
    const message = document.createElement('p');
    message.textContent = '短时间内输出了多个文件。是否继续？选择“退出”将关闭游戏。';
    const yes = document.createElement('button');
    yes.textContent = '继续';
    const no = document.createElement('button');
    no.textContent = '退出';
    const finish = answer => {
      if (fileOutputConfirmation?.dialog !== dialog) return;
      fileOutputConfirmation = undefined;
      dialog.remove();
      send({type: 'file-output-confirmation-result', id: data.id, answer: answer === true});
      if (!closed && !stopping) canvas.focus();
      if (!started && !closed && !stopping) startStartupTimer();
    };
    yes.onclick = () => finish(true);
    no.onclick = () => finish(false);
    dialog.oncancel = event => { event.preventDefault(); finish(false); };
    dialog.onclose = () => finish(false);
    dialog.append(message, yes, no); document.body.append(dialog);
    fileOutputConfirmation = {dialog, cancel: () => finish(false)};
    dialog.showModal(); yes.focus();
  }
  function closeConsoleWindow(notify = false) {
    cancelConsoleInput();
    const panel = consoleWindow;
    consoleWindow = undefined;
    if (!panel) return;
    panel.dialog.remove();
    if (notify) send({type: 'console-closed', generation: panel.generation});
    if (!closed && !stopping) canvas.focus();
  }
  function appendConsoleText(text, foreground, background) {
    const panel = consoleWindow;
    if (!panel || !text) return;
    const font = getComputedStyle(panel.output);
    panel.measure.font = `${font.fontStyle} ${font.fontWeight} ${font.fontSize} ${font.fontFamily}`;
    const space = panel.measure.measureText(' ').width;
    panel.columns ||= Math.max(1, Math.floor(panel.output.clientWidth / space));
    const color = `${foreground}:${background}`;
    const styled = value => {
      const run = document.createElement('span'); run.dataset.color = color;
      run.style.color = consolePalette[foreground]; run.style.backgroundColor = consolePalette[background];
      run.append(document.createTextNode(value)); return run;
    };
    const glyphColumns = glyph => Math.max(1, Math.round(panel.measure.measureText(glyph).width / panel.measure.measureText(' ').width));
    const writeRun = value => {
      const before = panel.row.textContent;
      let width = 0;
      for (const item of panel.segmenter ? panel.segmenter.segment(value) : value) width += glyphColumns(item.segment ?? item);
      let start = before.length, end = before.length, firstColumn = 0, lastColumn = 0, offset = 0, column = 0;
      for (const item of panel.segmenter ? panel.segmenter.segment(before) : before) {
        const glyph = item.segment ?? item, next = column + glyphColumns(glyph);
        if (next > panel.column && column < panel.column + width) {
          if (start === before.length) { start = offset; firstColumn = column; }
          end = offset + glyph.length; lastColumn = next;
        }
        offset += glyph.length; column = next;
      }
      if (start === before.length) firstColumn = lastColumn = column;
      const left = Math.max(0, panel.column - firstColumn), right = Math.max(0, lastColumn - panel.column - width);
      if (start === before.length && !left) {
        const last = panel.row.lastElementChild;
        if (last?.dataset.color === color) last.firstChild.appendData(value);
        else panel.row.append(styled(value));
      } else {
        const oldRuns = [...panel.row.children];
        // Replacing half of a wide glyph leaves the other cell blank, with its old colors.
        const padding = (count, index) => {
          let at = 0, old;
          for (const run of oldRuns) {
            if (index >= at && index < at + run.textContent.length) { old = run; break; }
            at += run.textContent.length;
          }
          const span = old ? old.cloneNode(false) : document.createElement('span');
          if (!old) { span.dataset.color = '7:0'; span.style.color = consolePalette[7]; span.style.backgroundColor = consolePalette[0]; }
          span.append(document.createTextNode(' '.repeat(count))); return span;
        };
        const fragment = document.createDocumentFragment();
        let offset = 0, inserted = false;
        const insert = () => {
          if (left) fragment.append(padding(left, start));
          fragment.append(styled(value));
          if (right) fragment.append(padding(right, end - 1));
          inserted = true;
        };
        for (const run of oldRuns) {
          const valueBefore = run.textContent, next = offset + valueBefore.length;
          const copy = part => {
            if (!part) return;
            const span = run.cloneNode(false); span.append(document.createTextNode(part)); fragment.append(span);
          };
          if (next <= start) fragment.append(run);
          else {
            copy(valueBefore.slice(0, Math.max(0, start - offset)));
            if (!inserted) insert();
            copy(valueBefore.slice(Math.max(0, end - offset)));
          }
          offset = next;
        }
        if (!inserted) insert();
        panel.row.replaceChildren(fragment);
      }
      panel.cursor = start + left + value.length; panel.column += width;
      panel.rowLength = panel.row.textContent.length;
    };
    const nextRow = () => {
      panel.output.append(document.createTextNode('\n'));
      panel.row = document.createElement('span'); panel.output.append(panel.row);
      panel.cursor = panel.rowLength = panel.column = 0;
    };
    const write = value => {
      const width = panel.columns;
      let used = panel.column;
      let run = '';
      const glyphs = panel.segmenter ? panel.segmenter.segment(value) : value;
      for (const item of glyphs) {
        const glyph = item.segment ?? item;
        const advance = glyphColumns(glyph);
        // Filling the last column delays wrapping until the next printable glyph.
        if (used > 0 && used + advance > width + 0.01) {
          if (run) writeRun(run);
          nextRow(); run = ''; used = 0;
        }
        run += glyph; used += advance;
      }
      if (run) writeRun(run);
    };
    let bell = false;
    for (let i = 0; i < text.length;) {
      const character = text[i++];
      if (character === '\r') panel.cursor = panel.column = 0;
      else if (character === '\b' && panel.column) {
        const before = panel.row.textContent;
        let column = 0, offset = 0, found = false;
        for (const item of panel.segmenter ? panel.segmenter.segment(before) : before) {
          const glyph = item.segment ?? item, next = column + glyphColumns(glyph);
          if (next >= panel.column) { panel.column = column; panel.cursor = offset; found = true; break; }
          column = next; offset += glyph.length;
        }
        if (!found) { --panel.column; panel.cursor = before.length; }
      }
      else if (character === '\n') {
        nextRow();
      } else if (character === '\t') {
        // Legacy WriteConsoleW advances to a tab stop without erasing existing cells.
        panel.column += Math.min(panel.columns - panel.column, 8 - (panel.column & 7));
        panel.cursor = panel.row.textContent.length;
      } else if (character === '\x07') bell = true;
      else if (character === '\b') { /* Already at the row start. */ }
      else if (character === '\0') write(' ');
      else {
        const start = i - 1;
        while (i < text.length) {
          const code = text.charCodeAt(i);
          if (code === 0 || (code >= 7 && code <= 10) || code === 13) break;
          ++i;
        }
        write(text.slice(start, i));
      }
    }
    // A page has no OS console bell; coalesce this write into a visual alert.
    if (bell) panel.dialog.animate([{opacity: 0.5}, {opacity: 1}], {duration: 150});
    panel.output.scrollTop = panel.output.scrollHeight;
  }
  function setConsoleColor(foreground, background) {
    const panel = consoleWindow;
    panel.foreground = foreground; panel.background = background;
    panel.input.style.color = consolePalette[foreground];
    panel.input.style.backgroundColor = consolePalette[background];
  }
  function openConsoleWindow(data) {
    if (consoleWindow) { consoleWindow.caption.textContent = data.text; return; }
    const gameActive = document.activeElement === canvas || document.activeElement === document.body;
    const dialog = document.createElement('dialog');
    dialog.setAttribute('aria-label', '控制台'); dialog.tabIndex = -1;
    Object.assign(dialog.style, {position: 'fixed', inset: '16px 16px auto auto', margin: '0',
      width: 'min(640px, calc(100vw - 32px))', height: 'min(480px, calc(100vh - 32px))',
      boxSizing: 'border-box', padding: '12px', zIndex: '1000', background: '#000', color: '#c0c0c0'});
    const header = document.createElement('div'); header.style.display = 'flex'; header.style.gap = '8px';
    const caption = document.createElement('strong'); caption.textContent = data.text; caption.style.flex = '1';
    const close = document.createElement('button'); close.type = 'button'; close.textContent = '关闭';
    close.onclick = () => closeConsoleWindow(true);
    header.append(caption, close);
    const output = document.createElement('pre');
    Object.assign(output.style, {height: 'calc(100% - 64px)', overflow: 'auto', whiteSpace: 'pre', margin: '8px 0'});
    output.tabIndex = 0; output.setAttribute('aria-label', '控制台输出');
    const form = document.createElement('form'); form.hidden = true;
    const input = document.createElement('input'); input.type = 'text'; input.setAttribute('aria-label', '控制台输入');
    input.style.width = '60%';
    const submit = document.createElement('button'); submit.type = 'submit'; submit.textContent = '确定';
    const cancel = document.createElement('button'); cancel.type = 'button'; cancel.textContent = '取消';
    form.append(input, submit, cancel); dialog.append(header, output, form); document.body.append(dialog);
    const row = document.createElement('span'); output.append(row);
    consoleWindow = {dialog, caption, output, form, input, cancel, generation: data.generation,
      row, cursor: 0, rowLength: 0, column: 0,
      segmenter: typeof Intl.Segmenter === 'function' ? new Intl.Segmenter(undefined, {granularity: 'grapheme'}) : null,
      measure: document.createElement('canvas').getContext('2d')};
    setConsoleColor(data.foreground, data.background);
    dialog.onclose = () => { if (consoleWindow?.dialog === dialog) closeConsoleWindow(true); };
    try { dialog.show(); }
    catch (error) { closeConsoleWindow(true); throw error; }
    if (gameActive) canvas.focus();
  }
  function consoleCommand(data) {
    if (closed || stopping) return;
    if (data.op === 'open') openConsoleWindow(data);
    else if (data.op === 'close') closeConsoleWindow();
    else if (consoleWindow?.generation === data.generation) {
      if (data.op === 'write') appendConsoleText(data.text, data.foreground, data.background);
      else if (data.op === 'color') setConsoleColor(data.foreground, data.background);
      else if (data.op === 'focus') {
        const inConsole = consoleWindow.dialog.contains(document.activeElement);
        const inGame = document.activeElement === canvas || document.activeElement === document.body;
        if (data.options && inGame) consoleWindow.dialog.focus();
        else if (!data.options && inConsole) canvas.focus();
      }
    }
  }
  const startStartupTimer = () => {
    clearTimeout(startupTimer);
    startupTimer = setTimeout(() => report('游戏启动超时，请检查运行日志。'), 45000);
  };
  function consoleInputDialog(title, signal) {
    return new Promise(resolve => {
      const panel = consoleWindow;
      if (signal.aborted || !panel) { resolve(null); return; }
      const {dialog, form, caption, input, cancel} = panel;
      caption.textContent = title;
      input.value = ''; form.hidden = false;
      const abort = () => finish(null);
      const escape = event => { if (event.key === 'Escape') { event.preventDefault(); abort(); } };
      const finish = value => {
        signal.removeEventListener('abort', abort);
        dialog.removeEventListener('keydown', escape);
        form.onsubmit = null; cancel.onclick = null; form.hidden = true;
        if (typeof value === 'string') appendConsoleText(value + '\r\n', panel.foreground, panel.background);
        if (!closed && !stopping) canvas.focus();
        resolve(value);
      };
      signal.addEventListener('abort', abort, {once: true});
      dialog.addEventListener('keydown', escape);
      form.onsubmit = event => { event.preventDefault(); finish(input.value); };
      cancel.onclick = abort;
      input.focus();
    });
  }
  function requestConsoleInput(data) {
    if (closed || stopping || consoleInput || consoleWindow?.generation !== data.generation) {
      send({type: 'console-input-result', id: data.id, value: null});
      return;
    }
    clearTimeout(startupTimer);
    const request = {id: data.id, controller: new AbortController()};
    consoleInput = request;
    const signal = request.controller.signal;
    consoleInputDialog(data.title, signal).catch(error => { console.error(error); return null; }).then(value => {
      send({type: 'console-input-result', id: data.id, value: !stopping && typeof value === 'string' ? value : null});
    }).finally(() => {
      if (consoleInput === request) consoleInput = undefined;
      if (!started && !closed && !stopping) startStartupTimer();
    });
  }
  if (options.signal) listen(options.signal, 'abort', () => {
    closeConsoleWindow(true);
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
      movie = new MoviePlayback({canvas, context, output: masterVolume, base, state});
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
    } else if (data.type === 'size') {
      width = data.width; height = data.height;
      applyVideoLayout();
    }
    else if (data.type === 'window-zoom') {
      width = data.width; height = data.height; windowZoom = data.zoom;
      applyWindowZoom();
      applyWindowInterpolation();
    }
    else if (data.type === 'window-fit') {
      width = data.width; height = data.height; windowFit = data.enabled;
      windowFitRequested = true;
      if (!windowZoom) windowZoom = 1;
      applyWindowZoom();
    }
    else if (data.type === 'window-interpolation') {
      windowInterpolation = data.enabled;
      applyWindowInterpolation();
    }
    else if (data.type === 'screen-mode') void requestScreenMode(data).catch(report);
    else if (data.type === 'master-volume') masterVolume.gain.setValueAtTime(data.gain, context.currentTime);
    else if (data.type === 'log') (console[data.level] || console.log)(data.message);
    else if (data.type === 'error') report(data.message);
    else if (data.type === 'save-error') {
      console.error(data.message);
      stoppedReject?.(new Error(data.message));
      options.onError?.(new Error(data.message));
    }
    else if (data.type === 'stopped') {
      stopping = true;
      fileOutputConfirmation?.cancel();
      closeConsoleWindow();
      stopMovie();
      if (stoppedResolve) stoppedResolve();
      else if (started) options.onExit?.();
      else readyReject(new Error('播放器在游戏启动前退出，请检查运行日志。'));
    }
    else if (data.type === 'console-input') requestConsoleInput(data);
    else if (data.type === 'file-output-confirmation') requestFileOutputConfirmation(data);
    else if (data.type === 'console-command') {
      try { consoleCommand(data); } catch (error) { report(String(error)); }
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
    if (consoleInput || consoleWindow?.dialog.contains(document.activeElement)) {
      if (event.type === 'keyup') send({type: 'key', key: keys[name], pressed: false});
      return;
    }
    event.preventDefault();
    send({type: 'key', key: keys[name], pressed: event.type === 'keydown'});
    if (event.type === 'keydown') void context.resume().catch(() => {});
  };
  listen(document, 'keydown', keyEvent); listen(document, 'keyup', keyEvent);
  // The site's touch controls live outside the iframe, but belong to the same player.
  const gameFocused = () => !page.document.hidden && page.document.hasFocus() && !consoleWindow?.dialog.contains(document.activeElement);
  const focus = () => send({type: 'focus', focused: gameFocused(), fullscreen: !!page.document.fullscreenElement});
  // Firefox's blur precedes the settled focus state when entering the iframe.
  // Focus can also return directly to the iframe after switching tabs.
  const scheduleFocus = () => queueMicrotask(focus);
  for (const target of new Set([page, window])) {
    listen(target, 'focus', scheduleFocus); listen(target, 'blur', scheduleFocus);
  }
  listen(page.document, 'visibilitychange', focus);
  listen(page.document, 'fullscreenchange', focus);
  listen(page.document, 'fullscreenchange', applyWindowInterpolation);
  listen(page.document, 'fullscreenchange', applyVideoLayout);
  listen(document, 'fullscreenchange', applyVideoLayout);
  for (const target of new Set([page, window])) listen(target, 'resize', applyVideoLayout);
  listen(document, 'focusin', focus);
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
  let previousJoystickAvailable;
  const joystickAvailable = () => Array.from(navigator.getGamepads()).some(pad => pad?.connected);
  const updateJoystickAvailability = () => {
    const available = joystickAvailable();
    if (available !== previousJoystickAvailable) {
      previousJoystickAvailable = available;
      send({type: 'joystick-availability', available});
    }
  };
  listen(page, 'gamepadconnected', updateJoystickAvailability);
  listen(page, 'gamepaddisconnected', updateJoystickAvailability);
  const pollGamepad = () => {
    updateJoystickAvailability();
    const pad = gameFocused() ? [...navigator.getGamepads()].find(p => p?.mapping === 'standard') : null;
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

  startStartupTimer();
  const shutdown = async () => {
    fileOutputConfirmation?.cancel();
    closeConsoleWindow();
    screenModeButton?.remove(); screenModeButton = undefined;
    if (page.document.fullscreenElement === screenModeRoot) await page.document.exitFullscreen();
    if (screenModeRoot) screenModeRoot.replaceWith(canvas);
    closed = true; controller.abort(); cancelAnimationFrame(gamepadFrame); stopMovie(); node?.disconnect(); masterVolume.disconnect();
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
    node.connect(masterVolume);
    audioWorker.postMessage({type: 'start', runtimeBase: base, packages: options.packages,
      workId: options.workId, sampleRate: context.sampleRate,
      audioPort: audioChannel.port2, controlPort: controlChannel.port1},
      [audioChannel.port2, controlChannel.port1]);
    const audioCapabilities = await Promise.race([audioReady, ready]);
    const offscreen = canvas.transferControlToOffscreen();
    worker.postMessage({type: 'start', canvas: offscreen, runtimeBase: base, packages: options.packages,
      workId: options.workId, sampleRate: context.sampleRate, arguments: options.arguments || [],
      joystickAvailable: joystickAvailable(),
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
        fileOutputConfirmation?.cancel();
        closeConsoleWindow();
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
