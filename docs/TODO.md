# Status and next steps

Last updated 2026-10-26, at the end of the first session.

## Where things stand

**Done**

- Found the project: [p2r3/babel-usb](https://github.com/p2r3/babel-usb) (PortalRunner). It's cloned as the `babel-usb/` submodule, with its TinyUSB-with-MTP submodule.
- Read how it actually works (`src/monolith/usb_mtp_impl.c.h`):
  - It's an **MTP** device ("USB of Babel", read-only), not mass storage.
  - Each directory lists **4,901 objects**: 4,900 folders named with two characters from a 70-symbol alphabet (`A-Z a-z 0-9 ! space & ( ) - _ +`), plus one object named `file`.
  - The path is read as a bijective base-4900 number and written out as bijective base-256 bytes (`fs_append_output()`). `file-to-path.js` does the reverse.
  - Folder names aren't hex. The brief described hex names, so the screen supports both (see decision 1 below).
- Designed and built the screen concept, **The Air Shaft**, in `design/babel-screen.js`:
  - You look down a Borges air shaft. Each floor is a folder, with a railing, a slab, five shelves of books, and two lamps on opposite corners.
  - The next floor's name tab flickers through every possibility, locks when the host opens a folder, and falls into the path bar.
  - The book at the bottom is the `file`. Reading it raises and opens the book: hex on the left page, text on the right with an illuminated initial.
  - States: freefall with no host (lantern light, random wandering), lamps igniting on connect, descend, back, rewind to root, read, unplug.
  - 16-colour palette snapped to RGB565 (a 4bpp canvas, so every pixel is one hex digit), Bayer-dithered material ramps, hand-made 5×7 and 3×5 bitmap fonts, mip-mapped shelf textures and light maps.
- Checked the Babel-70 maths. `tools/verify.js` compares the renderer against both `file-to-path.js` and a byte-for-byte port of the C `fs_append_output()`, and all cases match. For example, "Hello" lives at `/AD/HU/Uy/ws`.
- Exported seamless GIF loops of one host session to `design/out/`: `babel-screen-hex.gif`, `babel-screen-b70.gif` and `babel-screen-hex@2x.gif`. Pillow decodes the hex GIF cleanly.

**Written but not checked yet**

- `design/index.html`, the interactive concept page. Its script passes `node --check`, but **it has never been opened in a browser** and **hasn't been published as a claude.ai artifact**.

## Next: finish the screen design

1. **Open `design/index.html` in a browser and fix what's broken.** Check:
   - Integer pixel scaling of every canvas, at desktop and phone width.
   - The 14-key keyboard rows at about 400 px wide.
   - The MTP log rows, copying the path, and switching between the two naming modes.
   - Whether the anatomy pins land on the right features. Their positions are hard-coded in `PINS`.
   - The "≈ real size" thumbnail.
2. **Publish the page as an artifact.** It needs `babel-screen.js` published alongside it as a supporting file, because the page loads it with `<script src="babel-screen.js">`.
3. **Review the motion in the GIFs.** Watch for:
   - The tab hand-off: two tabs can sit 1 to 3 px apart mid-descent.
   - The lens punch (`fovKick`, 7%).
   - The lamp-ignite cascade timing.
   - Whether the idle freefall speed (0.55 floors/s) feels right.
4. **Optional polish.** Not started:
   - A boot splash: the gallery icon, then "USB OF BABEL", then a Borges line.
   - A low-battery state.
   - Sound through the ADV's ES8311: a page turn on read, a soft tick on lock.

## Next: the firmware

### Decisions to make first

1. **Hex names or Babel-70 names?**
   - Hex: 256 folders per level, one byte per level, and the path can be read by eye (`/48/65/6C/6C/6F` is "Hello").
   - Babel-70, as shipped: 4,900 folders per level, about 1.53 bytes per level. Paths are about 35% shallower for the same file, but unreadable.
   - Test how deep each host lets you go before picking. That means Windows Explorer's MTP view, macOS (OpenMTP or Android File Transfer) and Linux (gvfs). Path length limits may cap the largest file you can reach.
   - If you go with hex, set `handle_count` to 257 in the firmware. The handle numbers shown in the page's MTP log for hex mode assume this.
2. **Toolchain.** babel-usb is ESP-IDF 5 with PlatformIO (`board = esp32s3usbotg`) and a forked TinyUSB that adds MTP. Pick one:
   - **(a)** Stay on ESP-IDF and add M5GFX and M5Unified as IDF components.
   - **(b)** Move to Arduino as an IDF component.
   - Either way the TinyUSB MTP fork has to keep working. Check that M5Unified supports the Cardputer ADV board (Stamp-S3A).
3. **Launcher packaging.** Confirm what Launcher (bmorcelli) needs from an app: a single `.bin`, partition and offset expectations, and whether native USB needs anything special after Launcher boots it.

### Work items

1. **Port babel-usb to the Cardputer ADV.**
   - New PlatformIO env or IDF target for the ESP32-S3FN8. It has 8 MB flash and no PSRAM.
   - Keep the native USB PHY setup from `src/init.c`.
   - Check that the ADV's USB-C port is the S3's native USB, and that MTP enumerates.
2. **Fix path tracking so Back works.**
   - `fs_get_object_handles()` only ever appends when the parent handle changes, so browsing back up corrupts the path.
   - Handles encode their depth: `depth = (handle - 1) / handle_count` and `index = (handle - 1) % handle_count`.
   - Keep a stack of folder indices by depth. Truncate it to the parent's depth before pushing, then rebuild `output_file` from the stack. That's cheap at these depths.
   - The screen's `up()` and back states depend on this.
3. **Display task plus event queue.**
   - The MTP handlers run on the TinyUSB task, so they should only post events: connect, disconnect, root, enter(index), open(size).
   - A separate display task owns the canvas and runs the scene at about 30 fps.
   - The hook points are shown in `design/index.html` under "Wiring it to babel-usb".
4. **Port the renderer to C++.**
   - Write `design/tools/export-c.js` to dump the palette (RGB565), both fonts, the 20 wall textures with 5 mips each, the three light maps and the vignette as `const` tables in a header.
   - Port the `Scene` state machine with the same timings. They're listed on the concept page.
   - Use a 4bpp `M5Canvas` (16,200 bytes) with a palette.
   - The shaft costs three dot products, one divide and two table reads per pixel, with no trig. Measure the frame rate, and move to fixed point if it's needed.
   - A full 16-bit frame over SPI at 40 MHz takes about 13 ms.
5. **Look up a file from the keyboard.**
   - Type text on the Cardputer and the shaft dives to its path. That's `file-to-path.js` on the device, using big-number division by 4900 over the byte array.
   - The keyboard API differs between the original Cardputer and the ADV, so check what M5Cardputer or M5Unified expose.
6. **IMU tilt.** Read the BMI270, low-pass it, and map it to a vanishing-point offset of ±6 px and a roll of ±2°. The renderer already takes `tilt.x`, `tilt.y` and `tilt.roll`.
7. **Test on real hardware.**
   - Legibility at 0.105 mm per pixel. 5×7 text is 0.74 mm tall, and the 3×5 units may be too small.
   - Colour and dithering on the IPS panel.
   - Frame rate while USB is busy.

## Notes

- The alphabet array in the repo has 74 entries, but only the first 70 are used (`% 70`). `[ ] ^ =` never appear in folder names.
- Folder names can contain spaces. The screen shows them as `␣`, but the copied path keeps real spaces.
- Every source of randomness in `babel-screen.js` is seeded, so a given timeline renders the same frames in the browser and in Node.

## Session 2 notes (firmware research, no firmware code yet)

- Decided to skip publishing the concept page as an artifact; the GIFs and PNGs are the reference. A headless Chrome check showed the page works but overflows horizontally at phone width.
- Cardputer ADV display, from M5GFX autodetect (same as the original Cardputer):
  - ST7789 on SPI3_HOST, MOSI 35, SCLK 36, DC 34, CS 37, RST 33, no MISO, 40 MHz.
  - The panel is 135×240 at offset (52, 40), colours inverted, RGB order, rotation 1.
  - Backlight is PWM on GPIO 38, at 256 Hz.
  - For `esp_lcd`, use swap_xy on, mirror_x on, mirror_y off, a gap of (40, 53), and invert_color on.
- ADV internal I2C is SDA 8 / SCL 9: TCA8418 keyboard (likely address 0x34), BMI270, ES8311. G5/G6 go to the external port.
- Build environment: PlatformIO 6.2.0 is already installed with ESP-IDF 5.5.5 (babel-usb was built on 5.5.0). To reuse it with no download, use
  `platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip`, `framework = espidf`.
- babel-usb build: the root CMakeLists adds `lib/espressif-tinyusb-component` to EXTRA_COMPONENT_DIRS and `-Iinclude` (for `tusb_config.h`). The sdkconfig is IDF 5.5.0 with 2 MB flash, so change it to 8 MB, 240 MHz and -O2 for the Cardputer.
- Launcher accepts merged images (bootloader + partition table + app at 0x0) and plain app images, so ship a merged `.bin`.

### Remaining firmware steps (estimate: one focused session)
1. Create a `firmware/` PlatformIO project that reuses babel-usb's MTP code, TinyUSB component and descriptors.
2. Write an ST7789 driver on `esp_lcd` and a C port of `babel-screen.js` (renderer and scene), plus a display task fed by an event queue from the MTP handlers.
3. Fix back-navigation path tracking (a stack by depth).
4. Optional: the TCA8418 keyboard lookup.
5. Build, produce the merged `.bin`, and test on hardware.
