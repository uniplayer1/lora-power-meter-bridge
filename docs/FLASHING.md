# Flashing and updating the boards

Both boards run **the same firmware**. Whether a board is the basement meter node
or the flat gateway is chosen afterwards on its setup page, so there is only one
file to get and nothing to build.

| You want to… | Use | Settings |
|---|---|---|
| put the firmware on a new board | the **browser flasher**, or `…-factory.bin` with esptool | fresh |
| update a board that already runs it | `…-update.bin` via its web page, over WiFi | kept |
| change the code yourself | `./flash.sh` from a checkout | kept |

All files come from the **[Releases page](https://github.com/uniplayer1/lora-power-meter-bridge/releases/latest)**.
Every release lists them with SHA-256 checksums.

---

## 1. First install over USB

Screw the antenna on first: transmitting without one can damage the radio.
Use a USB-C cable that carries **data**; many cables only carry power.

### Option A: from the browser (easiest)

1. Open the flasher page in **Chrome or Edge** on a computer:
   `https://uniplayer1.github.io/lora-power-meter-bridge/`
   (Firefox, Safari and phones cannot do Web Serial.)
2. Plug in the board, click **Connect & install**, pick the port
   (`USB JTAG/serial debug unit`).
3. For a new board, tick **Erase device**. Confirm and wait about 30 s.
4. Press **RST** on the board once. The display shows the setup WiFi and its code.

### Option B: with esptool (any OS, command line)

```bash
pip install "esptool>=5"
```

Download `lilygo-power-<version>-factory.bin`, then:

```bash
esptool --chip esp32s3 --before default-reset --after watchdog-reset write-flash 0x0 lilygo-power-<version>-factory.bin
```

The factory image contains everything: bootloader, partition table, boot pointer
and app. Flashing it **erases all settings** (WiFi, role, pairing), which is what
you want for a new board.

`--after watchdog-reset` matters. This board has no USB-UART chip, so the usual
`hard-reset` does nothing and the board would stay in download mode, looking dead.

### The board is not detected

1. Hold **BOOT**, press and release **RST**, then release **BOOT**. The board is
   now in download mode and stays there.
2. Flash again. Afterwards press **RST** so the firmware starts.
3. Still nothing: try another cable or USB port. On Linux, add your user to the
   `dialout` group (or `uucp` on Arch).

### Next step

Set up both boards as described in **[INSTALL.md](INSTALL.md)**: join the WiFi
the display shows and choose the role.

---

## 2. Updating later, over WiFi (OTA)

No cable, no computer tools. Settings and pairing are kept.

1. Download `lilygo-power-<version>-update.bin` from the latest release.
   Use the **update** file, not the factory file; the page refuses the latter.
2. Open the board's web page:
   - **Gateway (flat):** `http://lilygo-power-XXXX.local`, or its IP from your
     router. `XXXX` is on the display.
   - **Meter node (basement):** join its own WiFi (`LilyGo-Power-XXXX`, code on
     the display) and open `http://192.168.4.1`. If you also put it in your home
     WiFi, its `.local` address works from home too.
3. Tab **System** → **Firmware-Update** → choose the file → **Hochladen**.
4. The board restarts after about 10 s. **System → Firmware** shows the new version.

Update **both boards** to the same version, one after the other. While only one is
updated the link usually keeps working, but only matching versions are tested
together.

The board keeps two firmware slots. If an upload is interrupted or the file is
bad, the board keeps running the old version; just try again.

---

## 3. From source (developers)

Requires [PlatformIO](https://platformio.org/) and Python 3.

```bash
./flash.sh
```

`./flash.sh` builds, flashes every board connected by USB, and reads the firmware
stamp back off each one. That proves the new image is really running. Other
commands: `./flash.sh build`, `./flash.sh 1|2` (one board), `./flash.sh erase`
(full wipe), `./flash.sh monitor`, `./flash.sh native` (host unit tests).
See the [README](../README.md#build-flash-verify).

To package your own build the same way CI does:

```bash
python3 tools/make_release.py
```

This writes `dist/` with the update image, the factory image, the browser-flasher
parts and the checksums.

---

## 4. Publishing a release (maintainers)

Releases are built by GitHub Actions (`.github/workflows/ci.yml`), never by hand:

1. Bump the version in `VERSION` (for example `0.2.0`) and commit it to `main`.
2. Tag that commit and push the tag:

   ```bash
   git tag v0.2.0
   ```

   ```bash
   git push origin v0.2.0
   ```

3. The workflow runs the unit tests, builds the firmware with the version stamped
   in, creates the GitHub Release with all files attached, and deploys the
   browser flasher.

The tag must match `VERSION`; otherwise the build stops with a message saying so.

Every other push and every pull request also builds the images. They are
attached to the workflow run as the artifact `firmware-<version>-dev.<sha>`,
which is handy for testing a change before releasing it.

**One-time setup for the browser flasher:** in the repository settings →
*Pages*, set *Source* to **GitHub Actions**. Until then, the release itself still
works; only the *deploy browser flasher* job fails.
