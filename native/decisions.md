# Decisions

The record of the port of Earheart from [Electron](https://www.electronjs.org/) to [gesso](https://github.com/wakamex/gesso): what was decided and why, and what was measured. The plan is `plan.md`.

## Reference machines

| Name | Machine | Used for |
|---|---|---|
| Linux | Fedora 44, Ryzen 9 3950X (16 cores, 32 threads), 32 GB, RTX 3090, headless under Xvfb | builds, tests, corpus runs |
| Mac | Mac mini, Apple M2 Pro, 16 GB, macOS 26.6.2, Xcode 26.5 (rented) | macOS builds and tests |

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

The cost: a machine with an NVIDIA card whose driver offers CUDA and no Vulkan runs cleanup on the CPU, where the Electron build would use the GPU. The Linux reference machine is such a machine. Whether Vulkan is slower than CUDA where both work is not yet measured; it needs the Windows machine.

### Catalogue prompts are written out; other models use llama.cpp's template reader (M1, 2026-10-11)

`node-llama-cpp` renders the chat prompt itself, from wrappers it picks per model family. llama.cpp's C interface instead reads the template stored in the model file and supports a fixed list of templates. The native build writes out the prompts of the catalogue's families as `node-llama-cpp` renders them, and `tools/prompt-oracle.mjs` prints the Electron build's tokens for a model so a test can compare. Qwen3 gets ChatML with an empty thought after the assistant header; Gemma 3 gets a BOS token and its turn markers. A model the user adds from Hugging Face that is neither goes through `llama_chat_apply_template`, so its prompt can differ from the Electron build's.

### Sampling follows node-llama-cpp's defaults, including top-k 40 when the setting is 0 (M1, 2026-10-11)

The sampler chain is top-k, top-p, min-p when set, temperature, then a seeded random pick, in `node-llama-cpp`'s order, with its defaults of top-k 40 and top-p 0.95 and no repeat penalty (its penalty defaults to 1, which turns it off). Earheart's "Polished" style sets top-k to 0, meaning off, but `main/util/cleanup-turn.js` forwards top-k only when it is above 0, so the library's default of 40 applies. The native build does the same, to match what users get today.

Cleanup threads: `node-llama-cpp` uses the number of physical cores. SDL reports logical cores only, so the native build uses half of them. This matters only when cleanup runs on the CPU.

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
