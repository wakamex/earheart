// Measures what Chromium's microphone processing is worth to the transcript. The Electron build
// records through getUserMedia with echo cancellation, noise suppression and automatic gain on
// (renderer/microphone.js); the native build records what SDL hands it, unprocessed. For each
// clip and condition this plays the audio into the Electron build's real overlay as the
// microphone (scripts/e2e-latency.js, which saves what the overlay captured), then transcribes
// both the captured audio and the audio as played with the same engine, and scores each against
// the clip's reference text with the evaluation's own normaliser.
//
//   node native/tools/eval-mic-processing.mjs --exe=native/zig-out/bin/earheart --models=<dir> \
//     --corpus=<eval cache>/corpus/fleurs-en_us-test --out=<dir outside the repo> [--clips=40]
//
// Conditions: the clip as it is; 20 dB quieter (a low-gain microphone); with white noise 15 dB
// below the speech (a noisy room). Linux, under an X display (it runs Electron with xvfb-run).
import { createRequire } from "node:module";
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const require = createRequire(import.meta.url);
const evalLib = require("../../scripts/stt-eval.js");
const { wavToFloat32, encodeWav, toPcm16, wavDurationSec } = require("../../main/util/wav");

const args = Object.fromEntries(process.argv.slice(2).map((a) => a.replace(/^--/, "").split(/=(.*)/s)));
for (const need of ["exe", "models", "corpus", "out"]) if (!args[need]) throw new Error(`--${need}= is required`);
const model = args.model || "parakeet-tdt-0.6b-v3-int8";
const repo = path.resolve(path.dirname(new URL(import.meta.url).pathname), "../..");
fs.mkdirSync(args.out, { recursive: true });

const rows = evalLib.parseFleursTsv(fs.readFileSync(path.join(args.corpus, "test.tsv"), "utf8"));
// Every Nth clip, so the sample spans the corpus's speakers.
const step = Math.floor(rows.length / Number(args.clips || 40));
const clips = rows.filter((_, i) => i % step === 0).slice(0, Number(args.clips || 40));

function mulberry32(a) {
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const CONDITIONS = {
  "as recorded": (s) => s,
  "20 dB quieter": (s) => s.map((x) => x * 0.1),
  "noise 15 dB below speech": (s, seed) => {
    const rms = Math.sqrt(s.reduce((n, x) => n + x * x, 0) / s.length);
    // White noise of amplitude a has RMS a / sqrt(3).
    const amp = rms * 10 ** (-15 / 20) * Math.sqrt(3);
    const rand = mulberry32(seed);
    return s.map((x) => x + (rand() * 2 - 1) * amp);
  },
};

function transcribe(files) {
  const list = path.join(args.out, "list.txt");
  fs.writeFileSync(list, files.join("\n") + "\n");
  const out = execFileSync(args.exe, ["transcribe", "--models", args.models, "--model", model, "--json", "--list", list], { maxBuffer: 1 << 26 }).toString();
  return Object.fromEntries(out.trim().split("\n").map((l) => JSON.parse(l)).map((r) => [r.file, r.text]));
}

const results = {};
for (const [condition, change] of Object.entries(CONDITIONS)) {
  const dir = path.join(args.out, condition.replace(/\W+/g, "-"));
  fs.mkdirSync(dir, { recursive: true });
  const pairs = [];
  for (const [i, clip] of clips.entries()) {
    const played = path.join(dir, `played-${clip.file}`);
    const captured = path.join(dir, `captured-${clip.file}`);
    const { samples } = wavToFloat32(fs.readFileSync(path.join(args.corpus, "pcm16", clip.file)));
    fs.writeFileSync(played, encodeWav(toPcm16(Float32Array.from(change(Array.from(samples), i + 1))), 16000));
    // A capture that starts late loses the clip's first words. One taken alone is about 0.2 s shorter
    // than the clip; on a busy machine some lose a second or more, so those are taken again.
    const length = wavDurationSec(fs.readFileSync(played));
    for (let attempt = 0; attempt < 3 && !(fs.existsSync(captured) && wavDurationSec(fs.readFileSync(captured)) >= length - 0.5); attempt++) {
      // Chromium plays the file as the microphone and loops it; the take stops when the clip ends.
      const run = spawnSync("xvfb-run", ["-a", "npx", "electron", "scripts/e2e-latency.js", "--no-sandbox", `--wav=${played}`, `--models=${args.models}`, "--config=stt-only", `--talk=${(length - 0.1).toFixed(2)}`, "--runs=1", "--debug-chunks"], { cwd: repo, encoding: "utf8", maxBuffer: 1 << 26 });
      const saved = /captured wav saved: (.*)/.exec(run.stdout || "");
      if (saved) fs.copyFileSync(saved[1].trim(), captured);
    }
    if (!fs.existsSync(captured)) {
      console.error(`${condition} ${clip.file}: no capture`);
      continue;
    }
    pairs.push({ clip, played, captured });
    console.error(`${condition}: ${i + 1}/${clips.length}`);
  }
  const texts = transcribe(pairs.flatMap((p) => [p.played, p.captured]));
  const counts = (key) => pairs.map((p) => evalLib.editCounts(evalLib.normalise(p.clip.raw), evalLib.normalise(texts[p[key]] || "")));
  const score = (key) => ({ wer: evalLib.corpusWer(counts(key)), empty: pairs.filter((p) => !(texts[p[key]] || "").trim()).length });
  // How sure the difference is: the clips drawn again with replacement, both versions of a clip
  // together, 10,000 times; the middle 95% of the differences in corpus WER.
  const raw = counts("played"), processed = counts("captured"), rand = mulberry32(1), diffs = [];
  for (let n = 0; n < 10000; n++) {
    const pick = pairs.map(() => Math.floor(rand() * pairs.length));
    diffs.push(evalLib.corpusWer(pick.map((i) => raw[i])) - evalLib.corpusWer(pick.map((i) => processed[i])));
  }
  diffs.sort((a, b) => a - b);
  results[condition] = { clips: pairs.length, sdlRaw: score("played"), chromiumProcessed: score("captured"), rawMinusProcessed95: [diffs[250], diffs[9749]] };
  fs.writeFileSync(path.join(args.out, "results.json"), JSON.stringify(results, null, 2) + "\n");
}
const pct = (x) => `${(x * 100).toFixed(2)}`;
console.log("| condition | clips | WER, audio as played (what SDL records) | WER, as Chromium's overlay captured it | difference in points, 95% interval |\n|---|---|---|---|---|");
for (const [name, r] of Object.entries(results)) console.log(`| ${name} | ${r.clips} | ${pct(r.sdlRaw.wer)}% | ${pct(r.chromiumProcessed.wer)}% | ${pct(r.sdlRaw.wer - r.chromiumProcessed.wer)} (${pct(r.rawMinusProcessed95[0])} to ${pct(r.rawMinusProcessed95[1])}) |`);
