// @ts-check
// Game languages offered by the language button. The fixed list does not
// depend on which CPKs are installed: a language with no matching CPK falls
// back cleanly to English in-game, by the engine's own design.
// state.lang indexes into this list; `ui` is the launcher locale that language
// shows (locales/<ui>.js), so the launcher follows the game's language.
// Keep in sync by hand with installer/SoulsOfTheReaper.iss
// (GetLangIdCountryForActiveLanguage) if a language is added or removed.

/** @typedef {{code: string, name: string, langId: number, countryId: number, ui: string}} Language */

/** @type {readonly Language[]} */
export const LANGS = [
  { code: 'EN', name: 'English', langId: 1, countryId: 103, ui: 'en' },
  { code: 'DE', name: 'Deutsch', langId: 3, countryId: 24, ui: 'de' },
  { code: 'ES', name: 'Español', langId: 5, countryId: 31, ui: 'es' },
  { code: 'IT', name: 'Italiano', langId: 6, countryId: 50, ui: 'it' },
  { code: 'PT', name: 'Português', langId: 9, countryId: 84, ui: 'pt' },
  { code: 'RU', name: 'Русский', langId: 12, countryId: 88, ui: 'ru' },
];
