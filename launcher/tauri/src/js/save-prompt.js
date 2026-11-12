// @ts-check
// First launch of a Development build: it keeps its saves apart from the
// normal ones, so offer to copy the normal saves in (Rust: game/saves.rs).

/** @typedef {'copy' | 'fresh' | null} SaveAnswer */

/** @param {import('./types').Context} ctx */
export function createSavePrompt(ctx) {
  const { doc, t } = ctx;
  /** @param {string} id */
  const el = id => /** @type {HTMLElement} */ (doc.getElementById(id));
  const prompt = el('savePrompt');
  /** @type {((answer: SaveAnswer) => void) | null} */
  let answer = null;
  /** @param {SaveAnswer} value */
  const close = value => { prompt.hidden = true; const done = answer; answer = null; done?.(value); };
  el('saveCopy').addEventListener('click', () => close('copy'));
  el('saveFresh').addEventListener('click', () => close('fresh'));
  el('saveCancel').addEventListener('click', () => close(null));

  /**
   * Asks, and resolves with the choice (null: cancelled, do not launch).
   * @param {{from: string, to: string}} offer
   * @returns {Promise<SaveAnswer>}
   */
  function ask(offer) {
    el('savePromptText').textContent = t('saves.question', offer);
    prompt.hidden = false;
    /** @type {HTMLElement} */ (el('saveCopy')).focus();
    return new Promise(resolve => { answer = resolve; });
  }

  /**
   * Before launching `buildId`: ask when it is a Development build's first
   * launch and set its save folder up. False when the user cancelled.
   * @param {string | null} buildId
   */
  async function before(buildId) {
    const build = ctx.caps.builds.find(b => b.id === buildId);
    if (!build || !build.development) return true;
    const offer = await ctx.host.invoke('development_saves', { settings: ctx.settings, build: build.id });
    if (!offer) return true;
    const choice = await ask(offer);
    if (!choice) return false;
    ctx.showStatus(t(choice === 'copy' ? 'status.copyingSaves' : 'status.startingFresh'));
    await ctx.host.invoke('prepare_saves', { settings: ctx.settings, build: build.id, copy: choice === 'copy' });
    return true;
  }

  return { before };
}
