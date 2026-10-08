// Tauri host adapter, appended after the shared launcher script. The Rust
// host injects LAUNCHER_CAPS (platform, builds, folder defaults),
// LAUNCHER_SETTINGS and SAVED_STATE before the page runs; this file wires the
// page's host messages and the launch-settings panel to Tauri commands.
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
  document.getElementById('buildList').textContent = caps.builds.length
    ? caps.builds.map(b => `${b.label}: ${b.executable}`).join('\n')
    : 'No game build found next to the launcher or under port/out/build.';
  if (!caps.builds.length) showStatus('No game build found. Open launch settings (⚙) for details.');
  document.getElementById('setupBtn').addEventListener('click', e => {
    e.stopPropagation(); fillSettings(); panel.hidden = !panel.hidden;
  });
  document.getElementById('saveSetup').addEventListener('click', async () => {
    try { readSettings(); settings.uiState = uiState(); await invoke('save_settings', { settings }); panel.hidden = true; showStatus('Launch settings saved.'); }
    catch (error) { showStatus(String(error)); }
  });
  document.getElementById('cancelSetup').addEventListener('click', () => { panel.hidden = true; });

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
