// @ts-check
// Main menu view. Each row lists its allowed state values; a stored value
// outside that list (e.g. "In-game" after switching to a normal build)
// displays and launches as the row's first choice.
// Language is not one of these rows: it lives in the top-left language button
// (lang-popup.js). Keyboard and gamepads all compete for player 1 at the title
// screen, so there is no control-mode row; only the mouse-as-camera toggle and
// the key remap stay here.
import { makeRow } from './rows.js';
import { IN_GAME } from './profiles.js';
import { FPS, SHADOW_ASPECTS, SHADOW_CUSTOM } from './state.js';

/**
 * @typedef {object} RowDef
 * @property {string} key state key the row changes
 * @property {string} label
 * @property {string} [tip]
 * @property {{v: any, t: string}[]} opts
 * @property {boolean} [novals] shows no value (input or nav rows)
 * @property {'remap' | 'profiles'} [nav] the view the row opens
 * @property {() => HTMLInputElement} [input]
 */

/** @param {import('./types').Context} ctx */
export function createMainView(ctx) {
  const { doc, state, caps, rules, t } = ctx;
  const menu = /** @type {HTMLElement} */ (doc.getElementById('menu'));
  let lastKey = 'path';

  /** @param {string[]} labels */
  const opts = labels => labels.map((text, v) => ({ v, t: text }));

  /** @returns {RowDef[]} */
  function mainRows() {
    const ext = rules.extras(), ingame = ext ? [{ v: -1, t: t('value.inGame') }] : [];
    /** @type {RowDef[]} */
    const rows = [];
    if (caps.builds && caps.builds.length)
      rows.push({ key: 'build', label: t('menu.gameVersion'), opts: caps.builds.map(b => ({ v: b.id, t: b.label })), tip: t('menu.gameVersion.tip') });
    const shadowOk = rules.shadowOk(), shadow = rules.shadow();
    if (rules.winVk())
      rows.push({ key: 'backend', label: t('menu.renderer'), opts: opts(['D3D12', 'Vulkan'].concat(shadowOk ? ['Shadow (beta)'] : [])),
        tip: shadow ? t('menu.shadow.tip') : t('menu.renderer.tip') });
    // The Shadow renderer has no render paths of its own (Windows: it is the Renderer;
    // macOS: it is the only one, so there is nothing to choose).
    if (rules.mac) { /* no Renderer or Render Path row */ }
    else if (rules.vk() || rules.linux)
      rows.push({ key: 'vkpath', label: t('menu.renderPath'), opts: opts(['FSI', 'Host'].concat(shadowOk && rules.linux ? ['Shadow (beta)'] : [])),
        tip: shadow ? t('menu.shadow.tip') : t('menu.renderPath.vulkanTip') });
    else if (!shadow)
      rows.push({ key: 'path', label: t('menu.renderPath'), opts: opts(['ROV', 'RTV']), tip: t('menu.renderPath.tip') });
    // The frame limiter and the 60 Hz game timing (vsync) are shared by both
    // renderers. Vulkan has no ROV path, so it offers the RTV frame rates.
    rows.push({ key: 'fps', label: t('menu.frameRate'), opts: opts(rules.fpsList().map(f => f + ' FPS')) });
    rows.push({ key: 'vsyncMode', label: t('menu.vsync'), tip: t('menu.vsync.tip'),
      opts: [{ v: -1, t: t('value.auto') }, { v: 1, t: t('value.on') }, { v: 0, t: t('value.off') }] });
    if (shadow) {
      // It presents its own picture (no FSR/CAS) and draws at any height directly, so
      // Image Scaling and the 1×/2×/3× Internal Resolution give way to these two.
      rows.push({ key: 'shres', label: t('menu.renderResolution'), tip: t('menu.renderResolution.tip') + ' ' + t('menu.renderResolution.nativeTip'),
        opts: [{ v: -1, t: t('value.native') }].concat(opts(['720p', '1080p', '1440p', '2160p']), [{ v: SHADOW_CUSTOM, t: t('value.custom') }]) });
      // Any height works, so odd displays and supersampling get a box to type one in.
      if (state.shres === SHADOW_CUSTOM)
        rows.push({ key: 'shcustom', label: t('menu.customHeight'), input: heightInput, novals: true, opts: [], tip: t('menu.customHeight.tip') });
      rows.push({ key: 'shdyn', label: t('menu.dynamicResolution'), opts: opts([t('value.off'), t('value.on')]), tip: t('menu.dynamicResolution.tip') });
      rows.push({ key: 'shaspect', label: t('menu.aspectRatio'), tip: t('menu.aspectRatio.tip') + ' ' + t('menu.aspectRatio.autoTip'),
        opts: [{ v: -1, t: t('value.auto') }].concat(opts(SHADOW_ASPECTS)) });
    } else {
      // Vulkan's scaling row puts Off (plain bilinear) first so it stays the default.
      rows.push(rules.vk()
        ? { key: 'vkscale', label: t('menu.imageScaling'), opts: opts([t('value.off'), 'FSR', 'CAS']), tip: t('menu.imageScaling.tip') }
        : { key: 'scale', label: t('menu.imageScaling'), opts: opts(['FSR', 'CAS', t('value.off')]), tip: t('menu.imageScaling.tip') });
      rows.push({ key: 'res', label: t('menu.internalResolution'), opts: ingame.concat(opts(['1×', '2×', '3×'])),
        tip: t('menu.internalResolution.tip') + (ext ? ' ' + t('menu.inGame.tip') : '') });
    }
    rows.push({ key: 'display', label: t('menu.display'), opts: ingame.concat(opts([t('value.fullscreen'), t('value.windowed')])),
      tip: ext ? t('menu.inGame.tip') : undefined });
    rows.push({ key: 'mouse', label: t('menu.mouse'), opts: opts([t('value.off'), t('value.on')]) });
    // The cvar behind this only exists in extras builds, and not in releases before it.
    if (rules.glyphs()) rows.push({ key: 'glyphs', label: t('menu.buttonIcons'), opts: opts(['Xbox', 'PlayStation', t('value.auto')]), tip: t('menu.buttonIcons.tip') });
    // Player 1's profile, from the chosen build's save folder (hosts that keep profiles).
    if (ctx.profiles) {
      // Choose in game is always offered, last: the game then asks Player 1 at start.
      rows.push({ key: 'profile', label: t('menu.profile'), tip: t('menu.profile.tip'),
        opts: ctx.profiles.list().map(p => ({ v: p.xuid, t: p.name })).concat([{ v: IN_GAME, t: t('menu.profile.inGame') }]) });
      rows.push({ key: 'profiles', label: t('menu.manageProfiles'), nav: 'profiles', novals: true, opts: [] });
    }
    rows.push({ key: 'remap', label: t('menu.remap'), nav: 'remap', novals: true, opts: [] });
    return rows;
  }

  /** @type {Record<string, any>} */
  const values = state;
  /** @param {RowDef} d */
  const effIdx = d => Math.max(0, d.opts.findIndex(o => o.v === values[d.key]));
  /**
   * The value a row launches with: its stored value, or its first choice when the
   * stored one is not offered.
   * @param {string} key
   */
  function eff(key) {
    const d = /** @type {RowDef} */ (mainRows().find(r => r.key === key));
    return d.opts[effIdx(d)].v;
  }
  /**
   * @param {string} key
   * @param {number} dir
   */
  function cycle(key, dir) {
    const d = /** @type {RowDef} */ (mainRows().find(r => r.key === key)), n = d.opts.length;
    values[key] = d.opts[(effIdx(d) + dir + n) % n].v;
    if (key === 'path' && state.fps >= FPS[state.path].length) state.fps = FPS[state.path].length - 1;
    lastKey = key; ctx.render();
  }

  function render() {
    menu.innerHTML = '';
    // The profiles row lists the chosen build's save folder: load it on a change.
    if (ctx.profiles && caps.builds?.length) ctx.profiles.follow(eff('build'));
    const rows = mainRows();
    if (!rows.some(d => d.key === lastKey)) lastKey = rows[0].key;
    rows.forEach(d => {
      const row = makeRow(doc, {
        label: d.label, value: d.novals ? undefined : d.opts[effIdx(d)].t, tip: d.tip,
        onLeft: d.novals ? undefined : () => cycle(d.key, -1),
        onRight: d.novals ? undefined : () => cycle(d.key, 1),
        onClick: () => {
          lastKey = d.key;
          if (d.nav) { state.view = d.nav; ctx.render(); }
          else if (d.input) /** @type {HTMLInputElement} */ (row.querySelector('input')).focus();
          else cycle(d.key, 1);
        },
        cls: (d.key === lastKey ? 'active' : '') + (d.nav ? ' nav' : ''),
      });
      row.dataset.key = d.key;
      if (d.input) row.appendChild(d.input());
      row.addEventListener('focus', () => {
        lastKey = d.key;
        doc.querySelectorAll('.opt-row').forEach(x => x.classList.remove('active')); row.classList.add('active');
      });
      menu.appendChild(row);
    });
    const act = /** @type {HTMLElement | null} */ (menu.querySelector('.opt-row.active'));
    if (act) act.focus({ preventScroll: true });
  }

  /**
   * Shared behaviour of the row text boxes.
   * @param {HTMLInputElement} input
   */
  function boxKeys(input) {
    // Keep Space/Enter/arrows for typing instead of the row's own key handling.
    input.addEventListener('keydown', e => {
      e.stopPropagation();
      if (e.key === 'Enter') /** @type {HTMLElement} */ (input.closest('.opt-row')).focus();
    });
    input.addEventListener('click', e => e.stopPropagation());
  }

  // Number box for the Shadow renderer's custom render height.
  function heightInput() {
    const input = doc.createElement('input');
    input.className = 'opt-name'; input.type = 'text'; input.inputMode = 'numeric'; input.maxLength = 4; input.spellcheck = false;
    input.placeholder = '1080'; input.value = String(rules.customHeight()); input.setAttribute('aria-label', t('menu.customHeight'));
    input.addEventListener('input', () => { input.value = input.value.replace(/\D/g, ''); state.shcustom = Number(input.value) || 0; });
    // Out-of-range entries snap into range when the box is left.
    input.addEventListener('blur', () => { state.shcustom = rules.customHeight(); input.value = String(state.shcustom); });
    boxKeys(input);
    return input;
  }

  return { render, eff };
}
