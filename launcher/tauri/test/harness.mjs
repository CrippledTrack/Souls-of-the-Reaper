// Runs the launcher page (src/index.html) in jsdom: the page modules are
// imported here and started against the jsdom window, with a stubbed host.
import { readFileSync } from 'node:fs';
import { JSDOM } from 'jsdom';
import { start } from '../src/js/main.js';

const indexHtml = readFileSync(new URL('../src/index.html', import.meta.url), 'utf8');
export const tick = () => new Promise(resolve => setTimeout(resolve, 0));

export const BUILDS = [
  { id: 'base', label: 'Base', update: false, extras: false, executable: '/r/linux-amd64-relwithdebinfo/diablo3' },
  { id: 'update-extras', label: 'Update + Extras', update: true, extras: true, glyphs: true, executable: '/r/linux-amd64-update-extras-relwithdebinfo/diablo3' },
];

export function page(globals, { expectError = false } = {}) {
  // jsdom does not run module scripts; start() is called directly instead.
  const dom = new JSDOM(indexHtml);
  Object.assign(dom.window, globals);
  let error = null;
  try { start(dom.window); } catch (e) { if (!expectError) throw e; error = e; }
  const doc = dom.window.document;
  const rows = () => [...doc.querySelectorAll('#menu .opt-row')].map(r => ({
    row: r, label: r.querySelector('.opt-label').textContent, value: r.querySelector('.opt-val')?.textContent,
  }));
  const row = label => rows().find(r => r.label === label);
  const click = el => el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  const next = label => click(row(label).row.querySelector('[data-a="r"]'));
  return { dom, doc, rows, row, click, next, error };
}

export function tauriPage({ platform = 'linux', builds = BUILDS, settings = {}, saved = { res: -1, display: -1 }, failLaunch = false,
  canBuild = true, responses = {}, caps = {}, events = true, expectError = false } = {}) {
  const calls = [];
  const listeners = {};
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === 'launch_game' && failLaunch) throw 'Default.xex does not match';
    if (command in responses) return responses[command](args);
  };
  const listen = async (event, handler) => { listeners[event] = handler; return () => {}; };
  const p = page({
    __TAURI__: { core: { invoke }, event: events ? { listen } : {} },
    LAUNCHER_CAPS: { platform, updateInstalled: !!settings.updateInstalled, backends: platform === 'windows' ? ['d3d12', 'vulkan'] : ['vulkan'], builds, canBuild, defaults: { gameDir: '/r/game', gameDirUpdate: '/r/game', userDataRoot: '/d/sotr', userDataRootUpdate: '/d/sotr-update', userDataRootDevelopment: '/d/sotr-development', userDataRootUpdateDevelopment: '/d/sotr-update-development' }, ...caps },
    LAUNCHER_SETTINGS: { gameDir: '', gameDirUpdate: '', userDataRoot: '', userDataRootUpdate: '', vulkanDevice: -1, ...settings },
    SAVED_STATE: { ...saved, username: settings.username ?? '' },
  }, { expectError });
  const emit = (event, payload) => listeners[event]({ payload });
  return { ...p, calls, emit };
}

// A build with the Shadow renderer and its converter staged (every build made since it
// is part of the normal build).
export const SHADOW_BUILD = { id: 'update-extras', label: 'Update + Extras', update: true, extras: true, glyphs: true, shadow: true, executable: '/r/linux-amd64-update-relwithdebinfo/diablo3' };
// A standalone launcher's Development build (its own commit).
export const DEVELOPMENT_BUILD = { id: 'update-extras-development', label: 'Update + Extras (Development)', update: true, extras: true, glyphs: true, shadow: true, development: true, executable: '/b/sources/bbbbbbbbbbbb/port/out/build/linux-amd64-update-relwithdebinfo/diablo3' };
export const TOOLS_OK = { ready: true, tools: [], sdk: { installed: true }, converter: { installed: true } };
