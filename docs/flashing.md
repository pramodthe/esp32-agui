# Flashing the ESP32-S3-Touch-AMOLED-1.8

This board is an **ESP32-S3** with USB-C (native USB Serial/JTAG). Connect it with a USB-C
**data** cable and flash directly with `idf.py`. Source the IDF env first
(`. ~/esp/esp-idf/export.sh`) and build (`idf.py build` from `esp32-agui/`).

Working **inside the devcontainer**? It can't see USB at all — use the
[RFC2217 bridge](#flashing-from-inside-the-devcontainer-rfc2217-bridge) below instead.

## What actually gets written

`idf.py -p <PORT> flash` writes **three** regions for this app — it does **not** erase the whole
chip:

| Offset | Image | What it is |
|---|---|---|
| `0x0` | `bootloader/bootloader.bin` | 2nd-stage bootloader |
| `0x8000` | `partition_table/partition-table.bin` | partition map (nvs / phy_init / 3 MB app / 256 KB alarmimg) |
| `0x10000` | `esp32_agui.bin` | the application |

The partition map ([../esp32-agui/partitions.csv](../esp32-agui/partitions.csv)) places **`nvs` at
`0x9000`** (right after the partition table). NVS is where the device stores your WiFi credentials,
Soniox key, AG-UI URL + token, time zone, voice, volume, and screen settings — so **don't overwrite
it**. The separate **`alarmimg`** partition (256 KB, at `0x310000`) holds the user-uploaded alarm
graphic; it's written at runtime by the captive portal, never by the flasher.

## Normal flash + monitor

```bash
cd esp32-agui
idf.py -p <PORT> flash monitor      # exit monitor with Ctrl-]
# <PORT>: /dev/ttyACM0 (Linux) · /dev/cu.usbmodem* (macOS) · COMx (Windows)
```

If `Connecting...` won't sync, force ROM download mode: **hold BOOT, tap RESET, release BOOT**,
then retry. Monitor baud is 115200.

> ⚠️ **Flash the app alone at `0x10000`** (`build/esp32_agui.bin`) on subsequent reflashes.
> Flashing a **merged** image at `0x0` (`idf.py merge-bin` → `build/merged-binary.bin`) re-pads the
> `nvs` region with `0xFF` and **wipes your saved WiFi/keys** — you'll have to re-provision via the
> `AMOLED-setup` captive portal. App-only reflash keeps NVS intact:
>
> ```bash
> esptool.py --chip esp32s3 -p <PORT> write_flash 0x10000 build/esp32_agui.bin
> ```
>
> **When the partition table changes** (e.g. the `alarmimg` partition was added), flash the table
> once too — a full `idf.py flash` writes bootloader + `partition-table.bin`@`0x8000` + app and still
> leaves `nvs`@`0x9000` untouched. After that, app-only reflashes are enough again.

**Verify the running build** from the boot log's `ELF file SHA256:` line — the compile-time
timestamp is stale on incremental builds, so the ELF hash is the reliable identity check.

## Flashing from inside the devcontainer (RFC2217 bridge)

The devcontainer (Docker Desktop for Mac) can **never** see the board's USB port — the Docker VM
has no USB passthrough. Serial-over-TCP works instead: a tiny bridge on the Mac exposes the port,
and in-container `esptool` speaks `rfc2217://` to it. Build, flash, and monitor all run in the
container; the Mac only runs the bridge. (Hardware-verified 2026-07-08.)

**On the Mac** (one-time setup: `brew install esptool`):

1. Put the board in ROM download mode: unplug → **hold BOOT** → plug in → release.
   (esptool's auto-reset doesn't survive the bridge on this board — see gotchas below.)
2. `ls /dev/cu.usbmodem*` — the name can change on every re-plug; always re-check.
3. `esp_rfc2217_server -v -p 4000 /dev/cu.usbmodemXXXX` — leave it running.

**In the container** (source `/opt/esp/idf/export.sh`, build, then from `esp32-agui/build/`):

```bash
esptool.py --chip esp32s3 \
  -p 'rfc2217://host.docker.internal:4000?ign_set_control' -b 460800 \
  --before no_reset --after no_reset write_flash @flash_args
```

Then unplug/replug the board (or tap reset) to boot the new firmware — `--after no_reset` leaves it
parked in the bootloader. The NVS rules above apply unchanged: `@flash_args` is the full
three-image flash; swap in `0x10000 esp32_agui.bin` for an app-only reflash.

**Monitor over the bridge:** [`tools/monitor-bridge.sh`](../tools/monitor-bridge.sh)
(`Ctrl-]` quits).

### Bridge gotchas (hardware-verified)

- **Auto-reset fails over RFC2217.** esptool can't detect that the remote port is USB-Serial/JTAG
  ("Device PID identification is only supported on COM and /dev/ serial ports"), so
  `--before default_reset` classic-resets the chip into a *normal* boot (`Wrong boot mode detected
  (0x2b)`). Hence the manual download-mode step and `--before no_reset`.
- **A physical re-plug kills the server** (`[Errno 6] Device not configured`) and may rename the
  port — restart `esp_rfc2217_server` with a re-checked `/dev/cu.usbmodem*` after every re-plug.
- **Attaching a client reboots a running board** — the server re-applies port settings on connect
  and the control-line transitions trip the S3 reset logic. Every monitor session therefore starts
  with a fresh boot log. A board sitting in download mode is *not* reset by attaching, which is why
  flashing is stable. For a non-disruptive live tail, monitor locally on the Mac instead:
  `idf.py monitor --no-reset -p /dev/cu.usbmodemXXXX`.
- **One client at a time** — the bridge serves a single connection; stop the monitor before
  flashing and vice versa.

## Restoring factory Xiaozhi firmware

The factory images are **full 16 MB flash dumps** in [../Firmware/](../Firmware/) — write at `0x0`
(this restores the entire stock experience, WiFi-setup UI included, and overwrites everything):

- **V1 (SH8601 + FT3168)** → `ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_250805.bin`
- **V2 (CO5300 + CST816)** → `ESP32-S3-Touch-AMOLED-1.8-V2-FactoryXiaozhi_260601.bin`

```bash
esptool.py --chip esp32s3 -p <PORT> write_flash 0x0 \
  Firmware/ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_250805.bin
```

## Quick reference

- Build: `idf.py build` in `esp32-agui/`. Show sizes: `/idf-size`.
- Flash helper: `/idf-flash`. Monitor: `/idf-monitor` (baud 115200).
- Download mode: hold BOOT, tap RESET, release BOOT (or hold BOOT while plugging in).
- App-only reflash (keeps NVS): `write_flash 0x10000 build/esp32_agui.bin`.
- From the devcontainer: `esp_rfc2217_server` on the Mac + esptool
  `-p 'rfc2217://host.docker.internal:4000?ign_set_control' --before no_reset --after no_reset`;
  monitor via `tools/monitor-bridge.sh`.
