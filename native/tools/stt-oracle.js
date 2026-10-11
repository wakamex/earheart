// Transcribes WAV files with the Electron build's speech addon (sherpa-onnx-node) and the
// recognizer settings of main/engines/engine-worker.js, without Electron: the reference for the
// native build on machines where Electron cannot be driven. On Linux its output over the
// evaluation corpus is identical to scripts/eval-stt.js's, which goes through the app's worker.
// Prints one JSON line per file, in the shape of `earheart transcribe --json`.
//
//   node native/tools/stt-oracle.js MODELS_DIR MODEL_ID FILE.wav...
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const sherpaOnnx = require("sherpa-onnx-node");
const { getModel } = require("../../main/engines/registry");
const { wavToFloat32, SAMPLE_RATE } = require("../../main/util/wav");

const [models, id, ...files] = process.argv.slice(2);
const { sherpa } = getModel("stt", id);
const dir = path.join(models, "stt", id);
const numThreads = Math.max(1, Math.min(8, os.cpus().length - 1));
const recognizer = new sherpaOnnx.OfflineRecognizer({
  featConfig: { sampleRate: SAMPLE_RATE, featureDim: 80 },
  modelConfig: {
    transducer: { encoder: path.join(dir, sherpa.encoder), decoder: path.join(dir, sherpa.decoder), joiner: path.join(dir, sherpa.joiner) },
    tokens: path.join(dir, sherpa.tokens),
    numThreads,
    provider: "cpu",
    modelType: sherpa.modelType || "nemo_transducer",
    debug: false,
  },
});
for (const file of files) {
  const { samples, sampleRate } = wavToFloat32(fs.readFileSync(file));
  const stream = recognizer.createStream();
  stream.acceptWaveform({ sampleRate, samples });
  const t0 = Date.now();
  recognizer.decode(stream);
  const result = recognizer.getResult(stream);
  console.log(JSON.stringify({ file, text: (result && result.text ? result.text : "").trim(), decodeMs: Date.now() - t0, threads: numThreads }));
}
