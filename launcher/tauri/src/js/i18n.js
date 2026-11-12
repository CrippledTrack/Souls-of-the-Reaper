// @ts-check
// Launcher text. Each locale is a module of named keys (locales/<code>.js);
// en.js defines every key and is the fallback for keys a locale leaves out.
// t(key, {name}) fills {name} placeholders.
import en from '../locales/en.js';
import de from '../locales/de.js';
import es from '../locales/es.js';
import fr from '../locales/fr.js';
import it from '../locales/it.js';
import pt from '../locales/pt.js';
import ru from '../locales/ru.js';

/** @typedef {import('./types').MessageKey} MessageKey */
/** @typedef {import('./types').Params} Params */

/** @type {Record<string, import('./types').Translation>} */
export const LOCALES = { en, de, es, fr, it, pt, ru };

/**
 * @param {string} text
 * @param {Params} [params]
 */
export function format(text, params) {
  if (!params) return text;
  return text.replace(/\{(\w+)\}/g, (match, name) => (name in params ? String(params[name]) : match));
}

/**
 * A translator for whichever locale `locale()` names when it is called.
 * @param {() => string} locale
 * @returns {import('./types').Translate}
 */
export function createI18n(locale) {
  return (key, params) => {
    const text = LOCALES[locale()]?.[key] ?? en[key];
    return text === undefined ? key : format(text, params);
  };
}

/**
 * Fills the static text of the page: `data-i18n="key"` sets an element's text,
 * `data-i18n-html="key"` its markup (the locale files are ours), and
 * `data-i18n-attr="title:key;aria-label:key"` attributes.
 * @param {ParentNode} root
 * @param {import('./types').Translate} t
 */
export function applyStatic(root, t) {
  root.querySelectorAll('[data-i18n]').forEach(el => {
    el.textContent = t(/** @type {MessageKey} */ (/** @type {HTMLElement} */ (el).dataset.i18n));
  });
  root.querySelectorAll('[data-i18n-html]').forEach(el => {
    el.innerHTML = t(/** @type {MessageKey} */ (/** @type {HTMLElement} */ (el).dataset.i18nHtml));
  });
  root.querySelectorAll('[data-i18n-attr]').forEach(el => {
    (/** @type {HTMLElement} */ (el).dataset.i18nAttr ?? '').split(';').forEach(pair => {
      const [attr, key] = pair.split(':');
      if (attr && key) el.setAttribute(attr.trim(), t(/** @type {MessageKey} */ (key.trim())));
    });
  });
}

/**
 * Text for something the host sent: a message ({key, params}, Rust:
 * message::Message) in the current language, or a plain string as it is.
 * @param {import('./types').Translate} t
 * @param {unknown} value
 */
export function describe(t, value) {
  if (value && typeof value === 'object' && typeof (/** @type {any} */ (value).key) === 'string') {
    const { key, params } = /** @type {import('./types').HostMessage} */ (value);
    return t(/** @type {MessageKey} */ (key), params);
  }
  return value == null ? '' : String(value);
}
