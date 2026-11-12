// @ts-check
// Menu state (restored from the host's SAVED_STATE) and the rules derived from
// it and the host's capabilities.
import { newBinds } from './remap.js';

/** Frame rates for D3D12 ROV, and for RTV, Vulkan and Shadow. Both start at 30, so
 * an index means the same rate in either list (up to 144). */
export const FPS = [[30, 60, 120, 144], [30, 60, 120, 144, 240]];
/** Version of the saved `fps` index: before 2 the RTV/Vulkan/Shadow list began at 60. */
export const FPS_VERSION = 2;
/** Shadow Render Resolution presets; 0 is the game's own 720 lines. */
export const SHADOW_HEIGHTS = [0, 1080, 1440, 2160];
export const SHADOW_ASPECTS = ['16:9', '16:10', '21:9'];
/** Render Resolution value for a typed height (state.shcustom lines). */
export const SHADOW_CUSTOM = 9;
export const SHADOW_MIN = 360;
export const SHADOW_MAX = 2880;

/**
 * Defaults = RTV @ 120fps (path:1, fps:2 -> FPS[1][2]=120); they apply only on a
 * fresh install. Otherwise the host injects the last-used menu choices as
 * SAVED_STATE, so the launcher reopens on whatever you last played with.
 * vsyncMode -1 is Auto (on for caps up to 60 FPS); the older on/off `vsync`
 * key, which only Vulkan showed, is not read so D3D12 users keep the automatic rule.
 * @type {import('./types').UiState & {profile: string}}
 */
const DEFAULTS = {
  lang: 0, path: 1, fps: 2, scale: 0, vkscale: 0, res: 0, display: 0, mouse: 0, glyphs: 2, vkpath: 0,
  vsyncMode: -1, backend: 0, build: null, profile: '', shres: -1, shaspect: -1, shcustom: 1080, shdyn: 0,
};
const STRING_KEYS = ['build'];

/**
 * @param {import('./types').SavedState | undefined} saved
 * @param {number} languages how many game languages there are
 * @returns {import('./types').MenuState}
 */
export function createState(saved, languages) {
  /** @type {Record<string, unknown>} */
  const values = { ...DEFAULTS };
  if (saved) {
    Object.keys(DEFAULTS).forEach(key => {
      const value = /** @type {Record<string, unknown>} */ (saved)[key];
      if (typeof value === 'number' || (STRING_KEYS.includes(key) && typeof value === 'string')) values[key] = value;
    });
  }
  const state = /** @type {import('./types').MenuState} */ ({ ...values, view: 'main', binds: newBinds() });
  if (state.lang >= languages) state.lang = 0; // saved language no longer offered
  return state;
}

/**
 * A frame rate saved before 30 FPS joined the RTV/Vulkan/Shadow list points one
 * entry too low there: move it up so the same rate stays chosen.
 * @param {import('./types').MenuState} state
 * @param {import('./types').Rules} rules
 * @param {import('./types').SavedState | undefined} saved
 */
export function migrateFps(state, rules, saved) {
  if (!saved || typeof saved.fps !== 'number' || saved.fpsVersion === FPS_VERSION) return;
  if (rules.fpsList() === FPS[1]) state.fps = Math.min(state.fps + 1, FPS[1].length - 1);
}

/**
 * Menu state worth restoring next time (not the remap view or bindings).
 * @param {import('./types').MenuState} state
 * @param {import('./types').Rules} rules
 * @returns {import('./types').UiState}
 */
export function uiState(state, rules) {
  const { lang, path, fps, scale, vkscale, res, display, mouse, glyphs, vkpath, vsyncMode, backend, shres, shaspect, shcustom, shdyn } = state;
  const build = rules.curBuild();
  return { lang, path, fps, fpsVersion: FPS_VERSION, scale, vkscale, res, display, mouse, glyphs, vkpath, vsyncMode, backend, shres, shaspect, shcustom, shdyn, build: build ? build.id : null };
}

/**
 * @param {import('./types').Caps} caps
 * @param {import('./types').MenuState} state
 * @returns {import('./types').Rules}
 */
export function createRules(caps, state) {
  const linux = caps.platform === 'linux';
  // macOS: Vulkan through MoltenVK, where only the Shadow renderer draws.
  const mac = caps.platform === 'macos';
  const curBuild = () => (caps.builds && caps.builds.length ? caps.builds.find(b => b.id === state.build) || caps.builds[0] : null);
  // Windows hosts whose single executable holds both renderers offer a choice.
  const winVk = () => !linux && !mac && Array.isArray(caps.backends) && caps.backends.includes('vulkan');
  const vk = () => linux || mac || (winVk() && state.backend === 1);
  // The Shadow renderer (beta): offered for builds that have it with its shader
  // converter staged (releases before it lack both). It is a Render Path choice on Linux and a
  // Renderer choice on Windows, always on for macOS, and replaces the rows it makes moot.
  const shadowOk = () => { const b = curBuild(); return !!(b && b.shadow); };
  const shadow = () => shadowOk() && (mac || (linux ? state.vkpath === 2 : state.backend === 2));
  return {
    linux, mac, winVk, vk, curBuild, shadowOk, shadow,
    extras: () => { const b = curBuild(); return !!(b && b.extras); },
    glyphs: () => { const b = curBuild(); return !!(b && b.extras && b.glyphs); },
    fpsList: () => FPS[vk() || shadow() ? 1 : state.path],
    customHeight: () => Math.min(SHADOW_MAX, Math.max(SHADOW_MIN, Math.round(Number(state.shcustom) || 1080))),
  };
}
