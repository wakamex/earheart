// Prints, for each GGUF given, the exact prompt the Electron build's cleanup engine evaluates for a
// transcript: the text node-llama-cpp renders around the user turn, its tokens, and what ends the
// reply. The native build's prompt is tested against this output.
//
//   node native/tools/prompt-oracle.mjs MODEL.gguf [MODEL.gguf ...] > prompts.json
import { createRequire } from "node:module";
import * as mod from "node-llama-cpp";
const require = createRequire(import.meta.url);
const { cleanupUserTurn, cleanupChatWrapper } = require("../../main/util/cleanup-turn.js");

const turn = cleanupUserTurn("SYSTEM PROMPT", "um so this is the transcript");
const llama = await mod.getLlama({ gpu: false, logLevel: "error" });
const out = {};
for (const file of process.argv.slice(2)) {
  const model = await llama.loadModel({ modelPath: file });
  const wrapper = cleanupChatWrapper(mod, model);
  const state = wrapper.generateContextState({
    chatHistory: [{ type: "user", text: turn }, { type: "model", response: [] }],
  });
  const tokens = Array.from(state.contextText.tokenize(model.tokenizer));
  out[file.split("/").pop()] = {
    wrapper: wrapper.wrapperName,
    turn,
    json: state.contextText.toJSON(),
    rendered: model.detokenize(tokens, true),
    tokens,
    stopGenerationTriggers: (state.stopGenerationTriggers || []).map((t) => t.toJSON()),
    ignoreStartText: (state.ignoreStartText || []).map((t) => t.toJSON()),
  };
  await model.dispose();
}
console.log(JSON.stringify(out, null, 2));
