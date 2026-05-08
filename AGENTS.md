# Agent instructions for lumehead

This is the **canonical instruction file for AI coding agents** working on
this repo. It is read by Google Antigravity, Cursor, Aider, Claude Code,
OpenAI Codex, and any other agent that follows the `AGENTS.md` convention.
GitHub Copilot reads it via [`.github/copilot-instructions.md`](.github/copilot-instructions.md),
which is a thin pointer to this file.

**Trust these instructions.** Only fall back to repo-wide search when something
documented here is missing or demonstrably wrong.

## Repository summary

**lumehead** is a 16×8 addressable LED matrix toolhead display for 3D printers.
The same animation logic lives in three layers that intentionally mirror each
other and must be kept in sync:

| Layer | Location | Language / runtime |
| --- | --- | --- |
| Browser simulator (authoring tool) | [`simulator/index.html`](simulator/index.html) | Vanilla JS + HTML5 canvas, no build step |
| Klipper plugin | [`klipper/led_matrix_display.py`](klipper/led_matrix_display.py) | Python 2/3 (Klipper `extras` module) |
| Standalone firmware | [`firmware/src/main.cpp`](firmware/src/main.cpp) | C++ / FastLED on ESP32-S3 via PlatformIO |

Drawing primitives (`setPixel`, `drawGlyph`, `drawSprite`, `getColor`,
`mapCoord`, `FONT5x7`) deliberately have **the same names and same maths**
across all three layers.

Repo size: small (~25 source files, single-digit MB excluding `docs/media`
and `firmware/.pio`). Single contributor. Git default branch: `main`.

## Git workflow & commits

- **Conventional Commits are mandatory.** Format: `<type>(<scope>): <subject>`.
  - Types in use: `feat`, `fix`, `docs`, `ci`, `refactor`, `chore`,
    `test`, `style`, `perf`.
  - Optional scopes seen in history: `firmware`, `klipper`, `simulator`,
    `ci`. Pick one if the change is layer-specific; omit it for repo-wide
    changes.
  - Subject: imperative mood, lower-case, no trailing period. Soft cap
    72 chars.
  - Examples taken from the log:
    - `feat(firmware): replace boot fill with scrolling 'HELLO' marquee`
    - `fix(firmware): use GPIO 4/5 for inter-board I2C wiring`
    - `docs(firmware): update wiring diagram for GPIO 4/5 inter-board I2C`
    - `ci: only ship factory.bin in releases (drop piecewise artifacts)`
- **Split unrelated changes into separate commits.** When in doubt,
  one commit per Conventional Commit `type`.
- **Commits are GPG-signed by the user's local identity.** Use plain
  `git commit -m "..."`. **Never** override `user.name` / `user.email`
  on the command line, never pass `--no-verify`. The user's
  `commit.gpgsign=true` and signing key are already configured globally;
  overriding identity bypasses signing.
- **Push directly to `main`.** This is a single-contributor repo; there
  is no PR review gate. Run `git push` after each logical commit so the
  CI badge stays current.
- If you ever need to rewrite history (e.g. after `git filter-branch`
  which strips signatures), re-sign with:
  ```powershell
  git rebase --root --exec "git commit --amend --no-edit -S --no-verify"
  git push --force-with-lease
  ```
  Note: `--no-verify` is acceptable here because amend-during-rebase
  triggers no commit hooks worth bypassing; this is the one exception.
- For destructive ops (`push --force`, `reset --hard` on a published
  branch, deleting tags or releases) confirm with the user first.

## Environment

- Dev OS: **Windows / PowerShell**. Use `;` to chain commands. Quote paths.
- CI OS: **Ubuntu** (GitHub Actions hosted runner).
- Python: **3.12** (CI). Local Python only required for PlatformIO bootstrap.
- PlatformIO is installed at `$env:USERPROFILE\.platformio\penv\Scripts`
  on Windows; **always prepend it to `$env:Path`** before running `pio`:
  ```powershell
  $env:Path = "$env:USERPROFILE\.platformio\penv\Scripts;$env:Path"
  ```

## Build, test, validate

There are **no unit tests, no linters, and no formatters configured**. Do
not invent test/lint commands — there is nothing to invoke.

### Firmware (PlatformIO)

Always run from the repo root, pointing PIO at the `firmware/` directory.

| Goal | Command (PowerShell) | Notes |
| --- | --- | --- |
| Build the default env | `pio run -d firmware` | Default env is `waveshare-esp32-s3-matrix-master`. ~80–120 s cold, seconds when cached. |
| Build all envs | `pio run -d firmware` then `pio run -d firmware -e waveshare-esp32-s3-matrix-slave-0 -e waveshare-esp32-s3-matrix-slave-1` | Each takes ~90 s cold. |
| Build a specific env | `pio run -d firmware -e <env>` | See env list below. |
| Upload to a board | `pio run -d firmware -e <env> -t upload --upload-port COM3` | Master typically `COM3`, slave `COM6`. Detect with `pio device list`. |
| List devices | `pio device list` | |

Valid envs (must match `firmware/hardware.json`):

- `waveshare-esp32-s3-matrix-master`
- `waveshare-esp32-s3-matrix-slave-0`
- `waveshare-esp32-s3-matrix-slave-1`

Convention: **`<hardware-key>-<role>`** where `<role>` is `master` or
`slave-N` (N starts at 0).

**Known build/runtime gotchas — do not regress these:**

1. The Waveshare ESP32-S3-Matrix is **4 MB flash**, not 8 MB. The default
   board JSON declares 8 MB. `platformio.ini` already overrides this with
   `board_upload.flash_size = 4MB`, `board_build.partitions = default.csv`,
   `board_build.flash_size = 4MB`. Without these overrides the device
   bootloader rejects the image with
   `Detected size(4096k) smaller than the size in the binary image header(8192k)`.
2. **All pins, addresses, and the slave ID are passed as `-DLUMEHEAD_*`
   build flags from `platformio.ini`.** Do not hard-code them in
   `main.cpp` — add a flag instead. Existing flags:
   `LUMEHEAD_LED_DATA_PIN`, `LUMEHEAD_HOST_I2C_SDA`,
   `LUMEHEAD_HOST_I2C_SCL`, `LUMEHEAD_INTER_I2C_SDA`,
   `LUMEHEAD_INTER_I2C_SCL`, `LUMEHEAD_HOST_I2C_ADDR`,
   `LUMEHEAD_SLAVE_I2C_BASE`, `LUMEHEAD_SLAVE_ID`,
   `LUMEHEAD_ROLE_MASTER`, `LUMEHEAD_ROLE_SLAVE`.
3. FastLED's `CRGB::operator=` is **not** `volatile`-qualified. The slave
   role keeps a `volatile CRGB g_back[64]` back buffer written from the
   I2C ISR; assign through a `const_cast<CRGB*>(g_back)` pointer (pattern
   already in `main.cpp`). Do not naively assign to a `volatile CRGB[]`.
4. During the boot "HELLO" marquee the master overrides
   `FastLED.setBrightness(255)` for full-power preview, then restores
   `g_state.brightness` on first host command. Do not lock brightness to a
   dim value or you will dim the preview.

### Klipper plugin

Pure Python with no build step. Only validation is "Python imports
without syntax errors". Run from the repo root:

```powershell
python -c "import ast; ast.parse(open('klipper/led_matrix_display.py').read())"
```

The plugin is intended to be copied into `~/klipper/klippy/extras/` on a
Klipper host; do **not** add `pip install`-style packaging.

### Simulator

Pure HTML/JS, no build step. Validate by opening
`simulator/index.html` in a browser. There is no headless test rig.

## CI / release pipeline

Single workflow: [`.github/workflows/firmware-release.yml`](workflows/firmware-release.yml)
(workflow name: `release`). Triggers:

- `release: published`
- `workflow_dispatch` with optional `tag` input

What it does (kept here so the agent does not need to re-read the YAML):

1. **`matrix` job** — reads `firmware/hardware.json` and emits one
   `(hardware, role)` row per env into a build matrix.
2. **`build` job** (one run per env) — installs PlatformIO + esptool,
   runs `pio run -e <env>`, then `python -m esptool merge_bin` to produce
   a single `lumehead-<env>-factory.bin` (flashable at offset `0x0`).
   Uploads it as a workflow artifact and attaches it to the release.
   **Only `-factory.bin` is shipped** — do not reintroduce the piecewise
   `-app.bin` / `-bootloader.bin` / `-partitions.bin` artifacts that were
   removed in commit `fb637c0`.
3. **`klipper-plugin` job** — packages the plugin as
   `lumehead-klipper-<tag>.{tar.gz,zip}` and attaches to the release.

Adding a board automatically gets release artifacts — no workflow edits
needed. **The CI badge in `README.md` reflects this workflow.**

A previous failure mode worth not repeating: `python -m esptool` is not
present by default on the runner. CI installs it explicitly via
`pip install --upgrade platformio esptool`. Keep `esptool` in that
install line.

To re-trigger a release build for an existing tag:

```powershell
gh workflow run release -R celloza/lumehead -f tag=v0.0.1
```

## Project layout

```
lumehead/
├── README.md                            # User-facing landing page
├── AGENTS.md                            # ← this file (canonical agent instructions)
├── LICENSE                              # MIT
├── .github/
│   ├── copilot-instructions.md          # Pointer to AGENTS.md (for GitHub Copilot)
│   └── workflows/firmware-release.yml   # Release pipeline (workflow id: 'release')
├── docs/
│   ├── adding-hardware.md               # How to register a new board
│   ├── creating-animations.md           # Three-layer porting walkthrough
│   └── media/                           # GIF previews + demo MP4 (binary; large)
├── simulator/
│   └── index.html                       # Vanilla JS + canvas authoring tool
├── klipper/
│   ├── led_matrix_display.py            # Klipper extras plugin (single module)
│   └── README.md                        # Install & config guide
└── firmware/
    ├── platformio.ini                   # Build config (per-board [hw_*] sections)
    ├── hardware.json                    # Source of truth for CI build matrix
    ├── README.md                        # Firmware docs (envs, protocol, wiring)
    ├── .gitignore                       # ignores .pio/
    └── src/main.cpp                     # Single source for both master & slave
```

### Key files for typical change types

| Task | Touch these files |
| --- | --- |
| Add a new animation | All three layers: `simulator/index.html`, `klipper/led_matrix_display.py`, `firmware/src/main.cpp`. Add a GIF under `docs/media/` and a row in `README.md`'s **Visualizations** table. See [`docs/creating-animations.md`](docs/creating-animations.md). |
| Add a new hardware target | `firmware/hardware.json` (CI matrix), `firmware/platformio.ini` (`[hw_*]` + `[env:*]` blocks), `firmware/README.md` (Supported hardware table). See [`docs/adding-hardware.md`](docs/adding-hardware.md). Do not edit the workflow. |
| Change pin assignment | `firmware/platformio.ini` build flag, **and** the wiring diagram in `firmware/README.md`. Do not hard-code in `main.cpp`. |
| Add a new I2C command | `firmware/src/main.cpp` (`handleHostCommand`) and document it in `firmware/README.md`'s host I2C protocol table. Mirror in `klipper/led_matrix_display.py` if user-facing. |

### Architecture facts that save searching

- **Master role** is host-facing I2C slave at `0x42`, renders the full
  16×8 frame, drives its onboard panel (cols 0–7), and pushes cols 8–15
  to slave 0 over `Wire1` at 400 kHz.
- **Slave role** listens on `Wire` at `LUMEHEAD_SLAVE_I2C_BASE +
  LUMEHEAD_SLAVE_ID` (default `0x43 + N`), receives row-by-row RGB, blits
  to its onboard panel.
- **Inter-board protocol** (constants in `main.cpp`): `0xF0 FRAME_BEGIN`,
  `0xF1 FRAME_ROW` + `rowIdx` + `8×RGB` (24 bytes), `0xF2 FRAME_END`.
- **Host protocol** commands: `0x01 SET_MODE`, `0x02 SET_PROGRESS`,
  `0x03 SET_COLOR`, `0x04 SET_BRIGHTNESS`, `0x05 SET_TEXT`, `0xFF CLEAR`.
- **Mode ids**: `0 OFF, 1 MARQUEE, 2 STATIC, 3 PROGRESS, 4 PULSE,
  5 HEATING, 6 PRINTING, 7 LEVELING`.
- The onboard 8×8 WS2812 panel on the Waveshare board is hard-wired to
  **GPIO 14**. Inter-board I2C is on GPIO **4 / 5**. Host I2C is on
  GPIO **8 / 9**.

## Validation checklist before opening a PR

1. **Always** run `pio run -d firmware` and confirm all three envs build
   clean (master + both slaves). No warnings introduced.
2. If the Klipper plugin changed, confirm it parses:
   `python -c "import ast; ast.parse(open('klipper/led_matrix_display.py').read())"`.
3. If you added or renamed an animation, confirm it works in the
   simulator (`simulator/index.html`) before porting.
4. If you added a new build flag, document it in this file and in
   [`docs/adding-hardware.md`](docs/adding-hardware.md).
5. Use Conventional Commits for every commit message. Example:
   `feat(firmware): add MODE_RAINBOW visualization`.
6. Do not edit files under `firmware/.pio/` — that directory is build
   output and is gitignored.

## Out of scope

This repo is pure embedded + Klipper + browser JS. Cross-cloud / Azure /
.NET / generator skills do not apply here. Do not attempt to add Docker,
Bicep, Terraform, or cloud deployment artifacts.
