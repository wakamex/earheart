// Downloads catalogue models with the Electron build's own downloader (checksums, resumable
// parts, completion markers), for tests and measurements until the native build downloads its own.
//   node native/tools/fetch-model.js MODELS_DIR KIND/ID...     e.g. stt/parakeet-tdt-0.6b-v3-int8
const path = require("node:path");
const manager = require("../../main/engines/model-manager");
const registry = require("../../main/engines/registry");

(async () => {
  const [dir, ...models] = process.argv.slice(2);
  for (const [kind, id] of models.map((m) => m.split("/"))) {
    const model = registry.getModel(kind, id);
    if (!model) throw new Error(`no ${kind} model "${id}" in the catalogue`);
    if (!manager.isInstalled(path.resolve(dir), model)) await manager.download(path.resolve(dir), model);
    console.log(`${kind}/${id}: installed`);
  }
})().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
