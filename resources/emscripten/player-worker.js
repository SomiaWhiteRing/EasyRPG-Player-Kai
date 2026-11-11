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

// Like GGPO/Slippi, repeat unacknowledged inputs in new packets. The game still
// receives each frame exactly once, in order; only the wire is unreliable.
function createNetplayInputTransport(send, deliver, disconnect) {
  const marker = 0x31504e49; // INP1
  const limit = 256;
  let match = 0, localNext = 1, acknowledged = 0, receivedThrough = 0, delivered = 0;
  let ready = false, lastSent = 0;
  const pending = [], incoming = new Map();
  let packetsSent = 0, packetsReceived = 0, repeatedFrames = 0, highestSent = 0;
  let lastPacketAt = 0, lastAckAt = 0;
  const transmit = (ackOnly = false) => {
    if (!match) return;
    const count = ackOnly ? 0 : pending.length;
    const packet = new DataView(new ArrayBuffer(16 + count));
    packet.setUint32(0, marker, true);
    packet.setInt32(4, match, true);
    packet.setInt32(8, receivedThrough, true);
    packet.setInt32(12, count ? acknowledged + 1 : 0, true);
    for (let i = 0; i < count; ++i) packet.setUint8(16 + i, pending[i]);
    repeatedFrames += Math.max(0, Math.min(highestSent, acknowledged + count) - acknowledged);
    highestSent = Math.max(highestSent, acknowledged + count);
    ++packetsSent;
    if (!ackOnly) lastSent = performance.now();
    send(packet.buffer);
  };
  const flush = () => {
    while (ready && delivered < receivedThrough) {
      const frame = ++delivered, input = incoming.get(frame);
      incoming.delete(frame);
      deliver(match, frame, input);
    }
  };
  // This continues even when prediction has reached its limit and no new game
  // frames are produced, including when the last input or its ACK was lost.
  const timer = setInterval(() => {
    if (pending.length && performance.now() - lastSent >= 50) transmit();
  }, 50);
  return {
    close: () => clearInterval(timer),
    stats(now) {
      return {
        inputSentPackets: packetsSent, inputReceivedPackets: packetsReceived, inputRepeatedFrames: repeatedFrames,
        inputPending: pending.length, inputIncoming: incoming.size, inputAcknowledged: acknowledged,
        inputReceivedThrough: receivedThrough, inputDelivered: delivered, inputLocalNext: localNext,
        inputPacketAgeMs: lastPacketAt ? Math.round(now - lastPacketAt) : -1,
        inputAckAgeMs: lastAckAt ? Math.round(now - lastAckAt) : -1,
      };
    },
    start(id, remoteReady) {
      match = id;
      localNext = 1;
      acknowledged = receivedThrough = delivered = 0;
      ready = remoteReady;
      pending.length = 0;
      incoming.clear();
      packetsSent = packetsReceived = repeatedFrames = highestSent = lastPacketAt = lastAckAt = 0;
    },
    ready(id) {
      if (id !== match) return;
      ready = true;
      flush();
    },
    send(id, frame, input) {
      if (id !== match || frame !== localNext || pending.length === limit) {
        disconnect(pending.length === limit ? 'input_backlog' : 'input_sequence');
        return;
      }
      pending.push(input);
      ++localNext;
      transmit();
    },
    receive(data) {
      const count = data.byteLength - 16;
      if (count < 0 || count > limit) return disconnect('input_packet_size');
      const packet = new DataView(data);
      if (packet.getUint32(0, true) !== marker) return disconnect('input_packet_marker');
      if (packet.getInt32(4, true) !== match || !match) return;
      ++packetsReceived;
      lastPacketAt = performance.now();
      const ack = packet.getInt32(8, true), first = packet.getInt32(12, true);
      if (ack < 0 || ack >= localNext || (count ? first < 1 || first + count - 1 > delivered + limit : first !== 0)) {
        disconnect('input_ack_range');
        return;
      }
      if (ack > acknowledged) {
        pending.splice(0, ack - acknowledged);
        acknowledged = ack;
        lastAckAt = performance.now();
      }
      for (let i = 0; i < count; ++i) {
        const frame = first + i;
        if (frame > delivered) incoming.set(frame, packet.getUint8(16 + i));
      }
      while (incoming.has(receivedThrough + 1)) ++receivedThrough;
      flush();
      if (count) transmit(true);
    },
  };
}

function createNetplayRouteSelection({round, paths, offer, inputProtocol}, {sendControl, sendSocket, socketReady, receive, activate, disconnect, transportStats, reportFailure}) {
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
        if (active === candidate) reportFailure(data.reason || 'peer_closed');
        send('failed', id);
        dispose(candidate);
        if (active === candidate) disconnect();
      } else if (data.type === 'ice-pair' && data.final && active === candidate) {
        send('ice-pair', id, {value: data.value});
      } else if (data.type === 'transport-stats' && active === candidate) {
        transportStats(data.value);
      }
    };
    postMessage({type: 'netplay-rtc', path: id, port: port2, iceServers, offer, relayOnly, inputProtocol, compareRelay: false}, [port2]);
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
        activate(active.port, round, inputProtocol);
        for (const packet of active.early) receive(packet);
        active.early.length = 0;
        active.port?.postMessage({type: 'ice-pair'});
      }
    },
  };
}

function createNetplayDiagnostics(send, snapshot, requestStats) {
  let match = 0, startedAt = 0, lastTick = 0, lastPoll = 0, lastSample = 0, lastRequest = 0;
  let timer, wait, frame = {}, sequence = 0, droppedEvents = 0;
  let waitCount = 0, waitTotalMs = 0, waitMaxMs = 0, workerGapMs = 0, gameTickGapMs = 0, hostGapMs = 0;
  let runtime = {visible: 1, focused: 1}, hostAt = 0;
  const queued = [];
  const timestamp = now => Math.round(performance.timeOrigin + now);
  const flush = () => {
    while (queued.length && send(queued[0])) queued.shift();
  };
  const emit = (kind, details = {}) => {
    if (!match) return;
    const now = performance.now();
    const report = {
      v: 1, kind, sequence: ++sequence, atMs: timestamp(now), match, matchStartedAtMs: timestamp(startedAt),
      ...frame, ...runtime, waitCount, waitTotalMs: Math.round(waitTotalMs), waitMaxMs: Math.round(waitMaxMs),
      workerGapMs, gameTickGapMs, hostGapMs, gameTickAgeMs: Math.round(now - lastTick),
      hostAgeMs: hostAt ? Math.round(now - hostAt) : -1, ...snapshot(now), ...details,
    };
    if (queued.length === 32) { queued.shift(); ++droppedEvents; }
    queued.push({...report, droppedEvents});
    flush();
  };
  const reportWait = now => {
    if (!wait || wait.reported || now - wait.at < 100) return;
    wait.reported = true;
    emit('wait_start', {waitId: wait.id, waitStartedAtMs: timestamp(wait.at),
      durationMs: Math.round(now - wait.at), waitFrame: wait.frame});
  };
  const finishWait = (now, reason) => {
    if (!wait) return;
    const duration = now - wait.at;
    waitTotalMs += duration;
    waitMaxMs = Math.max(waitMaxMs, duration);
    reportWait(now);
    if (wait.reported) emit('wait_end', {reason, waitId: wait.id, waitStartedAtMs: timestamp(wait.at),
      durationMs: Math.round(duration), waitFrame: wait.frame});
    wait = undefined;
  };
  const end = reason => {
    if (!match) return;
    finishWait(performance.now(), reason);
    emit('end', {reason});
    clearInterval(timer);
    timer = undefined;
    match = 0;
  };
  return {
    flush,
    end,
    reset() {
      end('session_close');
      queued.length = 0;
      droppedEvents = 0;
    },
    failure(reason) { emit('input_failure', {reason}); },
    runtime(data) {
      const previous = runtime;
      runtime = {visible: Number(data.visible !== false), focused: Number(data.focused !== false)};
      hostAt = performance.now();
      hostGapMs = Math.max(hostGapMs, data.gapMs ?? 0);
      if (previous.visible !== runtime.visible || previous.focused !== runtime.focused) emit('lifecycle');
      if (data.gapMs >= 250) emit('runtime_gap', {reason: 'host', durationMs: data.gapMs});
    },
    progress(id, simulated, confirmed, received, waiting, presentationQueue, peerReady, replayFrame) {
      const now = performance.now();
      const starting = id !== match;
      if (id !== match) {
        end('match_changed');
        match = id;
        startedAt = lastTick = lastPoll = lastSample = lastRequest = now;
        waitCount = waitTotalMs = waitMaxMs = workerGapMs = gameTickGapMs = hostGapMs = 0;
        timer = setInterval(() => {
          const current = performance.now(), gap = Math.max(0, Math.round(current - lastPoll - 250));
          lastPoll = current;
          workerGapMs = Math.max(workerGapMs, gap);
          if (gap >= 250) emit('runtime_gap', {reason: 'worker', durationMs: gap});
          reportWait(current);
          if (current - lastRequest >= 1000) { lastRequest = current; requestStats(); }
          if (current - lastSample >= 5000) {
            lastSample = current;
            emit('sample', wait ? {waitId: wait.id, durationMs: Math.round(current - wait.at)} : {});
          }
          flush();
        }, 250);
      }
      const tickGap = Math.max(0, Math.round(now - lastTick));
      gameTickGapMs = Math.max(gameTickGapMs, tickGap);
      lastTick = now;
      frame = {frame: simulated, confirmedFrame: confirmed, receivedFrame: received,
        waiting: Number(waiting), presentationQueue, peerReady: Number(peerReady), replayFrame};
      if (starting) emit('start');
      if (tickGap >= 250) emit('runtime_gap', {reason: 'game_tick', durationMs: tickGap});
      if (waiting) {
        wait ??= {id: ++waitCount, at: now, frame: simulated, reported: false};
        reportWait(now);
      } else finishWait(now, 'input_resumed');
    },
  };
}

function createNetplay(endpoint, rtcSupported, runtimeBase) {
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
  let rtcPort, rtcRound, rtcSelected = false, routeSelection, inputTransport, peerMatch;
  const directoryTimeout = 5000;
  let subscription, directoryDeadline = 0, nextProbe = 0, retry = 0;
  let fallback = false, nextPoll = 0, poll;
  let rooms, identity, roomSettings, online = -1;
  let probe = 0, probeSent = 0, latency = -1;
  let resumeToken, resumeTimer, resumeDeadline = 0;
  let inputMatch, lastInputAt = 0, inputs = 0, peakInputGap = 0;
  let sentMatch = 0, sentFrame = -1;
  const avatarPaths = new Set(), avatars = new Map();
  const inbox = [], frames = [];
  let diagnosticsAccepted = false, rtcStats = {}, rtcStatsAt = 0;
  const client = new URL(runtimeBase).pathname.split('/').filter(Boolean).at(-1) || 'unknown';
  const transportStats = value => { rtcStats = value; rtcStatsAt = performance.now(); };
  const diagnostics = createNetplayDiagnostics(
    report => {
      if (!diagnosticsAccepted || socket?.readyState !== WebSocket.OPEN || socket.bufferedAmount > 16_384) return false;
      socket.send('room:diagnostic:' + JSON.stringify({...report, client}));
      return true;
    },
    now => ({
      peerRttMs: latency, socketState: socket?.readyState ?? 3, socketBufferedBytes: socket?.bufferedAmount ?? 0,
      queuedFrames: frames.length, deliveredInputs: inputs, paused: Number(Boolean(engine?.paused)),
      ...inputTransport?.stats(now), ...rtcStats, rtcStatsAgeMs: rtcStatsAt ? Math.round(now - rtcStatsAt) : -1,
    }),
    () => rtcPort?.postMessage({type: 'transport-stats'}),
  );
  const isIdentity = person => typeof person?.name === 'string' && typeof person.avatar === 'string';
  const updateDirectory = (list, local, count) => {
    if (!Array.isArray(list) || !list.every(room => Number.isSafeInteger(room?.id) &&
        Array.isArray(room.players) && room.players.every(isIdentity) &&
        (room.locked === undefined || typeof room.locked === 'boolean'))) return;
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
    peerMatch = undefined;
  };
  const closeRtc = () => {
    inputTransport?.close();
    inputTransport = undefined;
    if (routeSelection) routeSelection.close();
    else closeNetplayPort(rtcPort);
    routeSelection = undefined;
    rtcPort = undefined;
    rtcRound = undefined;
    rtcSelected = false;
    rtcStats = {};
    rtcStatsAt = 0;
  };
  const close = () => {
    diagnostics.reset();
    diagnosticsAccepted = false;
    clearTimeout(resumeTimer);
    resumeToken = undefined;
    resumeDeadline = 0;
    const old = socket;
    socket = undefined;
    clear();
    state = joinResult = 0;
    roomSettings = '';
    closeRtc();
    old?.close();
  };
  const send = (...values) => {
    if (state < 3) return;
    const direct = rtcSelected && (values.length > 1 || resumeToken);
    if (direct ? !rtcPort : socket?.readyState !== WebSocket.OPEN) return;
    if (values.length === 3) {
      [sentMatch, sentFrame] = values;
      if (inputTransport) {
        if (sentFrame >= 0) {
          inputTransport.send(...values);
          return;
        }
        inputTransport.start(sentMatch, peerMatch === sentMatch);
      }
    }
    const bytes = new DataView(new ArrayBuffer(values.length * 4));
    values.forEach((value, index) => bytes.setInt32(index * 4, value, true));
    if (direct) {
      rtcPort?.postMessage({type: 'data', data: bytes.buffer}, [bytes.buffer]);
    } else {
      socket.send(bytes.buffer);
    }
  };
  return {
    close,
    send,
    progress: diagnostics.progress,
    endMatch: diagnostics.end,
    runtime: diagnostics.runtime,
    stopDirectory,
    refreshDirectory,
    state: () => state,
    joinResult: () => joinResult,
    roomSettings() {
      const snapshot = roomSettings;
      roomSettings = undefined;
      return snapshot;
    },
    setPassword(enabled, code) {
      if ((state === 2 || state === 3) && socket?.readyState === WebSocket.OPEN) {
        socket.send('room:password:' + JSON.stringify({enabled, code}));
      }
    },
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
    open(room, mode, password = null) {
      if (room < 1 || room > 999999) return;
      close();
      const address = new URL(String(room), base);
      address.searchParams.set('guest', guest);
      address.searchParams.set('roomAccess', '1');
      if (mode) address.searchParams.set('mode', mode);
      if (rtcSupported) {
        address.searchParams.set('rtc', '1');
        address.searchParams.set('rtcProbe', '2');
      }
      address.searchParams.set('heartbeat', '1');
      address.searchParams.set('inputStats', '1');
      address.searchParams.set('diagnostics', '1');
      address.searchParams.set('routeSelection', '1');
      address.searchParams.set('inputProtocol', '1');
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
        if (inRoom || (result && result !== 4 && result !== 5)) refreshDirectory();
      };
      const receiveInput = (match, frame, input) => {
        if (frames.length >= 256) {
          diagnostics.failure('gameplay_queue');
          return ended();
        }
        const now = performance.now();
        if (inputMatch === match && lastInputAt) peakInputGap = Math.max(peakInputGap, now - lastInputAt);
        inputMatch = match;
        lastInputAt = now;
        inputs++;
        frames.push([match, frame, input]);
      };
      const enableInputTransport = protocol => {
        if (protocol !== 1) return;
        inputTransport = createNetplayInputTransport(
          data => rtcPort?.postMessage({type: 'input', data}, [data]), receiveInput,
          reason => { diagnostics.failure(reason); ended(); });
      };
      const receiveMatchData = data => {
        if (inputTransport && data instanceof ArrayBuffer && data.byteLength >= 16) {
          inputTransport.receive(data);
        } else if (data instanceof ArrayBuffer && data.byteLength === 8) {
          const packet = new DataView(data), kind = packet.getInt32(0, true), number = packet.getInt32(4, true);
          if (kind === 0) send(1, number);
          else if (kind === 1 && number === probe) latency = Math.trunc(performance.now() - probeSent);
        } else if (data instanceof ArrayBuffer && data.byteLength === 12 && frames.length < 256) {
          const packet = new DataView(data);
          const match = packet.getInt32(0, true), frame = packet.getInt32(4, true);
          if (frame >= 0) {
            receiveInput(match, frame, packet.getInt32(8, true));
          } else {
            frames.push([match, frame, packet.getInt32(8, true)]);
            peerMatch = match;
            inputTransport?.ready(match);
          }
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
        if (typeof data === 'string' && data.startsWith('room:settings:')) {
          let settings;
          try { settings = JSON.parse(data.slice(14)); } catch { return; }
          if (typeof settings?.enabled === 'boolean' && typeof settings.code === 'string' && settings.code.length <= 64) {
            roomSettings = JSON.stringify({enabled: settings.enabled, code: settings.code});
          }
          return;
        }
        if (typeof data === 'string' && data.startsWith('room:rejected:')) {
          const reasons = {full: 1, closed: 2, password_required: 4, password_incorrect: 5};
          const reason = data.slice(14);
          ended(Object.hasOwn(reasons, reason) ? reasons[reason] : 0);
          return;
        }
        if (data === 'room:diagnostics:1') {
          diagnosticsAccepted = true;
          diagnostics.flush();
          return;
        }
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
              receive: receiveMatchData, transportStats, disconnect: ended,
              reportFailure: reason => { diagnostics.failure(reason); diagnostics.end(reason); },
              activate(port, round, protocol) {
                rtcPort = port; rtcRound = round; rtcSelected = Boolean(port);
                if (port) enableInputTransport(protocol);
              },
            });
          } else if (message) routeSelection?.receive(message);
        } else if (typeof data === 'string' && /^room:resume:[A-Za-z0-9_-]{32}$/.test(data)) {
          resumeToken = data.slice('room:resume:'.length);
        } else if (data === '2' || data === '3' || data === '4') {
          if (data === '2') {
            diagnostics.end('peer_left');
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
          inputTransport?.close();
          inputTransport = undefined;
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
          const {iceServers, offer, negotiationStartedAt, scoped, relayOnly, compareRelay, inputProtocol} = JSON.parse(data.slice('rtc:start:'.length));
          const iceDiagnostics = Number.isSafeInteger(negotiationStartedAt);
          closeRtc();
          rtcPort = port1;
          enableInputTransport(inputProtocol);
          rtcRound = iceDiagnostics && scoped ? negotiationStartedAt : undefined;
          port1.onmessage = ({data}) => {
            if (socket !== connection && rtcPort !== port1) return;
            if (data.type === 'ice-stats') {
              if (iceDiagnostics && socket?.readyState === WebSocket.OPEN) {
                socket.send('rtc:ice-stats:' + JSON.stringify({...data.snapshot, negotiationStartedAt}));
              }
              if (data.snapshot.phase === 'fallback' || data.snapshot.phase === 'failure') port1.close();
              return;
            }
            if (rtcPort !== port1) return;
            if (data.type === 'data') receiveMatchData(data.data);
            else if (data.type === 'transport-stats') transportStats(data.value);
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
              diagnostics.failure(data.reason || 'peer_closed');
              diagnostics.end(data.reason || 'peer_closed');
              rtcPort = undefined;
              if (data.diagnosticsPending) setTimeout(() => port1.close(), 500);
              else port1.close();
              sendRtc('fail');
              if (rtcSelected && resumeToken) ended();
            }
          };
          postMessage({type: 'netplay-rtc', port: port2, iceServers, offer, diagnostics: iceDiagnostics, relayOnly, compareRelay, inputProtocol}, [port2]);
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
        connection.onopen = () => {
          if (socket === connection && !resumeToken) connection.send('room:join:' + JSON.stringify({code: password}));
        };
        connection.onmessage = receiveRoomMessage;
        connection.onclose = connection.onerror = () => {
          if (socket !== connection) return;
          if (rtcSelected && resumeToken) {
            diagnostics.failure('control_disconnected');
            socket = undefined;
            diagnosticsAccepted = false;
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
        netplay: createNetplay(data.netplayEndpoint, data.netplayRtc, data.runtimeBase),
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
      else if (data.type === 'netplay-runtime') engine.netplay?.runtime(data);
      else if (data.type === 'focus') {
        engine._web_focus(data.focused ? 1 : 0, data.fullscreen ? 1 : 0);
        engine.netplay?.runtime(data);
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
