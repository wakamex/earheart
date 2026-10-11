// Writes the hotkey manager's expected behaviour, by running main/hotkeys.js against a stand-in
// for Electron's globalShortcut through random sequences of changes: hotkeys applied, the discard
// hotkey armed and let go, everything suspended and resumed, and other apps taking and releasing
// keys in between. tests/hotkeys.c replays the sequences against src/hotkeys.c.
//   node native/tools/gen-hotkey-vectors.js
// One file per system, since the messages name keys as that system does.
const fs = require("node:fs");
const path = require("node:path");
const Module = require("node:module");

const INVALID = "Bad+Combo"; // what Electron throws for
const INVALID_MESSAGE = "conversion failure";
const held = new Set();
let taken = new Set();
const globalShortcut = {
  register(accelerator) {
    if (accelerator === INVALID) throw new Error(INVALID_MESSAGE);
    if (taken.has(accelerator) || held.has(accelerator)) return false;
    held.add(accelerator);
    return true;
  },
  unregister: (accelerator) => void held.delete(accelerator),
  unregisterAll: () => held.clear(),
};
const load = Module._load;
Module._load = function (request, ...rest) {
  if (request === "electron") return { globalShortcut };
  if (request.endsWith("/util/logger")) return { warn() {}, info() {}, error() {} };
  return load.call(this, request, ...rest);
};

function mulberry32(a) {
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Few keys, so the hotkeys collide with each other and with other apps often.
const KEYS = ["", "CommandOrControl+Shift+Space", "Alt+P", "Control+Alt+D", "Super+num3", "Alt+Shift+C", INVALID];

function generate(platform) {
  Object.defineProperty(process, "platform", { value: platform });
  for (const id of Object.keys(require.cache)) if (id.includes(`${path.sep}main${path.sep}`)) delete require.cache[id];
  const hotkeys = require("../../main/hotkeys");
  const cases = [];
  for (let seed = 1; seed <= 250; seed++) {
    const rand = mulberry32(seed * 7919);
    const pick = () => KEYS[Math.floor(rand() * KEYS.length)];
    hotkeys.unregisterAll();
    hotkeys.armDiscard(false);
    hotkeys.applyAll({ record: "", pause: "", discard: "", correct: "", onCorrect() {} });
    held.clear();
    taken = new Set();
    let current = { record: "", pause: "", discard: "", correct: "" };
    const steps = [];
    const length = 4 + Math.floor(rand() * 10);
    for (let i = 0; i < length; i++) {
      const roll = rand();
      const step = {};
      if (roll < 0.62) {
        // Mostly one or two hotkeys change at a time, as a save from Settings does.
        const next = { ...current };
        const names = ["record", "pause", "discard", "correct"];
        const changes = rand() < 0.25 ? 4 : 1 + Math.floor(rand() * 2);
        for (let c = 0; c < changes; c++) next[names[Math.floor(rand() * 4)]] = pick();
        const out = hotkeys.applyAll({ ...next, onCorrect() {}, onRecord() {}, onPause() {}, onDiscard() {} });
        step.apply = [next.record, next.pause, next.discard, next.correct];
        step.results = ["record", "pause", "discard", "correct"].map((n) => [out[n].ok ? 1 : 0, out[n].empty ? 1 : 0, out[n].unbound ? 1 : 0, out[n].error || ""]);
        // What Settings would then hold: the hotkeys refused keep their saved value.
        for (const n of names) if (out[n].ok || out[n].empty) current[n] = next[n];
      } else if (roll < 0.74) {
        step.arm = rand() < 0.5 ? 1 : 0;
        hotkeys.armDiscard(!!step.arm);
      } else if (roll < 0.86) {
        step.suspend = rand() < 0.5 ? 1 : 0;
        step.lost = hotkeys.suspend(!!step.suspend).length;
      } else {
        // Another app takes a key this app does not hold now, or lets one go.
        const key = KEYS[1 + Math.floor(rand() * (KEYS.length - 2))];
        if (taken.has(key)) taken.delete(key);
        else if (!held.has(key)) taken.add(key);
        step.taken = [...taken].sort();
      }
      step.held = [...held].sort();
      steps.push(step);
    }
    cases.push(steps);
  }
  return { invalid: INVALID, invalidMessage: INVALID_MESSAGE, cases };
}

for (const platform of ["linux", "darwin", "win32"]) {
  const file = path.join(__dirname, "..", "tests", "vectors", `hotkeys-${platform}.json`);
  fs.writeFileSync(file, JSON.stringify(generate(platform)) + "\n");
  console.log(`${file}: ${fs.statSync(file).size} bytes`);
}
