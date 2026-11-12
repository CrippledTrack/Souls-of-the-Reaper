// @ts-check
// Build panel: checks the build tools, asks before downloading the sources or
// the SDK, runs scripts/build_client.py (start_build) into the game folders and
// refreshes Game Version when it finishes.
import { uiState } from './state.js';

/**
 * @param {import('./types').Context} ctx
 * @param {{showBuilds(builds: import('./types').Build[]): void, hideSettings(): void}} settingsPanel
 */
export function createBuildPanel(ctx, settingsPanel) {
  const { doc, caps, settings, state, host, rules, t, showStatus } = ctx;
  const describe = ctx.describe;
  /** @param {string} id */
  const el = id => /** @type {HTMLInputElement} */ (doc.getElementById(id));
  const buildPanel = el('buildPanel');
  const buildInputs = ['buildIso', 'buildTitleUpdate', 'buildCmake'];
  const optUpdate = el('optUpdate');
  const optExtras = el('optExtras');
  const buildStep = el('buildStep');
  const buildLog = el('buildLog');
  const startButton = el('startBuild');
  /** @type {string[]} */
  const logLines = [];
  /** @type {string[]} */
  const buildWarnings = [];
  // What the panel runs, from the click that starts it until it ends: 'build'
  // (its tool check and download prompt included) or 'install'. Set before the
  // first await so a second click is ignored; the host turns one away too.
  /** @type {'build' | 'install' | null} */
  let job = null;
  /** @param {string} text */
  const setStep = text => { buildStep.textContent = text; };

  // build_windows.py finds CMake, Ninja and Clang (Visual Studio, LLVM) by itself.
  if (caps.platform === 'windows') el('cmakeLabel').hidden = true;
  el('openBuild').hidden = !caps.canBuild;

  // The title update is an overlay on the base game files (always extracted, never
  // modified), so the panel asks for what to compile, not for four peer builds: the
  // base game, plus the update when it is ticked (both executables, so Base is there
  // too), and the optional PC features apply to all of it. The result is the
  // build_client.py variant ids.
  const selectedVariants = () => {
    const plain = optExtras.checked ? '' : '-plain';
    return optUpdate.checked ? [`base${plain}`, `update${plain}`] : [`base${plain}`];
  };
  // Version (standalone launchers only): the stable release, or Development, the
  // launcher's own version (when its sources can be fetched). A source checkout
  // builds itself.
  const offersDevelopment = () => !!caps.standalone && caps.canBuildDevelopment === true;
  el('versionRow').hidden = !offersDevelopment();
  let channel = 'stable';
  const channelButtons = /** @type {HTMLElement[]} */ ([...doc.querySelectorAll('[data-channel]')]);
  function showChannel() {
    channelButtons.forEach(b => {
      const on = b.dataset.channel === channel;
      b.classList.toggle('on', on); b.setAttribute('aria-checked', String(on));
    });
  }
  channelButtons.forEach(b => b.addEventListener('click', () => {
    channel = b.dataset.channel === 'development' && offersDevelopment() ? 'development' : 'stable';
    showChannel(); updateSummary();
    // Each version has its own sources and SDK, so the tool list follows the choice.
    checkTools();
  }));
  function updateSummary() {
    const variants = selectedVariants();
    const update = variants.some(v => v.startsWith('update')), plain = variants[0].endsWith('-plain');
    el('buildSummary').textContent = [
      t(update ? (plain ? 'build.summary.updatePlain' : 'build.summary.update') : (plain ? 'build.summary.basePlain' : 'build.summary.base')),
      optUpdate.checked ? t('build.summary.overlay') : '',
      // A launcher outside a checkout fetches what it builds: say which version.
      !caps.standalone ? ''
        : channel === 'development' ? t('build.summary.development', { commit: caps.version?.label ?? '' })
          : caps.stable ? t('build.summary.stable', { tag: caps.stable.tag }) : '',
    ].filter(Boolean).join(' ');
  }
  [optUpdate, optExtras].forEach(box => box.addEventListener('change', updateSummary));
  function titleUpdateAdded() { optUpdate.checked = true; updateSummary(); }
  el('buildTitleUpdate').addEventListener('change', event => {
    if (/** @type {HTMLInputElement} */ (event.target).value.trim()) titleUpdateAdded();
  });

  // Build tools: the host looks for Python, Git, a compiler, CMake and Ninja before
  // anything runs and the panel links to the download of each one that is missing.
  const toolList = el('toolList');
  const sdkPrompt = el('sdkPrompt');
  // The last report, shown again in a new language.
  /** @type {import('./types').ToolReport | null | undefined} */
  let shownReport;
  /** @param {import('./types').ToolReport | null} report */
  function renderTools(report) {
    shownReport = report;
    toolList.textContent = '';
    if (!report) { toolList.textContent = t('build.tools.unavailable'); showWinget(null); return; }
    /**
     * @param {boolean} ok
     * @param {string} text
     * @param {string} [note]
     * @param {string} [url]
     */
    const add = (ok, text, note, url) => {
      const item = doc.createElement('li');
      item.className = ok ? 'ok' : 'miss';
      item.append(`${ok ? '✓' : '✗'} ${text}`);
      if (!ok && url) {
        const link = doc.createElement('a');
        link.href = url; link.textContent = ' ' + t('build.tools.getIt');
        item.append(link);
      }
      if (!ok && note) { const small = doc.createElement('small'); small.textContent = note; item.append(small); }
      toolList.append(item);
    };
    report.tools.forEach(tool => add(tool.found, tool.label, describe(tool.note), tool.url));
    // Warnings do not block Build; they explain failures that may come later.
    (report.warnings ?? []).forEach(warning => {
      const item = doc.createElement('li');
      item.className = 'warn'; item.textContent = `! ${describe(warning)}`;
      toolList.append(item);
    });
    const sdk = report.sdk;
    if (report.sources) add(false, t('build.tools.sources', { commit: report.sources.commit.slice(0, 8) }), t('build.tools.sources.note'));
    add(sdk.installed, sdk.commit ? t('build.tools.sdk.pinned', { commit: sdk.commit.slice(0, 8) }) : t('build.tools.sdk'),
      t(sdk.stale ? 'build.tools.sdk.stale' : 'build.tools.sdk.missing'));
    // The Shadow renderer's shader converter, built once like the SDK (sources from
    // before it was part of every build do not report one).
    const converter = report.converter;
    if (converter) add(converter.installed, converter.commit ? t('build.tools.converter.pinned', { commit: converter.commit.slice(0, 8) }) : t('build.tools.converter'),
      t(converter.stale ? 'build.tools.converter.stale' : 'build.tools.converter.missing'));
    showWinget(report);
  }

  // Windows with winget: one button installs every missing tool. Its note says
  // what pressing it agrees to, since winget accepts the license agreements.
  const wingetRow = el('wingetRow');
  /** @param {import('./types').ToolReport | null} report */
  function showWinget(report) {
    const missing = report && report.winget ? report.tools.filter(tool => !tool.found && tool.winget) : [];
    // A check that finishes while winget runs must not offer a second install.
    wingetRow.hidden = job === 'install' || !missing.length;
    if (wingetRow.hidden) return;
    const vs = missing.some(tool => tool.id === 'msvc');
    el('wingetNote').textContent = t('build.winget.note', { tools: missing.map(tool => tool.label).join(', ') }) +
      (vs ? ' ' + t('build.winget.visualStudio') : '');
  }
  el('installTools').addEventListener('click', async () => {
    if (job) return;
    job = 'install';
    logLines.length = 0; buildLog.textContent = '';
    setStep(t('build.step.installing'));
    setBuilding(true);
    // winget and the installers it starts are not stopped halfway.
    el('cancelBuild').hidden = true;
    wingetRow.hidden = true;
    let report = null;
    try { report = await host.invoke('install_tools', { cmake: el('buildCmake').value.trim() }) || null; }
    catch (error) { setStep(describe(error)); }
    finally { job = null; setBuilding(false); }
    if (!report) { checkTools(); return; }
    renderTools(report);
    setStep(t(report.ready ? 'build.step.installed' : 'build.step.stillMissing'));
  });
  async function checkTools() {
    let report = null;
    try { report = await host.invoke('check_build_tools', { cmake: el('buildCmake').value.trim(), channel }) || null; }
    catch (error) { setStep(describe(error)); }
    renderTools(report);
    return report;
  }

  // Answers the open download prompt, if any. Closing the panel answers no, so
  // the request it was asked for is dropped and a later Build starts afresh.
  /** @type {((answer: boolean) => void) | null} */
  let answerPrompt = null;
  /**
   * Resolves true when the user agrees to download what is missing: the game
   * sources (a launcher outside a checkout), the ReXGlue SDK and/or the shader
   * converter (XenosRecomp).
   * @param {import('./types').ToolReport} report
   * @returns {Promise<boolean>}
   */
  function askDownloads(report) {
    const { sdk, sources, converter } = report;
    const parts = [];
    if (sources) parts.push(t('build.download.sources', { repository: sources.repository, commit: sources.commit.slice(0, 8) }));
    if (sdk.stale) parts.push(t('build.download.sdkRebuild', { commit: (sdk.commit ?? '').slice(0, 8) }));
    else if (!sdk.installed) {
      const repository = sdk.repository || t('build.download.pinned');
      const source = sdk.commit ? t('build.download.atCommit', { source: repository, commit: sdk.commit.slice(0, 8) }) : repository;
      parts.push(t('build.download.sdk', { source }));
    }
    if (converter && !converter.installed) {
      parts.push(t(converter.stale ? 'build.download.converterRebuild' : 'build.download.converter',
        { commit: (converter.commit ?? '').slice(0, 8) }));
    }
    el('sdkPromptText').textContent =
      t(parts.length > 1 ? 'build.download.questionMany' : 'build.download.questionOne', { needs: parts.join(t('build.download.and')) });
    sdkPrompt.hidden = false;
    return new Promise(resolve => {
      answerPrompt = answer => { answerPrompt = null; sdkPrompt.hidden = true; resolve(answer); };
      el('sdkYes').onclick = () => answerPrompt?.(true);
      el('sdkNo').onclick = () => answerPrompt?.(false);
    });
  }
  el('recheckTools').addEventListener('click', checkTools);
  el('buildCmake').addEventListener('change', checkTools);

  function openBuild() {
    buildInputs.forEach(key => { el(key).value = /** @type {string} */ (settings[key] ?? ''); });
    settingsPanel.hideSettings(); buildPanel.hidden = false;
    showChannel();
    updateSummary();
    checkTools();
  }
  /** @param {boolean} on */
  function setBuilding(on) {
    startButton.hidden = on;
    el('recheckTools').disabled = on;
    // The running build's log sits right after the tool list and takes its place.
    el('toolsLabel').hidden = on;
    el('cancelBuild').hidden = !on;
    el('closeBuild').disabled = on;
    el('setupBtn').disabled = on;
    el('playBtn').disabled = on;
    buildPanel.querySelectorAll('input, [data-pick]').forEach(input => { /** @type {HTMLInputElement} */ (input).disabled = on; });
  }
  /** @param {string} line */
  function log(line) {
    logLines.push(line);
    if (logLines.length > 400) logLines.shift();
    buildLog.textContent = logLines.join('\n');
    buildLog.scrollTop = buildLog.scrollHeight;
  }
  host.listen('build-output', ({ payload }) => {
    const step = /^::step::(\d+)\/(\d+)::(.*)$/.exec(payload);
    // Step names come from the build script, in English.
    if (step) setStep(t('build.step.progress', { step: step[1], steps: step[2], name: step[3] }));
    // The game is built, but a step the script treats as optional was not.
    else if (payload.startsWith('::warning::')) { buildWarnings.push(payload.slice(11)); log(t('build.log.warning', { warning: payload.slice(11) })); }
    else if (payload !== '::done::') log(payload);
  });
  el('openBuild').addEventListener('click', openBuild);
  el('closeBuild').addEventListener('click', () => {
    buildPanel.hidden = true;
    answerPrompt?.(false);
  });
  buildPanel.querySelectorAll('[data-pick]').forEach(node => {
    const button = /** @type {HTMLElement} */ (node);
    button.addEventListener('click', async () => {
      const kind = button.dataset.pick ?? '';
      try {
        const path = await host.invoke('pick_file', { kind });
        if (!path) return;
        /** @type {HTMLInputElement} */ (button.previousElementSibling).value = path;
        // A title update means an update build is wanted, in place of the base one.
        if (kind === 'titleUpdate') titleUpdateAdded();
      } catch (error) { setStep(describe(error)); }
    });
  });
  el('cancelBuild').addEventListener('click', () => {
    setStep(t('build.step.cancelling'));
    host.invoke('cancel_build').catch(error => { setStep(describe(error)); });
  });
  startButton.addEventListener('click', async () => {
    if (job) return;
    job = 'build'; startButton.disabled = true;
    try { await runBuild(); }
    finally { job = null; startButton.disabled = false; }
  });
  /**
   * The build to select after building: of the versions just compiled (this version's
   * builds only), the update over the base game and + Extras over the plain one.
   * @param {import('./types').Build[]} builds
   * @param {import('./types').BuildRequest} request
   */
  function pickBuilt(builds, request) {
    const development = request.channel === 'development';
    const update = request.variants.some(v => v.startsWith('update'));
    const extras = request.variants.some(v => !v.endsWith('-plain'));
    const score = (/** @type {import('./types').Build} */ b) => (b.update ? 2 : 0) + (b.extras ? 1 : 0);
    return builds
      .filter(b => !!b.development === development && (!b.update || update) && (!b.extras || extras))
      .sort((a, b) => score(b) - score(a))[0];
  }

  async function runBuild() {
    buildInputs.forEach(key => { settings[key] = el(key).value.trim(); });
    settings.uiState = uiState(state, rules);
    /** @type {import('./types').BuildRequest} */
    const request = {
      iso: settings.buildIso ?? '', titleUpdate: settings.buildTitleUpdate ?? '', cmake: settings.buildCmake ?? '',
      variants: selectedVariants(), downloadSdk: false, downloadSources: false,
      channel,
    };
    logLines.length = 0; buildLog.textContent = ''; buildWarnings.length = 0;
    setStep(t('build.step.checkingTools'));
    // An unavailable check (null) is left to the host, which checks again before it runs anything.
    const report = await checkTools();
    if (report && !report.ready) {
      setStep(t('build.step.installMissing'));
      return;
    }
    const converterMissing = !!(report && report.converter && !report.converter.installed);
    if (report && (report.sources || !report.sdk.installed || converterMissing)) {
      const agreed = await askDownloads(report);
      if (!agreed) { setStep(t('build.step.declined')); return; }
      // One approval covers the SDK and the shader converter (--no-sdk-download refuses both).
      request.downloadSdk = !report.sdk.installed || converterMissing;
      request.downloadSources = Boolean(report.sources);
    }
    setStep(t('build.step.checkingInputs'));
    setBuilding(true);
    try {
      const builds = await host.invoke('start_build', { settings, request });
      // Select what was just built, so Play runs it.
      const pick = pickBuilt(builds, request);
      if (pick) state.build = pick.id;
      settingsPanel.showBuilds(builds);
      // Back to the main screen; failures and cancellations stay in the panel.
      buildPanel.hidden = true;
      const built = pick ? pick.label : '';
      showStatus(buildWarnings.length ? t('status.built.warning', { warning: buildWarnings.join(' ') })
        : !pick ? t('status.built')
          : t(request.channel === 'development' ? 'status.built.development' : 'status.built.selected', { build: built }));
    } catch (error) {
      setStep(describe(error));
    } finally { setBuilding(false); }
  }

  // Same signal as the launcher's update autoselect (caps.updateInstalled): once the
  // update is there, the Build panel opens with it ticked, so a rebuild keeps it.
  // A saved package path counts too. Applied once, so a deliberate untick later is
  // left alone.
  if (caps.updateInstalled || (settings.buildTitleUpdate ?? '').trim()) titleUpdateAdded();

  // Text this panel sets itself, again in a new language.
  ctx.onLanguage(() => {
    el('buildIntro').textContent = t(caps.platform === 'windows' ? 'build.intro.windows'
      : caps.platform === 'macos' ? 'build.intro.macos' : 'build.intro');
    channelButtons.forEach(b => {
      b.textContent = b.dataset.channel === 'development'
        ? t('build.version.development', { commit: caps.version?.label ?? '' })
        : t('build.version.stable', { tag: caps.stable?.tag ?? '' });
    });
    updateSummary();
    if (shownReport !== undefined) renderTools(shownReport);
  });

  return { openBuild, setStep };
}
