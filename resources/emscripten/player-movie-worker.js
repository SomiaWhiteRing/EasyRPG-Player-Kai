/* global createMovieDecoder */
// One movie per Worker. Termination releases WASM, WORKERFS and pending reads.
let decoder;
let opened = false;
const failure = error => postMessage({type: 'error', error: error?.message || String(error)});
self.addEventListener('unhandledrejection', event => failure(event.reason));
self.onmessage = async ({data}) => {
  try {
    if (data.type === 'open') {
      if (decoder) throw new Error('Movie decoder already started');
      importScripts(new URL('movie-decoder.js', self.location.href).href);
      const emit = message => postMessage(message, [message.data.buffer]);
      decoder = await createMovieDecoder({
        locateFile: name => new URL(name, self.location.href).href,
        onVideo: (pixels, width, height, time, duration) =>
          emit({type: 'video', data: pixels, width, height, time, duration}),
        onAudio: (samples, rate, time) =>
          emit({type: 'audio', data: samples, rate, time, duration: samples.length / (2 * rate)}),
      });
      decoder.FS.mkdir('/movie');
      // Read Blob slices on demand instead of copying an entire movie into WASM.
      const external = !(data.blob instanceof Blob);
      decoder.FS.mount(decoder.FS.filesystems.WORKERFS,
        {blobs: [{name: 'input', data: external ? data.blob.source : data.blob}]}, '/movie');
      if (external) {
        importScripts(new URL('player-files.js', self.location.href).href, data.blob.module);
        self.installPlayerFileCache(decoder.FS, '/movie', self.easyRpgStorage.read);
      }
      if (decoder.ccall('movie_open', 'number', ['string'], ['/movie/input']) < 0)
        throw new Error(decoder.UTF8ToString(decoder._movie_error()));
      opened = true;
      postMessage({type: 'ready'});
    } else if (data.type === 'pull' && opened) {
      const result = decoder._movie_step();
      if (result < 0) throw new Error(decoder.UTF8ToString(decoder._movie_error()));
      if (result === 0) {
        opened = false;
        decoder._movie_close();
        postMessage({type: 'end'});
      }
    }
  } catch (error) {
    opened = false;
    failure(error);
  }
};
