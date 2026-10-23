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
  const fields = ['gameDir', 'userDataRoot', 'userDataRootUpdate', 'vulkanDevice'];

  // Menu state worth restoring next time (not the remap view or bindings).
  const uiState = () => {
    const { lang, path, fps, scale, vkscale, res, display, mouse, vkpath, vsync, backend } = state;
    return { lang, path, fps, scale, vkscale, res, display, mouse, vkpath, vsync, backend, build: curBuild() ? curBuild().id : null };
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

  // External links (the font credit under Play) open in the default browser;
  // a Tauri webview ignores target="_blank" on its own.
  document.addEventListener('click', e => {
    const link = e.target.closest && e.target.closest('a[href]');
    if (!link || !/^https:\/\//i.test(link.getAttribute('href'))) return;
    e.preventDefault();
    invoke('open_external', { url: link.href }).catch(error => showStatus(String(error)));
  });

  document.getElementById('deviceLabel').hidden = !(caps.backends || []).includes('vulkan');
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
  if (caps.platform === 'windows') {
    // build_windows.py finds CMake, Ninja and Clang (Visual Studio, LLVM) by itself.
    document.getElementById('cmakeLabel').hidden = true;
    document.getElementById('buildIntro').textContent = 'Builds the game from your disc image and, optionally, the USA Title Update package. Needs Visual Studio 2022 (C++ and CMake workload), LLVM Clang, Git and Python.';
  }
  document.getElementById('openBuild').hidden = !caps.canBuild;
  // The title update is an overlay on the base game, and each variant is a full separate
  // compile, so an update build replaces its base counterpart instead of joining it.
  // Only these events switch the selection; the user can still tick Base again.
  const variantBox = value => variantBoxes.find(b => b.value === value);
  const BASE_OF = { update: 'base', 'update-plain': 'base-plain' };
  function titleUpdateAdded() {
    const bases = Object.entries(BASE_OF).filter(([, base]) => variantBox(base).checked);
    if (bases.length) {
      bases.forEach(([tu, base]) => { variantBox(base).checked = false; variantBox(tu).checked = true; });
    } else if (!variantBoxes.some(b => b.checked && b.value.startsWith('update'))) {
      variantBox('update').checked = true;
    }
  }
  variantBoxes.forEach(box => box.addEventListener('change', () => {
    const base = BASE_OF[box.value];
    if (base && box.checked) variantBox(base).checked = false;
  }));
  document.getElementById('buildTitleUpdate').addEventListener('change', event => {
    if (event.target.value.trim()) titleUpdateAdded();
  });
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
      // A title update means an update build is wanted, in place of the base one.
      if (kind === 'titleUpdate') titleUpdateAdded();
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
      buildStep.textContent = 'Build complete. Choose it under Game Version.';
      showStatus('');
    } catch (error) {
      buildStep.textContent = String(error);
    } finally { setBuilding(false); }
  });
  // Same signal as the launcher's update autoselect (caps.updateInstalled): once the
  // update is there, the Build panel opens on update instead of Base so a rebuild
  // does not make both. A saved package path counts too. Applied once, so a
  // deliberate Base tick later is left alone.
  if (caps.updateInstalled || (settings.buildTitleUpdate ?? '').trim()) titleUpdateAdded();
  if (!caps.builds.length && caps.canBuild) openBuild();

  window.LAUNCHER_HOST = {
    async post(message) {
      try {
        if (message.type !== 'play') { await invoke('window_action', { action: message.type }); return; }
        // Only Save applies the panel fields; Play keeps the saved folders.
        settings.uiState = uiState();
        // The player name is a main-screen row; keep it with the other saved settings.
        const name = String(message.config.username ?? '').trim();
        if (name !== settings.username) {
          settings.username = name;
          invoke('save_settings', { settings }).catch(error => showStatus(String(error)));
        }
        const button = document.getElementById('playBtn');
        button.disabled = true; showStatus('Starting game…');
        try { await invoke('launch_game', { settings, options: message.config }); }
        finally { button.disabled = false; }
      } catch (error) { showStatus(String(error)); }
    },
  };
})();
