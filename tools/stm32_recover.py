#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Based on fm1-esp32-unbrick tools/esp32_recover.py (MIT, dag), which drives MvaveFM1Unbricker's
# fm1_transporter_recover.py (MIT, Michael P Jones) unchanged.
"""stm32_recover: MvaveFM1Unbricker's fm1_transporter_recover.py, unchanged, driving the FM-1 Transporter
protocol on an STM32F4DISCOVERY board (fm1-stm32-unbrick).

The board speaks the Transporter's line protocol on SEGGER RTT channel 0. OpenOCD's RTT server turns that into
a TCP port on the Mac, so the "serial port" is a pyserial URL, by default socket://localhost:19021. Every check,
dump, comparison and write is the original script's; reads go as 4 KiB requests, each checked by the
firmware's CRC-32 and retried on a mismatch; a sector write is retried only on `ERR crc` (the payload arrived
damaged and nothing was erased).

Start OpenOCD first (see README), then e.g.
  stm32_recover.py log
  stm32_recover.py info --wait 180
  stm32_recover.py dump backups --package FM-1.fwsc
  stm32_recover.py restore FM-1.fwsc --ref backups/<dump>.bin           (dry run)
  stm32_recover.py restore FM-1.fwsc --ref backups/<dump>.bin --write   (writes)
Not `wizard`, `setup` or `flash-xiao` (they are for the XIAO RP2040)."""
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
UNBRICKER = Path(os.environ.get("FM1_UNBRICKER", HERE.parent / "third_party" / "MvaveFM1Unbricker"))
sys.path.insert(0, str(UNBRICKER))

try:
    import fm1_transporter_recover as ftr  # noqa: E402
except ImportError:
    sys.exit("stm32_recover: fm1_transporter_recover.py not found in %s\n"
             "  git clone https://github.com/Quixotic7/MvaveFM1Unbricker %s\n"
             "  (or set FM1_UNBRICKER to its folder)" % (UNBRICKER, UNBRICKER))

DEFAULT_PORT = os.environ.get("FM1_PORT", "socket://localhost:19021")
CHUNK = 4096
READ_TRIES = 8
WSEC_TRIES = 5


def candidate_ports():
    return [DEFAULT_PORT]


def open_serial(path, timeout=0.3):
    import serial
    s = serial.serial_for_url(path, do_not_open=True)
    s.baudrate, s.timeout = 115200, timeout
    s.open()
    return s


def open_port(path):
    try:
        s = open_serial(path)
    except Exception as e:
        ftr.log.debug("cannot open %s: %s", path, e)
        return None
    return ftr.Transporter(s, path)


def show_log(argv):
    port = argv[argv.index("--port") + 1] if "--port" in argv else DEFAULT_PORT
    try:
        s = open_serial(port, 1.0)
    except Exception as e:
        print("cannot open %s: %s (is OpenOCD's RTT server running?)" % (port, e))
        return 1
    s.reset_input_buffer()
    s.write(b"log\n")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        line = s.readline().decode("ascii", "replace").strip()
        if line.startswith("LOG "):
            n = int(line.split()[1])
            data = b""
            while len(data) < n:
                chunk = s.read(n - len(data))
                if not chunk:
                    break
                data += chunk
            sys.stdout.write(data.decode("ascii", "replace"))
            print("[%s: %d bytes of log]" % (port, n))
            return 0
    print("no fm1-stm32-unbrick board answered `log` on %s" % port)
    return 1


def resync(t):
    """After a broken transfer: let the rest of it arrive, then throw it away."""
    time.sleep(0.5)
    t.s.reset_input_buffer()


def read_chunk(t, addr, n):
    reply = t.request("read %#x %d" % (addr, n), 30.0)
    if reply != "DATA %d" % n:
        raise ftr.TransporterError("read 0x%X+0x%X: %s" % (addr, n, reply))
    data = bytearray()
    t.s.timeout = 5
    while len(data) < n:
        chunk = t.s.read(n - len(data))
        if not chunk:
            raise ftr.TransporterError("read 0x%X+0x%X stalled at %d bytes" % (addr, n, len(data)))
        data += chunk
    end = t._readline(5)
    want = "END %08X" % ftr.crc32(data)
    if end != want:
        raise ftr.TransporterError("read 0x%X+0x%X: transfer check failed (%s, local %s)"
                                   % (addr, n, end or "no END line", want))
    return bytes(data)


def read_flash(self, addr, length, label="Reading"):
    if addr < 0 or length <= 0 or addr + length > ftr.FLASH_SIZE:
        raise ftr.fu.SafetyError("read 0x%X+0x%X is outside the flash" % (addr, length))
    data = bytearray()
    prog = ftr.fu.Progress(label, length)
    retries = 0
    try:
        while len(data) < length:
            a, n = addr + len(data), min(CHUNK, length - len(data))
            for attempt in range(READ_TRIES):
                try:
                    data += read_chunk(self, a, n)
                    break
                except ftr.TransporterError as e:
                    retries += 1
                    ftr.log.debug("retry %d of 0x%X: %s", attempt + 1, a, e)
                    resync(self)
            else:
                raise ftr.TransporterError("read 0x%X failed %d times: the RTT link is losing too many "
                                           "bytes" % (a, READ_TRIES))
            prog.update(n)
    finally:
        prog.close()
    if retries:
        ftr.log.info("  (%d chunk%s re-read after a transfer check failed)", retries, "s" * (retries != 1))
    return bytes(data)


def write_sector(self, addr, sec):
    for attempt in range(WSEC_TRIES):
        reply = _write_sector(self, addr, sec)
        if not reply.startswith("ERR crc"):
            return reply
        ftr.log.info("  sector 0x%05X: %s (the payload arrived damaged, nothing was written): resending",
                     addr, reply)
        resync(self)
    return reply


_read_flash, _write_sector = ftr.Transporter.read_flash, ftr.Transporter.write_sector
ftr.Transporter.read_flash = read_flash
ftr.Transporter.write_sector = write_sector
ftr.candidate_ports = candidate_ports
ftr.open_port = open_port

if __name__ == "__main__":
    argv = sys.argv[1:]
    if argv[:1] == ["log"]:
        sys.exit(show_log(argv))
    if argv[:1] and argv[0] in ("wizard", "flash-xiao", "setup"):
        sys.exit("stm32_recover: `%s` is for the XIAO RP2040 Transporter; use info, dump, restore or recover" % argv[0])
    if argv and "--port" not in argv and argv[0] not in ("-h", "--help", "--version"):
        argv += ["--port", DEFAULT_PORT]
    sys.exit(ftr.main(argv))
