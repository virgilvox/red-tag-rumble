#!/usr/bin/env node
// wavstat: the band statistics of a WAV, per slot of N seconds (deep-audio 6.3), no dependencies.
//
//   node tools/wavstat.mjs <file.wav> [slot seconds, default 1] [from seconds] [to seconds]
//   FROM_FRAME=668 node tools/wavstat.mjs build/out-2p.wav 1     # the gates measured from a 60 Hz frame on
//
// Reads 16 bit PCM or 32 bit float, mono or stereo (folded to mono). Per slot: the sample peak
// and RMS in dBFS, the energy shares under 120 Hz, 120 to 500, 500 to 2k, 2k to 6k and over 6k,
// the spectral centroid and flatness (Welch: the mean of 4096 point Hann power spectra, half
// overlapped). Then the cart gates of deep-audio 6.3 over the slots from FROM_FRAME (or the from
// argument) on: no slot with over 85 percent of its energy under 120 Hz, at least one slot per
// 5 s with over 12 percent over 2 kHz, the peak never over -0.8 dBFS. Exit 1 when a gate fails.
import { readFileSync } from 'node:fs';

const [file, slotArg, fromArg, toArg] = process.argv.slice(2);
if (!file) { console.error('usage: node tools/wavstat.mjs <file.wav> [slot s] [from s] [to s]'); process.exit(2); }
const buf = readFileSync(file);

// -- the WAV --------------------------------------------------------
function readWav(b) {
  if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE') throw new Error('not a RIFF WAVE file');
  let p = 12, fmt = null, data = null;
  while (p + 8 <= b.length) {
    const id = b.toString('ascii', p, p + 4), len = b.readUInt32LE(p + 4);
    if (id === 'fmt ') fmt = { format: b.readUInt16LE(p + 8), channels: b.readUInt16LE(p + 10), rate: b.readUInt32LE(p + 12), bits: b.readUInt16LE(p + 22) };
    if (id === 'data') data = { at: p + 8, len: Math.min(len, b.length - p - 8) };
    p += 8 + len + (len & 1);
  }
  if (!fmt || !data) throw new Error('no fmt or data chunk');
  const bytes = fmt.bits / 8, frames = Math.floor(data.len / (bytes * fmt.channels));
  const mono = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    let s = 0;
    for (let c = 0; c < fmt.channels; c++) {
      const o = data.at + (i * fmt.channels + c) * bytes;
      s += fmt.format === 3 && fmt.bits === 32 ? b.readFloatLE(o) : fmt.bits === 16 ? b.readInt16LE(o) / 32768 : fmt.bits === 24 ? (b.readIntLE(o, 3) / 8388608) : b.readInt32LE(o) / 2147483648;
    }
    mono[i] = s / fmt.channels;
  }
  return { rate: fmt.rate, mono };
}

// -- an in place radix 2 FFT -----------------------------------------
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const a = i + k, b2 = i + k + len / 2;
        const xr = re[b2] * cr - im[b2] * ci, xi = re[b2] * ci + im[b2] * cr;
        re[b2] = re[a] - xr; im[b2] = im[a] - xi; re[a] += xr; im[a] += xi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

const N = 4096, EDGES = [120, 500, 2000, 6000];
const hann = Float32Array.from({ length: N }, (_, i) => 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (N - 1)));
function welch(x, rate) {
  const psd = new Float64Array(N / 2); let frames = 0;
  const re = new Float64Array(N), im = new Float64Array(N);
  for (let s = 0; s + N <= x.length || (frames === 0 && s < x.length); s += N / 2) {
    for (let i = 0; i < N; i++) { re[i] = (s + i < x.length ? x[s + i] : 0) * hann[i]; im[i] = 0; }
    fft(re, im);
    for (let k = 1; k < N / 2; k++) psd[k] += re[k] * re[k] + im[k] * im[k];
    frames++;
    if (s + N >= x.length) break;
  }
  let sum = 0, wsum = 0, lsum = 0, nb = 0, am = 0;
  const bands = [0, 0, 0, 0, 0];
  for (let k = 1; k < N / 2; k++) {
    const f = k * rate / N, p = psd[k] / frames;
    sum += p; wsum += p * f;
    bands[f < EDGES[0] ? 0 : f < EDGES[1] ? 1 : f < EDGES[2] ? 2 : f < EDGES[3] ? 3 : 4] += p;
    if (f >= 20 && f <= 20000) { lsum += Math.log(p + 1e-24); am += p; nb++; }
  }
  if (sum <= 0) return { centroid: 0, flat: 0, share: [0, 0, 0, 0, 0] };
  return { centroid: wsum / sum, flat: Math.exp(lsum / nb) / (am / nb + 1e-30), share: bands.map((v) => 100 * v / sum) };
}

const db = (v) => (v > 1e-9 ? 20 * Math.log10(v) : -120);
const { rate, mono } = readWav(buf);
const slot = Number(slotArg || 1);
const from = Math.max(0, Number(fromArg || 0)), to = Math.min(mono.length / rate, Number(toArg || mono.length / rate));
const gateFrom = process.env.FROM_FRAME ? Number(process.env.FROM_FRAME) / 60 : from;
console.log(`${file}: ${(mono.length / rate).toFixed(1)} s at ${rate} Hz; slots of ${slot} s from ${from} s`);
console.log('    t s   peak    rms    <120  120-500 500-2k 2k-6k  >6k   centroid  flatness');
const rows = [];
for (let t = from; t + slot <= to + 1e-6; t += slot) {
  const a = Math.floor(t * rate), b = Math.floor((t + slot) * rate), x = mono.subarray(a, b);
  let pk = 0, acc = 0;
  for (const v of x) { const m = Math.abs(v); if (m > pk) pk = m; acc += v * v; }
  const s = welch(x, rate);
  const row = { t, peak: db(pk), rms: db(Math.sqrt(acc / Math.max(1, x.length))), ...s };
  rows.push(row);
  console.log(`  ${t.toFixed(1).padStart(6)} ${row.peak.toFixed(1).padStart(6)} ${row.rms.toFixed(1).padStart(6)}   ${s.share.map((v) => v.toFixed(0).padStart(5)).join('  ')}   ${s.centroid.toFixed(0).padStart(7)}   ${s.flat.toFixed(3)}`);
}
// the gates of deep-audio 6.3 over the slots from the fight on
const g = rows.filter((r) => r.t >= gateFrom - 1e-6 && r.rms > -60);
let fail = 0;
const low = g.filter((r) => r.share[0] > 85);
console.log(`gate: seconds over 85 percent under 120 Hz from ${gateFrom.toFixed(1)} s: ${low.length}${low.length ? ' (' + low.map((r) => r.t.toFixed(0)).join(' ') + ')' : ''}`);
if (low.length) fail++;
let worst = 0;
for (let t = gateFrom; t + 5 <= to + 1e-6; t += 5) {
  const w = g.filter((r) => r.t >= t - 1e-6 && r.t < t + 5 - 1e-6);
  const bright = w.filter((r) => r.share[3] + r.share[4] > 12).length;
  if (w.length && !bright) { console.log(`gate: no slot over 12 percent above 2 kHz in ${t.toFixed(0)} to ${(t + 5).toFixed(0)} s`); fail++; }
  worst = Math.max(worst, bright);
}
const peak = Math.max(...rows.map((r) => r.peak));
console.log(`gate: peak ${peak.toFixed(2)} dBFS (never over -0.8)`);
if (peak > -0.8) fail++;
console.log(fail ? `${fail} gate${fail > 1 ? 's' : ''} FAIL` : 'gates PASS');
process.exit(fail ? 1 : 0);
