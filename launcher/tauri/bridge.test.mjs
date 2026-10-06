import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const source = readFileSync(new URL('./bridge.js', import.meta.url), 'utf8');
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

async function harness(failLaunch = false) {
  const settings = { gameDir: '/tmp/game with spaces', executable: '/tmp/diablo3', userDataRoot: '/tmp/state', tu2: true, extras: true, vulkanDevice: 1, uiState: null };
  const elements = Object.fromEntries(['gameDir', 'executable', 'userDataRoot', 'tu2', 'extras', 'vulkanDevice', 'launchStatus', 'setup', 'setupToggle', 'saveSetup', 'deviceLabel', 'playBtn'].map(id => [id, { type: ['tu2', 'extras'].includes(id) ? 'checkbox' : id === 'vulkanDevice' ? 'number' : 'text', hidden: true, disabled: false }]));
  const calls = [];
  const window = { __TAURI__: { core: { invoke: async (command, args) => {
    calls.push({ command, args });
    if (command === 'load_settings') return settings;
    if (command === 'launch_game') { if (failLaunch) throw 'Missing executable'; return 42; }
  } } } };
  await new AsyncFunction('window', 'navigator', 'document', source)(window, { userAgent: 'Linux' }, { getElementById: id => elements[id] });
  return { window, elements, calls };
}

test('Play forwards current settings and UI state through the Tauri bridge', async () => {
  const { window, elements, calls } = await harness();
  elements.gameDir.value = '/tmp/new game';
  await window.chrome.webview.postMessage(JSON.stringify({ type: 'play', config: { resScale: 2 }, uiState: { res: 1 } }));
  const launch = calls.find(call => call.command === 'launch_game');
  assert.equal(launch.args.settings.gameDir, '/tmp/new game');
  assert.equal(launch.args.settings.vulkanDevice, 1);
  assert.equal(launch.args.options.resScale, 2);
  assert.deepEqual(launch.args.settings.uiState, { res: 1 });
  assert.equal(elements.playBtn.disabled, false);
  assert.match(elements.launchStatus.textContent, /PID 42/);
});

test('Launch failures stay visible and restore the Play button', async () => {
  const { window, elements } = await harness(true);
  await window.chrome.webview.postMessage(JSON.stringify({ type: 'play', config: {}, uiState: {} }));
  assert.equal(elements.launchStatus.textContent, 'Missing executable');
  assert.equal(elements.playBtn.disabled, false);
});
