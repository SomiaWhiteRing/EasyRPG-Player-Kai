// The Web engine runs in a dedicated Worker.
Module.arguments = ['--project-path', '/game', '--save-path', `/work-saves/${Module.workId}`,
  ...(Module.gameArguments || [])];
Module.preRun = [].concat(Module.preRun || [], () => {
  if (!Number.isSafeInteger(Module.workId) || Module.workId <= 0)
    throw new Error('Invalid workId');
  FS.mkdir('/game');
  FS.mount(WORKERFS, {packages: Module.gamePackages}, '/game');
  Module.fileCache = self.installPlayerFileCache(FS, '/game', Module.readGameFile);
  FS.chdir('/game');
  // The audio instance has no game loop or persistent state of its own.
  if (Module.audioOnly) return;
  FS.mkdir('/work-saves');
  const savePath = `/work-saves/${Module.workId}`;
  FS.mkdir(savePath);
  const configPath = '/home/web_user/.config';
  FS.mkdir(configPath);
  const paths = [savePath, configPath];
  if (Module.replayCheckpoint) return;
  if (Module.mountSaves) Module.mountSaves(FS, paths);
  else for (const path of paths) FS.mount(IDBFS, {}, path);
  addRunDependency('player-storage');
  FS.syncfs(true, error => {
    if (error) { abort(`Could not load player storage: ${error}`); return; }
    removeRunDependency('player-storage');
  });
});

Module.syncSaves = () => {
  // Serialize writes: a later snapshot must not overtake an earlier save.
  Module.savePending = (Module.savePending || Promise.resolve()).catch(() => {}).then(() =>
    new Promise((resolve, reject) => FS.syncfs(false, error => error ? reject(error) : resolve())));
  Module.savePending.catch(error => postMessage({type: 'save-error', message: `存档写入失败：${error}`}));
  return Module.savePending;
};
