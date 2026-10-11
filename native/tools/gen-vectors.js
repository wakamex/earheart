// Writes the expected results the native build's ported logic is tested against, by running the
// JavaScript it was ported from on generated inputs. Run it again whenever that JavaScript changes:
//   node native/tools/gen-vectors.js
//
// Audio inputs are built from a seed with the Mulberry32 generator, which gesso reproduces bit for
// bit, so the vector files hold only the recipe and the results.
const fs = require("node:fs");
const path = require("node:path");
const split = require("../../main/util/split-silence");
const { quietestOffset } = require("../../renderer/chunk-boundary");
const { containsSpeech } = require("../../renderer/speech-probe");

function mulberry32(a) {
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// A recording as a list of [seconds, amplitude] stretches of white noise: loud ones stand in for
// speech and clicks, quiet ones for pauses and room tone. Dictation-shaped: sentences of varied
// length, hesitations inside them, short clicks in the pauses, sometimes a long unbroken run.
function recipe(seed) {
  const rand = mulberry32(seed);
  const pick = (lo, hi) => Math.round((lo + rand() * (hi - lo)) * 1000) / 1000;
  const parts = [];
  const sentences = 3 + Math.floor(rand() * 10);
  const room = rand() < 0.3 ? pick(0.004, 0.009) : pick(0, 0.002);
  if (rand() < 0.5) parts.push([pick(0.05, 1.5), room]);
  for (let s = 0; s < sentences; s++) {
    const kind = rand();
    if (kind < 0.12) parts.push([pick(21, 34), pick(0.05, 0.3)]); // talks past the cap without a pause
    else if (kind < 0.3) parts.push([pick(0.2, 1.8), pick(0.02, 0.3)]); // a fragment
    else parts.push([pick(2, 12), pick(0.02, 0.3)]);
    const words = Math.floor(rand() * 3);
    for (let w = 0; w < words; w++) {
      parts.push([pick(0.08, 0.6), room]); // a hesitation
      parts.push([pick(0.1, 2.5), pick(0.015, 0.3)]);
    }
    parts.push([pick(0.12, 2.5), room]);
    if (rand() < 0.35) {
      parts.push([pick(0.005, 0.2), pick(0.05, 0.5)]); // a click or a tap
      parts.push([pick(0.05, 1.5), room]);
    }
  }
  return parts;
}

function render(parts, sampleRate, seed) {
  const rand = mulberry32(seed ^ 0x5eed);
  const total = parts.reduce((n, [sec]) => n + Math.round(sec * sampleRate), 0);
  const samples = new Float32Array(total);
  let at = 0;
  for (const [sec, amp] of parts) {
    const n = Math.round(sec * sampleRate);
    for (let i = 0; i < n; i++) samples[at++] = (rand() * 2 - 1) * amp;
  }
  return samples;
}

// A cheap fingerprint of float samples, exact in both languages: the sum of their bit patterns.
function fingerprint(samples) {
  const bits = new Uint32Array(samples.buffer, samples.byteOffset, samples.length);
  let sum = 0;
  for (let i = 0; i < bits.length; i++) sum = (sum + bits[i]) >>> 0;
  return sum;
}

const OPTIONS = [
  { name: "every pause", opts: { maxSec: 20 } },
  { name: "app", opts: { maxSec: 20, minSoundSec: 0.15, minUtteranceSec: 2, keepPauseSec: 0.5 } },
  { name: "short cap", opts: { maxSec: 6, minSoundSec: 0.15, minUtteranceSec: 2, keepPauseSec: 0.5 } },
  { name: "no pauses", opts: { maxSec: 20, minPauseSec: Infinity } },
  { name: "long pauses only", opts: { maxSec: 20, minPauseSec: 0.6, minSoundSec: 0.1, minUtteranceSec: 4, keepPauseSec: 0.25 } },
];

const jsonOpts = (o) => Object.fromEntries(Object.entries(o).map(([k, v]) => [k, v === Infinity ? "Infinity" : v]));
const cases = [];
for (let seed = 1; seed <= 40; seed++) {
  const sampleRate = seed % 7 === 0 ? 44100 : 16000;
  const parts = recipe(seed);
  const samples = render(parts, sampleRate, seed);
  const results = OPTIONS.map(({ name, opts }) => {
    const cuts = split.splitPoints(samples, sampleRate, opts);
    const edges = [0, ...cuts, samples.length];
    // The longest stretch, squeezed as the decoder would get it, and capped again as a salvage
    // range would be.
    let longest = 1;
    for (let i = 2; i < edges.length; i++) if (edges[i] - edges[i - 1] > edges[longest] - edges[longest - 1]) longest = i;
    const piece = samples.subarray(edges[longest - 1], edges[longest]);
    const squeezed = split.squeezePauses(piece, sampleRate, opts);
    const from = Math.floor(samples.length / 5);
    const to = Math.floor((samples.length * 4) / 5);
    return {
      name,
      opts: jsonOpts(opts),
      cuts,
      pauses: Number.isFinite(opts.minPauseSec ?? 0.2) ? split.pauseSpans(samples, sampleRate, opts).map((p) => [p[0], p[1], p.parts.length]) : [],
      soundSeconds: split.soundSeconds(samples, sampleRate, opts),
      squeezed: { from: edges[longest - 1], to: edges[longest], length: squeezed.length, same: squeezed === piece, fingerprint: fingerprint(squeezed) },
      fitCap: { from, to, cuts: split.fitCap(samples, sampleRate, from, to, opts) },
    };
  });
  const third = Math.floor(samples.length / 3);
  cases.push({
    seed,
    sampleRate,
    parts,
    frames: samples.length,
    fingerprint: fingerprint(samples),
    results,
    speech: [containsSpeech(samples, sampleRate), containsSpeech(samples.subarray(0, third), sampleRate), containsSpeech(samples.subarray(third, third + Math.round(0.4 * sampleRate)), sampleRate)],
    quietest: [quietestOffset(samples.subarray(0, third), Math.floor(0.3 * sampleRate), Math.floor(0.05 * sampleRate)), quietestOffset(samples.subarray(0, 100), 4800, 800)],
  });
}
// The speech probe's edges: room tone alone, digital silence, a low-gain voice, a click, a buffer
// shorter than one window.
const probes = [
  [[6, 0.006]],
  [[4, 0]],
  [[1, 0.0008], [1.5, 0.004], [0.5, 0.0008], [1.2, 0.004], [0.4, 0.0008], [1, 0.004], [0.5, 0.0008]],
  [[2, 0.001], [0.2, 0.3], [2, 0.001]],
  [[2, 0.001], [0.006, 0.9], [2, 0.001]],
  [[0.1, 0.2]],
].map((parts, i) => ({ seed: 1000 + i, sampleRate: 16000, parts, speech: containsSpeech(render(parts, 16000, 1000 + i), 16000) }));
fs.writeFileSync(path.join(__dirname, "../tests/vectors/audio.json"), JSON.stringify({ cases, probes }) + "\n");
console.log(`audio.json: ${cases.length} recordings, ${cases.reduce((n, c) => n + c.results.reduce((m, r) => m + r.cuts.length, 0), 0)} cuts`);
