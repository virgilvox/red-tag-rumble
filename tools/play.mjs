// Plays the cart in a real, visible Chrome with the keyboard only, over the DevTools protocol: the feel gate
// of docs/design/DEEP-plan.md section 10.
//
//   node tools/serve.mjs 8797 &
//   node tools/play.mjs 'http://127.0.0.1:8797/?v=1' build/play
//
// Launches its own Chrome (the Mac application path below) with a throwaway profile, so the save is fresh, and
// clicks Clock in. Then three fights against the EASY practice bot, all by keyboard (arrows, Z, X, A, S, Q, Enter):
//   1. YEETING BOX (LAUNCH, seat 1's first preset): the attack coach, a flip with air and an inversion, a KO
//   2. SPINNY PANCAKE (CHOP, one S tap in the garage): hold Z to spin up, hit the practice bot's side
//   3. POLITE DOORSTOP (ROLL OUT, five more S taps): drive at the practice bot's front, Z digs in, the underride
// The driver steers by the cart's read only rtr_probe export (the phase and each bot's pose), which a script
// injected before the page loads reaches by keeping the wasm instance. Writes PNGs at every menu step, a
// screencast JPEG every 3rd rendered frame through each fight (out/fight<n>/<seq>-f<sim frame>.jpg),
// probe.jsonl (one line per driver tick) and log.txt with the cart's console lines ([phase], [music], [hits],
// [perf]) interleaved with the key presses. Never use a background tab for this: a background tab suspends
// the cart. The window stays open until the run ends; the script kills its Chrome at the end.
import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, appendFileSync } from 'node:fs';
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
setTimeout(() => { log.push('DEADLINE'); finish(3); }, 900000);
let list;
for (let k = 0; k < 30; k++) { await sleep(500); try { list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(); if (list.find(t => t.type === 'page')) break; } catch {} }
const pg = list.find(t => t.type === 'page');
const ws = new WebSocket(pg.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pend = new Map();
let cast = null, castSeq = 0, lastFrame = 0;
const send = (method, params = {}, to = 20000) => new Promise((r) => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); setTimeout(() => { if (pend.has(i)) { pend.delete(i); r({ timeout: method }); } }, to); });
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && pend.has(d.id)) { pend.get(d.id)(d); pend.delete(d.id); }
  else if (d.method === 'Runtime.consoleAPICalled') log.push(d.params.args.map(a => a.value ?? a.description).join(' ').slice(0, 200));
  else if (d.method === 'Runtime.exceptionThrown') log.push('EXC ' + JSON.stringify(d.params.exceptionDetails).slice(0, 300));
  else if (d.method === 'Page.screencastFrame') {
    send('Page.screencastFrameAck', { sessionId: d.params.sessionId });
    if (cast) writeFileSync(`${cast}/${String(castSeq++).padStart(5, '0')}-f${lastFrame}.jpg`, Buffer.from(d.params.data, 'base64'));
  } };
await send('Runtime.enable'); await send('Page.enable');
/* keep the cart's instance and memory where Runtime.evaluate can reach them */
await send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => { const inst = WebAssembly.instantiate;
  WebAssembly.instantiate = async function (m, imp) { const r = await inst.call(WebAssembly, m, imp); const i = r.instance || r;
    window.__rtr = i; if (imp) for (const ns of Object.values(imp)) for (const v of Object.values(ns || {})) if (v instanceof WebAssembly.Memory) window.__rtrMem = v;
    return r; }; })();` });
await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
await send('Page.navigate', { url }, 30000);
const evalJs = async (expr) => { const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true }, 5000); return r.result && r.result.result ? r.result.result.value : undefined; };
let b; for (let k = 0; k < 20; k++) { await sleep(1000); b = await evalJs('JSON.stringify(document.getElementById("start").getBoundingClientRect())'); if (b) break; }
b = JSON.parse(b);
for (const type of ['mousePressed', 'mouseReleased']) await send('Input.dispatchMouseEvent', { type, x: b.x + b.width / 2, y: b.y + b.height / 2, button: 'left', clickCount: 1 });
for (let k = 0; k < 40; k++) { await sleep(1000); const s = await evalJs('document.getElementById("status").textContent'); if (s && s.startsWith('Running')) break; }
const t0 = Date.now();
const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }, 15000); if (s.result) writeFileSync(`${out}/${name}.png`, Buffer.from(s.result.data, 'base64')); log.push(`[shot] ${name} at ${Date.now() - t0}ms`); };
const VK = { Enter: 13, ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, KeyZ: 90, KeyX: 88, KeyA: 65, KeyS: 83, KeyQ: 81, KeyW: 87 };
const keyEv = (type, code) => send('Input.dispatchKeyEvent', { type, code, key: code.startsWith('Key') ? code[3].toLowerCase() : code, windowsVirtualKeyCode: VK[code] }, 5000);
const held = new Set();
const down = async (code) => { if (!held.has(code)) { held.add(code); await keyEv('keyDown', code); } };
const up = async (code) => { if (held.has(code)) { held.delete(code); await keyEv('keyUp', code); } };
const releaseAll = async () => { for (const k of [...held]) await up(k); };
const tap = async (code, n = 1) => { for (let i = 0; i < n; i++) { await keyEv('keyDown', code); await sleep(80); await keyEv('keyUp', code); await sleep(220); } log.push(`[key] tap ${code} x${n} at ${Date.now() - t0}ms`); };
const lastPhase = () => { for (let i = log.length - 1; i >= 0; i--) { const m = log[i].match(/\[phase\] (\d+) at frame (\d+)/); if (m) return Number(m[1]); } return -1; };
const waitPhase = async (n, ms) => { const t1 = Date.now(); const before = log.length; if (lastPhase() === n) { log.push(`[ok] phase ${n} already`); return true; } while (Date.now() - t1 < ms) { for (let i = before; i < log.length; i++) if (log[i].includes(`[phase] ${n} at`)) { log.push(`[ok] phase ${n} after ${Date.now() - t1}ms`); return true; } await sleep(100); } log.push(`[FAIL] phase ${n} not reached in ${ms}ms (last ${lastPhase()})`); return false; };
/* rtr_probe: [phase, frame, nbots, sim_on, paused, attract, 0, 0] then per slot x z h y hp hp_max alive inverted airborne ride_by spin cooldown weapon vx vz worst_panel */
const probe = async () => {
  const a = await evalJs(`(() => { const i = window.__rtr; if (!i || !i.exports.rtr_probe) return null; const mem = i.exports.memory || window.__rtrMem;
    return Array.from(new Float32Array(mem.buffer, i.exports.rtr_probe(), 88)); })()`);
  if (!a) return null;
  const bot = (s) => { const p = a.slice(8 + s * 16, 24 + s * 16); return { x: p[0], z: p[1], h: p[2], y: p[3], hp: p[4], hpMax: p[5], alive: p[6], inv: p[7], air: p[8], rideBy: p[9], spin: p[10], cd: p[11], wp: p[12], vx: p[13], vz: p[14], worst: p[15] }; };
  return { phase: a[0], frame: a[1], nbots: a[2], simOn: a[3], paused: a[4], me: bot(0), them: bot(1) };
};
const wrapPi = (a) => { while (a > Math.PI) a -= 2 * Math.PI; while (a < -Math.PI) a += 2 * Math.PI; return a; };

/* one fight: mode 'flip' closes and taps Z inside 1.0 u, 'spin' holds Z and comes at a side, 'wedge' comes at the
 * front and digs in with Z inside 2 u. Heading 0 is +z, forward is (sin h, cos h), Right lowers the heading. */
async function fight(n, mode) {
  const dir = `${out}/fight${n}`; mkdirSync(dir, { recursive: true });
  await send('Page.startScreencast', { format: 'jpeg', quality: 80, maxWidth: 1280, maxHeight: 800, everyNthFrame: 3 });
  cast = dir; castSeq = 0;
  const tF = Date.now(); let stuckT = 0, backT = 0, fireT = 0, lastHp = -1, events = 0, contactT = 0;
  while (Date.now() - tF < 90000) {
    const p = await probe();
    if (!p) { await sleep(50); continue; }
    lastFrame = p.frame;
    appendFileSync(`${out}/probe.jsonl`, JSON.stringify({ n, t: Date.now() - t0, ...p }) + '\n');
    if (p.phase !== 9) { if (p.phase >= 10) break; await sleep(50); continue; }
    if (p.paused) { await tap('Enter'); continue; }
    const me = p.me, th = p.them;
    if (lastHp >= 0 && th.hp < lastHp - 0.5) { events++; log.push(`[drive] fight ${n} hit ${(lastHp - th.hp).toFixed(1)} at f=${p.frame} them ${th.hp.toFixed(0)} inv=${th.inv} air=${th.air} worst=${th.worst}`); }
    lastHp = th.hp;
    const fwd = [Math.sin(th.h), Math.cos(th.h)], side = [Math.cos(th.h), -Math.sin(th.h)];
    let tx = th.x, tz = th.z;
    const dx0 = th.x - me.x, dz0 = th.z - me.z, dist = Math.hypot(dx0, dz0);
    if (mode === 'spin' && dist > 2.6) { const s = (me.x - th.x) * side[0] + (me.z - th.z) * side[1] >= 0 ? 1 : -1; tx += side[0] * s * 1.6; tz += side[1] * s * 1.6; }
    if (mode === 'wedge' && dist > 1.6) { tx += fwd[0] * 1.4; tz += fwd[1] * 1.4; }
    const dx = tx - me.x, dz = tz - me.z;
    const err = wrapPi(Math.atan2(dx, dz) - me.h);
    const keys = new Set();
    if (me.inv && ((p.frame / 30) | 0) % 3 === 0) { keys.add(((p.frame / 15) | 0) % 2 ? 'KeyZ' : 'KeyA'); }   /* try to right, then keep driving (treads and wheels drive upside down) */
    else if (backT > 0) { backT--; keys.add('ArrowDown'); if (mode !== 'wedge') keys.add(err > 0 ? 'ArrowRight' : 'ArrowLeft'); }
    else {
      if (err > 0.18) keys.add('ArrowLeft'); else if (err < -0.18) keys.add('ArrowRight');
      if (Math.abs(err) < 1.1 || dist > 3) keys.add('ArrowUp');
      const speed = Math.hypot(me.vx, me.vz);
      if (keys.has('ArrowUp') && speed < 0.3) { if (++stuckT > 25) { backT = 12; stuckT = 0; } } else stuckT = 0;
      if (mode === 'flip' && dist < 1.3 && Math.abs(err) < 0.45 && fireT <= 0) { keys.add('KeyZ'); fireT = 6; }
      if (mode === 'spin') keys.add('KeyZ');
      if (mode === 'wedge' && dist < 2.0 && Math.abs(err) < 0.4 && fireT <= 0) { keys.add('KeyZ'); fireT = 30; }
      if ((mode === 'wedge' || mode === 'flip') && dist > 3 && dist < 6 && Math.abs(err) < 0.2) keys.add('KeyQ');
    }
    if (fireT > 0) fireT--;
    /* a wedge pressed on a side with no ride backs off and comes again: closing speed is what rams */
    if (mode === 'wedge' && dist < 1.3 && !th.rideBy) { if (++contactT > 45) { backT = 24; contactT = 0; } } else contactT = 0;
    for (const k of [...held]) if (!keys.has(k)) await up(k);
    for (const k of keys) await down(k);
    await sleep(25);
  }
  await releaseAll();
  await send('Page.stopScreencast'); cast = null;
  log.push(`[drive] fight ${n} (${mode}) ended after ${Date.now() - tF}ms, ${events} hits seen, phase ${lastPhase()}`);
}

async function vote_and_fight(n, mode) {
  await tap('Enter'); const ok7 = await waitPhase(7, 5000); if (!ok7 && lastPhase() !== 7) { log.push('[retry] Enter garage'); await tap('Enter'); await waitPhase(7, 5000); }
  await sleep(800); await shot(`${n}-06-vote`); await tap('KeyZ');
  await waitPhase(8, 6000); await sleep(400); await shot(`${n}-07-countdown`);
  await waitPhase(9, 8000);
  await fight(n, mode);
  await waitPhase(12, 30000); await sleep(2000); await shot(`${n}-21-results`);
  await tap('KeyZ'); await waitPhase(13, 10000); await sleep(1200); await shot(`${n}-22-rematch`);
  await tap('KeyZ'); await waitPhase(6, 10000); await sleep(1500);
}

await sleep(5000); await shot('01-title');
await tap('KeyZ');                                      /* A on the title: FIGHT */
await sleep(800);
if (lastPhase() === 4) { await shot('02-rules'); await tap('Enter'); }
const ok5 = await waitPhase(5, 5000); if (!ok5 && lastPhase() !== 5) { await tap('Enter'); await waitPhase(5, 5000); }
await sleep(1500); await shot('03-lobby');
await tap('Enter'); const ok6 = await waitPhase(6, 5000); if (!ok6 && lastPhase() !== 6) { log.push('[retry] Enter lobby'); await tap('Enter'); await waitPhase(6, 6000); }
await sleep(400); await shot('1-04-garage-card'); await sleep(3200); await shot('1-05-garage');
await vote_and_fight(1, 'flip');
await shot('2-05-garage-before'); await tap('KeyS'); await sleep(800); await shot('2-05-garage');
await vote_and_fight(2, 'spin');
/* CHOP to ROLL OUT: STOMP, TOAST, SPIN, RANDOM (a 36 frame slot machine that eats input), ROLL OUT */
await tap('KeyS', 4); await sleep(1200); await tap('KeyS'); await sleep(800); await shot('3-05-garage');
await vote_and_fight(3, 'wedge');
await shot('99-end');
finish(0);
