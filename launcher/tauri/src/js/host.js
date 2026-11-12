// @ts-check
// The Tauri host: commands (Rust: commands/*) and events.

/**
 * @param {Window} win
 * @returns {import('./types').Host}
 */
export function createHost(win) {
  return {
    invoke: (command, args) => win.__TAURI__.core.invoke(command, args),
    listen: (event, handler) => win.__TAURI__.event.listen(event, handler),
  };
}
