// Locale files and the page's translated text.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { LOCALES, createI18n, describe } from '../src/js/i18n.js';
import { LANGS } from '../src/js/languages.js';
import { tick, tauriPage } from './harness.mjs';

const en = LOCALES.en;
const placeholders = text => [...text.matchAll(/\{(\w+)\}/g)].map(m => m[1]).sort();

test('Every locale only uses English keys, with the same placeholders', () => {
  for (const [code, messages] of Object.entries(LOCALES)) {
    for (const [key, text] of Object.entries(messages)) {
      assert.ok(key in en, `${code}.js has ${key}, which en.js lacks`);
      assert.deepEqual(placeholders(text), placeholders(en[key]), `${code}.js ${key} placeholders`);
    }
  }
});

test('Every game language shows an existing locale', () => {
  for (const lang of LANGS) assert.ok(lang.ui in LOCALES, `${lang.name}: no locales/${lang.ui}.js`);
});

test('Every key in the page markup exists in en.js', () => {
  const html = readFileSync(new URL('../src/index.html', import.meta.url), 'utf8');
  const keys = [
    ...[...html.matchAll(/data-i18n(?:-html)?="([^"]+)"/g)].map(m => m[1]),
    ...[...html.matchAll(/data-i18n-attr="([^"]+)"/g)].flatMap(m => m[1].split(';').map(pair => pair.split(':')[1])),
  ];
  assert.ok(keys.length > 30);
  for (const key of keys) assert.ok(key in en, `index.html uses ${key}, which en.js lacks`);
});

test('Missing keys fall back to English and placeholders are filled', () => {
  const t = createI18n(() => 'de');
  assert.equal(t('menu.play'), 'Spielen');
  assert.equal(t('status.saved'), en['status.saved'], 'not translated yet: English');
  assert.equal(t('settings.version', { version: 'abc' }), 'Launcher build abc');
  assert.equal(describe(t, { key: 'err.gameDir.missingCpk', params: { file: 'Patch.cpk', folder: '/g/update' } }),
    'Missing update asset CPKs/Patch.cpk in /g/update.');
  assert.equal(describe(t, { key: 'err.other', params: { detail: 'disk full' } }), 'disk full');
  assert.equal(describe(t, 'plain text'), 'plain text');
});

test('Host messages are shown in the launcher language', async () => {
  const p = tauriPage({
    responses: { launch_game: () => { throw { key: 'err.launch.noBuild', params: {} }; } },
  });
  p.click(p.doc.getElementById('playBtn'));
  await tick();
  assert.equal(p.doc.getElementById('launchStatus').textContent, en['err.launch.noBuild']);
});

test('Choosing a language translates the menu and the page text, panels included', () => {
  const { doc, click } = tauriPage({ caps: { version: { label: 'abc1234' } } });
  assert.equal(doc.documentElement.lang, 'en');
  assert.equal(doc.querySelector('.fontlink').title, '“Old Fenris” font by Fernando Forero');
  click(doc.getElementById('langBtn'));
  click([...doc.querySelectorAll('.lang-opt')].find(b => b.textContent === 'Español'));
  assert.equal(doc.documentElement.lang, 'es');
  assert.equal(doc.getElementById('playBtn').textContent, 'Jugar');
  assert.equal(doc.querySelector('.fontlink').title, 'Fuente «Old Fenris» por Fernando Forero');
  // Not translated yet: the panel keeps working in English.
  assert.equal(doc.querySelector('#setup h2').textContent, 'Launch settings');
  assert.equal(doc.getElementById('launcherVersion').textContent, 'Launcher build abc1234');
});
