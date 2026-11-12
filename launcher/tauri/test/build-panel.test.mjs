// Build panel: tools, downloads, variants and running a build.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tick, BUILDS, DEVELOPMENT_BUILD, TOOLS_OK, tauriPage } from './harness.mjs';

test('The shader converter is listed like the SDK and downloaded under the same approval', async () => {
  const missing = { ...TOOLS_OK, converter: { installed: false, commit: '990d03b28a27b50277ee5d8d942e1c5f873869d1' } };
  const { doc, click, calls } = tauriPage({
    settings: { buildIso: '/isos/d3.iso' },
    responses: { check_build_tools: () => missing, start_build: () => new Promise(() => {}) },
  });
  click(doc.getElementById('setupBtn'));
  click(doc.getElementById('openBuild'));
  await tick();
  assert.match(doc.getElementById('toolList').textContent, /✗ Shader converter \(XenosRecomp, pinned 990d03b2\)/);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(doc.getElementById('sdkPrompt').hidden, false);
  assert.match(doc.getElementById('sdkPromptText').textContent, /shader converter \(XenosRecomp, commit 990d03b2\)/);
  click(doc.getElementById('sdkYes'));
  await tick(); await tick();
  const { request } = calls.find(c => c.command === 'start_build').args;
  assert.equal(request.downloadSdk, true, 'one approval for both');
  assert.equal(request.channel, 'stable');
  // Older sources (a stable release from before) do not report a converter.
  const old = tauriPage({ settings: { buildIso: '/isos/d3.iso' }, responses: { check_build_tools: () => ({ ...TOOLS_OK, converter: null }) } });
  old.click(old.doc.getElementById('setupBtn'));
  old.click(old.doc.getElementById('openBuild'));
  await tick();
  assert.doesNotMatch(old.doc.getElementById('toolList').textContent, /Shader converter/);
});

test('A standalone launcher builds stable or Development, and selects a finished Development build', async () => {
  let finish;
  const caps = { standalone: true, canBuildDevelopment: true, stable: { tag: 'beta-1.4', commit: '095fb2c67e78b803f2c55d241bd238afb86fc72d' }, version: { commit: 'b'.repeat(40), dirty: false, label: 'bbbbbbbb' } };
  const { doc, click, calls } = tauriPage({ caps, settings: { buildIso: '/isos/d3.iso', buildTitleUpdate: '/isos/tu' },
    responses: { check_build_tools: () => TOOLS_OK, start_build: () => new Promise(resolve => { finish = resolve; }) } });
  click(doc.getElementById('setupBtn'));
  click(doc.getElementById('openBuild'));
  await tick();
  assert.equal(doc.getElementById('versionRow').hidden, false);
  const [stable, development] = doc.querySelectorAll('[data-channel]');
  assert.equal(stable.textContent, 'Stable (beta-1.4)');
  assert.equal(development.textContent, 'Development (bbbbbbbb)');
  assert.ok(stable.classList.contains('on'), 'stable by default');
  click(development);
  await tick();
  assert.equal(calls.filter(c => c.command === 'check_build_tools').at(-1).args.channel, 'development', 'its own sources and SDK');
  assert.match(doc.getElementById('buildSummary').textContent, /From Development: this launcher’s own version \(bbbbbbbb\)\.$/);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(calls.find(c => c.command === 'start_build').args.request.channel, 'development');
  finish([BUILDS[0], DEVELOPMENT_BUILD]);
  await tick(); await tick();
  assert.match(doc.getElementById('menu').textContent, /‹Update \+ Extras \(Development\)›/, 'the new build is selected');
  assert.equal(doc.getElementById('launchStatus').textContent, 'Development build complete. Update + Extras (Development) is selected under Game Version.');
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
      pick_file: ({ kind }) => (kind === 'titleUpdate' ? '/isos/title-update' : null),
      start_build: () => new Promise(resolve => { finish = resolve; }),
    },
  });
  doc.getElementById('buildIso').value = ' /isos/d3.iso ';
  click(doc.querySelector('[data-pick="titleUpdate"]'));
  await tick();
  assert.equal(doc.getElementById('buildTitleUpdate').value, '/isos/title-update');
  assert.equal(doc.getElementById('optUpdate').checked, true, 'choosing a title update selects it');
  assert.match(doc.getElementById('buildSummary').textContent, /^Will compile: Base and Update\./);
  click(doc.getElementById('startBuild'));
  await tick();
  const { request, settings } = calls.find(c => c.command === 'start_build').args;
  assert.deepEqual(JSON.parse(JSON.stringify(request)), { iso: '/isos/d3.iso', titleUpdate: '/isos/title-update', cmake: '', variants: ['base', 'update'], downloadSdk: false, downloadSources: false, channel: 'stable' });
  assert.equal(settings.buildIso, '/isos/d3.iso');
  assert.equal(doc.getElementById('playBtn').disabled, true);
  assert.equal(doc.getElementById('cancelBuild').hidden, false);
  assert.equal(doc.getElementById('toolsLabel').hidden, true, 'the log replaces the tool list while building');
  emit('build-output', '::step::2/5::Extract title update');
  emit('build-output', 'CPKs/Patch.cpk: 3,774,503 bytes');
  assert.equal(doc.getElementById('buildStep').textContent, 'Step 2 of 5: Extract title update…');
  assert.equal(doc.getElementById('buildLog').textContent, 'CPKs/Patch.cpk: 3,774,503 bytes');
  finish(BUILDS);
  await tick(); await tick();
  assert.equal(doc.getElementById('playBtn').disabled, false);
  assert.equal(doc.getElementById('cancelBuild').hidden, true);
  assert.equal(doc.getElementById('toolsLabel').hidden, false);
  assert.equal(rows()[0].label, 'Game Version');
  assert.equal(row('Game Version').value, 'Update + Extras', 'the update, with the extras, is selected');
  assert.match(doc.getElementById('buildList').textContent, /Update \+ Extras/);
  assert.equal(doc.getElementById('buildPanel').hidden, true, 'a finished build returns to the main screen');
  assert.equal(doc.getElementById('launchStatus').textContent, 'Build complete. Update + Extras is selected under Game Version.');
});

test('A double click on Build starts one build and the panel stays busy until it ends', async () => {
  let finish;
  const { doc, click, calls } = tauriPage({
    builds: [],
    responses: {
      // The host turns a second build away; that error must not reset the panel.
      start_build: () => (finish ? Promise.reject('A build is already running.') : new Promise(resolve => { finish = resolve; })),
    },
  });
  click(doc.getElementById('startBuild'));
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(calls.filter(c => c.command === 'start_build').length, 1);
  assert.equal(doc.getElementById('cancelBuild').hidden, false);
  assert.equal(doc.getElementById('playBtn').disabled, true);
  assert.equal(doc.getElementById('closeBuild').disabled, true);
  finish(BUILDS);
  await tick(); await tick();
  assert.equal(doc.getElementById('playBtn').disabled, false);
  assert.equal(doc.getElementById('cancelBuild').hidden, true);
  assert.equal(doc.getElementById('startBuild').disabled, false);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(calls.filter(c => c.command === 'start_build').length, 2, 'Build works again afterwards');
});

const tick1 = (doc, el, on) => { el.checked = on; el.dispatchEvent(new doc.defaultView.Event('change')); };

test('The Build panel asks what to build and summarises it', () => {
  const { doc } = tauriPage({ builds: [] });
  const summary = () => doc.getElementById('buildSummary').textContent;
  assert.equal(doc.getElementById('optUpdate').checked, false);
  assert.equal(doc.getElementById('optBase'), null, 'Base is not a separate choice');
  assert.match(summary(), /^Will compile: Base\.$/);
  tick1(doc, doc.getElementById('optUpdate'), true);
  assert.match(summary(), /^Will compile: Base and Update\. The update runs on top of the base game/);
  tick1(doc, doc.getElementById('optExtras'), false);
  assert.match(summary(), /^Will compile: Base and Update \(no extras\)\./);
});

test('Choosing a title update package selects the update build', () => {
  const { doc } = tauriPage({ builds: [] });
  const title = doc.getElementById('buildTitleUpdate');
  title.value = '/isos/title-update';
  title.dispatchEvent(new doc.defaultView.Event('change'));
  assert.equal(doc.getElementById('optUpdate').checked, true);
});

test('Build requests carry the matching variants, with Base along with the update', async () => {
  const { doc, click, calls } = tauriPage({ builds: [], responses: { start_build: async () => [] } });
  tick1(doc, doc.getElementById('optUpdate'), true);
  tick1(doc, doc.getElementById('optExtras'), false);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.deepEqual(JSON.parse(JSON.stringify(calls.find(c => c.command === 'start_build').args.request.variants)), ['base-plain', 'update-plain']);
});

test('update already installed opens the Build panel on the update', () => {
  const { doc } = tauriPage({ builds: [], settings: { updateInstalled: true } });
  assert.equal(doc.getElementById('optUpdate').checked, true);
});

test('A saved title update package opens the Build panel on the update', () => {
  const { doc } = tauriPage({ builds: [], settings: { buildIso: '/i.iso', buildTitleUpdate: '/isos/title-update' } });
  assert.equal(doc.getElementById('optUpdate').checked, true);
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

const TOOL = (id, found) => ({ id, label: id.toUpperCase(), found, path: '', url: found ? '' : `https://example.test/${id}`, note: '' });

const SDK = installed => ({ installed, repository: 'https://example.test/sdk.git', commit: 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518' });

test('Missing build tools are listed with download links and block the build', async () => {
  const report = { ready: false, tools: [TOOL('python', false), TOOL('git', true)], sdk: SDK(true) };
  const { doc, click, calls } = tauriPage({ builds: [], responses: { check_build_tools: () => report } });
  await tick();
  const items = [...doc.querySelectorAll('#toolList li')];
  assert.equal(items.length, 3);
  assert.equal(items[0].className, 'miss');
  assert.equal(items[0].querySelector('a').href, 'https://example.test/python');
  assert.equal(items[1].className, 'ok');
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(calls.some(c => c.command === 'start_build'), false);
  assert.match(doc.getElementById('buildStep').textContent, /missing tools/);
});

test('Windows offers to install missing tools with winget and says it accepts their licenses', async () => {
  const missing = { ...TOOL('msvc', false), label: 'Visual Studio 2022 C++ tools', winget: 'Microsoft.VisualStudio.2022.BuildTools' };
  const report = { ready: false, winget: true, tools: [missing, { ...TOOL('git', true), winget: 'Git.Git' }], sdk: SDK(true) };
  const after = { ...report, ready: true, tools: [{ ...missing, found: true }, report.tools[1]] };
  const { doc, click, calls } = tauriPage({ platform: 'windows', builds: [],
    responses: { check_build_tools: () => report, install_tools: async () => after } });
  await tick();
  assert.equal(doc.getElementById('wingetRow').hidden, false);
  const note = doc.getElementById('wingetNote').textContent;
  assert.match(note, /Visual Studio 2022 C\+\+ tools/);
  assert.match(note, /accepts their license agreements/);
  assert.doesNotMatch(note, /\bGIT\b/);
  click(doc.getElementById('installTools'));
  await tick(); await tick();
  assert.equal(calls.filter(c => c.command === 'install_tools').length, 1);
  assert.equal(doc.getElementById('wingetRow').hidden, true);
  assert.match(doc.getElementById('buildStep').textContent, /installed/);
  assert.equal(doc.querySelector('#toolList li').className, 'ok');
});

test('Checking tools while winget runs does not offer a second install', async () => {
  const missing = { ...TOOL('git', false), winget: 'Git.Git' };
  const report = { ready: false, winget: true, tools: [missing], sdk: SDK(true) };
  let finish;
  const { doc, click, calls } = tauriPage({ platform: 'windows', builds: [],
    responses: { check_build_tools: () => report, install_tools: () => new Promise(resolve => { finish = resolve; }) } });
  await tick();
  assert.equal(doc.getElementById('wingetRow').hidden, false);
  click(doc.getElementById('installTools'));
  await tick();
  assert.equal(doc.getElementById('recheckTools').disabled, true);
  click(doc.getElementById('recheckTools'));
  // A check started before the install, or by the CMake field, can still finish meanwhile.
  doc.getElementById('buildCmake').dispatchEvent(new doc.defaultView.Event('change'));
  await tick(); await tick();
  assert.equal(doc.getElementById('wingetRow').hidden, true);
  click(doc.getElementById('installTools'));
  await tick();
  assert.equal(calls.filter(c => c.command === 'install_tools').length, 1);
  finish({ ...report, ready: true, tools: [{ ...missing, found: true }] });
  await tick(); await tick();
  assert.equal(doc.getElementById('recheckTools').disabled, false);
  assert.match(doc.getElementById('buildStep').textContent, /installed/);
});

test('The winget button stays hidden without winget or missing tools', async () => {
  for (const report of [
    { ready: false, winget: false, tools: [{ ...TOOL('git', false), winget: 'Git.Git' }], sdk: SDK(true) },
    { ready: true, winget: true, tools: [{ ...TOOL('git', true), winget: 'Git.Git' }], sdk: SDK(true) },
  ]) {
    const { doc } = tauriPage({ platform: 'windows', builds: [], responses: { check_build_tools: () => report } });
    await tick();
    assert.equal(doc.getElementById('wingetRow').hidden, true);
  }
});

test('A missing SDK is only downloaded after the user agrees', async () => {
  const report = { ready: true, tools: [TOOL('git', true)], sdk: SDK(false) };
  const { doc, click, calls } = tauriPage({ builds: [], responses: { check_build_tools: () => report, start_build: async () => [] } });
  await tick();
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(doc.getElementById('sdkPrompt').hidden, false);
  assert.match(doc.getElementById('sdkPromptText').textContent, /c94f5ebd/);
  click(doc.getElementById('sdkNo'));
  await tick(); await tick();
  assert.equal(calls.some(c => c.command === 'start_build'), false);
  assert.match(doc.getElementById('buildStep').textContent, /Nothing was downloaded/);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  click(doc.getElementById('sdkYes'));
  await tick(); await tick();
  assert.equal(calls.find(c => c.command === 'start_build').args.request.downloadSdk, true);
});

test('Closing the Build panel at the download prompt drops that request', async () => {
  const report = { ready: true, tools: [TOOL('git', true)], sdk: SDK(false) };
  const { doc, click, calls } = tauriPage({ builds: [], responses: { check_build_tools: () => report, start_build: async () => [] } });
  await tick();
  doc.getElementById('buildIso').value = '/isos/old.iso';
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(doc.getElementById('sdkPrompt').hidden, false);
  click(doc.getElementById('closeBuild'));
  await tick();
  assert.equal(doc.getElementById('buildPanel').hidden, true);
  assert.equal(doc.getElementById('sdkPrompt').hidden, true, 'the prompt closes with the panel');
  click(doc.getElementById('openBuild'));
  await tick();
  assert.equal(doc.getElementById('sdkPrompt').hidden, true);
  assert.equal(doc.getElementById('startBuild').disabled, false);
  doc.getElementById('buildIso').value = '/isos/new.iso';
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  click(doc.getElementById('sdkYes'));
  await tick(); await tick();
  const builds = calls.filter(c => c.command === 'start_build');
  assert.equal(builds.length, 1);
  assert.equal(builds[0].args.request.iso, '/isos/new.iso');
});

test('Missing game sources are listed and downloaded only after the user agrees', async () => {
  const sources = { repository: 'https://github.com/o/r', commit: 'a'.repeat(40) };
  const report = { ready: true, tools: [TOOL('git', true)], sdk: SDK(true), sources };
  const { doc, click, calls } = tauriPage({ builds: [], responses: { check_build_tools: () => report, start_build: async () => [] } });
  await tick();
  assert.match(doc.getElementById('toolList').textContent, /Game sources \(aaaaaaaa\)/);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.match(doc.getElementById('sdkPromptText').textContent, /game sources/);
  assert.doesNotMatch(doc.getElementById('sdkPromptText').textContent, /ReXGlue SDK/);
  click(doc.getElementById('sdkYes'));
  await tick(); await tick();
  const { request } = calls.find(c => c.command === 'start_build').args;
  assert.equal(request.downloadSources, true);
  assert.equal(request.downloadSdk, false);
});

test('A stale SDK is shown as needing a rebuild and asks before it is rebuilt', async () => {
  const report = { ready: true, tools: [TOOL('git', true)], sdk: { ...SDK(false), stale: true } };
  const { doc, click, calls } = tauriPage({ builds: [], responses: { check_build_tools: () => report, start_build: async () => [] } });
  await tick();
  assert.match(doc.getElementById('toolList').textContent, /another version or patch set/);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.match(doc.getElementById('sdkPromptText').textContent, /rebuild of the ReXGlue SDK/);
  click(doc.getElementById('sdkYes'));
  await tick(); await tick();
  assert.equal(calls.find(c => c.command === 'start_build').args.request.downloadSdk, true);
});

test('Warnings are listed but do not block the build', async () => {
  const report = { ready: true, tools: [TOOL('git', true)], sdk: SDK(true), warnings: ['No Vulkan driver was found.'] };
  const { doc, click, calls } = tauriPage({ builds: [], responses: { check_build_tools: () => report, start_build: async () => [] } });
  await tick();
  const warn = doc.querySelector('#toolList li.warn');
  assert.match(warn.textContent, /No Vulkan driver/);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(calls.some(c => c.command === 'start_build'), true);
});

test('A standalone launcher says which version a build compiles', () => {
  const caps = { standalone: true, canBuildDevelopment: true, stable: { tag: 'beta-1.4', commit: '095fb2c67e78b803f2c55d241bd238afb86fc72d' }, version: { commit: 'b'.repeat(40), dirty: false, label: 'bbbbbbbb' } };
  const standalone = tauriPage({ builds: [], caps, responses: { check_build_tools: () => TOOLS_OK } });
  assert.match(standalone.doc.getElementById('buildSummary').textContent, /From the stable release beta-1\.4\.$/);
  const checkout = tauriPage({ builds: [], caps: { ...caps, standalone: false }, responses: { check_build_tools: () => TOOLS_OK } });
  assert.doesNotMatch(checkout.doc.getElementById('buildSummary').textContent, /stable release|own version/);
  assert.equal(checkout.doc.getElementById('versionRow').hidden, true, 'a checkout builds itself');
});

test('A standalone launcher that cannot fetch its own sources builds stable only', async () => {
  const caps = { standalone: true, canBuildDevelopment: false, stable: { tag: 'beta-1.4', commit: '095fb2c67e78b803f2c55d241bd238afb86fc72d' }, version: { commit: 'b'.repeat(40), dirty: true, label: 'bbbbbbbb (modified)' } };
  const { doc, click, calls } = tauriPage({ builds: [], caps, responses: { check_build_tools: () => TOOLS_OK } });
  assert.equal(doc.getElementById('versionRow').hidden, true);
  assert.match(doc.getElementById('buildSummary').textContent, /From the stable release beta-1\.4\.$/);
  click(doc.getElementById('startBuild'));
  await tick(); await tick();
  assert.equal(calls.find(c => c.command === 'start_build').args.request.channel, 'stable');
});

test('After a build the best of what was built is selected: update over base, extras over plain', async () => {
  const run = async (update, extras) => {
    const p = tauriPage({ builds: [], settings: { buildIso: '/isos/d3.iso' },
      responses: { start_build: async ({ request }) => {
        const all = [
          { id: 'base', label: 'Base', update: false, extras: false, executable: '/b' },
          { id: 'extras', label: 'Base + Extras', update: false, extras: true, executable: '/b' },
          { id: 'update', label: 'Update', update: true, extras: false, executable: '/u' },
          { id: 'update-extras', label: 'Update + Extras', update: true, extras: true, executable: '/u' },
        ];
        // An older plain build may still be around; only what was asked for is chosen from.
        return all.filter(b => (!b.update || request.variants.some(v => v.startsWith('update'))));
      } } });
    tick1(p.doc, p.doc.getElementById('optUpdate'), update);
    tick1(p.doc, p.doc.getElementById('optExtras'), extras);
    p.click(p.doc.getElementById('startBuild'));
    for (let i = 0; i < 4; i++) await tick();
    return p.row('Game Version').value;
  };
  assert.equal(await run(true, true), 'Update + Extras');
  assert.equal(await run(true, false), 'Update', 'built without the extras: the plain update');
  assert.equal(await run(false, true), 'Base + Extras');
  assert.equal(await run(false, false), 'Base');
});
