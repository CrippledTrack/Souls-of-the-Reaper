// Play, window controls and links.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tick, BUILDS, DEVELOPMENT_BUILD, tauriPage } from './harness.mjs';

test('Play sends the selected build, in-game choices and saved menu state', async () => {
  const { doc, click, next, calls } = tauriPage({ settings: { gameDirUpdate: '/games/update' } });
  next('Game Version');
  click(doc.getElementById('playBtn'));
  await tick();
  const launch = calls.find(c => c.command === 'launch_game');
  assert.equal(launch.args.options.build, 'update-extras');
  assert.equal(launch.args.options.resScale, 0);
  assert.equal(launch.args.options.fullscreen, null);
  assert.equal(launch.args.options.vkPath, 'fsi');
  assert.equal(launch.args.options.vsync, false, 'Auto VSync is off at the default 120 FPS');
  assert.equal(launch.args.settings.gameDirUpdate, '/games/update');
  assert.equal(launch.args.settings.uiState.build, 'update-extras');
  assert.equal(launch.args.settings.uiState.view, undefined);
});

test('The font credit under Play opens in the default browser', async () => {
  const { doc, click, calls } = tauriPage();
  const link = doc.querySelector('.foot a.fontlink');
  assert.match(link.href, /^https:\/\/fernandoforeroart\.com\//);
  click(link);
  await tick();
  assert.deepEqual(calls.filter(c => c.command === 'open_external').map(c => c.args.url), [link.href]);
});

test('The borderless window can still be closed when the host setup fails', async () => {
  // Without the event API the Build panel setup throws halfway through.
  const { doc, click, calls, error } = tauriPage({ events: false, expectError: true });
  assert.match(String(error), /listen/);
  click(doc.getElementById('winClose'));
  await tick();
  assert.deepEqual(calls.filter(c => c.command === 'window_action').map(c => c.args.action), ['close']);
});

test('Launch failures stay visible and restore the Play button', async () => {
  const { doc, click } = tauriPage({ failLaunch: true });
  click(doc.getElementById('playBtn'));
  await tick();
  assert.equal(doc.getElementById('launchStatus').textContent, 'Default.xex does not match');
  assert.equal(doc.getElementById('playBtn').disabled, false);
});

test('A Development build\'s first launch offers to copy the normal saves', async () => {
  const offer = { from: '/d/sotr-update', to: '/d/sotr-update-development' };
  const run = async (button, offered = offer) => {
    const p = tauriPage({ builds: [...BUILDS, DEVELOPMENT_BUILD], caps: { standalone: true },
      saved: { res: -1, display: -1, build: DEVELOPMENT_BUILD.id }, responses: { development_saves: () => offered } });
    p.click(p.doc.getElementById('playBtn'));
    for (let i = 0; i < 3; i++) await tick();
    const prompt = p.doc.getElementById('savePrompt');
    if (offered) {
      assert.equal(prompt.hidden, false);
      assert.match(p.doc.getElementById('savePromptText').textContent, /\/d\/sotr-update-development.*\/d\/sotr-update /);
      assert.ok(!p.calls.some(c => c.command === 'launch_game'), 'waits for the answer');
      p.click(p.doc.getElementById(button));
      for (let i = 0; i < 3; i++) await tick();
      assert.equal(prompt.hidden, true);
    }
    return p.calls;
  };
  let calls = await run('saveCopy');
  assert.deepEqual(calls.filter(c => c.command === 'prepare_saves').map(c => [c.args.build, c.args.copy]), [[DEVELOPMENT_BUILD.id, true]]);
  assert.ok(calls.some(c => c.command === 'launch_game'));
  calls = await run('saveFresh');
  assert.deepEqual(calls.filter(c => c.command === 'prepare_saves').map(c => c.args.copy), [false]);
  assert.ok(calls.some(c => c.command === 'launch_game'));
  calls = await run('saveCancel');
  assert.ok(!calls.some(c => c.command === 'prepare_saves' || c.command === 'launch_game'), 'cancel launches nothing');
  calls = await run(null, null);
  assert.ok(calls.some(c => c.command === 'launch_game'), 'no offer: straight to the game');
  assert.ok(!calls.some(c => c.command === 'prepare_saves'));
});

test('Normal builds launch without asking about saves', async () => {
  const { doc, click, calls } = tauriPage();
  click(doc.getElementById('playBtn'));
  await tick();
  assert.ok(!calls.some(c => c.command === 'development_saves'));
  assert.ok(calls.some(c => c.command === 'launch_game'));
});
