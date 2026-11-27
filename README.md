# Ink-boy

Ink-boy is a custom firmware project for the **Xteink X4 Pro**, based on [CrossInk 2.0.0](https://github.com/uxjulia/CrossInk). It combines the CrossInk reader with Ink-boy's Game Boy and Game Boy Color game library integration and the X4 Pro display-driver work.

> **Device warning:** This build is intended for the Xteink X4 Pro only. Do not flash it to other readers. It includes a custom display waveform/LUT that operates outside the panel's normal operating parameters. This may increase panel wear or shorten its lifespan. Use the firmware at your own discretion.

## Features

- CrossInk 2.0.0 reader foundation.
- Xteink X4 Pro-specific UC8279 display driver and fast-refresh tuning.
- Built-in Game Boy emulation using [Peanut-GB](https://github.com/deltabeard/Peanut-GB).
- Game Boy Color emulation using [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB).
- On-device Game Library for browsing and launching supported game ROMs.
- CrossInk reading features and settings inherited from the upstream project.

The emulator integration and display-driver changes are documented in [`docs/gameboy-emulators.md`](docs/gameboy-emulators.md).

## Installation

1. Open the [Ink-boy releases page](https://github.com/hellominecraft913-dev/Ink-boy/releases).
2. Download the firmware asset intended for the **Xteink X4 Pro**.
3. Flash it to the X4 Pro using the installation method documented with that release.
4. Copy compatible Game Boy or Game Boy Color ROM files that you are entitled to use to the reader's storage, then open the Game Library.

Do not use an Ink-boy X4 Pro firmware image on another device.

## Build from source

### Requirements

- Git
- [PlatformIO](https://platformio.org/) Core or the PlatformIO IDE extension
- The Xteink X4 Pro build environment (`x4-pro`)

Clone the repository:

```sh
git clone https://github.com/hellominecraft913-dev/Ink-boy.git
cd Ink-boy
```

Build the X4 Pro firmware:

```sh
pio run -e x4-pro
```

The FreeInk SDK is included directly in this repository under `freeink-sdk/`; it is **not** a Git submodule. The remaining `.gitmodules` entry is for the Tabler Icons assets. When a checkout needs that asset submodule, initialize it with:

```sh
git submodule update --init --recursive
```

## Credits

- [CrossInk](https://github.com/uxjulia/CrossInk) for the reader firmware foundation.
- [Peanut-GB](https://github.com/deltabeard/Peanut-GB) for Game Boy emulation.
- [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) for Game Boy Color emulation.
- [Thebitsworld](https://github.com/thebitsworld) for the custom display LUT work and testing support.

Ink-boy is an independent community firmware project and is not an official Xteink or CrossInk release.