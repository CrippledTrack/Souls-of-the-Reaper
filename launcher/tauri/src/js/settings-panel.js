// @ts-check
// Launch settings panel (⚙): game and save folders, Vulkan device, display
// server, the Development builds' save folders (standalone launchers), and the
// builds found. It also owns the list of builds Game Version offers.
import { uiState } from './state.js';

/** @param {import('./types').Context} ctx */
export function createSettingsPanel(ctx) {
  const { doc, caps, settings, host, rules, t, showStatus } = ctx;
  /** @param {string} id */
  const el = id => /** @type {HTMLInputElement} */ (doc.getElementById(id));
  const panel = el('setup');
  const fields = ['gameDir', 'userDataRoot', 'userDataRootUpdate', 'userDataRootDevelopment', 'userDataRootUpdateDevelopment', 'vulkanDevice', 'videoDriver'];

  // Display server: three buttons around a hidden field that readSettings/fillSettings use.
  const videoField = el('videoDriver');
  const videoButtons = /** @type {HTMLElement[]} */ ([...doc.querySelectorAll('[data-video]')]);
  function showVideo() {
    videoButtons.forEach(b => {
      const on = b.dataset.video === videoField.value;
      b.classList.toggle('on', on); b.setAttribute('aria-checked', String(on));
    });
  }
  videoButtons.forEach(b => b.addEventListener('click', () => { videoField.value = b.dataset.video ?? ''; showVideo(); }));

  let allBuilds = caps.builds;
  /**
   * Sets what Game Version offers.
   * @param {import('./types').Build[]} builds
   */
  function showBuilds(builds) {
    allBuilds = caps.builds = builds;
    listBuilds(); ctx.render();
  }
  // Development builds (a standalone launcher's own version) keep separate saves; their
  // folders are only shown where such builds can exist.
  el('developmentBox').hidden = !(caps.standalone && (caps.canBuildDevelopment || allBuilds.some(b => b.development)));

  function fillSettings() {
    fields.forEach(key => {
      const field = el(key);
      const value = /** @type {any} */ (settings[key] ?? '');
      field.value = key === 'vulkanDevice' && value < 0 ? '' : value;
      if (caps.defaults[key] !== undefined) field.placeholder = caps.defaults[key];
    });
    showVideo();
  }
  function readSettings() {
    fields.forEach(key => {
      const value = el(key).value.trim();
      settings[key] = key === 'vulkanDevice' ? (value === '' ? -1 : Number(value)) : value;
    });
  }

  el('deviceLabel').hidden = !(caps.backends || []).includes('vulkan');
  // The display server (X11/Wayland) only exists on Linux.
  el('videoLabel').hidden = caps.platform !== 'linux';
  function listBuilds() {
    el('buildList').textContent = allBuilds.length
      ? allBuilds.map(b => `${b.label}: ${b.executable}`).join('\n')
      : t('settings.buildsFound.none');
  }
  showBuilds(allBuilds);
  ctx.onLanguage(() => {
    listBuilds();
    if (caps.version) el('launcherVersion').textContent = t('settings.version', { version: caps.version.label });
  });
  if (!allBuilds.length) showStatus(t(caps.canBuild ? 'status.noBuild.canBuild' : 'status.noBuild'));
  el('setupBtn').addEventListener('click', e => {
    e.stopPropagation(); fillSettings(); panel.hidden = !panel.hidden;
  });
  el('saveSetup').addEventListener('click', async () => {
    try {
      readSettings(); settings.uiState = uiState(ctx.state, rules); await host.invoke('save_settings', { settings });
      panel.hidden = true; showBuilds(allBuilds);
      showStatus(t('status.saved'));
    }
    catch (error) { showStatus(ctx.describe(error)); }
  });
  el('cancelSetup').addEventListener('click', () => { panel.hidden = true; });

  return {
    showBuilds,
    hide() { panel.hidden = true; },
    hasBuilds: () => allBuilds.length > 0,
  };
}
