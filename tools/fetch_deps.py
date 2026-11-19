#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fetch everything this repository does not ship, at pinned commits, into third_party/ (macOS, Linux, Windows).

  - JieLi's WL82 flash loader (from kagaimiq/jl-uboot-tool), copied to src/ after a sha256 check.
    It is JieLi's code, not ours to redistribute: it is fetched here and built into your own firmware.
  - MvaveFM1Unbricker: the recovery script tools/stm32_recover.py drives.
  - TinyUSB, ST's CMSIS headers and SEGGER RTT: only needed to build the firmware.

Needs git on the PATH. Run from anywhere:  python3 tools/fetch_deps.py
"""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TP = ROOT / "third_party"

DEPS = [
    ("jl-uboot-tool", "https://github.com/kagaimiq/jl-uboot-tool", "adb3f18889e88ac512ce0a3c4d8cc3d3cb30696a"),
    ("MvaveFM1Unbricker", "https://github.com/Quixotic7/MvaveFM1Unbricker", "517b9387e62e908115e5eec66c32d0ac3289f039"),
    ("tinyusb", "https://github.com/hathach/tinyusb", "dae3f9a366bfcddbf9dcf1b48d7500286a849539"),
    ("cmsis_device_f4", "https://github.com/STMicroelectronics/cmsis_device_f4", "8c79dcf2f1d866d139a5d66240083ae6194b6dd1"),
    ("cmsis_core", "https://github.com/STMicroelectronics/cmsis_core", "01d18147527e30ed69ace1cb03b818de78bc7062"),
    ("RTT", "https://github.com/SEGGERMicro/RTT", "9106eaa39babe53e00994d7828c1afb29390cdc1"),
]
LOADER = TP / "jl-uboot-tool" / "data" / "loaderblobs" / "usb" / "wl82loader.bin"
LOADER_SHA256 = "d41da6126760c9d66660bcc0cac8d27d221806c5e369a8036921efe68dca5376"


def git(*args, quiet=True):
    return subprocess.run(["git", *args], check=True, capture_output=quiet, text=True).stdout


def fetch(name, url, commit):
    d = TP / name
    if not (d / ".git").is_dir():
        git("clone", "--quiet", "--filter=blob:none", url, str(d))
    try:
        git("-C", str(d), "cat-file", "-e", commit + "^{commit}")
    except subprocess.CalledProcessError:
        git("-C", str(d), "fetch", "--quiet", "origin", commit)
    git("-C", str(d), "-c", "advice.detachedHead=false", "checkout", "--quiet", commit)
    print("  %-18s @ %s" % (name, git("-C", str(d), "rev-parse", "--short", "HEAD").strip()))


def main():
    if not shutil.which("git"):
        sys.exit("fetch_deps: git not found on the PATH (install Git first)")
    TP.mkdir(exist_ok=True)
    print("Fetching pinned dependencies into third_party/ ...")
    for dep in DEPS:
        fetch(*dep)
    got = hashlib.sha256(LOADER.read_bytes()).hexdigest()
    if got != LOADER_SHA256:
        sys.exit("wl82loader.bin sha256 mismatch: %s (expected %s)" % (got, LOADER_SHA256))
    shutil.copyfile(LOADER, ROOT / "src" / "wl82loader.bin")
    print("  src/wl82loader.bin (sha256 verified)")
    print("Done. Build with:  python3 tools/build.py")


if __name__ == "__main__":
    main()
