// Reuse the existing launcher artwork and controls without duplicating its HTML.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const source = fileURLToPath(new URL('../v6_borderless_final.html', import.meta.url));
let html = readFileSync(source, 'utf8');
const bridge = readFileSync(new URL('./bridge.js', import.meta.url), 'utf8');
html = html.replace('<script>', '<script>\n(async () => {\n' + bridge + '\n');
html = html.replace('</script>', '\n})().catch(error => { document.getElementById("launchStatus").textContent = String(error); window.__TAURI__.core.invoke("frontend_report", {message: String(error)}); });\n</script>');
// Rendering-path selection only applies to D3D12. Linux uses the validated FSI path.
html = html.replace('rows.forEach(d=>{', "rows.filter(d => !(boot.platform === 'linux' && d.key === 'path') && d.key !== 'remap').forEach(d=>{");
html = html.replace('  render();', '  render();\n  await invoke("frontend_report", { message: "ready" });');
// Capture the existing control state when Play is clicked.
html = html.replace("type:'play',config:buildLaunchConfig()", "type:'play',config:buildLaunchConfig(),uiState:state");
html = html.replace('<div class="stage">', `
<style>
#setupToggle{position:fixed;right:18px;top:52px;z-index:20;background:#211a17;color:#edd6b2;border:1px solid #786047;padding:8px 14px;cursor:pointer}
#launchStatus{position:fixed;bottom:8px;left:16px;right:16px;z-index:20;color:#eed7b5;font:13px sans-serif;text-align:center}
#setup{position:fixed;inset:70px 35px auto;max-height:80vh;overflow:auto;z-index:30;background:#171310;border:1px solid #8f7250;color:#edd6b2;padding:22px;font:15px sans-serif;box-shadow:0 0 0 100vmax #000a}
#setup label{display:block;margin:14px 0}#setup input[type=text],#setup input[type=number]{display:block;width:100%;box-sizing:border-box;margin-top:6px;padding:9px;background:#27211c;border:1px solid #80674e;color:#fff}
#setup button{padding:10px 18px;background:#382719;color:#fff;border:1px solid #98744d;cursor:pointer}#setup[hidden]{display:none}
</style>
<button id="setupToggle">Launch settings</button>
<section id="setup" hidden aria-label="Launch settings">
<h2>Launch settings</h2>
<label>Game directory<input id="gameDir" type="text"></label>
<label>Game executable<input id="executable" type="text"></label>
<label>Save directory<input id="userDataRoot" type="text"></label>
<label><input id="tu2" type="checkbox"> Title Update 2</label>
<label><input id="extras" type="checkbox"> Extra features build</label>
<label id="deviceLabel">Vulkan device<input id="vulkanDevice" type="number" min="0" max="32" value="1"></label>
<p>Choose the executable matching your title update and extra features selection. Launch settings override the saved render scale. Use the in-game F4 menu for key bindings.</p>
<button id="saveSetup">Save and close</button>
</section>
<div id="launchStatus" role="status" aria-live="polite"></div>
<div class="stage">`);
html = '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"></head><body>' + html + '</body></html>';
mkdirSync(new URL('./dist', import.meta.url), { recursive: true });
writeFileSync(new URL('./dist/index.html', import.meta.url), html);
