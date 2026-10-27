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

// Existing Maniac video handling extracted from f28c77ac7's Web host.
(() => {
  const canvas = Module.canvas;
  let width = canvas.width, height = canvas.height;
  let windowInterpolation = false;
  let windowFit = true, windowZoom = 0, windowFitRequested = false;
  let screenMode, screenModeButton, screenModeRoot;
  let closed = false;
  const embeddedCanvasStyle = Object.fromEntries(['width', 'height', 'position', 'left', 'top', 'transform']
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
      canvas.style.transform = 'none';
      canvas.style.left = `${Math.trunc((window.innerWidth - width * zoom) / 2)}px`;
      canvas.style.top = `${Math.trunc((window.innerHeight - height * zoom) / 2)}px`;
    }
  };
  const applyWindowInterpolation = () => {
    canvas.style.imageRendering = windowInterpolation && !document.fullscreenElement ? 'auto' : 'pixelated';
  };
  const applyScreenMode = () => {
    if (!screenMode || !document.fullscreenElement) return;
    let w = window.innerWidth, h = window.innerHeight;
    if (!screenMode.layout) {
      const zoom = Math.max(1, Math.min(Math.floor(w / width), Math.floor(h / height)));
      w = width * zoom; h = height * zoom;
    } else if (!screenMode.stretch) {
      if (w * height > h * width) w = Math.floor(h * width / height);
      else h = Math.floor(w * height / width);
    }
    canvas.style.position = 'fixed';
    canvas.style.transform = 'none';
    canvas.style.width = `${w}px`; canvas.style.height = `${h}px`;
    canvas.style.left = `${Math.trunc((window.innerWidth - w) / 2)}px`;
    canvas.style.top = `${Math.trunc((window.innerHeight - h) / 2)}px`;
    applyWindowInterpolation();
  };
  const applyVideoLayout = () => {
    if (!windowZoom && !screenMode) return;
    if (screenMode && document.fullscreenElement) applyScreenMode();
    else {
      if (screenModeRoot) screenModeRoot.style.display = 'contents';
      applyWindowZoom();
    }
  };
  const requestScreenMode = async data => {
    if (closed) return;
    screenMode = data.fullscreen ? data : undefined;
    width = data.width; height = data.height;
    if (!data.fullscreen) {
      screenModeButton?.remove(); screenModeButton = undefined;
      if (document.fullscreenElement) await document.exitFullscreen();
      if (screenModeRoot) screenModeRoot.style.display = 'contents';
      windowZoom = data.zoom; windowFitRequested = true;
      applyWindowZoom(); applyWindowInterpolation();
      return;
    }
    if (document.fullscreenElement) { applyScreenMode(); return; }
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
        if (!screenMode || closed) return;
        try {
          screenModeRoot.style.cssText = 'display:block;width:100vw;height:100vh;background:black';
          await screenModeRoot.requestFullscreen();
          screenModeButton.remove(); screenModeButton = undefined;
          applyScreenMode(); canvas.focus();
        } catch (error) { console.error(error); }
      };
    }
  };
  Module.setManiacWindowZoom = (zoom, w, h, fit) => {
    width = w; height = h; windowZoom = zoom; windowFit = fit;
    applyWindowZoom(); applyWindowInterpolation();
    return true;
  };
  Module.setManiacWindowFit = (enabled, w, h) => {
    width = w; height = h; windowFit = enabled; windowFitRequested = true;
    if (!windowZoom) windowZoom = 1;
    applyWindowZoom();
  };
  Module.setManiacWindowInterpolation = enabled => {
    windowInterpolation = enabled; applyWindowInterpolation();
  };
  Module.setManiacScreenMode = (fullscreen, layout, stretch, w, h, zoom) => {
    void requestScreenMode({fullscreen, layout, stretch, width: w, height: h, zoom}).catch(console.error);
  };
  window.addEventListener('resize', applyVideoLayout);
  document.addEventListener('fullscreenchange', () => { applyVideoLayout(); applyWindowInterpolation(); });
  Module.onExit = () => { closed = true; screenModeButton?.remove(); };
})();

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
