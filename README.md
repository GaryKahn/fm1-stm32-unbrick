# fm1-stm32-unbrick

**Unbrick an M-VAVE FM-1 with an STM32F4DISCOVERY board.** USB cables only: no soldering, no wiring, no
extra modules. Runs on **macOS, Linux (including a Raspberry Pi) and Windows**.

For an FM-1 that shows **nothing on USB** after an interrupted firmware update (black screen, no sound, only
the charging LED). The board puts the FM-1's JieLi chip into its ROM USB boot mode, then the recovery script
backs up the whole flash, checks it and rewrites the firmware area with the official V15.

**Status:** used once, successfully, from macOS: an STM32F4DISCOVERY **MB997C** recovered an FM-1 whose V15
update was cut off by unplugging it (19 Nov 2026). The full flash was backed up, 143 app sectors were
rewritten and read back identical to V15, and the FM-1 booted. Linux and Windows use the same firmware,
script and OpenOCD commands but have not been run end to end yet; other board revisions (MB997D/E,
STM32F407G-DISC1) should behave the same but are untested.

```
Computer ── CN1 mini-USB (ST-LINK: flashing + SEGGER RTT) ──► STM32F407 ── CN5 micro-USB ──► FM-1 USB-C
                                                              PA12 = D+, PA11 = D-        (FM-1 on its battery)
```

## Credits

This is a port, standing on others' work:

- [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) (Leo Kuroshita): the USB_KEY entry,
  the ROM keep-alive pulses and the line protocol.
- [fm1-esp32-unbrick](https://github.com/dagnazty/fm1-esp32-unbrick) (dag): the ESP32-S3 port these sources
  are ported from (`jieli_uboot.c` is used unchanged).
- [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker) (Michael P Jones): the recovery
  script, its flash analysis and every safety check. `tools/stm32_recover.py` drives it unchanged.
- [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) (kagaimiq): the JieLi UBOOT protocol and loader.
- [TinyUSB](https://github.com/hathach/tinyusb) for the USB host, [SEGGER RTT](https://github.com/SEGGERMicro/RTT)
  for the link to the computer.

## You need

- An **STM32F4DISCOVERY** (STM32F407VG). Both **CN3** jumpers fitted, **JP1** fitted (the defaults).
- A **mini-USB data cable** (CN1 → computer) and a **micro-USB to USB-C data cable** (CN5 → FM-1).
- The FM-1 **charged**.
- The **official V15 `FM-1.fwsc`** (sha256 `db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a`).
  M-VAVE's site now serves V16, which the script refuses; the elektronauts FM-1 thread links an archived V15.
- Git, Python 3 with pyserial, OpenOCD and the Arm GCC toolchain: see **Install** for your system.

## Install

Every command in this README is the same on all three systems, typed in a terminal (Terminal on macOS, any
shell on Linux, PowerShell on Windows), with one exception: **on Windows type `py` where it says `python3`**.

### macOS

```bash
brew install git open-ocd
brew install --cask gcc-arm-embedded
python3 -m pip install --user pyserial || python3 -m pip install --user --break-system-packages pyserial
```
No driver needed for the ST-LINK.

### Linux (Debian, Ubuntu, Raspberry Pi OS)

```bash
sudo apt install git openocd gcc-arm-none-eabi libnewlib-arm-none-eabi python3-serial
```
Fedora: `sudo dnf install git openocd arm-none-eabi-gcc-cs arm-none-eabi-newlib python3-pyserial`.
Arch: `sudo pacman -S git openocd arm-none-eabi-gcc arm-none-eabi-newlib python-pyserial`.

The `openocd` package installs udev rules for the ST-LINK. If OpenOCD reports `LIBUSB_ERROR_ACCESS`, add
yourself to the `plugdev` group (`sudo usermod -aG plugdev $USER`), log out and back in, and replug the board.

### Windows (10/11)

1. **Git for Windows**: <https://git-scm.com/download/win> (or `winget install Git.Git`).
2. **Python 3**: <https://www.python.org/downloads/> (tick *Add python.exe to PATH*), then `py -m pip install pyserial`.
3. **Arm GNU Toolchain** (`arm-none-eabi`, Windows installer):
   <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>. Tick *Add path to environment variable*.
4. **OpenOCD**: the xPack build, <https://github.com/xpack-dev-tools/openocd-xpack/releases> (`win32-x64.zip`).
   Unzip it and add its `bin` folder to your PATH.
5. **ST-LINK driver**: ST's **STSW-LINK009** from st.com, so OpenOCD can open the board's ST-LINK.

Open a **new** PowerShell after installing so the PATH changes apply. Check: `git --version`, `py --version`,
`arm-none-eabi-gcc --version`, `openocd --version`.

## Get and build

```bash
git clone https://github.com/GaryKahn/fm1-stm32-unbrick
cd fm1-stm32-unbrick
python3 tools/fetch_deps.py
python3 tools/build.py
```
`fetch_deps.py` downloads pinned versions of everything this repo does not ship into `third_party/`,
including JieLi's flash loader (it is not ours to redistribute; it is sha256-checked and built into your own
firmware). `build.py` produces `build/fm1-stm32-unbrick.elf`. Linker warnings that `_close`, `_write` …
"will always fail" are expected.

Put the V15 `FM-1.fwsc` in the `fm1-stm32-unbrick` folder. Run every command below **from that folder**.

The tested firmware was built with Arm GCC 13.2 and has `build/fm1-stm32-unbrick.bin` sha256
`7eae7f3ac9799f4da5297317442aff58b7fd0058bc7139e21218752133950034`; the build is reproducible with that
compiler. Other GCC versions give a different but equivalent binary.

## 1. Board check (FM-1 not connected)

Connect **CN1 → computer** only.

```bash
openocd -f board/stm32f4discovery.cfg -f tools/openocd/flash.cfg
```
`Verified OK`; the green LED (LD4) blinks: the board is sending the recovery key.

**Terminal 1**, leave running for the whole session:
```bash
openocd -f board/stm32f4discovery.cfg -f tools/openocd/rtt.cfg
```
`Control block found at 0x2001....` and `Listening on port 19021 for rtt connections`.

**Terminal 2** (also in the `fm1-stm32-unbrick` folder):
```bash
python3 tools/stm32_recover.py log
```
Expect `CN5 power switch: PC0=1 (OFF - correct, LD7 must be dark)`, `bus idle: D+=0 D-=0` and
`KEY: ... packets`. **LD7 (green, left of CN5) must be off** while LD4 blinks. It flickers for a moment
whenever the board resets: that is normal, but never flash or reset the board with the FM-1 connected.

## 2. Rehearse (optional, no hardware)

```bash
python3 tools/stm32_recover.py recover FM-1.fwsc --dry-run --out rehearsal
```
The whole recovery against the script's simulated FM-1, so you know every prompt before the real run.
(The simulated flash is random: its "head DIFFERS" is expected there, not on the real FM-1.)

## 3. Recover (Terminal 2, Terminal 1 still running)

1. FM-1 **off**. Connect **CN5 → FM-1 USB-C**. LD7 stays off.
2. Run, and switch the FM-1 **on** once it says it is waiting:
   ```bash
   python3 tools/stm32_recover.py info --wait 180
   ```
   LEDs: green blinking → **orange** (ROM answered) → **blue** (FM-1 in UBOOT). Success is
   `Chip key 980F and flash 856014 (1 MiB): this is an FM-1 in UBOOT`.
   Ignore `could not identify the Transporter's console port`: this board keeps its log internally.
3. Back up, analyse, plan, confirm, write, verify:
   ```bash
   python3 tools/stm32_recover.py recover FM-1.fwsc --out backups
   ```
   Before typing `WRITE`, check the analysis: **(a) Head must say `head == V15`**. If the head differs,
   stop: this tool never writes the head (0x0000–0x3FFF) and cannot fix it. Only 0x4000–0x92FFF is written;
   the FM-1's own data (0x93000 onwards) is never touched. **Red LED** = a sector is being written.
4. After `Final full read EQUALS the expected image`: unplug the board from both ends, switch the FM-1 off
   and on. Stop OpenOCD in Terminal 1 with Ctrl+C. Keep `backups/` and `logs/`.

## Troubleshooting

- `python3 tools/stm32_recover.py log` shows the board's own log at any time.
- No orange/blue after switching on: FM-1 off, `python3 tools/stm32_recover.py rekey`, FM-1 on.
- `HOST: no device mounted yet ... conn=0`: the ROM answered but the USB host never saw the FM-1.
- `rtt: No control block found`: the firmware is not running; flash it again, then restart Terminal 1.
- OpenOCD cannot open the ST-LINK: Linux, see the `plugdev` note; Windows, install STSW-LINK009 and replug.
  An old ST-LINK firmware is fine (V2J24 worked); STM32CubeProgrammer can update it if OpenOCD refuses it.
- `cannot open socket://localhost:19021`: Terminal 1 (OpenOCD with `rtt.cfg`) is not running.

## USB host self-test (optional)

```bash
python3 tools/build.py --selftest
openocd -f board/stm32f4discovery.cfg -f tools/openocd/flash-selftest.cfg
```
This build **powers CN5** and runs the same USB host code against an ordinary USB flash drive (through a
micro-USB OTG adapter) up to INQUIRY: look for `SELFTEST PASSED` in the log. **Never connect the FM-1 while
the self-test build is on the board**; flash `tools/openocd/flash.cfg` again afterwards.

## How it differs from the ESP32-S3 / RP2040 versions

- One core, one loop: USB_KEY and pulses on PA11/PA12 as GPIO, then the pins go to the OTG FS controller
  (TinyUSB host, no class drivers: the MSC bulk endpoints are driven directly, as the originals do).
- The ACK probe drives D- high for 0.5 µs, releases it and reads 0.3 µs later (STM32 pins drive too hard to
  read while driving, as the weak RP2040/ESP32 pads do).
- The link to the computer is SEGGER RTT over the on-board ST-LINK (the Discovery's ST-LINK virtual COM port
  is not wired to the STM32), exposed by OpenOCD as `socket://localhost:19021`.
- PC0 holds the CN5 VBUS switch off; it is written after a delay because of the STM32F4 erratum on writes
  right after an RCC clock enable (the first build lost that write and powered CN5).

## License

MIT, see [LICENSE](LICENSE). Third-party components built into the firmware: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
Not affiliated with M-VAVE / Cuvave or JieLi. Use at your own risk: it worked once, on one unit.
