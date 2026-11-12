// Local profiles: Player 1's Profile row and the Profiles view (Rust: game/profiles.rs).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tick, tauriPage } from './harness.mjs';

const ME = { xuid: 'B13EBABEBABEBABE', name: 'Nephalem' };
const FRIEND = { xuid: 'B13EBABE12345678', name: 'Friend' };

/** A host whose save folders hold `profiles` (per build id), editable like the Rust side. */
function profileHost(profiles = { base: [ME, FRIEND], 'update-extras': [ME] }) {
  const folders = structuredClone(profiles);
  return {
    folders,
    responses: {
      list_profiles: ({ build }) => structuredClone(folders[build] ?? []),
      create_profile: ({ build, name }) => {
        if (folders[build].some(p => p.name.toLowerCase() === name.trim().toLowerCase())) {
          throw { key: 'err.profile.taken', params: { name: name.trim() } };
        }
        const made = { xuid: 'B13EBABE0000000' + folders[build].length, name: name.trim() };
        folders[build].push(made);
        return made;
      },
      rename_profile: ({ build, xuid, name }) => {
        const profile = folders[build].find(p => p.xuid === xuid);
        profile.name = name.trim();
        return profile;
      },
      delete_profile: ({ build, xuid }) => { folders[build] = folders[build].filter(p => p.xuid !== xuid); },
    },
  };
}

test('Player 1 plays the profile chosen on the main screen, from the chosen build’s save folder', async () => {
  const host = profileHost();
  const p = tauriPage({ settings: { profile: FRIEND.xuid }, saved: { res: -1, display: -1, build: 'base' }, responses: host.responses });
  await tick();
  assert.equal(p.row('Profile').value, 'Friend', 'the saved profile');
  p.next('Profile');
  assert.equal(p.row('Profile').value, 'Choose in game', 'offered after the profiles');
  p.next('Profile');
  assert.equal(p.row('Profile').value, 'Nephalem');
  p.click(p.doc.getElementById('playBtn'));
  await tick();
  const launch = p.calls.find(c => c.command === 'launch_game');
  assert.equal(launch.args.settings.profile, ME.xuid);
  assert.equal(p.calls.find(c => c.command === 'save_settings').args.settings.profile, ME.xuid);
  assert.equal('username' in launch.args.options, false, 'no player name any more');

  // Another build has another save folder: its profiles are listed instead.
  p.next('Game Version');
  await tick();
  assert.equal(p.calls.filter(c => c.command === 'list_profiles').at(-1).args.build, 'update-extras');
  assert.equal(p.row('Profile').value, 'Nephalem');
});

test('Choose in game is always offered and launches without a profile', async () => {
  const p = tauriPage({ saved: { res: -1, display: -1, build: 'base' }, responses: profileHost().responses });
  await tick();
  assert.equal(p.row('Profile').value, 'Nephalem', 'nothing chosen yet: the first profile');
  p.next('Profile'); p.next('Profile');
  assert.equal(p.row('Profile').value, 'Choose in game');
  p.click(p.doc.getElementById('playBtn'));
  await tick();
  assert.equal(p.calls.find(c => c.command === 'launch_game').args.settings.profile, 'ingame');
  // Kept across a change of Game Version, and with no profiles at all.
  p.next('Game Version');
  await tick();
  assert.equal(p.row('Profile').value, 'Choose in game');
  const empty = tauriPage({ responses: profileHost({ base: [], 'update-extras': [] }).responses });
  await tick();
  assert.equal(empty.row('Profile').value, 'Choose in game');
});

test('Profiles are made, renamed and deleted in the Profiles view; deleting asks first', async () => {
  const host = profileHost();
  const p = tauriPage({ saved: { res: -1, display: -1, build: 'base' }, responses: host.responses });
  const { doc, click } = p;
  await tick();
  click(p.row('Manage Profiles').row);
  const names = () => [...doc.querySelectorAll('#menu .opt-row[data-xuid]')].map(r => r.querySelector('.opt-label').textContent);
  assert.deepEqual(names(), ['Nephalem', 'Friend', '+ New Profile']);
  const type = async (value, key = 'Enter') => {
    const box = doc.querySelector('#menu input.opt-name');
    box.value = value;
    box.dispatchEvent(new doc.defaultView.KeyboardEvent('keydown', { key }));
    await tick(); await tick();
  };

  // New
  click(doc.querySelector('#menu .opt-row[data-xuid="new"]'));
  await type('  Buddy ');
  assert.deepEqual(host.folders.base.map(p => p.name), ['Nephalem', 'Friend', 'Buddy']);
  assert.deepEqual(names(), ['Nephalem', 'Friend', 'Buddy', '+ New Profile']);
  // A taken name is refused, with the reason shown.
  click(doc.querySelector('#menu .opt-row[data-xuid="new"]'));
  await type('friend');
  assert.equal(doc.querySelector('#menu .hint').textContent, 'A profile named friend already exists.');

  // Rename keeps the XUID (and so the saves).
  const friendRow = () => doc.querySelector(`#menu .opt-row[data-xuid="${FRIEND.xuid}"]`);
  click([...friendRow().querySelectorAll('.prof-act')].find(b => b.textContent === 'Rename'));
  await type('Pal');
  assert.equal(host.folders.base.find(p => p.xuid === FRIEND.xuid).name, 'Pal');

  // Delete asks; Cancel keeps it.
  const del = () => click([...friendRow().querySelectorAll('.prof-act')].find(b => b.textContent === 'Delete'));
  del();
  assert.equal(doc.getElementById('profilePrompt').hidden, false);
  assert.match(doc.getElementById('profilePromptText').textContent, /Delete Pal and all of its heroes/);
  click(doc.getElementById('profileKeep'));
  await tick();
  assert.equal(host.folders.base.length, 3);
  del();
  click(doc.getElementById('profileDelete'));
  await tick(); await tick();
  assert.deepEqual(host.folders.base.map(p => p.name), ['Nephalem', 'Buddy']);
  assert.equal(friendRow(), null);

  click(p.row('‹ Back').row);
  assert.ok(p.row('Profile'), 'back on the main screen');
});
