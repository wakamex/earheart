// Runs scripts/bench-cleanup.mjs's matrix through the native build: the same inputs, styles, seeds,
// prompts and scoring, with each clean done by `earheart clean --batch` in place of node-llama-cpp.
// Writes runs.jsonl, raw outputs and summary.json under --out/<id>-native/ in the benchmark's own
// shapes, so the two builds' summaries can be read side by side.
//
//   node native/tools/bench-cleanup-native.mjs --exe=native/zig-out/bin/earheart --models=<dir> \
//     --id=<catalogue id> --out=<dir outside the repo> [--seeds=17,29,43,61,79] \
//     [--corpus=fluent,reported] [--gpu]
//   node native/tools/bench-cleanup-native.mjs --compare <electron summary.json> <native summary.json>
import { createRequire } from "node:module";
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import readline from "node:readline";
import { plan } from "../../scripts/bench-cleanup.mjs";

const require = createRequire(import.meta.url);
const metrics = require("../../scripts/cleanup-metrics");

const args = Object.fromEntries(
  process.argv.slice(2).filter((a) => a.startsWith("--")).map((a) => {
    const [k, v] = a.slice(2).split(/=(.*)/s);
    return [k, v ?? true];
  })
);

if (args.compare) {
  const [a, b] = process.argv.slice(2).filter((x) => !x.startsWith("--")).map((f) => JSON.parse(fs.readFileSync(f, "utf8")).summary);
  const rows = [];
  const walk = (x, y, at) => {
    for (const k of new Set([...Object.keys(x || {}), ...Object.keys(y || {})])) {
      const [p, q] = [x?.[k], y?.[k]];
      if ((p && typeof p === "object" && !Array.isArray(p)) || (q && typeof q === "object" && !Array.isArray(q))) walk(p, q, `${at}${k}.`);
      else rows.push(`| ${at}${k} | ${JSON.stringify(p)} | ${JSON.stringify(q)} |`);
    }
  };
  walk(a, b, "");
  console.log("| measure | Electron build | native build |\n|---|---|---|\n" + rows.join("\n"));
  process.exit(0);
}

for (const need of ["exe", "models", "id", "out"]) if (!args[need]) throw new Error(`--${need}= is required`);
const opts = {
  seeds: String(args.seeds || "17,29,43,61,79").split(",").map(Number),
  corpora: String(args.corpus || "fluent,reported").split(","),
  gpu: !!args.gpu,
};
const turns = plan(opts);
const dir = path.join(args.out, `${args.id}-native${opts.gpu ? "-gpu" : ""}`);
fs.mkdirSync(path.join(dir, "raw"), { recursive: true });

const child = spawn(args.exe, ["clean", "--models", args.models, "--model", args.id, ...(opts.gpu ? [] : ["--cpu"]), "--batch"], {
  stdio: ["pipe", "pipe", "inherit"],
});
const lines = readline.createInterface({ input: child.stdout })[Symbol.asyncIterator]();
async function clean(t) {
  const s = t.sampling || {};
  child.stdin.write(
    JSON.stringify({
      systemPrompt: t.systemPrompt,
      transcript: t.input,
      temperature: s.temperature,
      topP: s.topP ?? undefined,
      topK: s.topK > 0 ? s.topK : undefined,
      minP: s.minP > 0 ? s.minP : undefined,
      seed: t.seed,
      fresh: true,
    }) + "\n"
  );
  const { value, done } = await lines.next();
  if (done) throw new Error("the native build stopped answering");
  return JSON.parse(value);
}

const startedAt = new Date().toISOString();
const loadAvgAtStart = os.loadavg();
await clean(turns[0]); // warm-up, as the benchmark does
const runs = [];
fs.writeFileSync(path.join(dir, "runs.jsonl"), "");
for (const [n, t] of turns.entries()) {
  const r = await clean(t);
  // The native build reports a reply that ran to the token cap as an error; the benchmark scores
  // that as the raw transcript being delivered.
  const runaway = !!r.error && /did not finish/.test(r.error);
  if (r.error && !runaway) throw new Error(`${t.corpus}/${t.style}#${t.index} s${t.seed}: ${r.error}`);
  const output = runaway ? "" : r.text;
  const stopReason = runaway ? "maxTokens" : "eogToken";
  const score = metrics.scoreOutput({ input: t.input, output, stopReason, systemPrompt: t.systemPrompt, corpus: t.corpus });
  const row = {
    corpus: t.corpus, style: t.style, index: t.index, seed: t.seed, stopReason,
    wallMs: Math.round(r.ms), ttftMs: null, genTokens: 0, firstBatch: 0, decodeTps: null,
    loadAvg1: Number(os.loadavg()[0].toFixed(2)),
    score: { ...score, delivered: undefined },
  };
  runs.push(row);
  fs.appendFileSync(path.join(dir, "runs.jsonl"), JSON.stringify(row) + "\n");
  fs.writeFileSync(path.join(dir, "raw", `${t.corpus}-${t.style}-${t.index}-s${t.seed}.txt`), output + "\n");
  console.error(`[${args.id}] ${n + 1}/${turns.length} ${t.corpus}/${t.style}#${t.index} s${t.seed}: ${row.wallMs} ms, fillers ${score.fillers} repeats ${score.repeats}${score.fidelityOk ? "" : " FIDELITY-FAIL"}${runaway ? " RUNAWAY" : ""}`);
}
child.stdin.end();
const manifest = {
  id: args.id, build: "native", backend: opts.gpu ? "gpu" : "cpu", seeds: opts.seeds,
  hardware: { cpu: os.cpus()[0]?.model, logicalCpus: os.cpus().length, ramGB: Math.round(os.totalmem() / 2 ** 30), os: `${os.type()} ${os.release()}` },
  startedAt, finishedAt: new Date().toISOString(),
  loadAvgAtStart: loadAvgAtStart.map((x) => Number(x.toFixed(2))), loadAvgAtEnd: os.loadavg().map((x) => Number(x.toFixed(2))),
};
fs.writeFileSync(path.join(dir, "summary.json"), JSON.stringify({ manifest, summary: metrics.summarizeRuns(runs) }, null, 2) + "\n");
console.log(path.join(dir, "summary.json"));
