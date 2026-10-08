// Tauri host adapter, appended after the shared launcher script. The Rust
// host injects LAUNCHER_CAPS (platform, builds, folder defaults, canBuild),
// LAUNCHER_SETTINGS and SAVED_STATE before the page runs; this file wires the
// page's host messages, the launch-settings panel and the Build panel to
// Tauri commands.
(() => {
  const invoke = window.__TAURI__.core.invoke;
  const caps = window.LAUNCHER_CAPS;
  const settings = window.LAUNCHER_SETTINGS;
  const status = document.getElementById('launchStatus');
  const panel = document.getElementById('setup');
  const fields = ['gameDir', 'gameDirTu2', 'userDataRoot', 'userDataRootTu2', 'vulkanDevice'];

  // Menu state worth restoring next time (not the remap view or bindings).
  const uiState = () => {
    const { lang, path, fps, scale, res, display, mouse, vkpath, vsync } = state;
    return { lang, path, fps, scale, res, display, mouse, vkpath, vsync, build: curBuild() ? curBuild().id : null };
  };
  function fillSettings() {
    fields.forEach(key => {
      const field = document.getElementById(key);
      const value = settings[key] ?? '';
      field.value = key === 'vulkanDevice' && value < 0 ? '' : value;
      if (caps.defaults[key] !== undefined) field.placeholder = caps.defaults[key];
    });
  }
  function readSettings() {
    fields.forEach(key => {
      const value = document.getElementById(key).value.trim();
      settings[key] = key === 'vulkanDevice' ? (value === '' ? -1 : Number(value)) : value;
    });
  }
  function showStatus(text) { status.textContent = text; }

  document.getElementById('deviceLabel').hidden = caps.platform !== 'linux';
  function listBuilds() {
    document.getElementById('buildList').textContent = caps.builds.length
      ? caps.builds.map(b => `${b.label}: ${b.executable}`).join('\n')
      : 'No game build found next to the launcher or under port/out/build.';
  }
  listBuilds();
  if (!caps.builds.length) {
    showStatus(caps.canBuild ? 'No game build found. Build it from your disc image.'
      : 'No game build found. Open launch settings (⚙) for details.');
  }
  document.getElementById('setupBtn').addEventListener('click', e => {
    e.stopPropagation(); fillSettings(); panel.hidden = !panel.hidden;
  });
  document.getElementById('saveSetup').addEventListener('click', async () => {
    try { readSettings(); settings.uiState = uiState(); await invoke('save_settings', { settings }); panel.hidden = true; showStatus('Launch settings saved.'); }
    catch (error) { showStatus(String(error)); }
  });
  document.getElementById('cancelSetup').addEventListener('click', () => { panel.hidden = true; });

  // Build panel: runs scripts/build_client.py (start_build) into the game
  // folders above and refreshes Game Version when it finishes.
  const buildPanel = document.getElementById('buildPanel');
  const buildInputs = { buildIso: 'buildIso', buildTitleUpdate: 'buildTitleUpdate', buildCmake: 'buildCmake' };
  const variantBoxes = [...document.querySelectorAll('#buildVariants input')];
  const buildStep = document.getElementById('buildStep');
  const buildLog = document.getElementById('buildLog');
  const logLines = [];
  const linux = caps.platform === 'linux';
  if (!linux) {
    // Compiling is automated on Linux only; Windows prepares game folders.
    document.getElementById('buildVariants').hidden = true;
    document.getElementById('cmakeLabel').hidden = true;
    document.getElementById('buildIntro').textContent = 'Prepares the game folders from your disc image and, optionally, the USA Title Update 2 package. Compiling from the launcher is Linux-only for now; see the README to build on Windows.';
    document.getElementById('startBuild').textContent = 'Prepare';
  }
  document.getElementById('openBuild').hidden = !caps.canBuild;
  function openBuild() {
    Object.keys(buildInputs).forEach(key => { document.getElementById(key).value = settings[key] ?? ''; });
    panel.hidden = true; buildPanel.hidden = false;
  }
  function setBuilding(on) {
    document.getElementById('startBuild').hidden = on;
    document.getElementById('cancelBuild').hidden = !on;
    document.getElementById('closeBuild').disabled = on;
    document.getElementById('setupBtn').disabled = on;
    document.getElementById('playBtn').disabled = on;
    buildPanel.querySelectorAll('input, [data-pick]').forEach(el => { el.disabled = on; });
  }
  function log(line) {
    logLines.push(line);
    if (logLines.length > 400) logLines.shift();
    buildLog.textContent = logLines.join('\n');
    buildLog.scrollTop = buildLog.scrollHeight;
  }
  window.__TAURI__.event.listen('build-output', ({ payload }) => {
    const step = /^::step::(\d+)\/(\d+)::(.*)$/.exec(payload);
    if (step) buildStep.textContent = `Step ${step[1]} of ${step[2]}: ${step[3]}…`;
    else if (payload !== '::done::') log(payload);
  });
  document.getElementById('openBuild').addEventListener('click', openBuild);
  document.getElementById('closeBuild').addEventListener('click', () => { buildPanel.hidden = true; });
  buildPanel.querySelectorAll('[data-pick]').forEach(button => button.addEventListener('click', async () => {
    const kind = button.dataset.pick;
    try {
      const path = await invoke('pick_file', { kind });
      if (!path) return;
      button.previousElementSibling.value = path;
      // A title update usually means a TU2 build is wanted.
      if (kind === 'titleUpdate' && !variantBoxes.some(b => b.checked && b.value.startsWith('tu2'))) {
        variantBoxes.find(b => b.value === 'tu2').checked = true;
      }
    } catch (error) { buildStep.textContent = String(error); }
  }));
  document.getElementById('cancelBuild').addEventListener('click', () => {
    buildStep.textContent = 'Cancelling…';
    invoke('cancel_build').catch(error => { buildStep.textContent = String(error); });
  });
  document.getElementById('startBuild').addEventListener('click', async () => {
    Object.keys(buildInputs).forEach(key => { settings[key] = document.getElementById(key).value.trim(); });
    settings.uiState = uiState();
    const request = {
      iso: settings.buildIso, titleUpdate: settings.buildTitleUpdate, cmake: settings.buildCmake,
      variants: variantBoxes.filter(b => b.checked).map(b => b.value),
    };
    logLines.length = 0; buildLog.textContent = '';
    buildStep.textContent = 'Checking inputs…';
    setBuilding(true);
    try {
      const builds = await invoke('start_build', { settings, request });
      // The page reads CAPS.builds (its copy of LAUNCHER_CAPS) on each render.
      caps.builds = CAPS.builds = builds;
      listBuilds(); render();
      buildStep.textContent = linux ? 'Build complete. Choose it under Game Version.' : 'Game folders are ready.';
      showStatus('');
    } catch (error) {
      buildStep.textContent = String(error);
    } finally { setBuilding(false); }
  });
  if (!caps.builds.length && caps.canBuild) openBuild();

  window.LAUNCHER_HOST = {
    async post(message) {
      try {
        if (message.type !== 'play') { await invoke('window_action', { action: message.type }); return; }
        // Only Save applies the panel fields; Play keeps the saved folders.
        settings.uiState = uiState();
        const button = document.getElementById('playBtn');
        button.disabled = true; showStatus('Starting game…');
        try { await invoke('launch_game', { settings, options: message.config }); }
        finally { button.disabled = false; }
      } catch (error) { showStatus(String(error)); }
    },
  };
})();
