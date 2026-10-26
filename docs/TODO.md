# Roadmap

## Done

- MTP drive with every file: Babel-70 names (as in p2r3/babel-usb) or hex names, switchable on the device.
- Path tracking by depth: browsing back up and reading `file` from a cached folder both give the right contents.
- Exact path-to-contents maths, tested against `file-to-path.js`'s algorithm (`make -C firmware/test test`).
- The Air Shaft screen, ported from `design/babel-screen.js` to C. Rows render on both cores. The port is checked frame by frame against the JS (`node design/tools/compare-c.js`).
- Look up a file from the keyboard, with the path shown on screen.
- Tilt from the BMI270, battery warning, boot splash, settings saved in NVS.

## To check on hardware

These parts are written from datasheets and M5Stack's libraries but need confirming on a real Cardputer ADV:

- **Panel orientation and offset.** The settings are `swap_xy`, `mirror(true, false)` and gap `(40, 53)` in `firmware/src/hw/display.c`.
- **Tilt direction.** `IMU_AXIS_*` and `IMU_SIGN_*` in `firmware/src/app.c` map the accelerometer's axes to the view, and the mapping is a guess.
- **Battery percentage.** It assumes 3.30 V is empty and 4.15 V is full (`firmware/src/hw/battery.c`).
- **Frame rate and free memory.** Press `alt` to see both. If the frame rate is low, move the shaft renderer to fixed-point maths.
- **USB after Launcher.** Check the drive still enumerates when Launcher boots the app.
- **Folder depth on each host.** Find how deep Windows Explorer, macOS (OpenMTP) and Linux (gvfs) will browse before path-length limits stop you. That decides how large a file each host can reach, and whether hex or Babel-70 is the better default.

## Known limitations

- `file` is capped at 4,096 bytes. That's the size of the device's output buffer, the same as upstream.
- The screen animates the first 256 levels of a path. Deeper browsing still serves the right file.
- `file` reports its size as unknown, as upstream does. Some hosts may show 0 bytes until they copy it.
- If the computer suspends the USB port while idle, the screen falls back to freefall until the next request.
- macOS needs a third-party MTP app.

## Ideas

- Report the real size of `file` in its object info.
- Repeat `del` while it is held. Read the keyboard on its interrupt pin instead of polling it.
- Design page: the concept page (`design/index.html`) overflows sideways at phone width.

## Notes

- Folder names can contain spaces. The screen draws them as `␣`.
- Every source of randomness in the renderer is seeded, so the JS design and the C port render the same frames.
- `design/tools/export-c.js` regenerates `firmware/src/screen/bs_tables.h` (palette and fonts) from the JS design. The wall textures and light maps are built at boot.
