# Porting Earheart from Electron to gesso

Status: in progress on the `feat/native-port` branch of `wakamex/earheart`. Written 2026-10-11 against Earheart v0.39.0. Decisions and measurements are recorded in `native/decisions.md`.

## Question and short answer

Can Earheart, today an [Electron](https://www.electronjs.org/) app in plain JavaScript, become a native C app on [gesso](https://github.com/wakamex/gesso), the small C library on [Simple DirectMedia Layer 3 (SDL3)](https://www.libsdl.org/) that gtube and gstream are built on?

Yes, as a rewrite in nine milestones. The speech and cleanup models link through C interfaces that already exist, and gesso already covers downloads, checksums, settings windows and text. Four things Earheart depends on exist in neither SDL3 nor gesso and have to be written per operating system: global hotkeys, sending a paste keystroke to another app, desktop notifications, and a tray-only app with a non-focusable overlay on every desktop. macOS matters to Earheart's maintainer and no gesso app has been released there, so the port treats macOS as a target of every milestone and is the occasion to bring gesso's macOS support to the level of Linux and Windows. gesso and gtube already build and run on Apple silicon.

The port removes Chromium and Node from the install and from memory. It does not shrink the models, which stay about 5 GB on disk (about 2.5 GB each for speech and cleanup) and are the larger share of memory while loaded.

## What exists today

### Earheart

| Part | Size | Role |
|---|---|---|
| `main/` and `preload.js` | 11,051 lines | pipeline, hotkeys, settings, tray, windows, updater, engine workers |
| `renderer/` | 7,580 lines | overlay, settings, wizard and correction pages (HTML, CSS, JS) |
| `scripts/` | 8,389 lines | smoke tests, screenshots, release automation, model evaluations |
| `test/` | 22,901 lines, 1,097 tests in 54 files | unit and contract tests on `node --test` |

Runtime dependencies are two native engines: `sherpa-onnx-node` for speech-to-text and `node-llama-cpp` for cleanup. Each runs in its own Electron `utilityProcess`, so a crash in one cannot take down the other or the app.

Release downloads for v0.39.0: Windows installer 396 MB, Linux AppImage 525 MB, Linux deb 390 MB, macOS dmg 181 MB (Apple Silicon only).

### gesso and its apps

gesso is 8,614 lines of C in 28 modules. The apps on it give the evidence for what a port can expect:

| App | State | Relevant precedent |
|---|---|---|
| gtube | released for Linux and Windows | one 4.9 MB executable, about 26 MB of memory while playing; AppKit code in an Objective-C file for macOS; release workflow with checksums and attestations |
| gstream | released for Linux and Windows | a rewrite of a Qt app done as numbered milestones with a parity list and recorded measurements in `decisions.md`; idles at 22 MB |
| gsignal | in progress | links a Rust static library under a C header; already lists desktop notifications and a tray icon as needs; three gesso interface requests are pending |

gesso's own tests pass on macOS 15 in GitHub Actions. No gesso app has a macOS release: gtube and gstream ship Linux and Windows archives only, and the project has no Apple developer account.

### macOS, tested 2026-10-11

A rented Mac mini is available for testing: Apple M2 Pro, 16 GB, macOS 26.6.2, Xcode 26.5, reachable over SSH with a desktop session logged in. On it, with Zig 0.17.0 and nothing else installed:

| Check | Result |
|---|---|
| `zig build test` in gesso at commit 521ce3a | all 12 test groups pass, 28 s from a clean clone including building SDL3 |
| `zig build test -Dvideo=true` in gesso | passes |
| `zig build --release=fast` in gtube v0.2.1 | a 4.5 MB arm64 executable linked against AppKit, Metal, CoreAudio and WebKit, ad hoc signed by the linker |
| `gtube --demo --shot` | exits 0 and writes a frame with the same layout as the Linux build's |

gesso takes SDL3 from the [castholm/SDL](https://github.com/castholm/SDL) Zig package. That package builds SDL natively on a Mac without trouble, as the results above show. Its limit is cross-compiling: for a macOS target on another host its `build.zig` stops unless it is given the macOS SDK's include, framework and library paths. Linux and Windows builds of gesso apps all come from one Linux host today; macOS builds have to come from a Mac or a macOS CI runner, which changes the shared release workflow.

Untested on macOS so far: any gesso window shown on a real display, the tray, the microphone, and everything that needs a permission prompt.

## Target architecture

One executable, `earheart`, written in C on gesso and built with [Zig](https://ziglang.org) 0.17, plus the engines' shared libraries beside it.

- App process: tray, hotkeys, microphone, overlay, settings, wizard and correction windows, pipeline state machine, history, updater.
- Two engine processes: the same executable started as `earheart --engine stt` and `earheart --engine cleanup`, talking to the app over their standard input and output. This keeps the crash isolation the Electron workers give, which the "never lose the user's words" rule depends on: when the speech engine dies mid-recording, the words already decoded are still delivered.
- Speech engine: the [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) C interface, the same library `sherpa-onnx-node` wraps, with the same models and thread settings.
- Cleanup engine: the [llama.cpp](https://github.com/ggml-org/llama.cpp) C interface (`llama.h`), the library `node-llama-cpp` wraps, with the same [GGUF](https://github.com/ggml-org/ggml/blob/master/docs/gguf.md) model files, Metal on Apple Silicon and Vulkan elsewhere where a GPU is present, and the CPU as fallback.
- Microphone: SDL3 audio recording, converted by SDL to 16 kHz mono. The waveform is drawn from the captured samples.
- Remote engines: the existing OpenAI-compatible HTTP paths, on `gs_http`.

The engines, the pipeline and the model catalogue stay in the app. gesso gains only the general desktop pieces listed in the next section.

`--toggle`, `--pause` and `--discard` keep working as they do now, since Wayland desktops that refuse global key grabs rely on them.

## What gesso needs

Each row is a request for the gesso agent, stated as the behaviour wanted. The gesso agent decides the design. Apps pin gesso by commit URL, so a request is usable only once its commits are pushed.

| Need | Why Earheart needs it | Present state | First needed |
|---|---|---|---|
| Global hotkeys: register, replace and release a key combination that fires while another app has focus, reporting a refused registration | the product's entry point; Earheart swaps two hotkeys transactionally and releases them while a settings field captures a key | absent from SDL3 and gesso. Windows has `RegisterHotKey`, macOS has `RegisterEventHotKey` (no permission prompt), X11 has `XGrabKey`. Wayland has the desktop portal's global shortcuts on some desktops and nothing on others | M2 |
| Send a paste keystroke to the focused app | the product's exit point | absent. Earheart today starts PowerShell on Windows, `osascript` on macOS and `xdotool`, `wtype` or `ydotool` on Linux. Native calls (`SendInput`, `CGEventPost`, XTest) remove a process start per dictation and, on macOS, the Automation permission; the Wayland tools stay as spawned programs | M2 |
| Clipboard snapshot and restore across all formats | after an auto-paste Earheart puts back images, HTML, rich text and file copies, never text alone | SDL3 reads and writes clipboard data by MIME type; whether every format survives a round trip on each system is untested | M2 |
| A window that never takes focus, stays above other windows including full-screen ones, appears on every workspace, and is transparent | the overlay strip must not steal focus from the app being dictated into | SDL3 has always-on-top, utility, not-focusable and transparent window flags. All-workspaces and above-full-screen have no SDL call. On native Wayland a window cannot place itself, so the overlay likely runs through XWayland | M4 |
| Rounded rectangles, opacity, simple animation timing, progress bars | the overlay and Earheart's design system (`DESIGN.md`, 757 lines) use them throughout | `gs_ui` draws square fills and one-point outlines | M4 |
| Several windows from one process, each with its own `gs_ui` | overlay, settings, wizard and correction can be open together | `gs_ui_new` takes a window, so it may already work; untested | M4 |
| Form controls: masked field for API keys, radio group, number field, link, sectioned scrolling page with an index, modal confirm | the settings page (675 lines of HTML) and wizard | `gs_ui` has buttons, toggles, sliders, fields, text areas, dropdowns, menus, tooltips and lists | M5 |
| Every control reachable and operable from the keyboard with a visible focus mark | a confirmed commitment in `PRODUCT.md` for motor and RSI users | lists and fields take keyboard focus; buttons, toggles and dropdowns need checking | M5 |
| A hotkey capture field | settings and wizard record a key combination | absent | M5 |
| JSON writing | settings and history are JSON files written atomically | `gs_json` only reads | M3 |
| Desktop notifications whose click reaches the app | every pipeline failure notifies and opens Settings on click | absent; gsignal has the same need on its roadmap | M7 |
| Secrets in the system keychain on macOS and the Secret Service on Linux | remote API keys are encrypted at rest today through Electron's `safeStorage` | `gs_secret` encrypts on Windows and writes an owner-only plain file elsewhere | M7 |
| Single instance with argument forwarding | `earheart --toggle` must reach the running app | absent | M2 |
| Start on login | an existing setting | absent; small per-system code that could equally live in the app | M7 |
| Self-update: download, verify, swap and relaunch | Earheart updates itself on all three systems | `gs_http` and `gs_sha256` cover download and verification; no gesso app updates itself yet | M7 |
| macOS app bundles: `Info.plist` with usage strings, icon, tray-only (no Dock icon) mode, signing with a named certificate, and a release job on a macOS runner | a bare executable cannot ask for the microphone and loses its permission grants on every update | gesso apps are bare executables, built for release on Linux only | M2 |
| A screen-reader bridge through [AccessKit](https://github.com/AccessKit/accesskit) | Electron exposes the interface to assistive technology without any work; a gesso app exposes nothing | deferred in gesso's README until an app is offered to the public, which Earheart is | M8 |

SDL3 already supplies the tray icon and menu, the microphone, file dialogs and opening a URL, so those need nothing from gesso.

## Milestones

Each milestone ends in a test that passes or fails. Measurements and decisions go in a `decisions.md`, as gstream's did.

### M0: baseline and parity list

Measure the Electron build on the reference machines: memory per process with models unloaded and loaded, what the 396 MB Windows installer consists of, cold start, hotkey to "Listening", and stop to pasted text (`scripts/e2e-latency.js`). Write the parity list: every control, tray item, setting, notification and command-line flag, each marked kept, replaced or dropped.

Passes when the numbers and the list are recorded. This is also the point to stop if the measured Electron share of memory and download is too small to justify the work.

### M1: engines from C, no interface

A command-line build: `earheart transcribe FILE.wav` and `earheart clean TEXT`, on Linux, Windows and macOS in CI and on the rented Mac (for Metal, which CI runners may not expose), with the engine processes and their pipe protocol.

Passes when transcripts of the `scripts/eval-stt.js` corpus are identical to the Electron build's for the same model, and cleanup outputs from `scripts/bench-cleanup.mjs` match its recorded numbers in `docs/cleanup-models.md`.

Risks settled here:

- Linking. sherpa-onnx and its ONNX Runtime are large C++ projects; the plan is to link their published shared libraries rather than build them with Zig. llama.cpp must either build under Zig for all three targets or link as a prebuilt shared library. gsignal met a library that does not build with the GNU toolchain on Windows, so this is checked first.
- Chat templates. `node-llama-cpp` formats prompts from the template stored in each model file. llama.cpp's C interface supports a fixed set of templates, so each catalogue model (Qwen3, Gemma 3, Granite 4.0) must produce the same prompt bytes as today.
- GPU parity. Record which backend each reference machine uses in both builds.

Stop limit: two failed approaches to linking either engine on any one system ends the port there, since the engines are not optional.

### M2: hotkey, record, paste

Tray icon, global hotkey, microphone, transcription, paste into the focused app, with clipboard fallback and restore, on Linux X11, Windows and macOS, with no windows beyond the tray. This is the smallest build that can be shown to someone. On macOS it is already an app bundle, since the microphone prompt needs a usage string and Accessibility is granted to a signed identity.

Passes when dictated text lands in a focused test window on all three systems, the previous clipboard contents (text, image, file copy) return afterwards, and `earheart --toggle` from a second process reaches the first.

Risk settled here: Chromium applies echo cancellation, noise suppression and automatic gain to the microphone today (`renderer/microphone.js`), and SDL delivers raw samples. Record the same speech through both paths and compare word error rate. If raw audio is measurably worse, pick a remedy then.

### M3: pipeline parity

Port the logic that makes transcripts correct: decoding one utterance at a time, silence splitting, the live preview whose committed chunks become the final transcript's prefix, stumble stripping, cleanup styles and context budget, the dictionary, history written before delivery, and every fallback.

Test method: the JavaScript helpers are the oracle. A script runs each pure helper over recorded inputs and writes input and output pairs; the C tests replay them. This reuses the knowledge in the 1,097 existing tests without translating them one by one.

Passes when the replayed vectors match, `scripts/eval-long-decode.js` (driving the new build) loses no words, and killing the speech engine mid-recording still delivers the words decoded so far, marked incomplete.

### M4: overlay

The strip with status dot, waveform, live transcript in two layers, progress, and the pause, discard and copy keys, following `DESIGN.md`.

Passes when a port of `scripts/overlay-smoke.js` passes against a fake microphone, the app being dictated into keeps keyboard focus throughout on all three systems, and headless screenshots match the README's states.

### M5: settings, wizard, correction, models

All settings sections, the first-run wizard with model downloads, the correction editor, the model manager (checksummed catalogue downloads, custom Hugging Face models, removal), and remote engines with their model lists and failure copy.

Passes when a port of `scripts/settings-smoke.js` passes, a settings file written by v0.39.0 loads unchanged, models already downloaded by the Electron build are found and not downloaded again, and every flow completes without a mouse.

### M6: macOS release parity

M1 to M5 each pass on macOS as well as on Linux and Windows. What remains is replacing the Electron build on Macs that already have it: the hardened runtime and signing with Earheart's existing self-signed certificate under the existing bundle identifier `dev.cleanunicorn.earheart`. macOS files permission grants under that identifier and certificate, so keeping both should carry users' Microphone and Accessibility grants over. Port the permission probes and repair flow from `main/output/mac-permissions.js`.

Passes when a person with a Mac dictates into another app on a machine that had the Electron build installed, without being asked for permissions again.

The rented Mac serves until then, and granting permissions on it needs its desktop session rather than SSH. It is a temporary machine, so anything it proves is also turned into a CI check on a macOS runner where the check can run without a person. This milestone needs the maintainer's certificate and a Mac that has had the Electron build installed.

### M7: updates, packaging, system integration

Installers in the formats shipped today (Windows installer and portable executable, AppImage, deb, dmg), self-update, notifications, start on login, the keychain, and the what's-new card from `CHANGELOG.md`.

Migration: Earheart's updater reads `latest*.yml`, downloads the installer it names, verifies its SHA-512 and runs it. If the gesso build ships in the same containers and publishes the same feed, existing users receive it as an ordinary update. Remote API keys stored by Electron's `safeStorage` cannot be read by the new build, so users of remote engines enter their keys once more.

Passes when a v0.39.0 install on each system updates itself to the gesso build from a local feed (`EARHEART_UPDATE_FEED`) and then updates itself again to a later gesso build.

### M8: cutover

Decide screen-reader support, run both builds side by side for a period chosen by the maintainer, then replace the Electron tree, rewrite `AGENTS.md`, `CONTRIBUTING.md`, the Makefile and CI for the Zig build, and port or retire each script in `scripts/`.

Passes when the M0 measurements are repeated on the gesso build and published beside the originals.

## Decisions taken

The port is being built without asking the maintainer first, in public, and offered to him when it works. These choices follow from that.

- Where the work lives: a `native/` directory beside the Electron tree on a branch of the public `wakamex/earheart` fork, keeping his file names where a file has a counterpart (`pipeline`, `hotkeys`, `deliver`) and his Makefile target names, so it can go upstream as that directory.
- Parity target: Earheart v0.39.0. Upstream cut 46 releases in the 30 days before this plan, so what ships after v0.39.0 is listed and ported in one pass before delivery.
- Existing data: the build reads models the Electron app already downloaded and imports its settings on first run, and writes its own settings and history elsewhere, so it cannot damage an existing install.
- macOS identity: a self-signed certificate of our own and the bundle identifier `dev.wakamex.earheart`, so the build runs beside an installed Electron Earheart without sharing its permission grants, settings or single-instance lock.
- Screen readers: no bridge. Every flow works from the keyboard, which `PRODUCT.md` commits to; it leaves screen-reader support undecided. The delivery notes say that Electron exposed the interface to assistive technology and this build does not.
- Wayland: today's behaviour, a system shortcut bound to `earheart --toggle`.

## Delivery point

The build is offered after M5, with three pieces of M7 brought forward because a user meets them in daily use: failure notifications, start on login, and keychain storage for remote API keys. It ships as an archive per system, as gtube does, with a signed app bundle on macOS.

Installers and self-update wait for his answer. They depend on his certificate, bundle identifier and release feed, and they are the work most likely to be redone to fit his release pipeline.

## Left for the maintainer

- Whether to adopt the build, run it beside the Electron one, or decline it. The codebase would move from plain JavaScript with no build step to C with Zig, against `CONTRIBUTING.md`'s promise of a codebase readable in an afternoon by anyone who can read Electron docs.
- Signing with his certificate under `dev.cleanunicorn.earheart`, which M6's permission carry-over and M7's update from the Electron build both need.
- The cutover in M8, including waiving the one-PR-per-feature rule and replacing golden rule 9 (stay close to Electron built-ins).

## Known unknowns

- How much memory and download the port saves. gtube and gstream use 22 to 26 MB; Earheart's Electron share is unmeasured until M0.
- Whether the prebuilt engine libraries keep the download small. A GPU backend can outweigh the app many times over.
- Whether SDL3's tray works on every Linux desktop Earheart supports today.
- How long each milestone takes. No estimate is given, because the two finished gesso ports (gtube, gstream) had no engine linking, no global input and no macOS release.
- How gesso windows, the tray and the overlay behave on a real macOS display. Only headless rendering has been checked.
