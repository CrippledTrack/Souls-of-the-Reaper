// @ts-check
// Local profiles (Rust: game/profiles.rs). A profile is an XUID with a name;
// the XUID names its save folder, so renaming never moves or hides saves. Each
// build's save folder has its own profiles. Player 1 plays the one chosen on the
// main screen; players who join pick theirs in game. The Profiles view makes,
// renames and deletes them (deleting only here, never in game).
import { makeRow } from './rows.js';

/** XAM keeps at most 15 bytes of a name. */
export const MAX_NAME_BYTES = 15;
/** The Profile row's choice to let the game ask Player 1 (SOTR_PROFILE left empty). */
export const IN_GAME = 'ingame';

/** @param {import('./types').Context} ctx */
export function createProfiles(ctx) {
  const { doc, settings, state, t } = ctx;
  /** @param {string} id */
  const el = id => /** @type {HTMLElement} */ (doc.getElementById(id));
  const menu = el('menu');
  /** @type {import('./types').Profile[]} */
  let list = [];
  /** @type {string | null | undefined} */
  let loadedFor;
  let note = '';
  /** Which row is being typed into: a profile's XUID, or 'new'. */
  let editing = '';

  /** @param {string | null} buildId */
  async function load(buildId) {
    loadedFor = buildId;
    let found = [];
    if (buildId) {
      try { found = (await ctx.host.invoke('list_profiles', { settings, build: buildId })) ?? []; }
      catch (error) { note = ctx.describe(error); }
    }
    if (loadedFor !== buildId) return;  // the build changed meanwhile
    list = found;
    // A profile this folder lacks (or none chosen yet) falls back to the first one.
    if (state.profile !== IN_GAME && !list.some(p => p.xuid === state.profile)) state.profile = list[0]?.xuid ?? IN_GAME;
    ctx.render();
  }

  /**
   * The profiles of `buildId`'s save folder, loaded when the build changes.
   * @param {string | null} buildId
   */
  function follow(buildId) { if (buildId !== loadedFor) load(buildId); }

  /**
   * Runs a profile command, then lists again (showing what went wrong, if anything).
   * @param {() => Promise<any>} action
   */
  async function change(action) {
    note = '';
    try {
      const result = await action();
      editing = '';
      return result;
    } catch (error) {
      note = ctx.describe(error);
      return null;
    } finally {
      const buildId = loadedFor ?? null;
      loadedFor = undefined;
      await load(buildId);
    }
  }

  /**
   * @param {string} value
   * @param {(name: string) => void} commit
   */
  function nameBox(value, commit) {
    const input = doc.createElement('input');
    input.className = 'opt-name'; input.type = 'text'; input.maxLength = MAX_NAME_BYTES; input.spellcheck = false;
    input.value = value; input.placeholder = t('profiles.namePlaceholder'); input.setAttribute('aria-label', t('profiles.name'));
    let done = false;
    const finish = (/** @type {boolean} */ keep) => {
      if (done) return;
      done = true;
      if (keep && input.value.trim() && input.value.trim() !== value) commit(input.value);
      else { editing = ''; ctx.render(); }
    };
    input.addEventListener('keydown', e => {
      e.stopPropagation();
      if (e.key === 'Enter') finish(true);
      else if (e.key === 'Escape') finish(false);
    });
    input.addEventListener('click', e => e.stopPropagation());
    input.addEventListener('blur', () => finish(true));
    return input;
  }

  /**
   * @param {string} text
   * @param {() => void} onClick
   */
  function action(text, onClick) {
    const button = doc.createElement('button');
    button.type = 'button'; button.className = 'prof-act'; button.textContent = text; button.tabIndex = -1;
    button.addEventListener('click', e => { e.stopPropagation(); onClick(); });
    return button;
  }

  const prompt = el('profilePrompt');
  /** @type {((yes: boolean) => void) | null} */
  let answer = null;
  /** @param {boolean} yes */
  const closePrompt = yes => { prompt.hidden = true; const done = answer; answer = null; done?.(yes); };
  el('profileDelete').addEventListener('click', () => closePrompt(true));
  el('profileKeep').addEventListener('click', () => closePrompt(false));

  /** @param {import('./types').Profile} profile */
  async function remove(profile) {
    el('profilePromptText').textContent = t('profiles.deleteQuestion', { name: profile.name });
    prompt.hidden = false;
    el('profileKeep').focus();
    const yes = await new Promise(resolve => { answer = resolve; });
    if (yes) await change(() => ctx.host.invoke('delete_profile', { settings, build: loadedFor, xuid: profile.xuid }));
  }

  function render() {
    menu.innerHTML = '';
    menu.appendChild(makeRow(doc, { label: t('remap.back'), onClick: () => { editing = ''; state.view = 'main'; ctx.render(); }, cls: 'nav back' }));
    const head = doc.createElement('div'); head.className = 'subhead';
    head.textContent = t('profiles.header');
    menu.appendChild(head);
    for (const profile of list) {
      const row = makeRow(doc, { label: profile.name, onClick: () => { editing = profile.xuid; ctx.render(); } });
      row.dataset.xuid = profile.xuid;
      if (editing === profile.xuid) {
        /** @type {HTMLElement} */ (row.querySelector('.opt-label')).textContent = '';
        row.appendChild(nameBox(profile.name, name =>
          change(() => ctx.host.invoke('rename_profile', { settings, build: loadedFor, xuid: profile.xuid, name }))));
      } else {
        const acts = doc.createElement('span'); acts.className = 'prof-acts';
        acts.append(action(t('profiles.rename'), () => { editing = profile.xuid; ctx.render(); }),
          action(t('profiles.delete'), () => remove(profile)));
        row.appendChild(acts);
      }
      menu.appendChild(row);
    }
    const add = makeRow(doc, { label: t('profiles.new'), onClick: () => { editing = 'new'; ctx.render(); }, cls: 'nav' });
    add.dataset.xuid = 'new';
    if (editing === 'new') {
      add.appendChild(nameBox('', async name => {
        const made = await change(() => ctx.host.invoke('create_profile', { settings, build: loadedFor, name }));
        if (made) state.profile = made.xuid;
      }));
    }
    menu.appendChild(add);
    const hint = doc.createElement('div'); hint.className = 'hint';
    hint.textContent = note || t('profiles.hint');
    menu.appendChild(hint);
    const box = /** @type {HTMLInputElement | null} */ (menu.querySelector('input.opt-name'));
    if (box) box.focus({ preventScroll: true });
    else /** @type {HTMLElement | null} */ (menu.querySelector('.opt-row'))?.focus({ preventScroll: true });
  }

  return {
    follow,
    render,
    /** The profiles of the shown build's save folder. */
    list: () => list,
  };
}
