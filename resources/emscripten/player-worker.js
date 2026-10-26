/* global createEasyRpgEngine */
let engine;
let joystickAvailable = false;
let firstFrame = true;
let forwardedSoundfont = '';
let consoleInputId = 0;
let consoleInput;
let fileOutputConfirmation;
let fileOutputConfirmationId = 0;
let consoleOwned = false;
let consoleGeneration = 0;
let consoleForeground = 7, consoleBackground = 0;

function consoleCommand(command) {
  if (command.op === 'close') {
    if (!consoleOwned) return true;
    consoleOwned = false;
    consoleGeneration++;
    finishConsoleInput(null);
  } else {
    if (engine?.stopping || engine?.stopped) return false;
    if (command.op === 'open') {
      if (consoleOwned && !command.options) return true;
      if (!consoleOwned) {
        consoleOwned = true;
        consoleGeneration++;
        consoleForeground = 7; consoleBackground = 0;
      }
    } else if (!consoleOwned) return false;
    if (command.op === 'color') {
      if (command.options & 8) consoleForeground = command.foreground & 15;
      if (command.options & 16) consoleBackground = command.background & 15;
    }
  }
  postMessage({type: 'console-command', ...command, generation: consoleGeneration,
    foreground: consoleForeground, background: consoleBackground});
  return true;
}

function finishConsoleInput(value) {
  const request = consoleInput;
  consoleInput = undefined;
  request?.resolve(typeof value === 'string' ? value : null);
}

function readConsoleLine(title) {
  if (!consoleOwned || engine?.stopping || engine?.stopped || consoleInput) return Promise.resolve(null);
  return new Promise(resolve => {
    consoleInput = {id: ++consoleInputId, resolve};
    postMessage({type: 'console-input', id: consoleInput.id, title, generation: consoleGeneration});
  });
}

function finishFileOutputConfirmation(answer) {
  const request = fileOutputConfirmation;
  fileOutputConfirmation = undefined;
  request?.resolve(answer === true);
}

function confirmFileOutput() {
  if (engine?.stopping || engine?.stopped || fileOutputConfirmation) return Promise.resolve(false);
  return new Promise(resolve => {
    fileOutputConfirmation = {id: ++fileOutputConfirmationId, resolve};
    postMessage({type: 'file-output-confirmation', id: fileOutputConfirmation.id});
  });
}

function report(error) {
  postMessage({type: 'error', message: error?.stack || String(error)});
}
self.addEventListener('unhandledrejection', event => report(event.reason));
self.addEventListener('error', event => report(event.error || event.message));

function createRenderer(canvas) {
  const gl = canvas.getContext('webgl2', {alpha: false, antialias: false, depth: false, stencil: false});
  if (!gl) throw new Error('当前浏览器无法在 Worker 中创建游戏画面。');
  const shader = (type, source) => {
    const value = gl.createShader(type);
    gl.shaderSource(value, source);
    gl.compileShader(value);
    if (!gl.getShaderParameter(value, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(value));
    return value;
  };
  const program = gl.createProgram();
  gl.attachShader(program, shader(gl.VERTEX_SHADER, `#version 300 es
    out vec2 uv;
    void main() {
      vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
      uv = vec2(p.x, 1.0-p.y);
      gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);
    }`));
  gl.attachShader(program, shader(gl.FRAGMENT_SHADER, `#version 300 es
    precision mediump float;
    in vec2 uv;
    uniform sampler2D pixels;
    out vec4 color;
    void main() { color = vec4(texture(pixels, uv).bgr, 1.0); }`));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
  gl.useProgram(program);
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  let width = 0, height = 0;
  canvas.addEventListener('webglcontextlost', event => {
    event.preventDefault(); report(new Error('游戏画面上下文已丢失，请重新启动游戏。'));
  });
  return (pointer, w, h, pitch) => {
    if (width !== w || height !== h) {
      width = canvas.width = w;
      height = canvas.height = h;
      gl.viewport(0, 0, w, h);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      postMessage({type: 'size', width: w, height: h});
    }
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, pitch / 4);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE,
      engine.HEAPU8.subarray(pointer, pointer + pitch * h));
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    if (firstFrame && engine.gameReady) {
      firstFrame = false;
      postMessage({type: 'ready', keys: engine.keyNames, width: w, height: h});
    }
  };
}

self.onmessage = async ({data}) => {
  try {
    if (data.type === 'joystick-availability') {
      joystickAvailable = data.available === true;
      return;
    }
    if (data.type === 'start') {
      if (engine) throw new Error('Player already started');
      joystickAvailable = data.joystickAvailable === true;
      const present = createRenderer(data.canvas);
      importScripts(new URL('player-files.js', data.runtimeBase).href,
        new URL('easyrpg-player.js', data.runtimeBase).href);
      let generation = 0;
      const audioPort = data.audioPort;
      audioPort.onmessage = ({data: state}) => {
        if (engine && state.generation === generation) engine.audioState = state;
      };
      engine = await createEasyRpgEngine({
        canvas: data.canvas, workId: data.workId, sampleRate: data.sampleRate,
        audioCapabilities: data.audioCapabilities,
        gamePackages: data.packages, gameArguments: data.arguments, keyNames: {},
        audioCommand: command => {
          if (command.op === 'config' && command.soundfont?.startsWith('/') && engine && command.soundfont !== forwardedSoundfont) {
            try {
              command.file = {path: command.soundfont, bytes: engine.FS.readFile(command.soundfont)};
              forwardedSoundfont = command.soundfont;
            } catch (error) { console.warn(`Soundfont could not be forwarded: ${error}`); }
          }
          if (command.op === 'bgm' || command.op === 'stop') {
            generation++;
            if (engine) engine.audioState = {playing: command.op === 'bgm', playedOnce: false, ticks: 0};
          }
          audioPort.postMessage({...command, generation}, command.file ? [command.file.bytes.buffer] : []);
        },
        locateFile: path => new URL(path, data.runtimeBase).href,
        readConsoleLine,
        confirmFileOutput,
        getJoystickAvailable: () => joystickAvailable,
        consoleCommand,
        getConsoleGeneration: () => consoleGeneration,
        present,
        print: (...values) => postMessage({type: 'log', level: 'info', message: values.join(' ')}),
        printErr: (...values) => postMessage({type: 'log', level: 'error', message: values.join(' ')}),
        onAbort: message => report(new Error(message)),
      });
      engine.initApi();
    } else if (engine) {
      if (data.type === 'console-input-result') {
        if (consoleInput?.id === data.id) finishConsoleInput(data.value);
      }
      else if (data.type === 'file-output-confirmation-result') {
        if (fileOutputConfirmation?.id === data.id) finishFileOutputConfirmation(data.answer);
      }
      else if (data.type === 'console-closed') {
        if (consoleOwned && data.generation === consoleGeneration) consoleCommand({op: 'close'});
      }
      else if (data.type === 'key') engine._web_key(data.key, data.pressed ? 1 : 0);
      else if (data.type === 'mouse') engine._web_mouse(data.x, data.y, data.focus ? 1 : 0);
      else if (data.type === 'touch') engine._web_touch(data.id, data.x, data.y, data.pressed ? 1 : 0);
      else if (data.type === 'gamepad') engine._web_gamepad(...data.axes, ...data.triggers);
      else if (data.type === 'focus') {
        engine._web_focus(data.focused ? 1 : 0, data.fullscreen ? 1 : 0);
      } else if (data.type === 'capture') engine._web_capture();
      else if (data.type === 'stop') {
        engine.stopping = true;
        consoleCommand({op: 'close'});
        finishConsoleInput(null);
        finishFileOutputConfirmation(false);
        if (engine.stopped) engine.syncSaves().then(() => postMessage({type: 'stopped'})).catch(() => {});
        else engine._web_stop();
      }
      else if (data.type === 'upload') {
        // A file picker can finish after exit, including while a save flush is being retried.
        if (engine.stopping || engine.stopped) return;
        const bytes = new Uint8Array(data.bytes);
        const pointer = engine._malloc(bytes.length);
        try {
          engine.HEAPU8.set(bytes, pointer);
          if (data.kind === 'save') engine.api_private.uploadSavegameStep2(data.slot, pointer, bytes.length);
          else if (data.kind === 'soundfont') {
            engine.api_private.uploadSoundfontStep2(data.name, pointer, bytes.length);
            forwardedSoundfont = '';
          }
          else if (data.kind === 'font') engine.api_private.uploadFontStep2(data.name, pointer, bytes.length);
          engine.api.refreshScene();
        } finally { engine._free(pointer); }
      }
    }
  } catch (error) { report(error); }
};
