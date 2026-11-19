# fm1-stm32-unbrick

**Unbrick an M-VAVE FM-1 with an STM32F4DISCOVERY board and a Mac.** USB cables only: no soldering, no
wiring, no extra modules.

For an FM-1 that shows **nothing on USB** after an interrupted firmware update (black screen, no sound, only
the charging LED). The board puts the FM-1's JieLi chip into its ROM USB boot mode, then the recovery script
backs up the whole flash, checks it and rewrites the firmware area with the official V15.

**Status:** used once, successfully: an STM32F4DISCOVERY **MB997C** recovered an FM-1 whose V15 update was
cut off by unplugging it (19 Nov 2026). The full flash was backed up, 143 app sectors were rewritten and
read back identical to V15, and the FM-1 booted. One board, one unit: other board revisions (MB997D/E,
STM32F407G-DISC1) should work the same but are untested.

```
Mac ── CN1 mini-USB (ST-LINK: flashing + SEGGER RTT) ──► STM32F407 ── CN5 micro-USB ──► FM-1 USB-C
                                                         PA12 = D+, PA11 = D-        (FM-1 runs on its battery)
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
  for the link to the Mac.

## You need

- An **STM32F4DISCOVERY** (STM32F407VG). Both **CN3** jumpers fitted, **JP1** fitted (the defaults).
- A **mini-USB data cable** (CN1 → Mac) and a **micro-USB to USB-C data cable** (CN5 → FM-1).
- The FM-1 **charged**.
- The **official V15 `FM-1.fwsc`** (sha256 `db1642b2…edb8a`; M-VAVE's site now serves V16, which the
  script refuses). The elektronauts FM-1 thread links an archived copy.
- A Mac (or Linux) with Homebrew:
  ```bash
  brew install open-ocd
  brew install --cask gcc-arm-embedded
  python3 -m pip install --user pyserial
  ```

## Build

```bash
git clone https://github.com/GaryKahn/fm1-stm32-unbrick && cd fm1-stm32-unbrick
sh tools/fetch_deps.sh      # pinned deps + JieLi's loader (sha256-checked) into third_party/ and src/
make                        # build/fm1-stm32-unbrick.elf
```
JieLi's loader is not in this repo (it is not ours to redistribute); `fetch_deps.sh` fetches it and it is
built into your own firmware. The tested build's `build/fm1-stm32-unbrick.bin` has sha256
`7eae7f3ac9799f4da5297317442aff58b7fd0058bc7139e21218752133950034`; the build is reproducible.

## 1. Board check (FM-1 not connected)

Connect **CN1 → Mac** only.

```bash
openocd -f board/stm32f4discovery.cfg -c "program build/fm1-stm32-unbrick.elf verify reset exit"
```
`Verified OK`; the green LED (LD4) blinks: the board is sending the recovery key.

Window 1, leave running:
```bash
openocd -f board/stm32f4discovery.cfg -c init \
  -c 'rtt setup 0x20000000 0x20000 "SEGGER RTT"' -c 'rtt start' \
  -c 'rtt polling_interval 1' -c 'rtt server start 19021 0'
```
`Control block found at 0x2001....` and `Listening on port 19021 for rtt connections`.

Window 2:
```bash
python3 tools/stm32_recover.py log
```
Expect `CN5 power switch: PC0=1 (OFF - correct, LD7 must be dark)`, `bus idle: D+=0 D-=0` and
`KEY: ... packets`. **LD7 (green, left of CN5) must be off** while LD4 blinks. It flickers for a moment
whenever the board resets: that is normal, but never reset or flash the board with the FM-1 connected.

## 2. Rehearse (optional, no hardware)

```bash
python3 tools/stm32_recover.py recover FM-1.fwsc --dry-run --out /tmp/fm1-dryrun
```
The whole recovery against the script's simulated FM-1, so you know every prompt before the real run.

## 3. Recover

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
   and on. Keep `backups/` and `logs/`.

## Troubleshooting

- `python3 tools/stm32_recover.py log` shows the board's own log at any time.
- No orange/blue after switching on: FM-1 off, `python3 tools/stm32_recover.py rekey`, FM-1 on.
- `HOST: no device mounted yet ... conn=0`: the ROM answered but the USB host never saw the FM-1.
- OpenOCD complains about the ST-LINK: update its firmware with STM32CubeProgrammer (V2J24 worked).

## USB host self-test (optional)

`make selftest` builds `build/fm1-selftest.elf`, which **powers CN5** and runs the same USB host code
against an ordinary USB flash drive (via a micro-USB OTG adapter) up to INQUIRY: `SELFTEST PASSED` in the
log. **Never connect the FM-1 while the self-test build is on the board**; flash `fm1-stm32-unbrick.elf`
back afterwards.

## How it differs from the ESP32-S3 / RP2040 versions

- One core, one loop: USB_KEY and pulses on PA11/PA12 as GPIO, then the pins go to the OTG FS controller
  (TinyUSB host, no class drivers: the MSC bulk endpoints are driven directly, as the originals do).
- The ACK probe drives D- high for 0.5 µs, releases it and reads 0.3 µs later (STM32 pins drive too hard to
  read while driving, as the weak RP2040/ESP32 pads do).
- The Mac link is SEGGER RTT over the on-board ST-LINK (the Discovery's ST-LINK virtual COM port is not
  wired to the STM32), exposed by OpenOCD as `socket://localhost:19021`.
- PC0 holds the CN5 VBUS switch off; it is written after a delay because of the STM32F4 erratum on writes
  right after an RCC clock enable (the first build lost that write and powered CN5).

## License

MIT, see [LICENSE](LICENSE). Third-party components built into the firmware: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
Not affiliated with M-VAVE / Cuvave or JieLi. Use at your own risk: it worked once, on one unit.
