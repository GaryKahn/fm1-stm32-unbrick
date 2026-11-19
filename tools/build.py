#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the firmware with arm-none-eabi-gcc (macOS, Linux, Windows). No make or shell needed.

  python3 tools/build.py              -> build/fm1-stm32-unbrick.elf + .bin   (the recovery firmware)
  python3 tools/build.py --selftest   -> build/fm1-selftest.elf               (USB host self-test, powers CN5:
                                                                               never with the FM-1 connected)
Run tools/fetch_deps.py first.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TP = Path("third_party")

MCU = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16"]
DEFS = ["-DSTM32F407xx", "-DHSE_VALUE=8000000",
        "-DRTT_USE_ASM=0", "-DSEGGER_RTT_MAX_NUM_UP_BUFFERS=1", "-DSEGGER_RTT_MAX_NUM_DOWN_BUFFERS=1",
        "-DBUFFER_SIZE_UP=16384", "-DBUFFER_SIZE_DOWN=16384",
        "-DSEGGER_RTT_MODE_DEFAULT=SEGGER_RTT_MODE_NO_BLOCK_TRIM"]
INCS = ["-Isrc", "-I" + str(TP / "tinyusb" / "src"), "-I" + str(TP / "RTT" / "RTT"), "-I" + str(TP / "RTT" / "Config"),
        "-I" + str(TP / "cmsis_device_f4" / "Include"), "-I" + str(TP / "cmsis_core" / "CMSIS" / "Core" / "Include")]
CFLAGS = ["-O2", "-g3", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
          "-ffunction-sections", "-fdata-sections", "-fno-common"]

C_SRCS = ["src/main.c", "src/board.c", "src/recovery.c", "src/host.c", "src/proto.c", "src/log.c",
          "src/jieli_uboot.c",
          str(TP / "cmsis_device_f4" / "Source" / "Templates" / "system_stm32f4xx.c"),
          str(TP / "tinyusb" / "src" / "tusb.c"), str(TP / "tinyusb" / "src" / "common" / "tusb_fifo.c"),
          str(TP / "tinyusb" / "src" / "host" / "usbh.c"),
          str(TP / "tinyusb" / "src" / "portable" / "synopsys" / "dwc2" / "hcd_dwc2.c"),
          str(TP / "tinyusb" / "src" / "portable" / "synopsys" / "dwc2" / "dwc2_common.c"),
          str(TP / "RTT" / "RTT" / "SEGGER_RTT.c")]
A_SRCS = [str(TP / "cmsis_device_f4" / "Source" / "Templates" / "gcc" / "startup_stm32f407xx.s"),
          "src/loader_blob.S"]


def run(cmd):
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode:
        sys.exit("build: failed: %s" % " ".join(cmd[:3]))


def main():
    ap = argparse.ArgumentParser(description="Build fm1-stm32-unbrick")
    ap.add_argument("--selftest", action="store_true", help="build the USB host self-test firmware instead")
    a = ap.parse_args()

    gcc = shutil.which("arm-none-eabi-gcc")
    if not gcc:
        sys.exit("build: arm-none-eabi-gcc not found on the PATH (see README: Install)")
    objcopy = shutil.which("arm-none-eabi-objcopy") or "arm-none-eabi-objcopy"
    size = shutil.which("arm-none-eabi-size")
    if not (ROOT / "src" / "wl82loader.bin").is_file() or not (ROOT / TP / "tinyusb").is_dir():
        sys.exit("build: dependencies missing - run:  python3 tools/fetch_deps.py")

    target = "fm1-selftest" if a.selftest else "fm1-stm32-unbrick"
    objdir = Path("build") / ("obj-selftest" if a.selftest else "obj")
    (ROOT / objdir).mkdir(parents=True, exist_ok=True)
    extra = ["-DFM1_SELFTEST"] if a.selftest else []

    objs = []
    for src in C_SRCS:
        obj = str(objdir / (Path(src).stem + ".o"))
        run([gcc, *MCU, *DEFS, *extra, *INCS, *CFLAGS, "-c", src, "-o", obj])
        objs.append(obj)
    for src in A_SRCS:
        obj = str(objdir / (Path(src).stem + ".o"))
        run([gcc, *MCU, "-c", src, "-o", obj])
        objs.append(obj)

    elf = str(Path("build") / (target + ".elf"))
    run([gcc, *objs, *MCU, "-T", "stm32f407vg.ld", "-Wl,--gc-sections",
         "-Wl,-Map=" + str(Path("build") / (target + ".map")),
         "--specs=nano.specs", "--specs=nosys.specs", "-u", "_printf_float", "-o", elf])
    if not a.selftest:
        run([objcopy, "-O", "binary", elf, str(Path("build") / (target + ".bin"))])
    if size:
        run([size, elf])
    print("Built %s" % elf)
    print("(linker warnings that _close, _write, _read ... \"will always fail\" are expected: the firmware uses none of them)")


if __name__ == "__main__":
    main()
