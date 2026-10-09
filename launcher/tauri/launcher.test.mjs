// Runs the shared launcher page in jsdom: as Launcher.ps1 loads it (no host
// capabilities) and as the Tauri build (dist/index.html, from prepare-ui.mjs).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { JSDOM } from 'jsdom';

const legacyHtml = readFileSync(new URL('../v6_borderless_final.html', import.meta.url), 'utf8');
const tauriHtml = readFileSync(new URL('./dist/index.html', import.meta.url), 'utf8');
const tick = () => new Promise(resolve => setTimeout(resolve, 0));

const BUILDS = [
  { id: 'base', label: 'Base', tu2: false, extras: false, executable: '/r/linux-amd64-relwithdebinfo/diablo3' },
  { id: 'tu2-extras', label: 'TU2 + Extras', tu2: true, extras: true, executable: '/r/linux-amd64-tu2-extras-relwithdebinfo/diablo3' },
];

function page(html, globals) {
  const dom = new JSDOM(html, { runScripts: 'dangerously', beforeParse: window => Object.assign(window, globals) });
  const doc = dom.window.document;
  const rows = () => [...doc.querySelectorAll('#menu .opt-row')].map(r => ({
    row: r, label: r.querySelector('.opt-label').textContent, value: r.querySelector('.opt-val')?.textContent,
  }));
  const row = label => rows().find(r => r.label === label);
  const click = el => el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
  const next = label => click(row(label).row.querySelector('[data-a="r"]'));
  return { dom, doc, rows, row, click, next };
}

function tauriPage({ platform = 'linux', builds = BUILDS, settings = {}, saved = { res: -1, display: -1 }, failLaunch = false,
  canBuild = true, responses = {} } = {}) {
  const calls = [];
  const listeners = {};
  const invoke = async (command, args) => {
    calls.push({ command, args });
    if (command === 'launch_game' && failLaunch) throw 'Default.xex does not match';
    if (command in responses) return responses[command](args);
  };
  const listen = async (event, handler) => { listeners[event] = handler; return () => {}; };
  const p = page(tauriHtml, {
    __TAURI__: { core: { invoke }, event: { listen } },
    LAUNCHER_CAPS: { platform, builds, canBuild, defaults: { gameDir: '/r/game', gameDirTu2: '/r/game-tu2', userDataRoot: '/d/sotr', userDataRootTu2: '/d/sotr-tu2' } },
    LAUNCHER_SETTINGS: { gameDir: '', gameDirTu2: '', userDataRoot: '', userDataRootTu2: '', vulkanDevice: -1, ...settings },
    SAVED_STATE: saved,
  });
  const emit = (event, payload) => listeners[event]({ payload });
  return { ...p, calls, emit };
}

test('Launcher.ps1 keeps its original rows and WebView2 message channel', () => {
  const posted = [];
  const { rows, row, click, doc } = page(legacyHtml, {
    chrome: { webview: { postMessage: m => posted.push(JSON.parse(m)) } },
    SAVED_STATE: { lang: 0, path: 1, fps: 1, scale: 0, res: 2, display: 1, mouse: 0 },
  });
  assert.deepEqual(rows().map(r => r.label),
    ['Render Path', 'Frame Rate', 'Image Scaling', 'Internal Resolution', 'Display', 'Mouse', 'Remap Controls ›']);
  assert.equal(row('Internal Resolution').value, '3×');
  click(doc.getElementById('playBtn'));
  const { config } = posted[0];
  assert.equal(posted[0].type, 'play');
  assert.equal(config.rtp, 'rtv');
  assert.equal(config.fps, 120);
  assert.equal(config.resScale, 3);
  assert.equal(config.fullscreen, false);
});

test('Linux shows Vulkan rows and hides D3D12-only options', () => {
  const { rows } = tauriPage();
  assert.deepEqual(rows().map(r => r.label),
    ['Game Version', 'Render Path', 'VSync', 'Internal Resolution', 'Display', 'Mouse', 'Remap Controls ›']);
  assert.equal(rows()[1].value, 'FSI');
});

test('Windows keeps the D3D12 rows and adds the build selector', () => {
  const { rows } = tauriPage({ platform: 'windows' });
  assert.deepEqual(rows().map(r => r.label).slice(0, 4), ['Game Version', 'Render Path', 'Frame Rate', 'Image Scaling']);
});

test('Extras builds offer In-game resolution and display; normal builds do not', () => {
  const { row, next } = tauriPage();
  assert.equal(row('Internal Resolution').value, '1×', 'normal build falls back to an explicit scale');
  assert.equal(row('Display').value, 'Fullscreen');
  next('Game Version');
  assert.equal(row('Game Version').value, 'TU2 + Extras');
  assert.equal(row('Internal Resolution').value, 'In-game');
  assert.equal(row('Display').value, 'In-game');
  next('Internal Resolution');
  assert.equal(row('Internal Resolution').value, '1×');
});

test('Play sends the selected build, in-game choices and saved menu state', async () => {
  const { doc, click, next, calls } = tauriPage({ settings: { gameDirTu2: '/games/tu2' } });
  next('Game Version');
  click(doc.getElementById('playBtn'));
  await tick();
  const launch = calls.find(c => c.command === 'launch_game');
  assert.equal(launch.args.options.build, 'tu2-extras');
  assert.equal(launch.args.options.resScale, 0);
  assert.equal(launch.args.options.fullscreen, null);
  assert.equal(launch.args.options.vkPath, 'fsi');
  assert.equal(launch.args.options.vsync, true);
  assert.equal(launch.args.settings.gameDirTu2, '/games/tu2');
  assert.equal(launch.args.settings.uiState.build, 'tu2-extras');
  assert.equal(launch.args.settings.uiState.view, undefined);
});

test('Launch failures stay visible and restore the Play button', async () => {
  const { doc, click } = tauriPage({ failLaunch: true });
  click(doc.getElementById('playBtn'));
  await tick();
  assert.equal(doc.getElementById('launchStatus').textContent, 'Default.xex does not match');
  assert.equal(doc.getElementById('playBtn').disabled, false);
});

test('Launch settings show defaults and save a blank Vulkan device as automatic', async () => {
  const { doc, click, calls } = tauriPage();
  click(doc.getElementById('setupBtn'));
  assert.equal(doc.getElementById('setup').hidden, false);
  assert.equal(doc.getElementById('gameDirTu2').placeholder, '/r/game-tu2');
  assert.equal(doc.getElementById('vulkanDevice').value, '');
  assert.match(doc.getElementById('buildList').textContent, /TU2 \+ Extras: .*tu2-extras/);
  doc.getElementById('userDataRoot').value = ' /saves ';
  click(doc.getElementById('saveSetup'));
  await tick();
  const saved = calls.find(c => c.command === 'save_settings').args.settings;
  assert.equal(saved.userDataRoot, '/saves');
  assert.equal(saved.vulkanDevice, -1);
  assert.equal(doc.getElementById('setup').hidden, true);
});

test('Missing builds hide the selector and explain where to look', () => {
  const { rows, doc } = tauriPage({ builds: [], canBuild: false });
  assert.equal(rows()[0].label, 'Render Path');
  assert.match(doc.getElementById('launchStatus').textContent, /No game build found/);
  assert.equal(doc.getElementById('buildPanel').hidden, true);
  assert.equal(doc.getElementById('openBuild').hidden, true, 'release installs cannot build');
});

test('A source checkout without builds opens the Build panel', () => {
  const { doc } = tauriPage({ builds: [], settings: { buildIso: '/isos/d3.iso' } });
  assert.equal(doc.getElementById('buildPanel').hidden, false);
  assert.equal(doc.getElementById('buildIso').value, '/isos/d3.iso');
  assert.match(doc.getElementById('launchStatus').textContent, /Build it from your disc image/);
});

test('Building streams progress, saves inputs and adds the new build to Game Version', async () => {
  let finish;
  const { doc, click, row, rows, calls, emit } = tauriPage({
    builds: [],
    responses: {
      pick_file: ({ kind }) => (kind === 'titleUpdate' ? '/isos/tu00000002_00000000' : null),
      start_build: () => new Promise(resolve => { finish = resolve; }),
    },
  });
  doc.getElementById('buildIso').value = ' /isos/d3.iso ';
  click(doc.querySelector('[data-pick="titleUpdate"]'));
  await tick();
  assert.equal(doc.getElementById('buildTitleUpdate').value, '/isos/tu00000002_00000000');
  assert.equal(doc.querySelector('#buildVariants input[value="tu2"]').checked, true, 'choosing a title update selects TU2');
  click(doc.getElementById('startBuild'));
  await tick();
  const { request, settings } = calls.find(c => c.command === 'start_build').args;
  assert.deepEqual(JSON.parse(JSON.stringify(request)), { iso: '/isos/d3.iso', titleUpdate: '/isos/tu00000002_00000000', cmake: '', variants: ['base', 'tu2'] });
  assert.equal(settings.buildIso, '/isos/d3.iso');
  assert.equal(doc.getElementById('playBtn').disabled, true);
  assert.equal(doc.getElementById('cancelBuild').hidden, false);
  emit('build-output', '::step::2/5::Extract title update');
  emit('build-output', 'CPKs/Patch.cpk: 3,774,503 bytes');
  assert.equal(doc.getElementById('buildStep').textContent, 'Step 2 of 5: Extract title update…');
  assert.equal(doc.getElementById('buildLog').textContent, 'CPKs/Patch.cpk: 3,774,503 bytes');
  finish(BUILDS);
  await tick(); await tick();
  assert.equal(doc.getElementById('playBtn').disabled, false);
  assert.equal(doc.getElementById('cancelBuild').hidden, true);
  assert.equal(rows()[0].label, 'Game Version');
  assert.equal(row('Game Version').value, 'Base');
  assert.match(doc.getElementById('buildList').textContent, /TU2 \+ Extras/);
  assert.equal(doc.getElementById('launchStatus').textContent, '');
});

test('Build failures and cancellation are reported in the panel', async () => {
  const { doc, click, calls } = tauriPage({ responses: { start_build: async () => { throw 'Build failed: wrong disc'; } } });
  click(doc.getElementById('setupBtn'));
  click(doc.getElementById('openBuild'));
  assert.equal(doc.getElementById('setup').hidden, true);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(doc.getElementById('buildStep').textContent, 'Build failed: wrong disc');
  assert.equal(doc.getElementById('startBuild').hidden, false);
  click(doc.getElementById('cancelBuild'));
  assert.ok(calls.some(c => c.command === 'cancel_build'));
});

test('Windows offers build choices and finds CMake by itself', () => {
  const { doc } = tauriPage({ platform: 'windows', builds: [] });
  assert.equal(doc.getElementById('buildVariants').hidden, false);
  assert.equal(doc.getElementById('cmakeLabel').hidden, true);
  assert.notEqual(doc.getElementById('startBuild').textContent, 'Prepare');
});

test('Cancelled settings edits are not used by Play', async () => {
  const { doc, click, calls } = tauriPage({ settings: { gameDir: '/games/base' } });
  click(doc.getElementById('setupBtn'));
  doc.getElementById('gameDir').value = '/typo';
  click(doc.getElementById('cancelSetup'));
  click(doc.getElementById('playBtn'));
  await tick();
  assert.equal(calls.find(c => c.command === 'launch_game').args.settings.gameDir, '/games/base');
});
