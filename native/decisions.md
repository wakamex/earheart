# Decisions

The record of the port of Earheart from [Electron](https://www.electronjs.org/) to [gesso](https://github.com/wakamex/gesso): what was decided and why, and what was measured. The plan is `plan.md`.

## Reference machines

| Name | Machine | Used for |
|---|---|---|
| Linux | Fedora 44, Ryzen 9 3950X (16 cores, 32 threads), 32 GB, RTX 3090, headless under Xvfb | builds, tests, corpus runs |
| Mac | Mac mini, Apple M2 Pro (6 performance and 4 efficiency cores), 16 GB, macOS 26.6.2, Xcode 26.5 (rented) | macOS builds, tests and timings |
| Windows | Windows 11, Ryzen 7 3800XT (8 cores, 16 threads), 32 GB, RTX 3080 | Windows tests and timings |

The Linux host runs other people's jobs at the same time: its load average was between 20 and 70 during the M0 and M1 runs. Memory figures and transcripts from it are sound; its timings are only good for comparing two builds measured side by side. It has no Vulkan driver installed, so llama.cpp's Vulkan backend finds no GPU there.

## Decisions

### Engines are the libraries the Electron build wraps, loaded at run time (M1, 2026-10-11)

The Electron build's two npm packages are wrappers. `sherpa-onnx-node` 1.13.3 wraps [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 1.13.3 and ships its C library `libsherpa-onnx-c-api` with [ONNX Runtime](https://onnxruntime.ai/) 1.24.4. `node-llama-cpp` 3.22.1 wraps [llama.cpp](https://github.com/ggml-org/llama.cpp) at the build its release calls `v0.5.0`, which is llama.cpp's build `b11146`.

The native build calls both through their C headers, vendored in `vendor/include`, and downloads the libraries with `tools/fetch-engines.sh`, which checks each archive's SHA-256. Building ONNX Runtime or llama.cpp from source with Zig was not attempted: prebuilt libraries exist for Linux x86-64, Windows x86-64 and macOS arm64, and running the code the Electron build runs is what makes identical transcripts a fair test.

Speech comes from sherpa-onnx's npm platform packages (`sherpa-onnx-linux-x64` and its siblings), which hold the C library beside the Node addon. These are the very files the Electron build ships. sherpa-onnx's GitHub release of the same version is a different build of the library: with it, 55 of the first 259 corpus clips came out with different words (int8 model), while ONNX Runtime was byte-identical in both. Swapping only that one library for the npm copy made the transcripts match.

Cleanup comes from llama.cpp's own release archives. Its output is sampled with a random seed, so there is no byte-for-byte test to protect, and llama.cpp's release is the plainer source.

The app opens the libraries with `SDL_LoadObject` from an `engines` folder beside the executable instead of linking them. Three things follow. The app starts when an engine is missing and reports it only when that engine is asked for, as the Electron build does. The Windows libraries are built with Microsoft's compiler and the app with Zig's MinGW target, and loading by name needs no import library. Cross-compiling the app needs no engine files at all.

The engines are 93 MB for Linux, 85 MB for Windows and 79 MB for macOS, unpacked. The Linux libraries need glibc 2.34, the system's `libstdc++` and `libgomp`, and `libvulkan` for the GPU backend. gtube and gstream run on glibc 2.27; Earheart's floor is set by the engines, as it is in the Electron build.

### Each engine runs in a worker process of the same executable (M1, 2026-10-11)

`earheart --engine stt` and `earheart --engine cleanup` serve requests on their standard input and output: two 32-bit lengths, a JSON object, then optional bytes (a WAV). The request names follow the Electron worker's (`load-stt`, `transcribe`, `clean`, `prime-cleanup`, `cancel-clean`). This keeps what the Electron build's two `utilityProcess` workers give: speech and cleanup run at the same time, a crash in one engine's native code cannot take down the other or the app, and ending a worker is how its models' memory is given back.

SDL hands a child's output over without waiting, so the app's reading thread sleeps on a semaphore while no request is outstanding and polls every 2 ms only while one is. An idle app with loaded models does not wake up.

### No CUDA libraries on Linux and Windows, pending a measurement (M1, 2026-10-11)

The Electron build ships llama.cpp three times on Linux and Windows: for the CPU, for Vulkan and for NVIDIA's CUDA. The CUDA copies are 508 MB of the 1,059 MB Linux install. The native build takes llama.cpp's Vulkan archive, which also holds the CPU backends.

The cost: a machine with an NVIDIA card whose driver offers CUDA and no Vulkan runs cleanup on the CPU, where the Electron build would use the GPU. The Linux reference machine is such a machine.

On the Windows reference machine the question did not arise: `node-llama-cpp`, with its CUDA packages installed, chose Vulkan there too, so both builds ran cleanup on the same backend (M1 measurements). CUDA against Vulkan on one machine is still unmeasured.

### Catalogue prompts are written out; other models use llama.cpp's template reader (M1, 2026-10-11)

`node-llama-cpp` renders the chat prompt itself, from wrappers it picks per model family. llama.cpp's C interface instead reads the template stored in the model file and supports a fixed list of templates. The native build writes out the prompts of the catalogue's families as `node-llama-cpp` renders them, and `tools/prompt-oracle.mjs` prints the Electron build's tokens for a model so a test can compare. Qwen3 gets ChatML with an empty thought after the assistant header; Gemma 3 gets a BOS token and its turn markers. A model the user adds from Hugging Face that is neither goes through `llama_chat_apply_template`, so its prompt can differ from the Electron build's.

### Sampling follows node-llama-cpp's defaults, including top-k 40 when the setting is 0 (M1, 2026-10-11)

The sampler chain is top-k, top-p, min-p when set, temperature, then a seeded random pick, in `node-llama-cpp`'s order, with its defaults of top-k 40 and top-p 0.95 and no repeat penalty (its penalty defaults to 1, which turns it off). Earheart's "Polished" style sets top-k to 0, meaning off, but `main/util/cleanup-turn.js` forwards top-k only when it is above 0, so the library's default of 40 applies. The native build does the same, to match what users get today.

Cleanup threads: `node-llama-cpp` computes with llama.cpp's count of math cores, which is the physical cores and, on Apple silicon, the performance cores. The native build counts the same (`src/cores.c`). A first version used half the logical processors, 5 on the Mac against 6, and was 7 to 10% slower on the CPU there.

### The app's logic runs as tasks that take turns under one lock (M3, 2026-10-11)

The Electron build's pipeline, live preview and timers all run on one JavaScript thread: between two awaits a function sees state nobody else changes, and its correctness rests on that. C has no awaits. Rewriting the logic as callbacks and hand-made state machines would have changed its shape and its interleavings both.

`src/loop.c` keeps the shape. Each asynchronous function of the JavaScript is a task on a thread of its own that holds one lock while it runs and lets go of it only where the JavaScript awaits: an engine call, a sleep. The lock is taken in turns, and a task's turn is taken when it is spawned, so tasks start in the order they were spawned; engine requests likewise go out in the order tasks made them (`eh_engine_reserve`). A timer cancelled under the lock never fires, as with `clearTimeout`. The window's thread will take the same lock to read state for a frame.

The cost is a thread per task in flight, a handful at most.

### Ported logic is tested against the JavaScript it came from (M3, 2026-10-11)

`tools/gen-vectors.js` runs the Electron build's own modules on generated inputs and writes inputs and results to `tests/vectors/`; the C tests replay them and compare exactly. Audio is rebuilt in C from a seed with the Mulberry32 generator, which gesso reproduces bit for bit, so the files hold recipes and not samples. Covered so far: the silence splitter and speech probe (200 recording and option pairs), the chunked decoder under a scripted fake engine (120 dictations), the stumble stripper and word comparison (1,463 texts), settings merging and migration (18 files) and cleanup prompt resolution (7 sections).

The stumble stripper is regular expressions over Unicode classes in JavaScript and hand-written scanners in C. `tools/gen-unicode.js` writes the class and case tables from Node's own Unicode data, so other scripts behave as they do there.

### Own data folder; the Electron build's models used in place (M3, 2026-10-11)

Settings, history and downloads live in a folder of this build's own (`earheart-native` under the system's place for application data, or `$EARHEART_DATA`). On a first run the Electron build's `settings.json` is taken over, without the API keys that only that program can decrypt. Models are looked for in this build's folder and then in the Electron build's, by its own completion markers and fingerprints, so nothing is downloaded twice and nothing of the Electron build's is written to.

The saved microphone is matched by SDL's device name. An id saved by the Electron build is Chromium's and matches nothing, so such a setting falls back to the system's default microphone with the overlay's "Saved mic missing" remark.

## Measurements

### M0: the Electron build, v0.39.0 (2026-10-11)

#### What the downloads contain

The v0.39.0 packages unpacked, in MB:

| Part | Linux (deb) | Windows (portable) | macOS arm64 (zip) |
|---|---|---|---|
| Download | 390 | 396 | 182 |
| Installed | 1,059 | 1,056 | 411 |
| Electron: Chromium, Node and their resources | 312 | 356 | 276 |
| llama.cpp through node-llama-cpp | 710 | 671 | 56 |
| of which CUDA libraries | 508 | 520 | none |
| sherpa-onnx with ONNX Runtime | 30 | 21 | 73 |
| The app's own code (`app.asar`) | 8 | 8 | 8 |

On Linux and Windows two thirds of the install is llama.cpp, and most of that is CUDA. Electron itself is 30% of the Linux install and 67% of the macOS one. The AppImage download is larger still, 525 MB.

#### Memory, idle

The app started with `--hidden` on Linux under Xvfb, no models loaded, measured as proportional set size over all its processes with `tools/mem.py`:

| Process | MiB |
|---|---|
| Main | 111 |
| GPU process | 55 |
| Settings window (it opened to report a missing speech model) | 46 |
| Overlay window | 35 |
| Network service | 24 |
| Audio service | 22 |
| Two zygotes | 20 |
| Total | 313 |

Without the Settings window the idle total is 267 MiB. gtube and gstream, the two released gesso apps, use 22 to 26 MiB.

#### Memory and latency while dictating

`scripts/e2e-latency.js` drives the real pipeline with a recording played as the microphone: 30 seconds of speech, live preview and cleanup on, the default models (Parakeet TDT 0.6B v3 full precision and Qwen3 4B), two runs in one process. Memory was sampled once a second.

| Process | MiB with both models loaded |
|---|---|
| Speech worker | 2,411 |
| Cleanup worker (its weights are in GPU memory, through CUDA) | 768 |
| Main, GPU process, overlay, services, zygotes | 267 |
| Total | 3,446 |

The peak during the run was 4,748 MiB. Electron's own processes are 8% of the loaded total.

| Measure | Run 1 | Run 2 |
|---|---|---|
| Stop to delivered text | 6.46 s | 6.19 s |
| of which decoding the recording's tail on the CPU | 5.84 s | 5.52 s |
| of which cleanup on the RTX 3090 | 0.60 s | 0.64 s |

The machine was under load from other jobs, so these times are for comparison with a native run made beside them, not figures to quote.

Not measured in M0: cold start and hotkey to "Listening". Both need a real desktop session, and the Linux host has none. They are measured for both builds together in M2.

#### What the port can and cannot save

Download: the Linux package is 390 MB. Without Electron and without CUDA the native build's engines are 93 MB unpacked, before compression, plus an executable of a few megabytes.

Memory: about 270 MiB of Electron processes, against 3.4 GiB with the default models loaded. The port removes 8% of the memory of a session with both models in use and nearly all of it when the models are unloaded after idling.

## Parity list

Every surface of the Electron build at v0.39.0 and what the native build does with it. "Kept" means the same behaviour and wording.

### Command line

| Electron build | Native build |
|---|---|
| `--toggle`, `--pause`, `--discard`, `--correct-last` forwarded to the running instance | kept |
| `--hidden` starts in the tray without the wizard | kept |
| `--smoke-test`, `--engine-check` for CI | kept |
| | added: `transcribe` and `clean` subcommands, which the evaluation scripts drive |

### Tray

| Electron build | Native build |
|---|---|
| Start or stop dictation, Cancel while one is in progress | kept |
| Output mode: paste, paste and keep on clipboard, clipboard only | kept |
| Copy last transcription, Correct last transcription | kept |
| Settings, Quit | kept |
| Update to vX, Downloading update, Restart to update | after M5, with the updater |

### Overlay

| Electron build | Native build |
|---|---|
| Strip at the bottom of the screen that never takes focus, above other windows, on every workspace | kept |
| Status dot, status text, timer, live waveform | kept |
| Live transcript in two layers: raw words, then the cleaned text settling in behind | kept |
| Transcribing and cleaning progress bars paced by measured decode speed | kept |
| Pause, discard and copy keys; done card with the result, marked when incomplete | kept |
| Position remembered between runs | kept |
| Update prompt with "Skip this version" and "Don't remind me" | after M5, with the updater |

### Settings

| Electron build | Native build |
|---|---|
| General: record, pause, discard and correct hotkeys with capture, reset and clear | kept |
| General: microphone picker, output mode, start on login | kept; start on login comes with M7's early pieces |
| General: "Fix auto-paste permission" on macOS | kept, for Accessibility only: native paste needs no Automation permission |
| Live transcript on or off | kept |
| Speech-to-text: on this computer or OpenAI-compatible; model picker with download, progress and removal | kept |
| Speech-to-text: add a model from Hugging Face (find versions, browse, add) | kept |
| Speech-to-text remote: base URL, API key, model, language, fetch models, test connection | kept |
| Cleanup: on or off, on this computer or OpenAI-compatible, model picker, Hugging Face models | kept |
| Cleanup remote: base URL, API key, model, fetch models, test connection | kept |
| Cleanup style: preset slider (Verbatim, Clean, Polished) or custom temperature, top-p, top-k, min-p | kept |
| Preferred terms, editable prompt with reset | kept |
| History: on or off, clear, paged list | kept |
| Advanced: maximum dictation length, unload models after idle, run the wizard again, open the error log | kept |
| Updates: check automatically, remind, check now, skip this version | after M5, with the updater |
| An index that follows the scroll, with keyboard focus handed between index and sections | kept |

### Wizard

| Electron build | Native build |
|---|---|
| Welcome, hotkey, microphone, speech-to-text, cleanup with quality and style, output, model download with progress | kept |
| Skip setup, Back, Download later | kept |
| Wayland note about binding a system shortcut | kept |

### Correction window

| Electron build | Native build |
|---|---|
| Edit the last transcript, select a range, enter its correct spelling | kept |
| Replace and remember (adds to preferred terms), Replace only, Undo replacement | kept |
| Copy corrected prompt and close, Ctrl or Cmd+Enter | kept |

### Notifications

| Electron build | Native build |
|---|---|
| Ready; no speech model; speech model not downloaded; hotkey not working | kept |
| Cleanup failed, used raw transcript; transcription interrupted; auto-paste failed, copied to clipboard; dictation failed | kept |
| Could not copy transcript; could not change the output mode | kept |
| Each opens Settings when clicked | kept |
| Update available | after M5, with the updater |

### Data

| Electron build | Native build |
|---|---|
| `settings.json` with deep-merged defaults, written atomically, owner-only | its own file in its own folder; the Electron build's settings are imported on first run |
| Remote API keys encrypted by Electron's `safeStorage` | cannot be read by another program; entered again, then kept through gesso's secret storage |
| History JSON | its own file |
| Models under `userData/models` with `.complete` markers | read in place from the Electron build's folder when it exists, so nothing is downloaded twice |
| `earheart.log` | its own file |

### Not carried over

| Electron build | Why |
|---|---|
| Screen-reader access, which Chromium provides without any work in the app | gesso draws its own interface and has no accessibility bridge yet. `PRODUCT.md` leaves screen-reader support undecided. Every flow stays usable from the keyboard. |
| CUDA for cleanup | see the decision above |
| The optional Python server in `stt-server/` | unchanged and separate: it is a remote engine either build can point at |

### M1: engines from C (2026-10-11)

#### Speech: identical transcripts

All 647 clips of the evaluation corpus (FLEURS en_us test, as `scripts/eval-stt.js` prepares it), both shipped Parakeet v3 models, 8 threads. On Linux the reference is the Electron build's own worker through `scripts/eval-stt.js`. On macOS and Windows it is `tools/stt-oracle.js`, the same addon and settings without Electron, which on Linux matches the worker on all 647 clips.

| System | Model | Identical transcripts | Decode time, reference | Decode time, native |
|---|---|---|---|---|
| Linux | int8 | 647 of 647 | not comparable (machine under load) | |
| Linux | full precision | 647 of 647 | not comparable | |
| macOS | int8 | 647 of 647 | 169.0 s | 160.0 s |
| macOS | full precision | 647 of 647 | 266.8 s | 257.2 s |
| Windows | int8 | 647 of 647 | 433.9 s | 430.6 s |
| Windows | full precision | 647 of 647 | 414.1 s | 412.4 s |

Transcripts differ between systems in both builds alike: 174 of the 647 int8 transcripts on macOS differ from Linux's. Each system is therefore compared with its own reference.

#### Cleanup: same prompts, same quality, same speed

Prompt tokens for Qwen3 4B, Gemma 3 1B, Gemma 3 4B and Granite 4.0 Micro are identical to `node-llama-cpp`'s (`zig build test` with `EARHEART_MODELS` set).

`scripts/bench-cleanup.mjs` and its native twin `tools/bench-cleanup-native.mjs`: the same inputs, styles, seeds and scoring, 20 cleans per model on the CPU and 5 on the GPU. Median seconds per clean of the FLUENT input in the default style:

| System | Model | Backend | Electron build | Native build |
|---|---|---|---|---|
| macOS | Gemma 3 1B | CPU, 6 threads | 2.40 | 2.36 |
| macOS | Gemma 3 1B | Metal | 1.94 | 1.93 |
| macOS | Qwen3 4B | CPU, 6 threads | 11.89 | 11.50 |
| macOS | Qwen3 4B | Metal | 5.56 | 5.40 |
| Windows | Gemma 3 1B | CPU, 8 threads | 8.42 | 5.21 |
| Windows | Gemma 3 1B | Vulkan | 1.08 | 1.00 |

On the Windows CPU the native build is 1.6 times faster. llama.cpp's own release holds a CPU backend per processor generation and picks one at run time; `node-llama-cpp`'s Windows package is one generic build.

Quality by the benchmark's scoring, macOS, Qwen3 4B, the shipped default: no fillers left, no fidelity failures and no runaway replies in either build; median content-word retention 0.94 in both for FLUENT in the default style (the figure `docs/cleanup-models.md` records), 0.90 in both for the polished style, 0.97 in both for REPORTED.

With the same seed the two builds write byte-identical text in 9 of 10 cleans on Metal. On the CPU 16 of 40 are identical: the CPU backends are different builds whose sums round differently, and one different token changes what follows. Gemma 3 1B is the more sensitive: its FLUENT/polished retention is 0.86 in the Electron build and 0.77 in the native one over five seeds, with the same count of fidelity failures.

#### What M1 settled

- Linking: nothing is linked. Both engines load at run time on all three systems, and CI builds and loads them on Linux, Windows and macOS on every push (`.github/workflows/native.yml`).
- Two things Windows needed: the engines folder made a DLL directory, since `LoadLibrary` does not look beside a library for its imports; and llama.cpp's functions looked up across its three libraries.
- Chat templates: written out per family and token-identical for the catalogue.
- GPU: Metal on the Mac and Vulkan on Windows in both builds. The Linux reference machine has no Vulkan driver, so the native build cleans on its CPU there, where the Electron build uses CUDA.

Not yet measured: Qwen3 4B on Windows (in progress when this was written), and Chromium's microphone processing against SDL's raw audio, which belongs to M2.
