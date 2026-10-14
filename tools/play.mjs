// Plays the cart in a real, visible Chrome with the keyboard only, over the DevTools protocol.
//
//   node tools/serve.mjs 8794 &
//   node tools/play.mjs 'http://127.0.0.1:8794/?v=1' build/play
//
// Launches its own Chrome (the Mac application path below) with a throwaway profile, clicks Clock in,
// then drives the keyboard path: title, the rules flash on a fresh save, lobby, garage (two row edits,
// Enter), vote (Right, Z), fight (Up, Right, Left, Down, Z, Q), pause (Enter, Z), the rest of the fight,
// results, the rematch vote. Writes a PNG at every step and log.txt with the cart's console lines
// ([phase], [music], [hits], [perf]) interleaved with the key presses. Never use a background tab for
// this: a background tab suspends the cart.
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
const [url, out] = process.argv.slice(2);
mkdirSync(out, { recursive: true });
const port = 9377;
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', [
  `--remote-debugging-port=${port}`, `--user-data-dir=${out}/profile`, '--no-first-run', '--no-default-browser-check',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
  '--window-size=1440,900', '--autoplay-policy=no-user-gesture-required', 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const log = [];
const finish = (code) => { writeFileSync(`${out}/log.txt`, log.join('\n')); chrome.kill(); process.exit(code); };
setTimeout(() => { log.push('DEADLINE'); finish(3); }, 420000);
let list;
for (let k = 0; k < 30; k++) { await sleep(500); try { list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(); if (list.find(t => t.type === 'page')) break; } catch {} }
const pg = list.find(t => t.type === 'page');
const ws = new WebSocket(pg.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); }
  else if (d.method === 'Runtime.consoleAPICalled') log.push(d.params.args.map(a => a.value ?? a.description).join(' ').slice(0, 200));
  else if (d.method === 'Runtime.exceptionThrown') log.push('EXC ' + JSON.stringify(d.params.exceptionDetails).slice(0, 300)); };
const send = (method, params = {}, to = 20000) => new Promise((r) => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); setTimeout(() => { if (pend.has(i)) { pend.delete(i); r({ timeout: method }); } }, to); });
await send('Runtime.enable'); await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
await send('Page.navigate', { url }, 30000);
const evalJs = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true }, 5000); return r.result && r.result.result ? r.result.result.value : undefined; };
let b; for (let k = 0; k < 20; k++) { await sleep(1000); b = await evalJs('JSON.stringify(document.getElementById("start").getBoundingClientRect())'); if (b) break; }
b = JSON.parse(b);
for (const type of ['mousePressed', 'mouseReleased']) await send('Input.dispatchMouseEvent', { type, x: b.x + b.width / 2, y: b.y + b.height / 2, button: 'left', clickCount: 1 });
for (let k = 0; k < 40; k++) { await sleep(1000); const s = await evalJs('document.getElementById("status").textContent'); if (s && s.startsWith('Running')) break; }
const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }, 15000); if (s.result) writeFileSync(`${out}/${name}.png`, Buffer.from(s.result.data, 'base64')); log.push(`[shot] ${name} at ${Date.now() - t0}ms`); };
const VK = { Enter: 13, ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, KeyZ: 90, KeyX: 88, KeyA: 65, KeyS: 83, KeyQ: 81, KeyW: 87 };
const down = (code) => send('Input.dispatchKeyEvent', { type: 'keyDown', code, key: code.startsWith('Key') ? code[3].toLowerCase() : code, windowsVirtualKeyCode: VK[code] }, 5000);
const up = (code) => send('Input.dispatchKeyEvent', { type: 'keyUp', code, key: code.startsWith('Key') ? code[3].toLowerCase() : code, windowsVirtualKeyCode: VK[code] }, 5000);
const tap = async (code, n = 1) => { for (let i = 0; i < n; i++) { await down(code); await sleep(80); await up(code); await sleep(220); } log.push(`[key] tap ${code} x${n} at ${Date.now() - t0}ms`); };
const hold = async (code, ms) => { await down(code); await sleep(ms); await up(code); log.push(`[key] hold ${code} ${ms}ms at ${Date.now() - t0}ms`); };
const phaseSeen = (n) => log.some(l => l.includes(`[phase] ${n} at`));
const lastPhase = () => { for (let i = log.length - 1; i >= 0; i--) { const m = log[i].match(/\[phase\] (\d+) at frame (\d+)/); if (m) return Number(m[1]); } return -1; };
const waitPhase = async (n, ms) => { const t1 = Date.now(); const before = log.length; while (Date.now() - t1 < ms) { for (let i = before; i < log.length; i++) if (log[i].includes(`[phase] ${n} at`)) { log.push(`[ok] phase ${n} after ${Date.now() - t1}ms`); return true; } await sleep(100); } log.push(`[FAIL] phase ${n} not reached in ${ms}ms (last ${lastPhase()})`); return false; };
const t0 = Date.now();
await sleep(5000); await shot('01-title');
await tap('KeyZ');                                      /* A on the title: FIGHT */
/* a fresh save plays the rules flash first (phase 4); Enter skips it; otherwise the lobby (5) */
await sleep(800);
if (lastPhase() === 4) { await shot('02-rules'); await tap('Enter'); }
const ok5 = await waitPhase(5, 5000); if (!ok5 && lastPhase() !== 5) { await tap('Enter'); await waitPhase(5, 5000); }
await sleep(1500); await shot('03-lobby');
await tap('Enter'); const ok6 = await waitPhase(6, 5000); if (!ok6 && lastPhase() !== 6) { log.push('[retry] Enter lobby'); await tap('Enter'); await waitPhase(6, 6000); }
await sleep(1200); await shot('04-garage');
await tap('ArrowDown'); await tap('ArrowRight'); await sleep(300); await tap('ArrowDown'); await tap('ArrowRight'); await sleep(600); await shot('05-garage-edited');
await tap('Enter'); const ok7 = await waitPhase(7, 4000); if (!ok7 && lastPhase() !== 7) { log.push('[retry] Enter garage'); await tap('Enter'); await waitPhase(7, 4000); }
await sleep(1000); await shot('06-vote'); await tap('ArrowRight'); await sleep(500); await shot('06b-vote-moved'); await tap('KeyZ');
await waitPhase(8, 4000); await sleep(500); await shot('07-countdown');
await waitPhase(9, 8000); await sleep(300); await shot('08-fight-0');
await down('ArrowUp'); await sleep(1200); await shot('09-up'); await up('ArrowUp');
await sleep(300); await shot('10-before-right');
await down('ArrowRight'); await sleep(500); await shot('11-right-a'); await sleep(500); await shot('12-right-b'); await up('ArrowRight');
await sleep(300); await down('ArrowLeft'); await sleep(600); await shot('13-left'); await up('ArrowLeft');
await down('ArrowDown'); await sleep(800); await shot('14-down'); await up('ArrowDown');
await down('ArrowUp'); await down('KeyZ'); await sleep(700); await shot('15-weapon'); await up('KeyZ');
await down('KeyQ'); await sleep(500); await shot('16-boost'); await up('KeyQ'); await up('ArrowUp');
await sleep(500); await tap('Enter'); await sleep(900); await shot('17-pause');
await tap('KeyZ'); await sleep(600); await shot('18-resumed');
/* drive at the practice bot for the rest of the fight, a bit of everything, until the KO or the clock */
const tFight = Date.now();
while (Date.now() - tFight < 80000 && !phaseSeen(10) && !phaseSeen(11) && !phaseSeen(12)) {
  await hold('ArrowUp', 900); if (phaseSeen(10) || phaseSeen(12)) break;
  await hold('ArrowRight', 300); await hold('KeyZ', 200); await hold('ArrowUp', 600); await hold('ArrowLeft', 300);
  if (log.some(l => l.includes('INVERTED') )) {}
  await tap('KeyX');                                     /* B rights the bot if it was flipped */
  if ((Date.now() - tFight) % 10000 < 2600) await shot(`19-fight-${Math.round((Date.now() - tFight) / 1000)}s`);
}
await shot('20-fight-end');
await waitPhase(12, 15000); await sleep(2500); await shot('21-results');
await tap('KeyZ'); await waitPhase(13, 8000); await sleep(1500); await shot('22-rematch');
await tap('KeyZ'); await sleep(2500); await shot('23-after-vote');
finish(0);
