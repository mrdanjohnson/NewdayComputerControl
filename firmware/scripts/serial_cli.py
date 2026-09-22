#!/usr/bin/env python3
"""Minimal scripted serial console for MacControl provisioning/testing.

`pio device monitor` needs an interactive terminal; this sends a fixed list
of CLI commands over pyserial and prints everything the device echoes back.

Usage:
  python3 scripts/serial_cli.py --port /dev/cu.usbmodem83101 \
      [--boot-wait 4] [--cmd "key create READ qa"] [--cmd ...] [--script-file cmds.txt]
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--boot-wait", type=float, default=4.0,
                    help="seconds to listen for the banner before sending commands")
    ap.add_argument("--settle", type=float, default=1.2,
                    help="seconds to wait for output after each command")
    ap.add_argument("--cmd", action="append", default=[], help="command to send (repeatable)")
    ap.add_argument("--script-file", help="file with one command per line")
    ap.add_argument("--reboot-first", action="store_true",
                    help="pulse DTR/RTS to reboot the device before listening")
    args = ap.parse_args()

    cmds = list(args.cmd)
    if args.script_file:
        with open(args.script_file, encoding="utf-8") as f:
            cmds += [ln.strip() for ln in f if ln.strip() and not ln.strip().startswith("#")]

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    try:
        # On the S3 USB-Serial/JTAG bridge the CDC lines drive EN (RTS) and
        # GPIO0 (DTR); leaving them asserted can hold the chip in reset.
        ser.dtr = False
        ser.rts = False
        if args.reboot_first:
            ser.rts = True
            time.sleep(0.2)
            ser.rts = False
            time.sleep(0.2)

        def drain(seconds: float) -> None:
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                chunk = ser.read(4096)
                if chunk:
                    sys.stdout.write(chunk.decode("utf-8", "replace"))
                    sys.stdout.flush()
                else:
                    time.sleep(0.05)

        drain(args.boot_wait)
        for cmd in cmds:
            print(f"\n>>> {cmd}", flush=True)
            ser.write(cmd.encode() + b"\n")
            drain(args.settle)
        print()
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
