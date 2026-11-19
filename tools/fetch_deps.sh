#!/bin/sh
# fm1-stm32-unbrick: fetch everything this repo does not ship, at pinned commits, into third_party/.
#   - JieLi's WL82 flash loader (from kagaimiq/jl-uboot-tool), copied to src/ after a sha256 check.
#     It is JieLi's code, not ours to redistribute: it is fetched here and built into your own firmware.
#   - MvaveFM1Unbricker (the recovery script tools/stm32_recover.py drives)
#   - TinyUSB, ST's CMSIS headers and SEGGER RTT (only needed to build the firmware)
set -eu
cd "$(dirname "$0")/.."
mkdir -p third_party

fetch() {   # fetch <dir> <url> <commit>
    if [ ! -d "third_party/$1/.git" ]; then
        git clone --quiet --filter=blob:none "$2" "third_party/$1"
    fi
    git -C "third_party/$1" fetch --quiet origin "$3" 2>/dev/null || true
    git -C "third_party/$1" -c advice.detachedHead=false checkout --quiet "$3"
    echo "  $1 @ $(git -C "third_party/$1" rev-parse --short HEAD)"
}

echo "Fetching pinned dependencies into third_party/ ..."
fetch jl-uboot-tool      https://github.com/kagaimiq/jl-uboot-tool          adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a
fetch MvaveFM1Unbricker  https://github.com/Quixotic7/MvaveFM1Unbricker      517b9387e62e908115e5eec66c32d0ac3289f039
fetch tinyusb            https://github.com/hathach/tinyusb                  dae3f9a366bfcddbf9dcf1b48d7500286a849539
fetch cmsis_device_f4    https://github.com/STMicroelectronics/cmsis_device_f4 8c79dcf2f1d866d139a5d66240083ae6194b6dd1
fetch cmsis_core         https://github.com/STMicroelectronics/cmsis_core    01d18147527e30ed69ace1cb03b818de78bc7062
fetch RTT                https://github.com/SEGGERMicro/RTT                  9106eaa39babe53e00994d7828c1afb29390cdc1

LOADER=third_party/jl-uboot-tool/data/loaderblobs/usb/wl82loader.bin
WANT=d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376
if command -v sha256sum >/dev/null 2>&1; then GOT=$(sha256sum "$LOADER" | cut -d' ' -f1)
else GOT=$(shasum -a 256 "$LOADER" | cut -d' ' -f1); fi
if [ "$GOT" != "$WANT" ]; then
    echo "wl82loader.bin sha256 mismatch: $GOT (expected $WANT)" >&2
    exit 1
fi
cp "$LOADER" src/wl82loader.bin
echo "  src/wl82loader.bin (sha256 verified)"
echo "Done. Build with: make"
