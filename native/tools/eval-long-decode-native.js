// scripts/eval-long-decode.js through the native build: the same recordings (FLEURS sentences of
// one speaker gender, 300 ms apart, or with --dictation the dictation-shaped ones with
// hesitations, breaths and clicks), the same short-clip baseline, the same scoring and the same
// gate. Each recording is decoded by `earheart transcribe --chunked`, the app's own piecewise
// decoder over its engine worker. Exits 1 when a recording fails the gate.
//
//   node native/tools/eval-long-decode-native.js --exe native/zig-out/bin/earheart \
//     --cache-dir <eval-stt cache> --models <dir> --out <file outside the repo> \
//     [--model ID] [--targets 120,180,300] [--dictation] [--seed 1]
const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const e = require("../../scripts/stt-eval");
const manifest = require("../../scripts/stt-eval-manifest");
const long = require("../../scripts/eval-long-decode");
const { encodeWav, wavDurationSec, SAMPLE_RATE } = require("../../main/util/wav");

const argv = process.argv.slice(2);
const flag = (name, fallback) => (argv.includes(name) ? argv[argv.indexOf(name) + 1] : fallback);
const opts = {
  exe: flag("--exe"), cacheDir: flag("--cache-dir"), models: flag("--models"), out: flag("--out"),
  model: flag("--model", "parakeet-tdt-0.6b-v3"), targets: flag("--targets", "120,180,300").split(",").map(Number),
  dictation: argv.includes("--dictation"), seed: Number(flag("--seed", "1")), cap: 20, minRatio: 0.95, maxWerOverShort: 0.03,
};
for (const need of ["exe", "cacheDir", "models", "out"]) if (!opts[need]) throw new Error(`--${need.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase())} is required`);

const corpusDir = path.join(opts.cacheDir, "corpus", manifest.CORPUS.id);
const rows = e.parseFleursTsv(fs.readFileSync(path.join(corpusDir, manifest.CORPUS.tsv), "utf8"));
const clipPath = (r) => path.join(corpusDir, "pcm16", r.file);
const clip = (r) => {
  const buf = fs.readFileSync(clipPath(r));
  return new Int16Array(buf.buffer.slice(buf.byteOffset + 44, buf.byteOffset + buf.length));
};
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "earheart-long-"));
const native = (extra, files) => {
  const list = path.join(tmp, "list.txt");
  fs.writeFileSync(list, files.join("\n") + "\n");
  const out = execFileSync(opts.exe, ["transcribe", "--models", opts.models, "--model", opts.model, "--json", ...extra, "--list", list], { maxBuffer: 1 << 28, stdio: ["ignore", "pipe", "inherit"] });
  return out.toString().trim().split("\n").map((l) => JSON.parse(l));
};

const result = { model: opts.model, build: "native", at: new Date().toISOString(), options: opts, runs: [] };
try {
  // The baseline: every sentence the longest recording uses, one clip a decode.
  const longest = long.pickSentences(rows, Math.max(...opts.targets));
  const short = native([], longest.map(clipPath));
  const clipScores = longest.map((r, i) => long.score(r.raw, short[i].text));
  for (const target of opts.targets) {
    const picked = long.pickSentences(rows, target);
    const baseline = long.prefixBaseline(clipScores, picked.length);
    const pcm = opts.dictation ? long.dictationRecording(picked.map(clip), opts.seed) : e.concatPcm16(picked.map(clip), Math.round(0.3 * SAMPLE_RATE));
    const wav = encodeWav(pcm);
    const file = path.join(tmp, `${target}.wav`);
    fs.writeFileSync(file, wav);
    const startedAt = Date.now();
    const [r] = native(["--chunked"], [file]);
    const row = {
      target, audioSec: wavDurationSec(wav), utterances: picked.length, baseline, mode: "pieces", cap: opts.cap, pauses: true, corpus: opts.dictation ? "dictation" : "read",
      pieces: r.pieces.length, failedPieces: r.pieces.filter((p) => !p.ok).length, unconfirmedPieces: r.pieces.filter((p) => p.ok && p.unconfirmed).length,
      noisePieces: r.pieces.filter((p) => p.noise).length, pieceSeconds: r.pieces.map((p) => +p.seconds.toFixed(2)),
      wallMs: Date.now() - startedAt, decodeMs: r.decodeMs, workerInputs: r.workerInputs, maxPieceSec: r.maxPieceSec,
      ...(r.error ? { error: r.error } : {}),
      ...long.score(picked.map((x) => x.raw).join(" "), r.text),
      text: r.text,
      // The baseline clips before and the next recording after go through fresh workers; a worker
      // that died mid-recording shows as failed pieces above.
      alive: true,
    };
    delete row.counts;
    row.reasons = long.judgePieces(row, { shortWer: baseline.wer, cap: opts.cap, minRatio: opts.minRatio, maxWerOverShort: opts.maxWerOverShort });
    console.error(`${row.corpus} ${row.audioSec.toFixed(1)} s: ratio ${row.ratio.toFixed(3)}, WER ${(row.wer * 100).toFixed(1)}% (short-clip ${(baseline.wer * 100).toFixed(1)}%), pieces ${row.pieces}, max ${row.maxPieceSec.toFixed(1)} s${row.reasons.length ? ` — FAIL ${row.reasons.join("; ")}` : ""}`);
    result.runs.push(row);
  }
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
fs.writeFileSync(path.resolve(opts.out), `${JSON.stringify(result, null, 2)}\n`);
process.stdout.write(`${long.markdown(result)}\n`);
process.exit(result.runs.some((r) => r.reasons.length) ? 1 : 0);
