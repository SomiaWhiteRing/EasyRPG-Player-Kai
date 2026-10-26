// Note: The `Module` context is already initialized as an
// empty object by emscripten even before the pre script
Module = { ...Module,
  preRun: [onPreRun],
  postRun: [],

  print: (...args) => {
    console.log(...args);
  },

  printErr: (...args) => {
    console.error(...args);
  },

  canvas: (() => {
    const canvas = document.getElementById('canvas');

    // See http://www.khronos.org/registry/webgl/specs/latest/1.0/#5.15.2
    canvas.addEventListener('webglcontextlost', event => {
      event.preventDefault();
    }, false);

    canvas.addEventListener('webglcontextrestored', () => {
      Module.api.resetCanvas();
    });

    return canvas;
  })(),

  setStatus: text => {
    if (!Module.setStatus.last) Module.setStatus.last = {
      time: Date.now(),
      text: ''
    };

    if (text !== Module.setStatus.text) {
      document.getElementById('status').innerHTML = text;
    }
  },

  totalDependencies: 0,

  monitorRunDependencies: left => {
    Module.totalDependencies = Math.max(Module.totalDependencies, left);
    Module.setStatus(left ? `Preparing... (${Module.totalDependencies - left}/${Module.totalDependencies})` : 'Downloading game data...');
  }
};

/**
 * Parses the current location query to setup a specific game
 */
function parseArgs () {
  const items = window.location.search.substr(1).split("&");
  let result = [];

  // Store saves in subdirectory `Save`
  result.push("--save-path");
  result.push("Save");

  for (let i = 0; i < items.length; i++) {
    const tmp = items[i].split("=");

    if (tmp[0] === "project-path" || tmp[0] === "save-path") {
      // Filter arguments that are set by us
      continue;
    }

    // Filesystem is not ready when processing arguments, store path to game
    if (tmp[0] === "game" && tmp.length > 1) {
      Module.game = tmp[1];
      continue;
    }

    result.push("--" + tmp[0]);

    if (tmp.length > 1) {
      const arg = decodeURI(tmp[1]);
      // Split except if it's a string
      if (arg.length > 0) {
        if (arg.startsWith('"') && arg.endsWith('"')) {
          result.push(arg.slice(1, -1));
        } else {
          result = [...result, ...arg.split(" ")];
        }
      }
    }
  }

  return result;
}

function onPreRun () {
  // Each game needs its own working directory before its save mount is created.
  FS.mkdir('easyrpg');
  FS.chdir('easyrpg');
  if (Module.game.length > 0) {
    FS.mkdir(Module.game);
    FS.chdir(Module.game);
  }
  if (Module.saveFs === undefined) Module.saveFs = IDBFS;

  // Retrieve save directory from persistent storage before using it
  FS.mkdir("Save");
  FS.mount(Module.saveFs, {}, 'Save');

  // For preserving the configuration. Shared across website
  FS.mkdir("/home/web_user/.config");
  FS.mount(IDBFS, {}, '/home/web_user/.config');

  addRunDependency('player-storage');
  FS.syncfs(true, function(error) {
    if (error) { abort(`Could not load player storage: ${error}`); return; }
    removeRunDependency('player-storage');
  });
}

// Keep save snapshots ordered while IDBFS writes are in flight.
Module.syncSaves = () => {
  Module.savePending = (Module.savePending || Promise.resolve()).catch(() => {}).then(() =>
    new Promise((resolve, reject) => FS.syncfs(false, error => error ? reject(error) : resolve())));
  Module.savePending.catch(error => {
    console.error('Could not save player storage:', error);
    Module.setStatus('Could not save player storage');
  });
  return Module.savePending;
};

Module.setManiacWindowZoom = (zoom, width, height, fit) => {
  if (document.fullscreenElement) return true;
  const factor = fit ? 1 : 1.5;
  const scale = Math.max(1, Math.min(zoom,
    Math.floor(window.innerWidth * factor / width), Math.floor(window.innerHeight * factor / height)));
  Module.canvas.style.width = `${width * scale}px`;
  Module.canvas.style.height = `${height * scale}px`;
  return true;
};

Module.setStatus('Downloading...');
Module.arguments = ["easyrpg-player", ...parseArgs()];

if (Module.game === undefined) {
  Module.game = "";
} else {
  Module.arguments.push("--game", Module.game);
  Module.game = Module.game.toLowerCase();
}

// Catch all errors occuring inside the window
window.addEventListener('error', (event) => {
  // workaround chrome bug: See https://github.com/EasyRPG/Player/issues/2806
  if (event.error.message.includes("side-effect in debug-evaluate") && event.defaultPrevented) {
    return;
  }

  Module.setStatus('Exception thrown, see JavaScript console…');
  Module.setStatus = text => {
    if (text) Module.printErr(`[post-exception status] ${text}`);
  };
});
