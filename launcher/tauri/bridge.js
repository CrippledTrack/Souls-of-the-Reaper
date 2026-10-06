const invoke = window.__TAURI__.core.invoke;
const settings = await invoke('load_settings');
const boot = { platform: navigator.userAgent.includes('Windows') ? 'windows' : 'linux' };
window.SAVED_STATE = settings.uiState || { path: 0, fps: 0, scale: 0, res: 0, display: 1, mouse: 0 };
if (boot.platform === 'linux') window.SAVED_STATE.path = 0;
const status = document.getElementById('launchStatus');
const panel = document.getElementById('setup');
const fields = ['gameDir', 'executable', 'userDataRoot', 'tu2', 'extras', 'vulkanDevice'];
function fillSettings() {
  fields.forEach(key => {
    const field = document.getElementById(key);
    if (field.type === 'checkbox') field.checked = settings[key];
    else field.value = settings[key];
  });
}
function readSettings() {
  fields.forEach(key => {
    const field = document.getElementById(key);
    settings[key] = field.type === 'checkbox' ? field.checked : field.type === 'number' ? Number(field.value) : field.value.trim();
  });
}
fillSettings();
document.getElementById('deviceLabel').hidden = boot.platform !== 'linux';
document.getElementById('setupToggle').onclick = () => { fillSettings(); panel.hidden = !panel.hidden; };
document.getElementById('saveSetup').onclick = async () => {
  try { readSettings(); await invoke('save_settings', { settings }); panel.hidden = true; status.textContent = 'Launch settings saved.'; }
  catch (error) { status.textContent = String(error); }
};
// Preserve the existing frontend's message contract with a Tauri adapter.
window.chrome = window.chrome || {};
window.chrome.webview = {
  postMessage: async raw => {
    const message = JSON.parse(raw);
    try {
      if (message.type === 'play') {
        readSettings(); settings.uiState = message.uiState;
        const button = document.getElementById('playBtn');
        button.disabled = true; status.textContent = 'Starting game…';
        try {
          const pid = await invoke('launch_game', { settings, options: message.config });
          status.textContent = `Game started (PID ${pid}). Logs are in your save directory.`;
        } finally { button.disabled = false; }
      } else { await invoke('window_action', { action: message.type }); }
    } catch (error) { status.textContent = String(error); }
  }
};
