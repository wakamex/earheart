// Writes the expected results of the Hugging Face model lookup, by running
// main/services/hf-models.js on repositories made up here: what a pasted address parses to, which
// versions a repository's files make and in what order, the model definition built from each, and
// the message for every way a repository can be unusable. tests/hf.c replays them against src/hf.c.
//   node native/tools/gen-hf-vectors.js
const fs = require("node:fs");
const path = require("node:path");
const hf = require("../../main/services/hf-models");

function mulberry32(a) {
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rand = mulberry32(20261012);
const pick = (list) => list[Math.floor(rand() * list.length)];
const sha = () => Array.from({ length: 64 }, () => "0123456789abcdef"[Math.floor(rand() * 16)]).join("");

const parses = [
  "unsloth/gemma-3-1b-it-GGUF", "  owner/model  ", "", "   ", "owner", "owner/model/extra", "https://huggingface.co/owner/model",
  "huggingface.co/owner/model", "http://hf.co/owner/model", "https://www.huggingface.co/models/owner/model", "https://huggingface.co/owner",
  "https://huggingface.co/owner/model/tree/main", "https://huggingface.co/owner/model/blob/v1.0/file.gguf", "https://huggingface.co/owner/model/resolve/refs%2Fpr%2F3/x.onnx",
  "https://huggingface.co/owner/model/tree/%E0%A4%A", "https://example.com/owner/model", "ftp://huggingface.co/owner/model", "https://huggingface.co/owner/model?library=gguf#files",
  "owner/model name", "ow ner/model", "https://huggingface.co/owner/model/tree", "https://huggingface.co/owner/model/commits/main", "hf.co/a.b-c_d/e.f-g_h",
  "https://huggingface.co/models", "https://huggingface.co/models/owner", "HTTPS://HuggingFace.co/Owner/Model", "//huggingface.co/owner/model", "owner/model/",
].map((input) => {
  try { return { input, out: hf.parseRepoInput(input) }; } catch (e) { return { input, error: e.message }; }
});

async function run(kind, owner, repo, ref, info, tree, status) {
  const fetchImpl = async (url) => {
    const body = url.includes("/tree/") ? tree : info;
    const code = status && (url.includes("/tree/") ? status.tree : status.info);
    if (code) return { status: code, ok: false, json: async () => ({}) };
    return { status: 200, ok: true, json: async () => body };
  };
  const list = kind === "cleanup" ? hf.listGgufQuants : hf.listSttVariants;
  const build = kind === "cleanup" ? hf.buildCleanupModel : hf.buildSttModel;
  const one = { kind, owner, repo, ref: ref || null, info, tree, status: status || null };
  try {
    const out = await list({ owner, repo, ref }, fetchImpl);
    one.out = out;
    one.models = out.variants.map((v) => build(out.repo, v));
  } catch (e) {
    one.error = e.message;
  }
  return one;
}

const file = (p, size, lfs) => ({ type: "file", path: p, size: lfs ? 135 : size, ...(lfs ? { lfs: { oid: lfs === "bad" ? "nothex" : sha(), size } } : {}) });

async function main() {
  const repos = [];
  // Language models: quantisations named every way repositories name them.
  const QUANTS = ["Q4_K_M", "Q4_K_S", "Q4_0", "Q5_K_M", "Q8_0", "Q6_K", "Q3_K_M", "Q2_K", "IQ4_XS", "IQ3_M", "BF16", "F16", "FP16", "F32", "fp32", "MXFP4", "Q4_K_L", "q4_k_m", "Q4_1", "Q5_0", "Q4_K"];
  const STEMS = ["gemma-3-1b-it", "Qwen3-4B-Instruct-2507", "model", "granite-3.3-2b", "Phi_4.mini"];
  for (let i = 0; i < 60; i++) {
    const stem = pick(STEMS), sep = pick(["-", ".", "_"]), tree = [];
    const count = 1 + Math.floor(rand() * 6);
    for (let q = 0; q < count; q++) {
      const quant = pick(QUANTS), dir = rand() < 0.2 ? `${pick(["gguf", "q", "sub dir"])}/` : "";
      if (rand() < 0.15) {
        const parts = 2 + Math.floor(rand() * 2);
        for (let s = 1; s <= parts; s++) tree.push(file(`${dir}${stem}${sep}${quant}-0000${s}-of-0000${parts}.gguf`, 1e9 + Math.floor(rand() * 1e9), rand() < 0.8));
      } else {
        tree.push(file(`${dir}${stem}${sep}${quant}.gguf`, Math.floor(2e8 + rand() * 6e9), rand() < 0.85 ? (rand() < 0.1 ? "bad" : true) : false));
      }
    }
    if (rand() < 0.3) tree.push(file(`${pick(["mmproj", "model-mmproj", "x.projector", "imatrix"])}${sep}F16.gguf`, 8e8, true));
    if (rand() < 0.3) tree.push(file(`${stem}.gguf`, 3e9, true)); // no quantisation in the name
    if (rand() < 0.2) tree.push(file(`${stem}-unknownquant.GGUF`, 2e9, true));
    tree.push(file("README.md", 4000), file(".gitattributes", 1500), { type: "directory", path: "gguf" });
    repos.push(await run("cleanup", pick(["unsloth", "bartowski", "Some.Org"]), `${stem}-GGUF`, rand() < 0.2 ? "refs/pr/2" : undefined, { sha: rand() < 0.8 ? sha().slice(0, 40) : undefined, gated: false }, tree));
  }
  repos.push(await run("cleanup", "o", "empty", undefined, { sha: "abc" }, [file("README.md", 10)]));
  repos.push(await run("cleanup", "o", "only-mmproj", undefined, { sha: "abc" }, [file("mmproj-model-f16.gguf", 10, true)]));
  repos.push(await run("cleanup", "o", "gated", undefined, { sha: "abc", gated: "manual" }, []));
  repos.push(await run("cleanup", "o", "missing", undefined, {}, [], { info: 404 }));
  repos.push(await run("cleanup", "o", "private", undefined, {}, [], { info: 401 }));
  repos.push(await run("cleanup", "o", "forbidden", undefined, { sha: "abc" }, [], { tree: 403 }));
  repos.push(await run("cleanup", "o", "broken", undefined, {}, [], { info: 500 }));
  repos.push(await run("cleanup", "o", "not-a-list", undefined, { sha: "abc" }, { error: "x" }));

  // Speech models: bundles as sherpa-onnx publishes them, and everything that is not one.
  const stt = (owner, repo, paths, extra = {}) => run("stt", owner, repo, extra.ref, { sha: "c0ffee" }, paths.map((p) => (typeof p === "string" ? file(p, Math.floor(1e6 + rand() * 6e8), /\.(onnx|data|weights)$/.test(p)) : p)));
  const T = ["encoder", "decoder", "joiner"];
  repos.push(await stt("csukuangfj", "sherpa-onnx-nemo-parakeet-tdt-0.6b-v2", [...T.map((c) => `${c}.onnx`), ...T.map((c) => `${c}.int8.onnx`), "tokens.txt", "README.md", "test_wavs/0.wav"]));
  repos.push(await stt("csukuangfj", "sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8", [...T.map((c) => `${c}.int8.onnx`), "tokens.txt"]));
  repos.push(await stt("csukuangfj", "sherpa-onnx-zipformer-en-2023-06-26", [...T.map((c) => `${c}-epoch-99-avg-1.onnx`), ...T.map((c) => `${c}-epoch-99-avg-1.int8.onnx`), "tokens.txt"]));
  repos.push(await stt("csukuangfj", "sherpa-onnx-whisper-turbo", ["turbo-encoder.onnx", "turbo-decoder.onnx", "turbo-encoder.int8.onnx", "turbo-decoder.int8.onnx", "turbo-tokens.txt", "turbo-encoder.weights", "turbo-decoder.weights"]));
  repos.push(await stt("csukuangfj", "sherpa-onnx-whisper-small.en", ["small.en-encoder.onnx", "small.en-decoder.onnx", "small.en-encoder.int8.onnx", "small.en-decoder.int8.onnx", "small.en-tokens.txt"]));
  repos.push(await stt("someone", "speech-export", ["distil-large-v3-encoder.onnx", "distil-large-v3-decoder.onnx", "distil-large-v3-tokens.txt"]));
  repos.push(await stt("someone", "speech-export", ["encoder.onnx", "decoder.onnx", "tokens.txt"]));
  repos.push(await stt("someone", "my-whisper-thing", ["encoder.onnx", "decoder.onnx", "tokens.txt"]));
  repos.push(await stt("nvidia", "canary-1b-onnx", ["encoder.onnx", "decoder.onnx", "tokens.txt"]));
  repos.push(await stt("nvidia", "canary-with-joiner", [...T.map((c) => `${c}.onnx`), "tokens.txt"]));
  repos.push(await stt("o", "no-onnx", ["model.safetensors", "tokens.txt"]));
  repos.push(await stt("o", "single-file", ["model.onnx", "model_quantized.onnx", "a/b/third.onnx", "x.onnx", "tokens.txt"]));
  repos.push(await stt("o", "no-decoder", ["encoder.onnx", "joiner.onnx", "tokens.txt"]));
  repos.push(await stt("o", "no-encoder", ["decoder.onnx", "tokens.txt"]));
  repos.push(await stt("o", "no-tokens", [...T.map((c) => `${c}.onnx`), "tokenizer.json"]));
  repos.push(await stt("o", "whisper-no-tokens", ["tiny-encoder.onnx", "tiny-decoder.onnx", "tokenizer.json"]));
  repos.push(await stt("o", "mixed-precision", ["encoder.int8.onnx", "decoder.onnx", "joiner.fp16.onnx", "tokens.txt"]));
  repos.push(await stt("o", "never-share", ["encoder.int8.onnx", "decoder.fp16.onnx", "joiner.fp16.onnx", "tokens.txt"]));
  repos.push(await stt("o", "nested-parakeet", ["onnx/int8/encoder.onnx", "onnx/int8/decoder.onnx", "onnx/int8/joiner.onnx", "onnx/encoder.onnx", "onnx/decoder.onnx", "onnx/joiner.onnx", "onnx/encoder.onnx.data", "onnx/tokens.txt", "tokens.txt"]));
  repos.push(await stt("o", "sidecars", ["encoder.onnx", "encoder.data", "encoder.onnx_data", "other/encoder.weights", "decoder.onnx", "joiner.onnx", "decoder_extra.data", "tokens.txt"]));
  repos.push(await stt("o", "two-tokens", ["base-encoder.onnx", "base-decoder.onnx", "base-tokens.txt", "tiny-encoder.onnx", "tiny-decoder.onnx", "tiny-tokens.txt", "tokens.txt"]));
  repos.push(await stt("o", "quantized-names", ["encoder_quantized.onnx", "decoder_quantized.onnx", "joiner_quantized.onnx", "encoder_q4f16.onnx", "decoder_q4f16.onnx", "joiner_q4f16.onnx", "encoder-half.onnx", "decoder-half.onnx", "joiner-half.onnx", "tokens.txt"]));
  repos.push(await stt("o", "Upper-Case", ["Encoder.ONNX", "Decoder.ONNX", "Joiner.ONNX", "Tokens.txt"]));
  repos.push(await stt("o", "ref", [...T.map((c) => `${c}.onnx`), "tokens.txt"], { ref: "v1.0" }));
  const PARTS = ["encoder", "decoder", "joiner", "tokens.txt", "model"], PRE = ["", "tiny-", "base.", "x_", "large-v3-", "sub/", "a/b/"], POST = ["", ".int8", "_fp16", "-q8", ".quantized", "-epoch-1"];
  for (let i = 0; i < 80; i++) {
    const paths = new Set();
    for (let n = 2 + Math.floor(rand() * 7); n > 0; n--) {
      const part = pick(PARTS);
      paths.add(part === "tokens.txt" ? `${pick(PRE)}tokens.txt` : `${pick(PRE)}${part}${pick(POST)}.onnx`);
    }
    repos.push(await stt(pick(["o", "nemo-org", "whisper-fans", "k2-fsa"]), pick(["speech", "parakeet-x", "sherpa-onnx-whisper-base", "canary-small", "zipformer"]), [...paths]));
  }
  const out = path.join(__dirname, "..", "tests", "vectors", "hf.json");
  fs.writeFileSync(out, JSON.stringify({ parses, repos, search: { cleanup: hf.searchUrl("cleanup"), stt: hf.searchUrl("stt") } }) + "\n");
  const ok = repos.filter((r) => r.out).length;
  const errors = new Set(repos.filter((r) => r.error).map((r) => r.error.replace(/\d+|\(.*?\)/g, "#").slice(0, 60)));
  console.log(`${out}: ${parses.length} addresses, ${repos.length} repositories (${ok} usable, ${errors.size} kinds of refusal), ${fs.statSync(out).size} bytes`);
}
main();
