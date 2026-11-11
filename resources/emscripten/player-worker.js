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
let nameInput, nameInputId = 0;

function beginNameInput(initial) {
  if (engine?.stopping || engine?.stopped || nameInput) return false;
  nameInput = {id: ++nameInputId};
  postMessage({type: 'name-input', id: nameInput.id, initial});
  return true;
}

function pollNameInput() {
  if (!nameInput || !('value' in nameInput)) return null;
  const result = {value: nameInput.value};
  nameInput = undefined;
  return result;
}

function cancelNameInput() {
  if (nameInput) postMessage({type: 'name-input-cancel', id: nameInput.id});
  nameInput = undefined;
}

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

function closeNetplayPort(port) {
  port?.postMessage({type: 'close'});
  port?.close();
}

function createNetplayRouteSelection({round, paths, offer}, {sendControl, sendSocket, socketReady, receive, activate, disconnect}) {
  const candidates = new Map();
  let selected, active;
  const send = (type, path, fields) => sendControl({type, round, path, ...fields});
  const sendData = (candidate, data) => {
    if (candidate.port) candidate.port.postMessage({type: 'data', data}, [data]);
    else sendSocket(data);
  };
  const stopProbe = candidate => {
    clearInterval(candidate.interval);
    clearTimeout(candidate.deadline);
  };
  const dispose = candidate => {
    stopProbe(candidate);
    closeNetplayPort(candidate.port);
    candidates.delete(candidate.id);
  };
  const isOpen = candidate => candidate?.id === 'websocket' ? socketReady() : candidate?.open;
  const receiveProbe = (candidate, data) => {
    if (!(data instanceof ArrayBuffer) || data.byteLength !== 10) return false;
    if (!candidate || active) return true;
    const packet = new DataView(data);
    const kind = packet.getUint8(0), sequence = packet.getUint8(1);
    if (packet.getFloat64(2, true) !== round) return true;
    if (kind === 128) {
      packet.setUint8(0, 129);
      sendData(candidate, data);
    } else if (kind === 129 && candidate.sentAt?.[sequence] !== undefined && candidate.rtts[sequence] === null) {
      const now = performance.now();
      if (now > candidate.probeUntil) return true;
      candidate.rtts[sequence] = Math.round(now - candidate.sentAt[sequence]);
      if (candidate.lastReply !== undefined) candidate.longestGap = Math.max(candidate.longestGap, now - candidate.lastReply);
      candidate.lastReply = now;
    }
    return true;
  };

  for (const {id, iceServers, relayOnly} of paths) {
    const candidate = {id, open: id === 'websocket', early: []};
    candidates.set(id, candidate);
    if (id === 'websocket') {
      send('ready', id);
      continue;
    }
    const {port1, port2} = new MessageChannel();
    candidate.port = port1;
    port1.onmessage = ({data}) => {
      if (candidates.get(id) !== candidate) return;
      if (data.type === 'open') {
        candidate.open = true;
        send('ready', id);
      } else if (data.type === 'signal') {
        send('signal', id, {value: data.value});
      } else if (data.type === 'data') {
        if (receiveProbe(candidate, data.data)) return;
        if (active === candidate) receive(data.data);
        else if (selected === candidate && candidate.early.length < 256) candidate.early.push(data.data);
      } else if (data.type === 'closed') {
        send('failed', id);
        dispose(candidate);
        if (active === candidate) disconnect();
      } else if (data.type === 'ice-pair' && data.final && active === candidate) {
        send('ice-pair', id, {value: data.value});
      }
    };
    postMessage({type: 'netplay-rtc', path: id, port: port2, iceServers, offer, relayOnly, compareRelay: false}, [port2]);
  }

  return {
    round,
    close() {
      for (const candidate of candidates.values()) dispose(candidate);
    },
    receiveSocket(data) {
      return receiveProbe(candidates.get('websocket'), data);
    },
    receive(message) {
      const {type, path} = message;
      if (message.round !== round || active) return;
      const candidate = candidates.get(path);
      if (type === 'signal') {
        candidate?.port?.postMessage({type: 'signal', value: message.value});
      } else if (type === 'probe' && isOpen(candidate) && !candidate.rtts) {
        const {samples, intervalMs, durationMs} = message;
        if (!Number.isSafeInteger(samples) || samples < 1 || samples > 32 ||
            !Number.isSafeInteger(intervalMs) || intervalMs < 1 ||
            !Number.isSafeInteger(durationMs) || durationMs < samples * intervalMs || durationMs > 5_000) return;
        candidate.sentAt = [];
        candidate.probeUntil = performance.now() + durationMs;
        candidate.rtts = Array(samples).fill(null);
        candidate.longestGap = 0;
        const sendSample = () => {
          const index = candidate.sentAt.length;
          const packet = new DataView(new ArrayBuffer(10));
          packet.setUint8(0, 128);
          packet.setUint8(1, index);
          packet.setFloat64(2, round, true);
          candidate.sentAt.push(performance.now());
          sendData(candidate, packet.buffer);
          if (candidate.sentAt.length === samples) clearInterval(candidate.interval);
        };
        candidate.interval = setInterval(sendSample, intervalMs);
        candidate.deadline = setTimeout(() => {
          stopProbe(candidate);
          candidate.sentAt = undefined;
          send('measurement', path, {rtts: candidate.rtts, longestGapMs: Math.round(candidate.longestGap)});
        }, durationMs);
        sendSample();
      } else if (type === 'select') {
        if (selected) selected.early.length = 0;
        selected = candidate;
        send(isOpen(candidate) ? 'accept' : 'failed', path);
      } else if (type === 'commit' && selected === candidate && candidate) {
        if (!isOpen(candidate)) {
          send('failed', path);
          disconnect();
          return;
        }
        active = candidate;
        for (const other of candidates.values()) {
          if (other === active) stopProbe(other);
          else dispose(other);
        }
        activate(active.port, round);
        for (const packet of active.early) receive(packet);
        active.early.length = 0;
        active.port?.postMessage({type: 'ice-pair'});
      }
    },
  };
}

function createNetplay(endpoint, rtcSupported) {
  if (!endpoint) return;
  const base = rtcSupported ? new URL('rtc/', endpoint) : new URL(endpoint);
  if (base.protocol === 'https:') base.protocol = 'wss:';
  else if (base.protocol === 'http:') base.protocol = 'ws:';
  else if (base.protocol !== 'wss:' && base.protocol !== 'ws:') return;
  const directory = new URL('rooms', base);
  const guest = crypto.getRandomValues(new Uint16Array(1))[0].toString(16).padStart(4, '0');
  directory.searchParams.set('guest', guest);
  const directoryHttp = new URL(directory);
  directoryHttp.protocol = base.protocol === 'wss:' ? 'https:' : 'http:';
  let socket, state = 0, joinResult = 0;
  let rtcPort, rtcRound, rtcSelected = false, routeSelection;
  const directoryTimeout = 5000;
  let subscription, directoryDeadline = 0, nextProbe = 0, retry = 0;
  let fallback = false, nextPoll = 0, poll;
  let rooms, identity, online = -1;
  let probe = 0, probeSent = 0, latency = -1;
  let resumeToken, resumeTimer, resumeDeadline = 0;
  let inputMatch, lastInputAt = 0, inputs = 0, peakInputGap = 0;
  let sentMatch = 0, sentFrame = -1;
  const avatarPaths = new Set(), avatars = new Map();
  const inbox = [], frames = [];
  const isIdentity = person => typeof person?.name === 'string' && typeof person.avatar === 'string';
  const updateDirectory = (list, local, count) => {
    if (!Array.isArray(list) || !list.every(room => Number.isSafeInteger(room?.id) &&
        Array.isArray(room.players) && room.players.every(isIdentity))) return;
    avatarPaths.clear();
    for (const {players} of list) for (const {avatar} of players) avatarPaths.add(avatar);
    if (local) avatarPaths.add(local.avatar);
    rooms = JSON.stringify(list);
    if (local) identity = JSON.stringify(local);
    online = Number.isInteger(count) && count >= 0 && count <= 0x7fffffff ? count : -1;
    return true;
  };
  const endDirectory = connection => {
    if (subscription !== connection) return;
    subscription = undefined;
    if (!fallback) {
      rooms = '';
      online = -1;
    }
    fallback = true;
    retry = performance.now() + 2000;
    connection.close();
  };
  const subscribe = () => {
    const connection = subscription = new WebSocket(directory);
    directoryDeadline = performance.now() + directoryTimeout;
    nextProbe = 0;
    connection.onerror = connection.onclose = () => endDirectory(connection);
    connection.onmessage = ({data}) => {
      if (subscription !== connection || typeof data !== 'string') return;
      if (data === 'directory:pong') {
        if (nextProbe) {
          directoryDeadline = 0;
          nextProbe = performance.now() + directoryTimeout;
        }
        return;
      }
      let snapshot;
      try { snapshot = JSON.parse(data); } catch { return; }
      if (!isIdentity(snapshot?.identity) || !updateDirectory(snapshot.rooms, snapshot.identity, snapshot.online)) return;
      fallback = false;
      directoryDeadline = 0;
      nextProbe = performance.now() + directoryTimeout;
      poll?.abort();
    };
  };
  const stopDirectory = () => {
    const previous = subscription;
    subscription = undefined;
    poll?.abort();
    poll = undefined;
    fallback = false;
    retry = 0;
    nextPoll = 0;
    rooms = identity = undefined;
    online = -1;
    previous?.close();
  };
  const refreshDirectory = () => {
    stopDirectory();
    rooms = '';
    fallback = true;
  };
  const clear = () => {
    inbox.length = 0;
    frames.length = 0;
    latency = -1;
    inputMatch = undefined;
    lastInputAt = inputs = peakInputGap = 0;
    sentMatch = 0;
    sentFrame = -1;
  };
  const closeRtc = () => {
    if (routeSelection) routeSelection.close();
    else closeNetplayPort(rtcPort);
    routeSelection = undefined;
    rtcPort = undefined;
    rtcRound = undefined;
    rtcSelected = false;
  };
  const close = () => {
    clearTimeout(resumeTimer);
    resumeToken = undefined;
    resumeDeadline = 0;
    const old = socket;
    socket = undefined;
    clear();
    state = joinResult = 0;
    closeRtc();
    old?.close();
  };
  const send = (...values) => {
    if (state < 3) return;
    const direct = rtcSelected && (values.length > 1 || resumeToken);
    if (direct ? !rtcPort : socket?.readyState !== WebSocket.OPEN) return;
    const bytes = new DataView(new ArrayBuffer(values.length * 4));
    values.forEach((value, index) => bytes.setInt32(index * 4, value, true));
    if (values.length === 3) [sentMatch, sentFrame] = values;
    if (direct) {
      rtcPort?.postMessage({type: 'data', data: bytes.buffer}, [bytes.buffer]);
    } else {
      socket.send(bytes.buffer);
    }
  };
  return {
    close,
    send,
    stopDirectory,
    refreshDirectory,
    state: () => state,
    joinResult: () => joinResult,
    rooms() {
      const now = performance.now();
      if (subscription && directoryDeadline && now >= directoryDeadline) endDirectory(subscription);
      if (subscription?.readyState === WebSocket.OPEN && !directoryDeadline && now >= nextProbe) {
        directoryDeadline = now + directoryTimeout;
        subscription.send('directory:ping');
      }
      if (!subscription && now >= retry) subscribe();
      if (fallback && !poll && now >= nextPoll) {
        const request = poll = new AbortController();
        const deadline = setTimeout(() => request.abort(), directoryTimeout);
        nextPoll = now + 1200;
        fetch(directoryHttp, {credentials: 'same-origin', cache: 'no-store', signal: request.signal}).then(async response => {
          const list = response.ok ? await response.json() : undefined;
          if (poll === request && fallback && !updateDirectory(list)) {
            rooms = '';
            online = -1;
          }
        }).catch(() => {
          if (poll === request && fallback) {
            rooms = '';
            online = -1;
          }
        }).finally(() => {
          clearTimeout(deadline);
          if (poll === request) poll = undefined;
        });
      }
      const snapshot = rooms;
      rooms = undefined;
      return snapshot;
    },
    identity() {
      const snapshot = identity;
      identity = undefined;
      return snapshot;
    },
    online: () => online,
    peerLatency(poll, reset) {
      if (reset) latency = -1;
      if (poll && state >= 3) {
        probe = (probe + 1) | 0;
        probeSent = performance.now();
        send(0, probe);
      }
      return latency;
    },
    avatar(path, pointer, size) {
      if (!avatarPaths.has(path)) return 0;
      if (!avatars.has(path)) {
        const source = new URL(path, directoryHttp);
        if (source.origin !== directoryHttp.origin) return 0;
        avatars.set(path, null);
        fetch(source).then(response => response.blob()).then(createImageBitmap).then(image => {
          const canvas = new OffscreenCanvas(size, size);
          const ctx = canvas.getContext('2d');
          const crop = Math.min(image.width, image.height);
          ctx.drawImage(image, (image.width - crop) / 2, (image.height - crop) / 2, crop, crop, 0, 0, size, size);
          image.close();
          avatars.set(path, ctx.getImageData(0, 0, size, size).data);
        }).catch(() => {});
      }
      const bytes = avatars.get(path);
      if (!bytes) return 0;
      engine.HEAPU8.set(bytes, pointer);
      return 1;
    },
    open(room, mode) {
      if (room < 1 || room > 999999) return;
      close();
      const address = new URL(String(room), base);
      address.searchParams.set('guest', guest);
      if (mode) address.searchParams.set('mode', mode);
      if (rtcSupported) {
        address.searchParams.set('rtc', '1');
        address.searchParams.set('rtcProbe', '2');
      }
      address.searchParams.set('heartbeat', '1');
      address.searchParams.set('inputStats', '1');
      address.searchParams.set('routeSelection', '1');
      if (rtcSupported) address.searchParams.set('rtcResume', '1');
      const sendRtc = value => {
        if (socket?.readyState === WebSocket.OPEN) {
          socket.send(rtcRound === undefined ? 'rtc:' + value : `rtc:round:${rtcRound}:${value}`);
        }
      };
      state = 1;
      const ended = (result = 0) => {
        const inRoom = state >= 2;
        close();
        state = -1;
        joinResult = result;
        if (inRoom || result) refreshDirectory();
      };
      const receiveMatchData = data => {
        if (data instanceof ArrayBuffer && data.byteLength === 8) {
          const packet = new DataView(data), kind = packet.getInt32(0, true), number = packet.getInt32(4, true);
          if (kind === 0) send(1, number);
          else if (kind === 1 && number === probe) latency = Math.trunc(performance.now() - probeSent);
        } else if (data instanceof ArrayBuffer && data.byteLength === 12 && frames.length < 256) {
          const packet = new DataView(data);
          const match = packet.getInt32(0, true), frame = packet.getInt32(4, true), now = performance.now();
          if (frame >= 0) {
            if (inputMatch === match && lastInputAt) peakInputGap = Math.max(peakInputGap, now - lastInputAt);
            inputMatch = match;
            lastInputAt = now;
            inputs++;
          }
          frames.push([match, frame, packet.getInt32(8, true)]);
        } else if (data instanceof ArrayBuffer && data.byteLength === 4 && inbox.length < 256) {
          inbox.push(new DataView(data).getInt32(0, true));
        } else {
          ended();
        }
      };
      const sendRouteControl = message => {
        if (socket?.readyState === WebSocket.OPEN) socket.send('route:' + JSON.stringify(message));
      };
      const receiveRoomMessage = ({data, target: connection}) => {
        if (socket !== connection) return;
        if (data instanceof ArrayBuffer && data.byteLength === 10) {
          routeSelection?.receiveSocket(data);
          return;
        }
        if (data === 'room:ping' || data === 'room:ping:stats') {
          connection.send('room:pong');
          if (data === 'room:ping:stats' && inputs) {
            connection.send(`room:input:${inputs}:${Math.round(peakInputGap)}:${latency}`);
          }
        } else if (typeof data === 'string' && data.startsWith('route:')) {
          let message;
          try { message = JSON.parse(data.slice(6)); } catch { return; }
          if (message?.type === 'rejected') {
            if (state === 1 && (message.round === undefined || message.round === routeSelection?.round)) {
              const reasons = {full: 1, closed: 2, transport: 3};
              ended(Object.hasOwn(reasons, message.reason) ? reasons[message.reason] : 0);
            }
          } else if (message?.type === 'start' && (state === 1 || state === 2)) {
            if (!Number.isSafeInteger(message.round) || !Array.isArray(message.paths) || message.paths.length > 3 ||
                !message.paths.every(path => ['websocket', 'direct', 'turn'].includes(path?.id))) return;
            closeRtc();
            routeSelection = createNetplayRouteSelection(message, {
              sendControl: sendRouteControl, socketReady: () => socket?.readyState === WebSocket.OPEN,
              sendSocket: bytes => { if (socket?.readyState === WebSocket.OPEN) socket.send(bytes); },
              receive: receiveMatchData, disconnect: ended,
              activate(port, round) { rtcPort = port; rtcRound = round; rtcSelected = Boolean(port); },
            });
          } else if (message) routeSelection?.receive(message);
        } else if (typeof data === 'string' && /^room:resume:[A-Za-z0-9_-]{32}$/.test(data)) {
          resumeToken = data.slice('room:resume:'.length);
        } else if (data === '2' || data === '3' || data === '4') {
          if (data === '2') {
            clear();
            resumeToken = undefined;
            closeRtc();
          }
          state = Number(data);
          resumeDeadline = 0;
          nextPoll = 0;
        } else if (data === 'rtc:webrtc') {
          rtcSelected = true;
          rtcPort?.postMessage({type: 'ice-pair'});
        } else if (data === 'rtc:websocket') {
          const port = rtcPort;
          rtcPort = undefined;
          rtcRound = undefined;
          rtcSelected = false;
          port?.postMessage({type: 'fallback'});
          if (port) setTimeout(() => port.close(), 500);
        } else if (typeof data === 'string' && /^rtc:probe:(ping|pong):[01]$/.test(data)) {
          const [, , kind, sample] = data.split(':');
          if (kind === 'ping') sendRtc('probe:pong:' + sample);
          else rtcPort?.postMessage({type: 'socket-probe-pong', sample: Number(sample)});
        } else if (typeof data === 'string' && data.startsWith('rtc:start:')) {
          const {port1, port2} = new MessageChannel();
          const {iceServers, offer, negotiationStartedAt, scoped, relayOnly, compareRelay} = JSON.parse(data.slice('rtc:start:'.length));
          const diagnostics = Number.isSafeInteger(negotiationStartedAt);
          closeRtc();
          rtcPort = port1;
          rtcRound = diagnostics && scoped ? negotiationStartedAt : undefined;
          port1.onmessage = ({data}) => {
            if (socket !== connection && rtcPort !== port1) return;
            if (data.type === 'ice-stats') {
              if (diagnostics && socket?.readyState === WebSocket.OPEN) {
                socket.send('rtc:ice-stats:' + JSON.stringify({...data.snapshot, negotiationStartedAt}));
              }
              if (data.snapshot.phase === 'fallback' || data.snapshot.phase === 'failure') port1.close();
              return;
            }
            if (rtcPort !== port1) return;
            if (data.type === 'data') receiveMatchData(data.data);
            else if (data.type === 'signal') sendRtc('signal:' + JSON.stringify(data.value));
            else if (data.type === 'diagnostic') {
              sendRtc('diag:' + data.milestone);
            }
            else if (data.type === 'ice-pair' && data.final === rtcSelected) {
              sendRtc(`ice-pair:${data.final ? 'webrtc' : 'pending'}:${data.value}`);
            }
            else if (data.type === 'socket-probe') sendRtc('probe:ping:' + data.sample);
            else if (data.type === 'open') sendRtc('ready');
            else if (data.type === 'closed') {
              rtcPort = undefined;
              if (data.diagnosticsPending) setTimeout(() => port1.close(), 500);
              else port1.close();
              sendRtc('fail');
              if (rtcSelected && resumeToken) ended();
            }
          };
          postMessage({type: 'netplay-rtc', port: port2, iceServers, offer, diagnostics, relayOnly, compareRelay}, [port2]);
        } else if (typeof data === 'string' && data.startsWith('rtc:signal:')) {
          try {
            rtcPort?.postMessage({type: 'signal', value: JSON.parse(data.slice('rtc:signal:'.length))});
          } catch {
            sendRtc('diag:invalid_signal');
            sendRtc('fail');
          }
        } else if (data instanceof ArrayBuffer && data.byteLength === 4 && inbox.length < 256) {
          inbox.push(new DataView(data).getInt32(0, true));
        } else if (!rtcSelected) {
          receiveMatchData(data);
        }
      };
      const connect = () => {
        const url = new URL(address);
        if (resumeToken) {
          url.searchParams.delete('mode');
          url.searchParams.set('resume', resumeToken);
        }
        const connection = socket = new WebSocket(url);
        connection.binaryType = 'arraybuffer';
        connection.onmessage = receiveRoomMessage;
        connection.onclose = connection.onerror = () => {
          if (socket !== connection) return;
          if (rtcSelected && resumeToken) {
            socket = undefined;
            connection.close();
            resumeDeadline ||= performance.now() + 15_000;
            if (performance.now() < resumeDeadline) resumeTimer = setTimeout(connect, 1000);
          } else {
            ended();
          }
        };
      };
      connect();
    },
    receive: () => inbox.shift(),
    receiveFrame() {
      const next = frames[0];
      // A faster peer can reach the rollback window before this browser's next tick.
      if (next?.[0] === sentMatch && next[1] > sentFrame + 1) return;
      return frames.shift();
    },
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
      const engineName = data.engineName;
      importScripts(new URL('player-files.js', data.runtimeBase).href,
        new URL(`${engineName}.js`, data.runtimeBase).href);
      let generation = 0;
      const audioPort = data.audioPort;
      audioPort.onmessage = ({data: state}) => {
        if (engine && state.generation === generation) engine.audioState = state;
      };
      engine = await createEasyRpgEngine({
        canvas: data.canvas, workId: data.workId, sampleRate: data.sampleRate,
        audioCapabilities: data.audioCapabilities,
        gamePackages: data.packages, gameArguments: data.arguments, keyNames: {},
        netplay: createNetplay(data.netplayEndpoint, data.netplayRtc),
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
        getPreloadedPackage: () => data.runtimeData,
        instantiateWasm: (imports, receiveInstance) => {
          const instance = new WebAssembly.Instance(data.wasmModule, imports);
          receiveInstance(instance, data.wasmModule);
          return instance.exports;
        },
        locateFile: path => new URL(path.replace(/^easyrpg-player\.wasm$/, `${engineName}.wasm`), data.runtimeBase).href,
        setStatus: () => postMessage({type: 'startup-progress'}),
        readConsoleLine,
        useSystemNameInput: () => data.systemNameInput === true,
        beginNameInput, pollNameInput, cancelNameInput,
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
      if (data.type === 'name-input-result') {
        if (nameInput?.id === data.id && !('value' in nameInput))
          nameInput.value = typeof data.value === 'string' ? data.value : null;
      }
      else if (data.type === 'console-input-result') {
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
      else if (data.type === 'netplay-network-change') engine.netplay?.refreshDirectory();
      else if (data.type === 'focus') {
        engine._web_focus(data.focused ? 1 : 0, data.fullscreen ? 1 : 0);
      } else if (data.type === 'capture') engine._web_capture();
      else if (data.type === 'stop') {
        engine.stopping = true;
        engine.netplay?.stopDirectory();
        cancelNameInput();
        consoleCommand({op: 'close'});
        finishConsoleInput(null);
        finishFileOutputConfirmation(false);
        if (engine.stopped) engine.syncSaves().then(() => postMessage({type: 'stopped'})).catch(() => {});
        else engine._web_stop();
      }
      else if (data.type === 'movie-state') {
        if (engine.webMovie?.id === data.id) Object.assign(engine.webMovie, data.state);
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
