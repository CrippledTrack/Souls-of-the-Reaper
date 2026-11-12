// @ts-check
// Launcher page. The Rust host injects LAUNCHER_CAPS (platform, builds, folder
// defaults, canBuild), LAUNCHER_SETTINGS (launcher.json) and SAVED_STATE (the
// last menu choices) before the page runs; start() builds the menu and wires
// the panels to the host's commands.
import { createBuildPanel } from './build-panel.js';
import { createHost } from './host.js';
import { applyStatic, createI18n, describe } from './i18n.js';
import { createLanguageMenu } from './lang-popup.js';
import { LANGS } from './languages.js';
import { buildLaunchConfig } from './launch-config.js';
import { createMainView } from './menu.js';
import { createProfiles } from './profiles.js';
import { createRemapView } from './remap.js';
import { createSavePrompt } from './save-prompt.js';
import { createSettingsPanel } from './settings-panel.js';
import { createRules, createState, uiState } from './state.js';

/** @param {Window & typeof globalThis} win */
export function start(win) {
  const doc = win.document;
  /** @type {import('./types').Caps} */
  const caps = Object.assign({ platform: 'windows', builds: [], defaults: {}, canBuild: false }, win.LAUNCHER_CAPS);
  const settings = win.LAUNCHER_SETTINGS;
  const saved = win.SAVED_STATE;
  const state = createState(saved, LANGS.length);
  const rules = createRules(caps, state);
  const status = /** @type {HTMLElement} */ (doc.getElementById('launchStatus'));
  const playBtn = /** @type {HTMLButtonElement} */ (doc.getElementById('playBtn'));

  const locale = () => LANGS[state.lang]?.ui ?? 'en';
  const t = createI18n(locale);
  // Text that follows the language: the page's static text, then what the
  // modules registered (panel texts they set themselves).
  /** @type {(() => void)[]} */
  const relabel = [() => { doc.documentElement.lang = locale(); applyStatic(doc, t); }];
  let shownLocale = '';

  /** @type {import('./types').Context} */
  const ctx = {
    win, doc, caps, settings, state, rules, t,
    host: createHost(win),
    describe: value => describe(t, value),
    showStatus(text) { status.textContent = text; },
    onLanguage(fn) { relabel.push(fn); if (shownLocale) fn(); },
    render() {},
  };
  const { host, showStatus } = ctx;

  // Profiles live in save folders: only the Tauri host (which reports the old
  // player name in SAVED_STATE) keeps them.
  state.profile = settings?.profile ?? '';
  if (saved && typeof saved.username === 'string') ctx.profiles = createProfiles(ctx);
  const remap = createRemapView(ctx);
  const mainView = createMainView(ctx);
  const languages = createLanguageMenu(ctx);
  ctx.render = () => {
    if (locale() !== shownLocale) { shownLocale = locale(); relabel.forEach(fn => fn()); }
    remap.stopListening();
    playBtn.textContent = t('menu.play');
    languages.renderButton();
    if (state.view === 'main') mainView.render();
    else if (state.view === 'profiles' && ctx.profiles) ctx.profiles.render();
    else remap.render();
  };
  ctx.render();

  // Window controls come before the panels: the window has no decorations, so if
  // anything below throws, close and drag must still work.
  /** @param {string} action */
  const windowAction = action => host.invoke('window_action', { action }).catch(error => showStatus(ctx.describe(error)));
  /** @type {HTMLElement} */ (doc.getElementById('winClose')).addEventListener('click', e => { e.stopPropagation(); windowAction('close'); });
  /** @type {HTMLElement} */ (doc.getElementById('dragHead')).addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    windowAction('dragStart');
  });

  const savePrompt = createSavePrompt(ctx);
  playBtn.addEventListener('click', async () => {
    playBtn.style.color = '#fff6df'; win.setTimeout(() => { playBtn.style.color = ''; }, 220);
    try {
      const options = buildLaunchConfig(state, rules, mainView.eff);
      // Only Save applies the panel fields; Play keeps the saved folders.
      settings.uiState = uiState(state, rules);
      // Player 1's profile is a main-screen row; keep it with the other saved settings.
      if (ctx.profiles) {
        const profile = String(mainView.eff('profile') ?? '');
        if (profile !== (settings.profile ?? '')) {
          settings.profile = profile;
          host.invoke('save_settings', { settings }).catch(error => showStatus(ctx.describe(error)));
        }
      }
      playBtn.disabled = true;
      try {
        // A Development build's first launch: offer the normal saves.
        if (!await savePrompt.before(options.build)) { showStatus(''); return; }
        showStatus(t('status.starting'));
        await host.invoke('launch_game', { settings, options });
      }
      finally { playBtn.disabled = false; }
    } catch (error) { showStatus(ctx.describe(error)); }
  });

  // External links (the font credit under Play) open in the default browser;
  // a Tauri webview ignores target="_blank" on its own.
  doc.addEventListener('click', e => {
    const target = /** @type {Element | null} */ (e.target);
    const link = target && target.closest ? /** @type {HTMLAnchorElement | null} */ (target.closest('a[href]')) : null;
    if (!link || !/^https:\/\//i.test(link.getAttribute('href') ?? '')) return;
    e.preventDefault();
    host.invoke('open_external', { url: link.href }).catch(error => showStatus(ctx.describe(error)));
  });

  // A finished build refreshes the settings panel's build list.
  const settingsPanel = createSettingsPanel(ctx);
  const buildPanel = createBuildPanel(ctx, { showBuilds: settingsPanel.showBuilds, hideSettings: settingsPanel.hide });
  if (!settingsPanel.hasBuilds() && caps.canBuild) buildPanel.openBuild();
}
