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
const { transcribeChunked, EVERY_PAUSE } = require("../../main/chunked-decode");
const wavUtil = require("../../main/util/wav");

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

/* ---------------- text ---------------- */

const { stripFillers, collapseRepeats, stripStumbles } = require("../../main/util/stumble-strip");

// Every string literal in test/unit.test.js, which holds the stumble stripper's own tests, as inputs: the cases its authors
// thought mattered.
function literalsOf(file) {
  const src = fs.readFileSync(path.join(__dirname, "../../test", file), "utf8");
  const out = new Set();
  for (const m of src.matchAll(/"((?:[^"\\\n]|\\.)*)"/g)) {
    try {
      out.add(JSON.parse(`"${m[1]}"`));
    } catch {}
  }
  return [...out];
}

// Sentences assembled from the pieces the stripper reacts to, so every seam is tried: fillers in
// each case, commas and sentence punctuation on either side, line breaks, repeats, other scripts.
function textInputs() {
  const rand = mulberry32(4242);
  const pick = (list) => list[Math.floor(rand() * list.length)];
  const fillers = ["um", "uh", "Um", "Uh", "umm", "uhh", "uhm", "erm", "Erm", "UM", "UH", "ermm", "uh-huh", "umbrella", "hum", "um_x", "Ümit"];
  const words = ["the", "the", "The", "file", "I", "I", "a", "a", "had", "very", "two", "2", "v", "V", "Walla", "walla", "éclair", "Éclair", "это", "Это", "ναι", "Ναι", "straße", "ﬁle", "don't", "don’t", "well-known", "so", "okay", "x1", "𝒳", "we", "is", "that's", "it"];
  const seams = [" ", " ", " ", "  ", ", ", ",", " ,", ". ", "! ", "? ", "... ", "\n", " \n", ".\n", "; ", ": ", "\t", "\u00a0", ".\" ", ".) ", "\n\n"];
  const out = ["", " ", "um", "Um.", "um um", "the the", "I I", "The the the", "um, uh, the the file", "Um... okay", "Do it. Um! Really?", "So, um, we", "is uh, for", "that's it, um.", "the um the file", "no no", "No, no"];
  for (let i = 0; i < 700; i++) {
    let s = rand() < 0.2 ? pick(seams) : "";
    const n = 1 + Math.floor(rand() * 9);
    for (let k = 0; k < n; k++) {
      const r = rand();
      const w = r < 0.35 ? pick(fillers) : pick(words);
      s += w;
      if (rand() < 0.35) s += pick([" ", "  "]) + (rand() < 0.8 ? w : w.toLowerCase());
      if (k < n - 1 || rand() < 0.4) s += pick(seams);
    }
    out.push(s);
  }
  return [...new Set([...out, ...literalsOf("unit.test.js").filter((t) => t.length < 400)])];
}

// residualTail is private to main/live-preview.js; this is its text, checked against the source so
// a change there fails the generator and is not silently missed.
function normWord(w) {
  return w.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, "");
}
function residualTail(live, committed) {
  if (!live) return "";
  const liveWords = live.split(/\s+/);
  const committedWords = committed ? committed.split(/\s+/) : [];
  const shared = Math.min(liveWords.length, committedWords.length);
  for (let i = 0; i < shared; i++) {
    if (normWord(liveWords[i]) !== normWord(committedWords[i])) return "";
  }
  return liveWords.slice(committedWords.length).join(" ");
}
{
  const src = fs.readFileSync(path.join(__dirname, "../../main/live-preview.js"), "utf8");
  for (const fn of [normWord, residualTail]) {
    const body = fn.toString().replace(/\s+/g, " ");
    const theirs = src.replace(/\/\/[^\n]*\n/g, "\n").replace(/\s+/g, " ");
    if (!theirs.includes(body)) throw new Error(`main/live-preview.js no longer contains ${fn.name} as copied here`);
  }
}

function tailPairs() {
  const rand = mulberry32(777);
  const pick = (list) => list[Math.floor(rand() * list.length)];
  const words = ["so", "the", "chunk", "boundary", "lands", "somewhere", "here", "Это", "ναι", "x1", "don't", "well-known", "okay"];
  const dress = (w) => pick([w, w, w, w.toUpperCase(), w + ",", w + ".", `"${w}"`, w + "?!", "…" + w]);
  const out = [["", ""], ["a b", ""], ["", "a b"], ["a  b", "a"], [" a b", "a"], ["a b ", "a"], ["a b", " a"], ["a b c", "A, b."], ["a b c", "a x"], ["a b", "a b c d"], ["... a", "a"]];
  for (let i = 0; i < 300; i++) {
    const n = 1 + Math.floor(rand() * 8);
    const base = Array.from({ length: n }, () => pick(words));
    const cut = Math.floor(rand() * (n + 2));
    const committed = base.slice(0, cut).map(dress);
    if (rand() < 0.2 && committed.length) committed[Math.floor(rand() * committed.length)] = pick(words);
    out.push([base.map(dress).join(pick([" ", "  ", "\n"])), committed.join(pick([" ", " ", "\t"]))]);
  }
  return out;
}

{
  const strip = textInputs().map((input) => [input, stripFillers(input), collapseRepeats(input), stripStumbles(input)]);
  const tails = tailPairs().map(([live, committed]) => [live, committed, residualTail(live, committed)]);
  const trims = [" a ", "\n\ta b\u00a0\u2028", "", "  ", "x", "\ufeffx\u3000"].map((t) => [t, t.trim()]);
  fs.writeFileSync(path.join(__dirname, "../tests/vectors/text.json"), JSON.stringify({ strip, tails, trims }) + "\n");
  console.log(`text.json: ${strip.length} texts (${strip.filter((r) => r[0] !== r[3]).length} changed), ${tails.length} tail pairs (${tails.filter((r) => r[2]).length} with a tail)`);
}

/* ---------------- settings ---------------- */

{
  const settings = require("../../main/settings");
  const { resolveCleanup } = require("../../main/cleanup-styles");
  const D = settings.DEFAULTS;
  // main/settings.js keeps its limits private; these are its two.
  const clampLimits = (m) => {
    for (const [section, key, min, max] of [["audio", "maxRecordingSeconds", 10, 3600], ["engines", "idleUnloadMinutes", 0, 240]]) {
      const v = m[section][key];
      m[section][key] = Number.isFinite(v) ? Math.round(Math.min(max, Math.max(min, v))) : D[section][key];
    }
    return m;
  };
  const wrapped = D.cleanup.systemPrompt.replace(/(.{60,76}) /g, "$1\n  ");
  const stored = [
    {},
    { hotkey: "Alt+D", output: { mode: "clipboard" } },
    { hotkey: 5, startOnBoot: "yes", output: "paste", stt: [], history: { limit: "many", enabled: false } },
    { stt: { baseUrl: "http://x/v1" } },
    { stt: { engine: "server", livePreview: { chunkSeconds: 5 } }, sttServer: { port: 1 } },
    { stt: { engine: "builtin", livePreview: { chunkSeconds: 7, enabled: false } } },
    { cleanup: { model: "m" } },
    { cleanup: { engine: "builtin", timeoutMs: 60000, temperature: 0.7 } },
    { cleanup: { engine: "builtin", style: "polished", temperature: 0.7 } },
    { cleanup: { engine: "builtin", systemPrompt: wrapped } },
    { cleanup: { engine: "builtin", systemPrompt: wrapped + " changed" } },
    { cleanup: { engine: "builtin", dictionary: ["Earheart", " gesso "], custom: { temperature: 1 } } },
    { cleanup: { engine: "builtin", dictionary: "none" } },
    { audio: { maxRecordingSeconds: 5 }, engines: { idleUnloadMinutes: 999 } },
    { audio: { maxRecordingSeconds: 90.6 }, engines: { idleUnloadMinutes: -3 } },
    { audio: { maxRecordingSeconds: null }, engines: { idleUnloadMinutes: "2" } },
    { overlay: { x: 10, y: 20 }, customModels: [{ id: "c1", kind: "stt" }], unknownKey: { kept: true } },
    { updates: { skippedVersion: "0.40.0", extra: 1 }, history: null },
  ].map((input) => ({ input, merged: clampLimits(settings.deepMerge(D, settings.migrateLegacy(JSON.parse(JSON.stringify(input))))) }));
  const cleanups = [
    D.cleanup,
    { ...D.cleanup, style: "verbatim" },
    { ...D.cleanup, style: "polished", dictionary: ["Earheart", "  ", " gesso ", 42] },
    { ...D.cleanup, style: "no-such-style" },
    { ...D.cleanup, style: "custom", custom: { temperature: 0.7, topP: 1, topK: 0, minP: 0 } },
    { ...D.cleanup, style: "custom", custom: {}, systemPrompt: "", dictionary: ["x"] },
    { style: "clean" },
  ].map((input) => ({ input, ...resolveCleanup(input) }));
  fs.writeFileSync(path.join(__dirname, "../tests/vectors/settings.json"), JSON.stringify({ stored, cleanups }) + "\n");
  console.log(`settings.json: ${stored.length} stored files, ${cleanups.length} cleanup sections`);
}

/* ---------------- chunked decode ---------------- */

// The PCM of a WAV as one number, exact in both languages.
function pcmFingerprint(wav) {
  let sum = 0;
  for (let i = 44; i + 1 < wav.length; i += 2) sum = (sum + wav.readUInt16LE(i) * ((i >> 1) % 251 + 1)) >>> 0;
  return sum;
}

const CODES = { ENGINE_EXITED: "exit", ENGINE_TIMEOUT: "timeout", EMPTY_SPEECH: "empty_speech" };

async function chunkedCase(seed) {
  const rand = mulberry32(seed * 7919);
  const parts = recipe(seed).slice(0, 4 + Math.floor(rand() * 14));
  // Some recordings are nothing but room tone or a click, and one is empty.
  if (seed % 11 === 0) parts.length = 0;
  if (seed % 13 === 0) parts.splice(0, parts.length, [1.5, 0.002], [0.05, 0.4], [1.2, 0.002]);
  const samples = render(parts, 16000, seed);
  const wav = wavUtil.encodeWav(wavUtil.toPcm16(samples), 16000);
  const frames = samples.length;
  // What the fake decoder answers, call by call; past the end of the script it answers normally.
  const script = [];
  const trouble = rand();
  for (let i = 0; i < 40; i++) {
    const x = rand();
    if (trouble < 0.35) script.push("ok");
    else if (trouble < 0.5) script.push(x < 0.5 ? "exit" : "ok");
    else if (trouble < 0.6) script.push("exit");
    else script.push(x < 0.55 ? "ok" : x < 0.7 ? "empty" : x < 0.8 ? "exit" : x < 0.88 ? "timeout" : x < 0.95 ? "error" : "space");
  }
  const salvageChunks = [];
  if (rand() < 0.5 && frames > 32000) {
    let at = Math.floor(rand() * 8000);
    const n = 1 + Math.floor(rand() * 4);
    for (let i = 0; i < n && at < frames; i++) {
      const to = Math.min(frames, at + 16000 + Math.floor(rand() * 16000 * 12));
      salvageChunks.push({ from: at, to, text: `salvage${i}` });
      at = rand() < 0.2 ? to - 4000 : to + (rand() < 0.5 ? 0 : Math.floor(rand() * 16000));
    }
    if (rand() < 0.3) salvageChunks.reverse();
  }
  const opts = {
    prefixText: rand() < 0.3 ? "prefix words" : "",
    salvageText: salvageChunks.length && rand() < 0.7 ? salvageChunks.map((c) => c.text).join(" ") : "",
    everyPause: rand() < 0.2,
    maxSec: rand() < 0.2 ? 8 : 20,
  };
  const calls = [];
  let restarts = 0;
  let decodeMs = 0;
  const runTranscribe = async (piece, { onDecodeMs }) => {
    const n = wavUtil.wavSampleFrames(piece);
    const outcome = script[calls.length] || "ok";
    calls.push([n, pcmFingerprint(piece)]);
    if (outcome === "exit") throw Object.assign(new Error("engine process exited"), { code: "ENGINE_EXITED" });
    if (outcome === "timeout") throw Object.assign(new Error("engine request timed out"), { code: "ENGINE_TIMEOUT" });
    if (outcome === "error") throw new Error("model exploded");
    onDecodeMs(n % 97);
    return outcome === "empty" ? "" : outcome === "space" ? "  " : ` t${n} `;
  };
  let result = null;
  let threw = null;
  try {
    result = await transcribeChunked(wav, {
      runTranscribe,
      restartStt: () => restarts++,
      maxSec: opts.maxSec,
      ...(opts.everyPause ? EVERY_PAUSE : {}),
      prefixText: opts.prefixText,
      salvageChunks,
      salvageText: opts.salvageText,
      onDecodeMs: (ms) => (decodeMs += ms),
    });
  } catch (err) {
    threw = { code: CODES[err?.code] || "failed", message: err?.message || "" };
  }
  return {
    seed, parts, frames, script, salvageChunks, opts, calls, restarts, decodeMs, threw,
    result: result && {
      text: result.text,
      partial: result.partial,
      pieces: result.pieces.map((p) => [p.fromFrame, p.toFrame, p.group, p.attempts, !!p.ok, !!p.skipped, !!p.noise, !!p.unconfirmed, p.text || "", p.ok ? "" : CODES[p.code] || (p.error ? "failed" : "")]),
    },
  };
}

(async () => {
  const chunked = [];
  for (let seed = 1; seed <= 120; seed++) chunked.push(await chunkedCase(seed));
  fs.writeFileSync(path.join(__dirname, "../tests/vectors/chunked.json"), JSON.stringify({ cases: chunked }) + "\n");
  const count = (f) => chunked.filter(f).length;
  console.log(
    `chunked.json: ${chunked.length} dictations, ${count((c) => c.threw)} failed outright, ${count((c) => c.result?.partial)} partial, ` +
      `${count((c) => c.salvageChunks.length)} with salvage, ${count((c) => c.result?.pieces.some((p) => p[6]))} with noise pieces, ` +
      `${count((c) => c.result?.pieces.some((p) => p[7]))} with unconfirmed pieces, ${count((c) => c.result?.pieces.some((p) => p[5]))} with skipped pieces`
  );
})();
