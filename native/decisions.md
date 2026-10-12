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

### Hotkeys, paste, one instance, the clipboard and the overlay window are gesso's (M2, 2026-10-11; from gesso 2026-10-12)

These are gesso's `gs_desk` (gesso pinned at `341834b`). They were written in the app for M2, a file per system, and went into gesso from there; the app's copies are gone. How each works:

- Hotkeys: X11 key grabs on a connection of the app's own, with libX11 opened at run time; `RegisterHotKey` on a thread with its own message queue; Carbon's `RegisterEventHotKey` on macOS, which needs no permission.
- The paste keystroke: on Linux the tools the Electron build runs, in its order (`wtype` or `ydotool` on Wayland, then `xdotool`); `SendInput` on Windows in place of starting PowerShell; a posted Cmd+V on macOS in place of `osascript`, so the Automation permission is no longer needed and Accessibility alone is.
- One instance: a socket in the data folder on Linux and macOS, a named pipe on Windows. A later instance hands over `--toggle`, `--pause` or `--discard` and exits.
- The clipboard's old contents: SDL lists the formats of what it put on the clipboard itself and, on macOS, of nothing else, so a snapshot through SDL came back empty there and the user's clipboard was not restored. macOS copies the pasteboard's items whole through AppKit, Windows copies every memory-backed format, and Linux keeps SDL's formats, which X11 reports for any owner.

### macOS: a bundle, a background app, and a named signing identity (M2, 2026-10-11)

`zig build bundle -Didentity=NAME` makes `Earheart.app` with gesso's bundle script: the executable and its engines, an `Info.plist` with the microphone usage string and `LSUIElement`, the icon set, and a signature. SDL also has to be told the app is a background one (`SDL_HINT_MAC_BACKGROUND_APP`), or it puts it in the Dock regardless.

macOS files the Accessibility grant under the app's designated requirement. An ad hoc signature gives each build a new one, and the grant was lost on the first rebuild. Signed with a self-signed certificate the requirement is `identifier "dev.wakamex.earheart" and certificate leaf = H"…"`, the same on every build, which is the arrangement `CONTRIBUTING.md` describes for the Electron build.

When a paste is refused for want of Accessibility the app has the system ask, once a run (`AXIsProcessTrustedWithOptions`), which is also what lists the app in System Settings for the user to switch on.

### Linux builds target glibc 2.34 (M2, 2026-10-11)

A build made on the development host needs that host's glibc (2.43) and does not start on Ubuntu 24.04. Release builds name the floor the engines set: `zig build -Dtarget=x86_64-linux-gnu.2.34`.

### The tray on Linux needs the AppIndicator library (M2, 2026-10-11)

SDL's tray loads `libayatana-appindicator` at run time and fails without it ("Could not load AppIndicator libraries"), as on the Fedora development host and on GitHub's Ubuntu runners. The app then runs without a tray, driven by its hotkeys and by `earheart --toggle`. Electron's tray speaks the StatusNotifierItem protocol itself. This is on gesso's list.

### Notices are desktop notifications, and the settings where there are none (2026-10-12)

The notices of `main/setup-notices.js` and the pipeline's are shown through gesso's `gs_notify`: "Earheart is ready" with the hotkey on a start that is not hidden, the speech model missing or unknown, the record hotkey refused on a hidden start, and what the pipeline reports while dictating. A click on any of them opens the settings. Where the desktop cannot show one (no notification service, or a bare executable on macOS) the settings window opens in its place, as the Electron build does, and every notice is in the log either way. They are queued by whichever task raises them and shown by the main thread.

Seen working on the rented Mac (macOS 26, the signed bundle): the system asked once for leave to notify, and after it was given a start showed "Earheart is ready — Press Cmd+Shift+Space to dictate." in the Notification Centre, and a click on it opened the settings window in front. The notice raised before the leave was given was dropped by the system, as gesso documents. On the Linux host, which has no notification service, a start that is not hidden opened the settings window in the notice's place. Not yet seen on a screen: Windows, and a Linux desktop with a notification service.

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
| Tooltips on the keys, naming the discard hotkey, and on a detail line cut at two lines | not carried yet: the overlay draws itself and is not one of gesso's gs_ui windows, which have tooltips. A cut detail's full text is in the log and in the notification |
| Status in semibold, timer in a monospace face | kept: gesso's system font at weight 600 and its monospace face |

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
| An index that follows the scroll, with keyboard focus handed between index and sections | the index follows the scroll and jumps to a section; after a jump the focus stays on the index entry, where the Electron build moves it into the section |
| API key fields that never show the key | shown as dots except while the field has the keyboard |
| Remote model names offered as completions in the model field | offered in a list under the field after Fetch models |
| Section icons | a coloured dot per section |
| An open form is told when the dictionary, output mode or reminders change elsewhere | such changes are merged when the form is saved; an open form does not show them until then |

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
| A question with Keep editing and Discard draft when closing with a draft | kept |
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
| Windows | Qwen3 4B | CPU, 8 threads | 32.27 | 27.94 |
| Windows | Qwen3 4B | Vulkan | 1.88 | 1.79 |

On the Windows CPU the native build is 1.6 times faster. llama.cpp's own release holds a CPU backend per processor generation and picks one at run time; `node-llama-cpp`'s Windows package is one generic build.

Quality by the benchmark's scoring, Qwen3 4B, the shipped default, on macOS and on Windows: no fillers left, no fidelity failures and no runaway replies in either build; median content-word retention 0.94 in both for FLUENT in the default style (the figure `docs/cleanup-models.md` records). On macOS the polished style gives 0.90 in both and REPORTED 0.97 in both; on Windows the polished style gives 0.92 in the Electron build and 0.90 in the native one, and REPORTED 0.93 and 0.97.

With the same seed the two builds write byte-identical text in 9 of 10 cleans on Metal. On the CPU 16 of 40 are identical: the CPU backends are different builds whose sums round differently, and one different token changes what follows. Gemma 3 1B is the more sensitive: its FLUENT/polished retention is 0.86 in the Electron build and 0.77 in the native one over five seeds, with the same count of fidelity failures.

#### What M1 settled

- Linking: nothing is linked. Both engines load at run time on all three systems, and CI builds and loads them on Linux, Windows and macOS on every push (`.github/workflows/native.yml`).
- Two things Windows needed: the engines folder made a DLL directory, since `LoadLibrary` does not look beside a library for its imports; and llama.cpp's functions looked up across its three libraries.
- Chat templates: written out per family and token-identical for the catalogue.
- GPU: Metal on the Mac and Vulkan on Windows in both builds. The Linux reference machine has no Vulkan driver, so the native build cleans on its CPU there, where the Electron build uses CUDA.

Chromium's microphone processing against SDL's raw audio belongs to M2.

### M2: tray, hotkey, record, paste (2026-10-11)

`tests/e2e.sh` runs a dictation as a user makes it: the app in the tray, the default record hotkey pressed, a FLEURS clip played as the microphone, the hotkey pressed again, and the text expected in a focused window (`tests/paste_target.c`), with the clipboard's old contents back afterwards; then the same started and stopped by `earheart --toggle`.

| System | Where | Hotkey | Paste into the focused window | Clipboard put back | Second instance |
|---|---|---|---|---|---|
| Linux X11 | Xvfb on the development host, an Ubuntu 24.04 container, and CI (ubuntu-24.04) | pressed with xdotool: pass | pass | pass | pass |
| Windows | CI (windows-2025) | pressed with `SendInput`: pass | pass | pass | pass |
| macOS | the rented Mac, through its remote desktop | the real Cmd+Shift+Space: pass | into TextEdit, after Accessibility was granted: pass | pass | pass |

On macOS without Accessibility the dictation ends with the text on the clipboard, the notice "Accessibility permission is off" and the system's prompt, as designed.

Booting the whole app and exiting (`--smoke-test`), both builds inside one X display on the Linux host, three runs each:

| Build | Time | Peak memory of the main process |
|---|---|---|
| Electron | 2.26 to 2.29 s | 182 to 184 MB |
| Native | 0.07 to 0.09 s | 8 to 9 MB |

Idle in the tray with no models loaded the native build is one process of 6 MiB, against the Electron build's 267 MiB. 
#### Chromium's microphone processing is worth 2.6 points of word error rate in a noisy room

The Electron build records through Chromium with echo cancellation, noise suppression and automatic gain on (`renderer/microphone.js`); the native build records what SDL hands it. `tools/eval-mic-processing.mjs` plays each of 40 FLEURS clips into the Electron build's real overlay as the microphone, keeps what the overlay captured, and transcribes both that and the clip as played with the same engine (Parakeet TDT 0.6B v3 int8). The table gives the word error rate (WER) of each against the reference text, lower is better, under three conditions: the clip as it is, 20 dB quieter (a low-gain microphone), and with white noise 15 dB below the speech. The last column is native minus Electron in percentage points, with the middle 95% of 10,000 resamplings of the clips.

| Condition | Clips | WER of the audio as played (what the native build records) | WER of the audio as Chromium captured it (what the Electron build records) | Difference in points, with 95% interval |
|---|---|---|---|---|
| As recorded | 40 | 6.07% | 5.29% | 0.79 (-0.12 to 1.82) |
| 20 dB quieter | 40 | 6.52% | 5.62% | 0.90 (-0.23 to 2.07) |
| Noise 15 dB below speech | 40 | 9.79% | 7.20% | 2.59 (0.34 to 5.32) |

The native build transcribes noisy audio 2.59 points worse. On clean and quiet audio the difference leans the same way and its interval includes zero. Decision: the native build records unprocessed audio through M5, and a noise suppressor is tried on these same clips before delivery; the candidates are the systems' own voice processing and [RNNoise](https://github.com/xiph/rnnoise). This is a gap against the Electron build until that closes it.

#### The overlay does not take the keyboard

With the overlay of M4 on screen, `tests/e2e.sh` checks mid-recording that the paste target still has the keyboard focus, and the paste then lands in it. On Linux this holds under a window manager (Openbox in CI). On an X display with no window manager SDL gives the keyboard to every window it shows, so the test starts one.

Not measured: hotkey to "Listening" in the Electron build, which has no marker to time against. In the native build the recorder is live within the same second as the key press on all three systems.

### M3: pipeline parity (2026-10-11)

Ported and compared exactly against the JavaScript (`tools/gen-vectors.js`, `zig build test`):

| Logic | Source | Cases |
|---|---|---|
| Silence splitter, pause squeezing, cap cuts, sound seconds | `main/util/split-silence.js` | 40 seeded recordings under 5 option sets |
| Forced boundary search | `renderer/chunk-boundary.js` | the same recordings |
| Speech probe | `renderer/speech-probe.js` | the same, and 6 edge cases |
| Chunked decoder: retries, padded second try, noise pieces, salvage, the stop after dead groups | `main/chunked-decode.js` | 120 dictations under a scripted fake engine, 6 failing outright, 34 partial, 56 with salvage |
| Stumble stripper | `main/util/stumble-strip.js` | 1,146 texts, 603 of them changed |
| Live preview's word comparison | `main/live-preview.js` | 311 pairs |
| Settings merge, migrations, limits | `main/settings.js` | 18 stored files |
| Cleanup prompt and sampling | `main/cleanup-styles.js` | 7 sections |

The pipeline (`src/pipeline.c`), the live preview (`src/preview.c`) and the recorder (`src/recorder.c`) are line-for-line ports that have no pure function to compare; they are tested whole, by `earheart dictate` and `tests/e2e.sh`.

Long recordings, `tools/eval-long-decode-native.js` (the recordings, baseline and gate of `scripts/eval-long-decode.js`), default model, Linux:

| Recording | Audio | Short-clip WER | Pieces | Longest piece | Word ratio | WER | Gate |
|---|---|---|---|---|---|---|---|
| read | 124.5 s | 4.2% | 18 | 10.5 s | 0.981 | 5.1% | pass |
| read | 182.9 s | 4.0% | 26 | 14.3 s | 0.990 | 4.6% | pass |
| read | 310.9 s | 5.7% | 44 | 14.7 s | 0.989 | 5.9% | pass |
| dictation | 155.2 s | 4.2% | 24 | 11.2 s | 0.990 | 5.5% | pass |
| dictation | 225.0 s | 4.0% | 32 | 14.7 s | 0.992 | 5.2% | pass |
| dictation | 382.4 s | 5.7% | 54 | 16.1 s | 0.995 | 6.2% | pass |

The gate is a word ratio of at least 0.95 and a WER no more than 3 points above the recording's own sentences decoded one clip at a time.

The speech engine dying: `earheart dictate --kill-stt-at-stop` makes every decode from the stop on kill the worker, the retry on a fresh worker included. The dictation still ends "done": the text the live preview had committed is delivered and written to history marked incomplete, with the "transcription interrupted" notice. `tests/e2e.sh` checks this on every CI run.

The remote (OpenAI-compatible) engines: `tests/remote.sh` runs a dictation transcribed and cleaned over HTTP against a stand-in service (`tests/fake_openai.py`) and checks the upload is a well-formed form with the model, language and API key, that the cleanup request carries no sampling settings strict servers refuse, and that a 401 comes back as "STT service error 401 — check the API key in Settings" without repeating the service's answer. It runs on all three systems in CI. The stand-in is written for this test, so the request shapes are checked against the Electron client's code and not against a real service.

### M4: overlay (2026-10-11)

The overlay is `src/overlay.c` on the drawing helpers of `src/draw.c`: a 500 point bar at the bottom centre of the main display with the live transcript in two layers, the state and its dot, the waveform, the timer, the phase's progress bar and the pause, done and discard keys. It fades over 200 ms, grows upward with the transcript, is dragged by its body, and remembers where it was put.

| Check | How | Result |
|---|---|---|
| States, keys, timer, countdown to the cap, progress and its hold, detail lines, the card growing | `tests/overlay.c`, a port of the overlay's own checks in `scripts/overlay-smoke.js`: a dictation's views stepped through the overlay's state and drawn without a window, with the layout and the pixels read back | its 44 checks pass on Linux, macOS and Windows in CI |
| The app dictated into keeps the keyboard | `tests/e2e.sh` asks the window system mid-recording (Linux, under Openbox), and on all three systems the paste then lands in the focused test window | pass in CI on ubuntu-24.04, windows-2025 and macos-15; on the rented Mac the paste landed in TextEdit with the overlay showing |
| The look | `earheart overlay-shot` frames beside the README's screenshots; a screenshot of each system's screen mid-recording kept by CI | the same layout, colours and marks; the differences are the two in the parity list (font weight and face) |

The microphone's own checks of `scripts/overlay-smoke.js` (a missing device falling back once, a stale stream from an abandoned start, the paused span left out of the capture) belong to `src/recorder.c` here and are not ported as tests yet; the recorder is covered by `tests/e2e.sh` and `earheart dictate`.

Decisions:

- The overlay draws itself with gesso's `gs_draw` shapes on SDL's renderer, in a window from `gs_overlay_window`; it is not a gs_ui window.
- Its state is a pure step from what the app says to a model, so it is tested without a window or a display.
- On macOS the paste is a Command key press around the V. A V carrying only the Command flag is read as a paste by Cocoa apps and as a plain V by apps that track modifier keys themselves, SDL's among them.

Found while testing it, and fixed: the engine client writing to a worker whose pipe had just closed (Windows), the start-up check waiting on a hotkey registration that waited on it (macOS), a second instance's request arriving before the first had finished starting (a crash), and the bar staying where the system first put it until the transcript grew.

### M5: settings, wizard, correction, models (2026-10-12)

The three framed windows exist and are driven end to end by tests that give the real windows mouse, wheel and keyboard events and check what they then show and save. They pass on Linux (under Openbox), macOS and Windows in CI.

| Window | Test | Checks | What is checked |
|---|---|---|---|
| Settings | `earheart settings-smoke` | 114 | every control a Tab stop and scrolled into view by Tab; the index gliding to a section and following the wheel; hotkey capture, Escape, Clear; cleanup off putting its fields out of reach; engine and style choices swapping their fields; an unreachable service reported; a Hugging Face address refused before the network is asked; a number outside its limits saved as the nearest with the window saying so; a hotkey another app holds keeping its saved value with the reason under the field; Save closing the window |
| Settings | the same, last part | 18 files | settings files as v0.39.0 writes them come back unchanged from an untouched form |
| Wizard | `earheart wizard-smoke` | 59 | seven steps forward and back; capture and Reset; cleanup off; the download put off; a refused record hotkey sending the user back to its step with nothing closed; Finish saving and opening the settings with the wizard's line; run again from the settings and skipped |
| Correction editor | `earheart correction-smoke` | 43 | picking by arrow keys and Shift; Replace and remember through Enter, the dictionary gaining the spelling; Undo; Replace only; Escape giving up a pick; an unapplied spelling stopping the copy; a second transcript not replacing a draft; the question on closing with a draft; Ctrl+Enter copying to the clipboard and closing |

Logic ported and compared exactly against the JavaScript:

| Logic | Source | Cases |
|---|---|---|
| Hotkey manager: pair changes, the armed discard hotkey, the correction hotkey, collisions, rollback, suspend | `main/hotkeys.js` | 2,140 steps in 250 random sequences, per system |
| Dictionary: terms, a form's edits merged into the live list, a spelling remembered | `renderer/vocabulary.js` | 300 merges, 70 spellings |
| Hugging Face lookup: address parsing, versions of a repository, model definitions, refusals | `main/services/hf-models.js` | 28 addresses, 173 repositories (83 usable, 18 kinds of refusal) |

Models: the Electron build's downloaded models are found where they are and not downloaded again (the wizard's last step shows them Ready, and `tests/e2e.sh` dictates with a model fetched once and cached). A model added from Hugging Face was taken through the whole path by hand: `csukuangfj/sherpa-onnx-whisper-tiny.en` (int8, two files with a published checksum and one without) looked up, added, downloaded, and dictated with; its completion marker carries the fingerprint the Electron build computes for the same definition.

Memory, one process, no models loaded:

| | Linux host (software OpenGL under Xvfb) | Mac (Metal) | Electron build, Linux (M0) |
|---|---|---|---|
| In the tray | 12 MiB | 13 MB footprint | 267 MiB |
| Settings window open | 147 MiB resident | 58 MB footprint | 313 MiB |

The Linux figure for the open window is mostly the software OpenGL stack that Xvfb forces; it is not what a desktop with a graphics driver shows, and was not measured there.

Decisions:

- The windows are built on gesso's gs_ui (pinned at `341834b`), which has the Tab order with its focus mark, disabled controls, the scrolling page, switches, radio buttons, the slider with stops, masked fields, the hotkey field and modal questions. `src/ui.c` is what is the app's own: the window with its icon, the colours, the button kinds (drawn through `gs_ui_activated`), and the list of controls the window tests look things up in. An earlier `src/ui.c` built all of those controls itself; gesso had them by the time M5 was written up, and the app moved onto them the same day.
- Save goes through one path for the settings window and the wizard (`src/commit.c`), which is the Electron build's: the app's own fields and those changed elsewhere come from the live settings, hotkeys change first, a refused one keeps its saved value.
- The hotkey rules are behind a backend of two functions, so they are tested without a desktop and the desktop code only grabs and releases keys.
- API keys are kept apart from the settings file, through gesso's `gs_secret`: in the login Keychain on macOS, the desktop's Secret Service on Linux, encrypted for the user's account on Windows, and in a file only the user can read where the system has no such place. The settings file never holds a key; one found there from an earlier version is moved out on the next start. The Electron build's own encrypted keys cannot be read by another program, so after taking over its settings the keys are entered once more.
- The correction editor's transcript is gesso's text area, and what is selected in it (`gs_ui_textarea_selection`) is what gets replaced, as in the Electron build. An earlier version showed the transcript as words to pick, from before gesso's text area reported its selection.
- A file from Hugging Face without a checksum is checked by its length, and one without a known length is fetched whole each time; the Electron build also compares a resumed download's HTTP validators, which the app does not do yet; gesso's HTTP module gives a response's headers as of the pin.
- The overlay's window exists only while it shows. Kept from start-up it took the tray app from 12 MiB to 85 MiB on the Linux host.
- On macOS a window shown by the app activates it, since an app without a Dock icon is not made active by showing a window.

Found while testing, and fixed: the events that wake the main thread carried no type, so the tray menu was never rebuilt after start-up; Xlib's default handler ended the process on an error for a window closed a moment before; a quit signal was not noticed on macOS until some other event arrived, 20 seconds with the settings window open.

Not in M5, by the plan: start on login, the keychain, notifications, the updater and its settings section. Open from M2: a noise suppressor to close the 2.6 point gap in noisy rooms.

