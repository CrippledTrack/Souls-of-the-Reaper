// @ts-check
// What Play sends to the host (Rust: game::GameOptions).
import { LANGS } from './languages.js';
import { SHADOW_ASPECTS, SHADOW_CUSTOM, SHADOW_HEIGHTS } from './state.js';

/**
 * @param {import('./types').MenuState} state
 * @param {import('./types').Rules} rules
 * @param {(key: string) => any} eff the value a menu row launches with (menu.js)
 * @returns {import('./types').LaunchConfig}
 */
export function buildLaunchConfig(state, rules, eff) {
  const b = rules.curBuild(), shadow = rules.shadow(), res = shadow ? 0 : eff('res'), display = eff('display');
  const fps = rules.fpsList()[eff('fps')];
  return {
    // The Shadow renderer (beta, every build with its converter staged): it draws the game itself, at
    // shadowHeight lines (0: the game's own 720) in the shadowAspect shape.
    shadow,
    // shadowNative: the host uses the display's height. shadowAspect 'auto': its shape.
    shadowNative: shadow && eff('shres') === -1,
    // Dynamic resolution: the height above becomes the most it draws at.
    shadowDynamic: shadow && state.shdyn === 1,
    shadowHeight: !shadow || eff('shres') === -1 ? 0 : eff('shres') === SHADOW_CUSTOM ? rules.customHeight() : SHADOW_HEIGHTS[eff('shres')],
    shadowAspect: !shadow ? '16:9' : eff('shaspect') === -1 ? 'auto' : SHADOW_ASPECTS[eff('shaspect')],
    // Selected build, Vulkan path, and null/0 for "In-game" display/resolution.
    build: b ? b.id : null,
    vkPath: ['fsi', 'host'][state.vkpath] || 'fsi',
    rtp: state.path === 0 ? 'rov' : 'rtv',
    fps,
    backend: rules.vk() ? 'vulkan' : 'd3d12',
    vsync: eff('vsyncMode') === -1 ? fps <= 60 : eff('vsyncMode') === 1,
    // Keyboard always races for a slot alongside gamepads - no launcher-side
    // control-mode pin. Only whether the mouse also drives the camera stays a
    // launcher choice.
    mnkMode: true,
    mnkMouse: state.mouse === 1,
    glyphs: rules.glyphs() ? ['xbox', 'playstation', 'auto'][eff('glyphs')] : 'xbox',
    present: rules.vk() ? ['bilinear', 'fsr', 'cas'][state.vkscale] : ['fsr', 'cas', 'bilinear'][state.scale],
    resScale: res + 1,
    fullscreen: display === -1 ? null : display === 0,
    lang: state.lang,
    langId: LANGS[state.lang].langId,
    countryId: LANGS[state.lang].countryId,
  };
}
