> [!WARNING]
> This project uses a custom waveform LUT that drives the e-ink panel outside of its normal operating parameters. It increases the refresh rate by roughly 2.5 times and may shorten the displays lifespan. I take no responsibility for any damage caused. If you wish to be on the safe side turn off the fast refresh rate in settings
> 


This is a fork of CrossInk with Game Boy and Game Boy Color emulators integrated into it

This is exclusivly for the Xteink X4 Pro - DO NOT attempt to flash to other devices as it will likely brick them

# Installation

1. Download the latest firmware.bin from the Releases page.
   
2. Flash it using a web flasher / SD card
   
3. Drop your Game Boy or Game Boy Color ROM into the SD Card.
   
4. Go to the Game Library and select your ROM.


## How to use

Simply drop your Game Boy or Game Boy Color ROM into your SD Card after flashing, then click on it in Game Library, simple!

### Development

to develop you need PlatformIO

Run:

```sh
git clone https://github.com/hellominecraft913-dev/Ink-boy
```

Then build the firmware using the correct environment. typically the command is:


```sh
pio run -e x4-pro
```

## Web Browsing
You can browse the web on the web search menu by inputting a link or a search term to the text box.
It mostly only works with simple text only websites right now, with more updates in the future.



### Reader Fonts

The default fonts have been replaced with Lexend Deca and Bitter. These fonts have been chosen specifically to improve reading fluency and e-ink performance. These 'sturdier' typefaces feature uniform stroke weights and open geometries, allowing the X4/X3 to render crisp, high-contrast text with font-aliasing on while significantly reducing ghosting and artifacts.

- [Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca) - A research-backed sans-serif typeface designed to improve reading fluency. Lexend was engineered based on the theory that reading issues are often a design problem (visual crowding) rather than a cognitive one.
- [Bitter](https://fonts.google.com/specimen/Bitter) - A "contemporary" slab serif typeface for text, it is specially designed for comfortably reading on digital screens. The consistent stroke weight of Bitter helps it render particularly well on e-ink devices. The medium weight has been chosen specifically for improved rendering on the X4/X3.

The UI now uses [Inter](https://fonts.google.com/specimen/Inter) as the display font which has improved readability at smaller sizes.

### Music and Supplemental Glyphs

- Built-in reader fonts include music notation, selected Cyrillic glyphs, and the Project Hail Mary CJK fallback ranges. Additional SD-card fonts retain emoji fallback support.

---

### Font Sizes

CrossInk includes 10 pt, 12 pt, 14 pt, and 16 pt built-in reader font sizes.

See [SD Card Fonts](./docs/sd-card-fonts.md) for installing additional font families and size ranges.

---

### Reader features

Reader Options, Focus Reading, Guide Dots, Force Paragraph Indents, reading stats, and finished-book behavior are documented in [Reader Features](./docs/reader-features.md).

### Custom button actions

CrossInk adds configurable button shortcuts.

See [Controls](./docs/controls.md) for the full action list and defaults.

---

## Tips for the best reading experience

CrossInk runs on an ESP32-C3 with limited RAM, so very large folders or complex EPUBs can be slower than they would be on a phone, tablet, or desktop app.

- Keep folders under about 200 files. For the smoothest browsing, aim for 50-100 files per folder.
- Having 1000+ books on the SD card is fine if they are split into smaller folders, such as by author, series, genre, or read/unread status.
- Avoid putting every book in the SD card root. The file browser has to scan and sort the current folder before it can show it.
- Text-first EPUBs are the best fit. Large image-heavy EPUBs, scanned books, comics, and omnibus files with thousands of sections may load slowly or fail under memory pressure.
- As a rough target, EPUBs under 20 MB tend to work the best. Files over 50 MB may still work, but they are more likely to be slow or memory-sensitive, especially if they contain many large images.
- If an EPUB is unusually slow, try [optimizing](./docs/webserver.md#epub-optimization) it with the built-in web optimizer (via File Transfer) before copying it to the SD card: remove unused high-resolution images, split very large omnibus files, and avoid embedding multiple full font families when possible.
- Use a reliable SD card and leave some free space. CrossInk stores settings, reading progress, cache files, stats, and generated book data on the card.

---

# Conturbutions

This firmware is based on [Crossink](https://github.com/uxjulia/CrossInk/). The gameboy emulator used is [Peanut-GB](https://github.com/deltabeard/Peanut-GB) and the gameboy color emulator is [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB). 

I would also like to thank [Thebitsworld](https://github.com/thebitsworld) for providing the custom LUT that makes games playable and for being a reliable tester of the firmware
