// Main menu rows: which are offered per platform, build and renderer, and what they send.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tick, BUILDS, SHADOW_BUILD, page, tauriPage } from './harness.mjs';

test('Linux shows Vulkan rows and hides D3D12-only options', () => {
  const { rows } = tauriPage();
  assert.deepEqual(rows().map(r => r.label),
    ['Game Version', 'Render Path', 'Frame Rate', 'VSync', 'Image Scaling', 'Internal Resolution', 'Display', 'Mouse', 'Profile', 'Manage Profiles', 'Remap Controls ›']);
  assert.equal(rows()[1].value, 'FSI');
  assert.equal(rows()[2].value, '120 FPS');
  assert.equal(rows()[3].value, 'Auto');
  assert.equal(rows()[4].value, 'Off');
});

test('Windows keeps the D3D12 rows and adds the build selector', () => {
  const { rows } = tauriPage({ platform: 'windows' });
  assert.deepEqual(rows().map(r => r.label).slice(0, 6),
    ['Game Version', 'Renderer', 'Render Path', 'Frame Rate', 'VSync', 'Image Scaling']);
  assert.equal(rows()[1].value, 'D3D12');
});

test('Windows can switch to Vulkan, which swaps in the Vulkan rows and is sent on Play', async () => {
  const { rows, row, next, doc, click, calls } = tauriPage({ platform: 'windows' });
  next('Renderer');
  assert.equal(row('Renderer').value, 'Vulkan');
  assert.deepEqual(rows().map(r => r.label).slice(0, 5), ['Game Version', 'Renderer', 'Render Path', 'Frame Rate', 'VSync']);
  assert.equal(row('Render Path').value, 'FSI');
  click(doc.getElementById('playBtn'));
  await tick();
  const options = calls.find(c => c.command === 'launch_game').args.options;
  assert.equal(options.backend, 'vulkan');
  assert.equal(options.vkPath, 'fsi');
  // Vulkan now gets the frame cap too; Auto VSync is off above 60 FPS.
  assert.equal(options.fps, 120);
  assert.equal(options.vsync, false);
  assert.equal(calls.find(c => c.command === 'launch_game').args.settings.uiState.backend, 1);
});

test('VSync Auto follows the frame cap on both renderers; On and Off override it', async () => {
  for (const backend of [0, 1]) {
    const launchWith = async saved => {
      const page = tauriPage({ platform: 'windows', saved: { backend, ...saved } });
      page.click(page.doc.getElementById('playBtn'));
      await tick();
      return page.calls.find(c => c.command === 'launch_game').args;
    };
    const at60 = await launchWith({ path: 1, fps: 0 });
    assert.equal(at60.options.fps, 60);
    assert.equal(at60.options.vsync, true, `backend ${backend}: Auto is on at 60 FPS`);
    assert.equal((await launchWith({ path: 1, fps: 1 })).options.vsync, false, 'Auto is off above 60 FPS');
    assert.equal((await launchWith({ path: 1, fps: 1, vsyncMode: 1 })).options.vsync, true, 'On');
    assert.equal((await launchWith({ path: 1, fps: 0, vsyncMode: 0 })).options.vsync, false, 'Off');
    // The older on/off key (only Vulkan showed it) no longer decides anything.
    const legacy = await launchWith({ path: 1, fps: 1, vsync: 1 });
    assert.equal(legacy.options.vsync, false);
    assert.equal(legacy.settings.uiState.vsyncMode, -1);
  }
});

test('Extras builds offer In-game resolution and display; normal builds do not', () => {
  const { row, next } = tauriPage();
  assert.equal(row('Internal Resolution').value, '1×', 'normal build falls back to an explicit scale');
  assert.equal(row('Display').value, 'Fullscreen');
  next('Game Version');
  assert.equal(row('Game Version').value, 'Update + Extras');
  assert.equal(row('Internal Resolution').value, 'In-game');
  assert.equal(row('Display').value, 'In-game');
  next('Internal Resolution');
  assert.equal(row('Internal Resolution').value, '1×');
});

test('Button Icons is only offered for Extras builds, is saved, and is sent on Play', async () => {
  const normal = tauriPage();
  assert.equal(normal.row('Button Icons'), undefined, 'normal builds have no such cvar');
  normal.click(normal.doc.getElementById('playBtn'));
  await tick();
  assert.equal(normal.calls.find(c => c.command === 'launch_game').args.options.glyphs, 'xbox');

  const { doc, click, row, next, calls } = tauriPage();
  next('Game Version');
  assert.equal(row('Game Version').value, 'Update + Extras');
  assert.equal(row('Button Icons').value, 'Auto', 'fresh installs pick the prompts from the controller');
  next('Button Icons');
  assert.equal(row('Button Icons').value, 'Xbox');
  next('Button Icons');
  assert.equal(row('Button Icons').value, 'PlayStation');
  click(doc.getElementById('playBtn'));
  await tick();
  const launch = calls.find(c => c.command === 'launch_game');
  assert.equal(launch.args.options.glyphs, 'playstation');
  assert.equal(launch.args.settings.uiState.glyphs, 1);

  // Switching back to a normal build never sends the extras-only choice.
  next('Game Version');
  assert.equal(row('Button Icons'), undefined);
  click(doc.getElementById('playBtn'));
  await tick();
  assert.equal(calls.filter(c => c.command === 'launch_game').at(-1).args.options.glyphs, 'xbox');
});

test('Button Icons Auto follows the controller and is sent as auto', async () => {
  const { doc, click, row, next, calls } = tauriPage({ saved: { build: 'update-extras', glyphs: 1 } });
  next('Button Icons');
  assert.equal(row('Button Icons').value, 'Auto');
  click(doc.getElementById('playBtn'));
  await tick();
  const launch = calls.find(c => c.command === 'launch_game');
  assert.equal(launch.args.options.glyphs, 'auto');
  assert.equal(launch.args.settings.uiState.glyphs, 2);
});

test('The saved Button Icons choice is restored with the other menu state', () => {
  const { row } = tauriPage({ saved: { res: -1, display: -1, build: 'update-extras', glyphs: 1 } });
  assert.equal(row('Button Icons').value, 'PlayStation');
});

test('Shadow is a Render Path choice of builds that have it and swaps the scaling rows for its own', async () => {
  const labels = p => p.rows().map(r => r.label);
  const p = tauriPage({ builds: [BUILDS[0], SHADOW_BUILD], saved: { res: -1, display: -1, build: SHADOW_BUILD.id } });
  assert.deepEqual(labels(p).slice(0, 6), ['Game Version', 'Render Path', 'Frame Rate', 'VSync', 'Image Scaling', 'Internal Resolution']);
  p.next('Render Path'); p.next('Render Path');
  assert.equal(p.row('Render Path').value, 'Shadow (beta)');
  assert.deepEqual(labels(p).slice(0, 7), ['Game Version', 'Render Path', 'Frame Rate', 'VSync', 'Render Resolution', 'Dynamic Resolution', 'Aspect Ratio']);
  // Defaults follow the display: the host works both out from the monitor.
  assert.equal(p.row('Render Resolution').value, 'Native');
  assert.equal(p.row('Aspect Ratio').value, 'Auto');
  p.click(p.doc.getElementById('playBtn'));
  await tick();
  const first = p.calls.find(c => c.command === 'launch_game').args.options;
  assert.deepEqual([first.shadow, first.shadowNative, first.shadowHeight, first.shadowAspect], [true, true, 0, 'auto']);
  assert.equal(first.shadowDynamic, false);
  p.next('Dynamic Resolution');
  assert.equal(p.row('Dynamic Resolution').value, 'On');
  assert.equal(first.vkPath, 'fsi', 'the SDK path stays valid; its backend draws nothing');

  // Presets: Native, 720p, 1080p, 1440p ... and 16:9, 16:10, 21:9 after Auto.
  for (let i = 0; i < 3; i++) p.next('Render Resolution');
  for (let i = 0; i < 3; i++) p.next('Aspect Ratio');
  assert.equal(p.row('Custom Height'), undefined);
  p.click(p.doc.getElementById('playBtn'));
  await tick();
  const preset = p.calls.filter(c => c.command === 'launch_game').at(-1).args;
  assert.deepEqual([preset.options.shadowNative, preset.options.shadowHeight, preset.options.shadowAspect], [false, 1440, '21:9']);
  assert.equal(preset.options.shadowDynamic, true);
  assert.equal(preset.settings.uiState.shdyn, 1);
  assert.equal(preset.settings.uiState.vkpath, 2);
  assert.equal(preset.settings.uiState.shres, 2);

  // Custom: a box for any height, kept within what the host accepts.
  p.next('Render Resolution'); p.next('Render Resolution');
  assert.equal(p.row('Render Resolution').value, 'Custom');
  const type = text => {
    const box = p.row('Custom Height').row.querySelector('input');
    box.value = text; box.dispatchEvent(new p.dom.window.Event('input'));
    return box;
  };
  assert.equal(type('1200p').value, '1200', 'digits only');
  p.click(p.doc.getElementById('playBtn'));
  await tick();
  const custom = p.calls.filter(c => c.command === 'launch_game').at(-1).args;
  assert.deepEqual([custom.options.shadowNative, custom.options.shadowHeight], [false, 1200]);
  assert.equal(custom.settings.uiState.shcustom, 1200);
  const box = type('99');
  box.dispatchEvent(new p.dom.window.Event('blur'));
  assert.equal(box.value, '360', 'snaps into range');

  // A build without it (a release from before, or no converter staged) has no such
  // choice, and a saved Shadow selection falls back to FSI.
  const normal = tauriPage({ builds: [BUILDS[0], SHADOW_BUILD], saved: { res: -1, display: -1, build: 'base', vkpath: 2 } });
  assert.equal(normal.row('Render Path').value, 'FSI');
  normal.next('Render Path'); normal.next('Render Path');
  assert.equal(normal.row('Render Path').value, 'FSI', 'FSI, Host, and round again');
  assert.equal(normal.row('Render Resolution'), undefined);

  // Windows: it is a Renderer, and has no render path of its own.
  const win = tauriPage({ platform: 'windows', builds: [BUILDS[0], SHADOW_BUILD], saved: { res: -1, display: -1, build: SHADOW_BUILD.id, backend: 2 } });
  assert.equal(win.row('Renderer').value, 'Shadow (beta)');
  assert.equal(win.row('Render Path'), undefined);
  assert.equal(win.row('Image Scaling'), undefined);
  assert.notEqual(win.row('Render Resolution'), undefined);
});

test('Button Icons is left out for extras builds from releases before it', async () => {
  const stable = { id: 'update-extras', label: 'Update + Extras (beta-1.4)', update: true, extras: true, glyphs: false, executable: '/s/diablo3' };
  const { rows, doc, click, calls } = tauriPage({ builds: [stable], saved: { res: -1, display: -1, glyphs: 1 } });
  assert.ok(!rows().some(r => r.label === 'Button Icons'));
  assert.ok(rows().some(r => r.label === 'Internal Resolution'), 'the other extras rows stay');
  click(doc.getElementById('playBtn'));
  await tick();
  assert.equal(calls.find(c => c.command === 'launch_game').args.options.glyphs, 'xbox');
});
