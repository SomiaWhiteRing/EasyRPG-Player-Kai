/* global createEasyRpgEngine */
let engine, audioPort, controlPort, pointer = 0, generation = 0;
const report = error => postMessage({type: 'error', message: error?.stack || String(error)});
self.addEventListener('error', event => report(event.error || event.message));
self.addEventListener('unhandledrejection', event => report(event.reason));

function command(data) {
  if (data.file) {
    const path = data.file.path;
    engine.FS.mkdirTree(path.slice(0, path.lastIndexOf('/')));
    engine.FS.writeFile(path, data.file.bytes);
    data.reloadSoundfont = true;
    delete data.file;
  }
  generation = data.generation;
  const bytes = new TextEncoder().encode(JSON.stringify(data) + '\0');
  const input = engine._malloc(bytes.length);
  try {
    engine.HEAPU8.set(bytes, input);
    engine._web_audio_command(input);
  } finally { engine._free(input); }
}

self.onmessage = async ({data}) => {
  try {
    if (data.type !== 'start' || engine) return;
    audioPort = data.audioPort;
    controlPort = data.controlPort;
    const engineName = data.engineName;
    importScripts(new URL('player-files.js', data.runtimeBase).href,
      new URL(`${engineName}.js`, data.runtimeBase).href);
    engine = await createEasyRpgEngine({
      audioOnly: true, noInitialRun: true, workId: data.workId,
      gamePackages: data.packages, sampleRate: data.sampleRate,
      getPreloadedPackage: () => data.runtimeData,
      instantiateWasm: (imports, receiveInstance) => {
        const instance = new WebAssembly.Instance(data.wasmModule, imports);
        receiveInstance(instance, data.wasmModule);
        return instance.exports;
      },
      locateFile: path => new URL(path.replace(/^easyrpg-player\.wasm$/, `${engineName}.wasm`), data.runtimeBase).href,
      setStatus: () => postMessage({type: 'startup-progress'}),
      print: (...values) => postMessage({type: 'log', level: 'info', message: values.join(' ')}),
      printErr: (...values) => postMessage({type: 'log', level: 'error', message: values.join(' ')}),
      onAbort: message => report(new Error(message)),
    });
    pointer = engine._malloc(1024 * 4);
    engine._web_audio_init(data.sampleRate);
    controlPort.onmessage = ({data}) => { try { command(data); } catch (error) { report(error); } };
    audioPort.onmessage = () => {
      try {
        engine.HEAPU8.fill(0, pointer, pointer + 4096);
        engine._web_audio_render(pointer, 1024);
        const pcm = engine.HEAP16.slice(pointer / 2, pointer / 2 + 2048);
        audioPort.postMessage({pcm}, [pcm.buffer]);
        controlPort.postMessage({...engine.audioState, generation});
      } catch (error) { report(error); }
    };
    postMessage({type: 'ready', capabilities: engine.audioCapabilities});
  } catch (error) { report(error); }
};
