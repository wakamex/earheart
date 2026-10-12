# Earheart, native

This folder is a port of [Earheart](https://github.com/cleanunicorn/earheart) from [Electron](https://www.electronjs.org/) to one C program built on [gesso](https://github.com/wakamex/gesso), a small C library over [Simple DirectMedia Layer (SDL)](https://www.libsdl.org/). The goal of the fork is to offer the maintainer the same app, behaving the same way at Earheart v0.39.0, as a program that starts in a tenth of a second and waits in the tray in about 12 MiB where the Electron build takes 267 MiB. Whether to adopt it is his decision; the Electron build in the rest of this repository is untouched.

It runs on Linux (X11), macOS and Windows. Speech-to-text and cleanup use the same engine libraries the Electron build ships, loaded at run time, and the models the Electron build has already downloaded are used where they are.

## Known gap: noisy rooms

The native build transcribes noisy audio worse than the Electron build. Chromium cleans up the microphone before the Electron build sees it (noise suppression, echo cancellation, automatic gain), and the native build records what the microphone delivers. On 40 clips of the [FLEURS](https://huggingface.co/datasets/google/fleurs) English test set played into the Electron build's own overlay and transcribed both ways with the same engine, the word error rate (WER, lower is better) was:

| Condition | Native build | Electron build | Difference in points, with 95% interval |
|---|---|---|---|
| Clip as recorded | 6.07% | 5.29% | 0.79 (-0.12 to 1.82) |
| 20 dB quieter | 6.52% | 5.62% | 0.90 (-0.23 to 2.07) |
| White noise 15 dB below the speech | 9.79% | 7.20% | 2.59 (0.34 to 5.32) |

With noise in the room the native build is 2.59 points worse. On clean and quiet audio the difference leans the same way and its interval includes zero. A noise suppressor has not been tried yet; the method and the script are in [decisions.md](decisions.md) under M2.

## Other differences from the Electron build

- No screen-reader access: gesso draws its own controls and has no accessibility bridge.
- No Updates section and no self-update. Installers and updates wait on the maintainer's signing identity and release feed.
- API keys saved by the Electron build are encrypted for that program alone, so they are entered once more.
- The overlay's keys have no tooltips, and each settings section has a coloured dot where the Electron build has an icon.

The full list, feature by feature, is the parity list in [decisions.md](decisions.md).

## Side by side with the Electron build

Each row shows one screen in the same state in both builds: the Electron build on the left, the native build on the right. Both were drawn on the same Linux machine into a virtual display ([Xvfb](https://www.x.org/releases/current/doc/man/man1/Xvfb.1.xhtml)) at the same size in pixels, so neither has a window frame. The Electron images come from `scripts/screenshots.js` and the native ones from `earheart screenshots DIR`, which fills each window with the same sample content.

| Screen | Electron | Native |
| --- | --- | --- |
| Overlay, recording with live transcript | ![Electron overlay while recording](docs/compare/electron-overlay-recording.png) | ![Native overlay while recording](docs/compare/native-overlay-recording.png) |
| Overlay, saved microphone missing | ![Electron overlay with the microphone notice](docs/compare/electron-overlay-microphone-fallback.png) | ![Native overlay with the microphone notice](docs/compare/native-overlay-microphone-fallback.png) |
| Overlay, paused | ![Electron overlay paused](docs/compare/electron-overlay-paused.png) | ![Native overlay paused](docs/compare/native-overlay-paused.png) |
| Overlay, cleaning up | ![Electron overlay while cleaning up](docs/compare/electron-overlay-processing.png) | ![Native overlay while cleaning up](docs/compare/native-overlay-processing.png) |
| Overlay, pasted | ![Electron overlay after pasting](docs/compare/electron-overlay-done.png) | ![Native overlay after pasting](docs/compare/native-overlay-done.png) |
| Settings, General | ![Electron settings](docs/compare/electron-settings.png) | ![Native settings](docs/compare/native-settings.png) |
| Set-up wizard, first step | ![Electron wizard](docs/compare/electron-wizard.png) | ![Native wizard](docs/compare/native-wizard.png) |
| Correct transcript | ![Electron correction window](docs/compare/electron-correction.png) | ![Native correction window](docs/compare/native-correction.png) |

The overlay matches closely; the native recording overlay is 2 pixels taller (154 against 152). The framed windows differ in ways the images show:

- Headings and field labels are semibold in the Electron build and regular weight in the native one.
- The wizard's step sits in the middle of the window in the Electron build and at the top in the native one.
- Command names inside the settings hints have a chip behind them in the Electron build and are plain text in the native one.
- In the correction window the native transcript box is taller, the buttons are pills where the Electron build has rounded rectangles, the focused field has a light outline where the Electron build has an accent one, and the selected words stay marked after the keyboard moves to the spelling field.

## Build and test

It needs [Zig](https://ziglang.org/) 0.17.0 and, for the engine libraries, `curl` and `tar`.

```sh
cd native
tools/fetch-engines.sh linux-x64      # or macos-arm64, windows-x64
zig build                              # zig-out/bin/earheart
zig build test                         # the ported logic against the JavaScript it came from
```

The window tests need a display. On Linux without one:

```sh
xvfb-run -a zig-out/bin/earheart settings-smoke tests/vectors/settings.json
xvfb-run -a zig-out/bin/earheart wizard-smoke
xvfb-run -a zig-out/bin/earheart correction-smoke
EARHEART_MODELS=<models folder> xvfb-run -a sh -c 'openbox & sleep 1; tests/e2e.sh'
```

On a Mac, `zig build bundle -Didentity=NAME` makes a signed `zig-out/Earheart.app`. Microphone, Accessibility and notification permissions are filed under the signature, so an ad hoc signature loses them at every build.

## Where things are

- [plan.md](plan.md): the milestones and what each had to pass.
- [decisions.md](decisions.md): every decision taken, the measurements, and the parity list.
- [gesso-requests.txt](gesso-requests.txt): what the port asked of gesso and what came of it.
- `src/`: the app. `tests/`: unit tests and their vectors. `tools/`: vector generators and measurement scripts.
