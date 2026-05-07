# Adding a new hardware target

The ESP32-S3 firmware in [`firmware/`](../firmware/) is structured so adding a
new board is a localised change in three places. CI then automatically builds
and publishes release binaries for it.

## Concepts

- **Hardware key** — a kebab-case identifier for the board (e.g.
  `waveshare-esp32-s3-matrix`). It appears in env names, artifact filenames,
  and `hardware.json`.
- **Role** — `master` (host-facing, renders the frame, drives the inter-board
  bus) or `slave-N` (pure pixel sink at I2C address `0x43 + N`).
- **Env name** — `<hardware-key>-<role>`, e.g.
  `waveshare-esp32-s3-matrix-slave-0`.

The same `src/main.cpp` is built for every (hardware, role) pair. All
hardware-specific values are passed via `-DLUMEHEAD_*` build flags, so no
source edits are needed for a new board.

## Build flags

Defined per hardware in `platformio.ini`:

| Flag | Default | Meaning |
| --- | --- | --- |
| `LUMEHEAD_LED_DATA_PIN` | `14` | GPIO driving the onboard 8×8 WS2812 panel. |
| `LUMEHEAD_HOST_I2C_SDA` | `8` | Master-only. SDA for the host I2C slave bus. |
| `LUMEHEAD_HOST_I2C_SCL` | `9` | Master-only. SCL for the host I2C slave bus. |
| `LUMEHEAD_INTER_I2C_SDA` | `4` | Inter-board I2C SDA (master `Wire1`, slave `Wire`). |
| `LUMEHEAD_INTER_I2C_SCL` | `5` | Inter-board I2C SCL. |
| `LUMEHEAD_HOST_I2C_ADDR` | `0x42` | Master I2C slave address (host-facing). |
| `LUMEHEAD_SLAVE_I2C_BASE` | `0x43` | Base address for slaves. |
| `LUMEHEAD_SLAVE_ID` | `0` | Per-slave id; effective address = base + id. |
| `LUMEHEAD_ROLE_MASTER` / `LUMEHEAD_ROLE_SLAVE` | — | Selects the role at compile time. |

## Step 1 — describe the board in `firmware/hardware.json`

This file drives release CI. Each entry lists the chip, flash params, and
the roles that should be built.

```json
{
  "hardware": {
    "waveshare-esp32-s3-matrix": {
      "display_name": "Waveshare ESP32-S3-Matrix",
      "chip": "esp32s3",
      "flash_size": "4MB",
      "flash_mode": "dio",
      "flash_freq": "80m",
      "roles": ["master", "slave-0", "slave-1"]
    },
    "my-new-board": {
      "display_name": "My New Board",
      "chip": "esp32s3",
      "flash_size": "8MB",
      "flash_mode": "qio",
      "flash_freq": "80m",
      "roles": ["master", "slave-0"]
    }
  }
}
```

The release workflow expands `(hardware × roles)` into one build job per
pair and produces `lumehead-<env>-factory.bin`, `-app.bin`, `-bootloader.bin`,
and `-partitions.bin` artifacts.

## Step 2 — add a `[hw_*]` section to `platformio.ini`

```ini
[hw_my_new_board]
board                   = esp32-s3-devkitc-1   ; or whatever PlatformIO board id matches
board_upload.flash_size = 8MB
board_build.partitions  = default.csv
board_build.flash_size  = 8MB
build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DLUMEHEAD_HW_MY_NEW_BOARD=1
    -DLUMEHEAD_LED_DATA_PIN=21
    -DLUMEHEAD_HOST_I2C_SDA=8
    -DLUMEHEAD_HOST_I2C_SCL=9
    -DLUMEHEAD_INTER_I2C_SDA=4
    -DLUMEHEAD_INTER_I2C_SCL=5
```

> Section keys must use underscores (`hw_my_new_board`); env keys must use
> hyphens (`my-new-board-master`). They are different strings — that is
> just PlatformIO's naming rule.

## Step 3 — declare the envs

Add one `[env:*]` per role you listed in `hardware.json`:

```ini
[env:my-new-board-master]
extends     = env, hw_my_new_board
build_flags =
    ${hw_my_new_board.build_flags}
    -DLUMEHEAD_ROLE_MASTER=1

[env:my-new-board-slave-0]
extends     = env, hw_my_new_board
build_flags =
    ${hw_my_new_board.build_flags}
    -DLUMEHEAD_ROLE_SLAVE=1
    -DLUMEHEAD_SLAVE_ID=0
```

## Step 4 — build and flash locally

```sh
pio run -d firmware -e my-new-board-master -t upload --upload-port COM3
pio run -d firmware -e my-new-board-slave-0 -t upload --upload-port COM6
```

## Step 5 — release

Cut a GitHub release. The
[`firmware-release`](../.github/workflows/firmware-release.yml) workflow
reads `hardware.json`, builds every (hardware, role) pair, and attaches the
binaries to the release.

## Multiple slaves

Each slave's I2C address is `LUMEHEAD_SLAVE_I2C_BASE + LUMEHEAD_SLAVE_ID`.
Add `slave-1`, `slave-2`, … entries to both `hardware.json` and
`platformio.ini` and bump `LUMEHEAD_SLAVE_ID` accordingly. The master can
be extended later to fan frames out to more than one slave on the same
inter-board bus.

## Updating documentation

When you add a board, also append a row to the **Supported hardware** table
in [`firmware/README.md`](../firmware/README.md).
