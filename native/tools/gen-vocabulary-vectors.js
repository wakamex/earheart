// Writes the dictionary rules' expected results, by running renderer/vocabulary.js on generated
// lists: terms cleaned up, a form's edits merged into the live list, and a spelling remembered.
//   node native/tools/gen-vocabulary-vectors.js
const fs = require("node:fs");
const path = require("node:path");
const { vocabularyTerms, mergeVocabulary, rememberVocabulary } = require("../../renderer/vocabulary");

function mulberry32(a) {
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rand = mulberry32(20261011);
const WORDS = ["Earheart", "gesso", "sherpa-onnx", "Kubernetes", " Zig ", "", "  ", "llama.cpp", "Ünïcode", "two words", "Earheart ", "tab\there", "Parakeet"];
const list = () => Array.from({ length: Math.floor(rand() * 7) }, () => WORDS[Math.floor(rand() * WORDS.length)]);
const merges = [];
for (let i = 0; i < 300; i++) {
  const baseline = list(), edited = list(), live = list();
  merges.push({ baseline, edited, live, terms: vocabularyTerms(edited), merged: mergeVocabulary(baseline, edited, live) });
}
const remembers = [];
const SPELLINGS = ["Earheart", " gesso ", "", "   ", "new term", "a".repeat(120), "b".repeat(121), "line\nbreak", "bell\x07", "ü".repeat(120), "ü".repeat(121)];
for (const spelling of SPELLINGS) {
  for (let i = 0; i < 6; i++) {
    const terms = list();
    let out = null, error = null;
    try { out = rememberVocabulary(terms, spelling); } catch (e) { error = e.message; }
    remembers.push({ terms, spelling, out, error });
  }
}
// A full dictionary: 256 terms, and one that is too long in all.
const full = Array.from({ length: 256 }, (_, i) => `term${i}`);
const long = Array.from({ length: 80 }, (_, i) => `${"x".repeat(100)}${i}`);
for (const [terms, spelling] of [[full, "one more"], [full, "term7"], [long, "y".repeat(110)], [long.slice(0, 40), "fits"]]) {
  let out = null, error = null;
  try { out = rememberVocabulary(terms, spelling); } catch (e) { error = e.message; }
  remembers.push({ terms, spelling, out, error });
}
const file = path.join(__dirname, "..", "tests", "vectors", "vocabulary.json");
fs.writeFileSync(file, JSON.stringify({ merges, remembers }) + "\n");
console.log(`${file}: ${merges.length} merges, ${remembers.length} remembered spellings`);
