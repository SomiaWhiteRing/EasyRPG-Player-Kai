// WORKERFS files are immutable slices of installed OPFS packs. Amortize the
// browser's per-read cost without copying the entire game into WASM memory.
self.installPlayerFileCache = function installPlayerFileCache(FS, root = '/game', read) {
  const budget = 32 * 1024 * 1024;
  const ops = FS.lookupPath(root).node.stream_ops;
  const entries = new Map();
  const reader = new FileReaderSync();
  const wholeFileLimit = 16 * 1024 * 1024;
  const blockSize = 512 * 1024;
  let bytes = 0;
  const stats = {hits: 0, misses: 0, bytes: 0, peakBytes: 0, readMs: 0};
  ops.read = (stream, buffer, offset, length, position) => {
    const node = stream.node;
    const count = Math.min(length, Math.max(0, node.size - position));
    let copied = 0;
    while (copied < count) {
      const start = node.size <= wholeFileLimit ? 0 : Math.floor((position + copied) / blockSize) * blockSize;
      const key = `${node.id}:${start}`;
      let value = entries.get(key);
      if (value) {
        stats.hits++;
        entries.delete(key);
      } else {
        stats.misses++;
        const end = Math.min(node.size, start + (node.size <= wholeFileLimit ? wholeFileLimit : blockSize));
        while (bytes + end - start > budget && entries.size) {
          const oldest = entries.keys().next().value;
          bytes -= entries.get(oldest).length;
          entries.delete(oldest);
        }
        const t = performance.now();
        value = read ? read(node.contents, start, end) : new Uint8Array(reader.readAsArrayBuffer(node.contents.slice(start, end)));
        stats.readMs += performance.now() - t;
        bytes += value.length;
      }
      entries.set(key, value);
      const from = position + copied - start;
      const size = Math.min(count - copied, value.length - from);
      buffer.set(value.subarray(from, from + size), offset + copied);
      copied += size;
    }
    stats.bytes = bytes;
    stats.peakBytes = Math.max(stats.peakBytes, bytes);
    return count;
  };
  return stats;
};

self.openPlayerStorage = function openPlayerStorage(options) {
  if (!options) return;
  importScripts(options.module);
  const adapter = self.easyRpgStorage;
  return {
    packages: adapter.packages(options.data),
    read: adapter.read,
    mountSaves: (FS, paths, write) => adapter.mountSaves(FS, paths, options.data, write),
    movieSource: source => ({url: adapter.movieUrl(source), source, module: options.module}),
  };
};
