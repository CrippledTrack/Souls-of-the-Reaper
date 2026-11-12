// @ts-check
// Language button + popup (top-left corner, always visible). The button shows
// the current language's code (e.g. "ES"); clicking it lists every language,
// each written in its own language. Choosing one also switches the launcher's
// own text (languages.js `ui`).
import { LANGS } from './languages.js';

/** @param {import('./types').Context} ctx */
export function createLanguageMenu(ctx) {
  const { doc, state } = ctx;
  const button = /** @type {HTMLElement} */ (doc.getElementById('langBtn'));
  const label = /** @type {HTMLElement} */ (doc.getElementById('langBtnLabel'));
  const popup = /** @type {HTMLElement} */ (doc.getElementById('langPopup'));

  function renderButton() {
    const lang = LANGS[state.lang];
    label.textContent = (lang.code || lang.name.slice(0, 2)).toUpperCase();
  }
  function renderPopup() {
    popup.innerHTML = '';
    LANGS.forEach((lang, i) => {
      const item = doc.createElement('button');
      item.type = 'button'; item.className = 'lang-opt' + (i === state.lang ? ' active' : ''); item.setAttribute('role', 'menuitem');
      item.textContent = lang.name;
      item.addEventListener('click', e => {
        e.stopPropagation();
        state.lang = i;
        close(); ctx.render();
      });
      popup.appendChild(item);
    });
  }
  function open() { renderPopup(); popup.classList.add('open'); button.setAttribute('aria-expanded', 'true'); }
  function close() { popup.classList.remove('open'); button.setAttribute('aria-expanded', 'false'); }

  button.addEventListener('click', e => {
    e.stopPropagation();
    if (popup.classList.contains('open')) close(); else open();
  });
  popup.addEventListener('click', e => e.stopPropagation());
  doc.addEventListener('click', () => close());
  doc.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });

  return { renderButton };
}
