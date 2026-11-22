// PCM is produced by the dedicated audio Worker, with no game/page relay.
class PlayerAudio extends AudioWorkletProcessor {
  constructor({processorOptions} = {}) {
    super();
    this.maxBlocks = processorOptions?.buffered ? 10 : 3;
    this.resumeBlocks = processorOptions?.buffered ? 5 : 2;
    this.queue = [];
    this.offset = 0;
    this.pending = 0;
    this.started = false;
    this.audioPort = null;
    this.rebuffering = true;
    this.gain = 0;
    this.last = [0, 0];
    this.port.onmessage = ({data}) => {
      if (data.type === 'start') this.started = true;
      if (data.type === 'connect') {
        this.audioPort = data.port;
        this.audioPort.onmessage = ({data}) => {
          this.pending--;
          this.queue.push(data.pcm);
        };
      }
    };
  }
  process(_inputs, outputs) {
    const channels = outputs[0];
    if (this.rebuffering && this.queue.length >= this.resumeBlocks) this.rebuffering = false;
    for (let i = 0; i < channels[0].length; i++) {
      const pcm = this.rebuffering ? null : this.queue[0];
      if (!pcm) {
        this.rebuffering = true;
        // A rare underrun must not jump straight from a nonzero sample to zero.
        this.gain = Math.max(0, this.gain - 1 / 64);
        channels[0][i] = this.last[0] * this.gain;
        channels[1][i] = this.last[1] * this.gain;
        continue;
      }
      this.gain = Math.min(1, this.gain + 1 / 64);
      this.last[0] = pcm[this.offset++] / 32768;
      this.last[1] = pcm[this.offset++] / 32768;
      channels[0][i] = this.last[0] * this.gain;
      channels[1][i] = this.last[1] * this.gain;
      if (this.offset === pcm.length) { this.queue.shift(); this.offset = 0; }
    }
    while (this.started && this.audioPort && this.queue.length + this.pending < this.maxBlocks) {
      this.pending++;
      this.audioPort.postMessage({type: 'audio'});
    }
    return true;
  }
}
registerProcessor('easyrpg-audio', PlayerAudio);
