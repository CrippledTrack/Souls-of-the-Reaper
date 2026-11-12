// Launch settings panel (⚙) and the Development builds' save folders.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tick, BUILDS, DEVELOPMENT_BUILD, TOOLS_OK, tauriPage } from './harness.mjs';

test('Linux Display Server is a launch setting, saved from the settings panel and sent on Play', async () => {
  const { doc, click, calls } = tauriPage({ settings: { videoDriver: 'x11' } });
  assert.equal(doc.getElementById('videoLabel').hidden, false);
  click(doc.getElementById('setupBtn'));
  assert.equal(doc.getElementById('videoDriver').value, 'x11');
  assert.equal(doc.querySelector('[data-video].on').dataset.video, 'x11');
  click(doc.querySelector('[data-video="wayland"]'));
  assert.equal(doc.querySelector('[data-video].on').dataset.video, 'wayland');
  click(doc.getElementById('saveSetup'));
  await tick();
  assert.equal(calls.find(c => c.command === 'save_settings').args.settings.videoDriver, 'wayland');
  assert.equal(tauriPage({ platform: 'windows' }).doc.getElementById('videoLabel').hidden, true);
});

test('Development builds are listed beside stable ones, and there is no experimental switch', async () => {
  const stable = { ...BUILDS[1], label: 'Update + Extras (beta-1.4)' };
  const { doc, click, calls } = tauriPage({ builds: [stable, DEVELOPMENT_BUILD], caps: { standalone: true, canBuildDevelopment: true },
    saved: { res: -1, display: -1, build: DEVELOPMENT_BUILD.id } });
  assert.match(doc.getElementById('menu').textContent, /Update \+ Extras \(Development\)/);
  click(doc.getElementById('setupBtn'));
  assert.equal(doc.getElementById('experimental'), null, 'the switch is gone');
  assert.match(doc.getElementById('buildList').textContent, /\(beta-1\.4\)[\s\S]*\(Development\)/);
  click(doc.getElementById('playBtn'));
  await tick(); await tick();
  const launch = calls.find(c => c.command === 'launch_game').args;
  assert.equal(launch.options.build, DEVELOPMENT_BUILD.id);
  assert.equal(launch.options.shadow, false, 'the Shadow renderer is a menu choice, off until picked');
});



test('Launch settings show defaults and save a blank Vulkan device as automatic', async () => {
  const { doc, click, calls } = tauriPage();
  click(doc.getElementById('setupBtn'));
  assert.equal(doc.getElementById('setup').hidden, false);
  assert.equal(doc.getElementById('gameDirUpdate'), null);
  assert.equal(doc.getElementById('vulkanDevice').value, '');
  assert.match(doc.getElementById('buildList').textContent, /Update \+ Extras: .*update-extras/);
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

test('Cancelled settings edits are not used by Play', async () => {
  const { doc, click, calls } = tauriPage({ settings: { gameDir: '/games/base' } });
  click(doc.getElementById('setupBtn'));
  doc.getElementById('gameDir').value = '/typo';
  click(doc.getElementById('cancelSetup'));
  click(doc.getElementById('playBtn'));
  await tick();
  assert.equal(calls.find(c => c.command === 'launch_game').args.settings.gameDir, '/games/base');
});

test('Launch settings show the launcher build revision when the host reports it', () => {
  const { doc } = tauriPage({ caps: { version: { commit: 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518', dirty: true, label: 'c94f5ebd (modified)' } } });
  assert.equal(doc.getElementById('launcherVersion').textContent, 'Launcher build c94f5ebd (modified)');
  assert.equal(tauriPage().doc.getElementById('launcherVersion').textContent, '');
});

test('Development builds have their own save folders, shown only where they can exist', async () => {
  const { doc, click, calls } = tauriPage({ caps: { standalone: true, canBuildDevelopment: true } });
  click(doc.getElementById('setupBtn'));
  const box = doc.getElementById('userDataRootUpdateDevelopment');
  assert.equal(doc.getElementById('developmentBox').hidden, false);
  assert.ok(doc.getElementById('developmentBox').contains(box));
  assert.equal(box.placeholder, '/d/sotr-update-development');
  box.value = ' /dev-saves ';
  click(doc.getElementById('saveSetup'));
  await tick();
  const saved = calls.find(c => c.command === 'save_settings').args.settings;
  assert.equal(saved.userDataRootUpdateDevelopment, '/dev-saves');
  assert.equal(saved.userDataRootDevelopment, '');
  for (const caps of [{}, { standalone: true, canBuildDevelopment: false }]) {
    assert.equal(tauriPage({ caps }).doc.getElementById('developmentBox').hidden, true, 'a checkout, or a launcher that cannot build Development');
  }
});
