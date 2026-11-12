// @ts-check
// Remap Controls view: lists the actions and records a key for one on click.
// The bindings are not sent to the game yet; the in-game F4 menu sets them.
import { makeRow } from './rows.js';

export const ACTIONS = ['moveUp', 'moveDown', 'moveLeft', 'moveRight', 'dpadUp', 'dpadDown', 'dpadLeft', 'dpadRight',
  'confirmA', 'cancelB', 'x', 'y', 'lb', 'rb', 'lt', 'rt', 'dodgeR3', 'inventory', 'pauseStart'];
/** @type {Record<string, string>} */
const DEFAULT_BINDS = {
  moveUp: 'W', moveDown: 'S', moveLeft: 'A', moveRight: 'D',
  dpadUp: 'Up / M', dpadDown: 'Down', dpadLeft: 'Left', dpadRight: 'Right',
  confirmA: 'Space / Enter', cancelB: 'Shift', x: 'J', y: 'U', lb: 'O', rb: 'K', lt: 'H', rt: 'L',
  dodgeR3: 'V', inventory: 'Tab / I', pauseStart: 'Esc',
};
export const newBinds = () => ({ ...DEFAULT_BINDS });

/** @type {Record<string, string>} */
const KEY_NAMES = {
  ' ': 'Space', ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right',
  Enter: 'Enter', Escape: 'Esc', Shift: 'Shift', Control: 'Ctrl', Alt: 'Alt', Tab: 'Tab',
};
/** @param {KeyboardEvent} e */
function keyLabel(e) {
  if (KEY_NAMES[e.key]) return KEY_NAMES[e.key];
  if (e.key.length === 1) return e.key.toUpperCase();
  return e.key;
}

/** @param {import('./types').Context} ctx */
export function createRemapView(ctx) {
  const { win, doc, state, t } = ctx;
  const menu = /** @type {HTMLElement} */ (doc.getElementById('menu'));
  /** @type {(() => void) | null} */
  let cancelListen = null;

  function stopListening() { if (cancelListen) { cancelListen(); cancelListen = null; } }

  /**
   * @param {string} action
   * @param {HTMLElement} row
   */
  function startListening(action, row) {
    stopListening();
    const valEl = /** @type {HTMLElement} */ (row.querySelector('.opt-val'));
    valEl.textContent = t('remap.pressKey'); valEl.classList.add('listening');
    /** @param {KeyboardEvent} e */
    function handler(e) {
      e.preventDefault();
      cleanup();
      if (e.key !== 'Escape') state.binds[action] = keyLabel(e);
      ctx.render();
    }
    function cleanup() { win.removeEventListener('keydown', handler, true); cancelListen = null; }
    win.addEventListener('keydown', handler, true);
    cancelListen = cleanup;
  }

  function render() {
    stopListening();
    menu.innerHTML = '';
    menu.appendChild(makeRow(doc, { label: t('remap.back'), onClick: () => { state.view = 'main'; ctx.render(); }, cls: 'nav back' }));
    const head = doc.createElement('div'); head.className = 'subhead';
    head.textContent = t('remap.header');
    menu.appendChild(head);
    ACTIONS.forEach(a => {
      const row = makeRow(doc, {
        label: t(/** @type {import('./types').MessageKey} */ (`remap.action.${a}`)), value: state.binds[a],
        onClick: () => startListening(a, row),
      });
      menu.appendChild(row);
    });
    const hint = doc.createElement('div'); hint.className = 'hint'; hint.textContent = t('remap.cancelHint');
    menu.appendChild(hint);
    menu.appendChild(makeRow(doc, { label: t('remap.reset'), onClick: () => { state.binds = newBinds(); ctx.render(); }, cls: 'nav' }));
    const first = /** @type {HTMLElement | null} */ (menu.querySelector('.opt-row'));
    if (first) first.focus({ preventScroll: true });
  }

  return { render, stopListening };
}
