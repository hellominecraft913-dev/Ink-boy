# Agnes NES integration (Xteink X4 Pro)

This overlay targets the current `main` branch of `hellominecraft913-dev/Ink-boy`.
It adds an Agnes-powered `.nes` activity and mirrors Ink-boy's current guarded
fast-refresh profile: acceleration is only enabled on the validated UC8279
variant `0x68`; other panel variants retain the stock refresh waveform.

## Upstream dependency

The upstream Agnes core is MIT licensed and exposes `agnes_make`,
`agnes_load_ines_data`, `agnes_next_frame`, `agnes_get_screen_pixel`, and
`agnes_set_input`. Its upstream repository is https://github.com/kgabis/agnes.
`scripts/fetch_agnes.py` obtains `agnes.c`, `agnes.h`, and `LICENSE` from the
upstream `master` branch on the first X4 Pro build and leaves them in a gitignored
vendor directory. This requires internet access on the build machine once.

## Build and use

After applying `patches/agnes-nes-integration.patch` in the root of a fresh,
recursive clone of Ink-boy:

```powershell
git status --short
pio run -e x4-pro
```

Copy homebrew or legally obtained `.nes` iNES files to the SD card's `/Books`
folder and select one from the file browser. Use the on-screen D-pad and A/B/
Select/Start buttons; a long Back press or the on-screen EXIT button exits.

## Compatibility and limitations

Agnes upstream documents NROM, UxROM, MMC1, and MMC3 support; other mappers may
not load. The upstream project lists APU/audio emulation as TODO, so this port is
silent. NES RGB output is converted using weighted luminance into four grayscale
shades, then spatially dithered for the monochrome e-ink panel. ROM images are
limited to 4 MiB by this frontend. The fast-refresh waveform is hardware-gated,
not forced on SSD1677, UC8179, UC8253, or unvalidated UC8279 variants.

The overlay patch was checked against the local project snapshot available in
this workspace. The latest public `main` source was inspected for the current
fast-refresh APIs, but a fresh full repository archive could not be downloaded
in this environment; apply the patch to your current up-to-date GitHub checkout,
not to an older copy of the project.
