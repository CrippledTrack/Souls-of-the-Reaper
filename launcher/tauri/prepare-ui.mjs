// Builds dist/index.html from the shared launcher page (also used by the
// Windows Launcher.ps1) plus the Tauri-only settings panel and host bridge.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const source = fileURLToPath(new URL('../v6_borderless_final.html', import.meta.url));
let html = readFileSync(source, 'utf8');
const bridge = readFileSync(new URL('./bridge.js', import.meta.url), 'utf8');

function inject(marker, replacement) {
  if (!html.includes(marker)) throw new Error(`Launcher HTML no longer contains: ${marker}`);
  html = html.replace(marker, replacement);
}

const panelStyle = `<style>
  .winctl{display:flex;gap:2px}
  .wico-gear{width:auto;height:auto;font:12px/1 var(--data)}
  #launchStatus{position:absolute;left:8%;right:8%;bottom:1.6%;z-index:6;min-height:12px;
    font:10px/1.3 var(--data);color:var(--bone);text-align:center;text-shadow:0 1px 3px #000;pointer-events:none}
  #setup{position:absolute;inset:44px 18px 18px;z-index:8;overflow-y:auto;padding:14px 16px;
    background:rgba(8,6,5,.97);border:1px solid rgba(122,106,76,.35);box-shadow:0 6px 18px rgba(0,0,0,.55);
    font:11px/1.4 var(--data);color:var(--bone)}
  #setup[hidden]{display:none}
  #setup h2{margin:0 0 8px;font:400 13px var(--disp);letter-spacing:.08em;text-transform:uppercase;color:#e2d7bd}
  #setup label{display:block;margin:9px 0;color:var(--bone-dim)}
  #setup label[hidden]{display:none}
  #setup input{display:block;width:100%;margin-top:4px;padding:6px 7px;background:rgba(39,33,28,.8);
    border:1px solid rgba(122,106,76,.45);color:#e2d7bd;font:11px var(--data)}
  #setup input::placeholder{color:var(--muted)}
  #setup pre{margin:4px 0 0;white-space:pre-wrap;word-break:break-all;color:var(--muted);font:10px/1.4 var(--data)}
  #setup .acts{display:flex;gap:8px;justify-content:flex-end;margin-top:12px}
  #setup button{padding:6px 14px;background:rgba(46,30,24,.9);color:#e9dcbf;border:1px solid rgba(158,43,28,.55);
    cursor:pointer;font:11px var(--data)}
  #setup button:hover{border-color:var(--crimson-hi)}
</style>`;

inject('<div class="stage">', `${panelStyle}
<div class="stage">
  <div id="launchStatus" role="status" aria-live="polite"></div>
  <section id="setup" hidden aria-label="Launch settings">
    <h2>Launch settings</h2>
    <label>Game folder<input id="gameDir" type="text" spellcheck="false"></label>
    <label>TU2 game folder<input id="gameDirTu2" type="text" spellcheck="false"></label>
    <label>Save folder<input id="userDataRoot" type="text" spellcheck="false"></label>
    <label>TU2 save folder<input id="userDataRootTu2" type="text" spellcheck="false"></label>
    <label id="deviceLabel">Vulkan device (blank = automatic)<input id="vulkanDevice" type="number" min="0" max="32"></label>
    <label>Builds found<pre id="buildList"></pre></label>
    <p>Empty fields use the folder shown. TU2 needs a staged TU2 game folder and keeps separate saves. Use the in-game F4 menu for key bindings.</p>
    <div class="acts"><button id="cancelSetup" type="button">Cancel</button><button id="saveSetup" type="button">Save</button></div>
  </section>`);
inject('<button id="winClose"', '<button id="setupBtn" class="wbtn" type="button" aria-label="Launch settings" title="Launch settings"><span class="wico wico-gear">⚙</span></button>\n    <button id="winClose"');
inject('</script>', `</script>\n<script>\n${bridge}</script>`);

html = '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"></head><body>' + html + '</body></html>';
mkdirSync(new URL('./dist', import.meta.url), { recursive: true });
writeFileSync(new URL('./dist/index.html', import.meta.url), html);
