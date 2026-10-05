#!/usr/bin/env python3
r"""
wakes-sp1 console logger.

Tees the SP-1's USB CDC console to the terminal AND to a timestamped file under
logs/, so a capture can be read back verbatim instead of retyped. Timestamps every
line, which matters for anything observed over time (charge curves, ladder sweeps).

    python -m pip install --user pyserial

    python C:\sp1-ws\wakes-sp1\tools\console-log.py --list
    python C:\sp1-ws\wakes-sp1\tools\console-log.py
    python C:\sp1-ws\wakes-sp1\tools\console-log.py COM7

(C:\sp1-ws standing for your west workspace, as in docs/BUILD.md.) An absolute path on
purpose: the workspace root is one level ABOVE this repo, and that is where the build
commands run from, so a relative "tools\console-log.py" fails there with Errno 2.

Requires M1c firmware or later to be FLASHED -- earlier builds have no USB at all,
so there is no serial device to open.

Ctrl-C to stop. Files land in logs/ (gitignored) as sp1-YYYYmmdd-HHMMSS.log.

If the port disappears -- unplugged, powered off, OR the device reset -- the logger
waits and reconnects rather than dying, and writes a marker line into the log. That
makes a spontaneous device reset VISIBLE in the capture (marker, then a fresh
banner) instead of appearing as the log simply ending. --no-reconnect disables it.

Sessions this logger could not see -- the USB-C port in another host's hands (the OP-XY),
or the device on battery -- are KEPT on the device (firmware/src/sp1_logbuf.h, the newest
~14 minutes in RAM). Keep Wakes ON afterwards, plug it into this computer and start the
logger: after the banner the device plays the stored log back between
"=== STORED LOG ... ===" markers, each line stamped with the device's uptime
("[+hh:mm:ss.mmm]") -- this logger's own timestamps on those lines are the time of the
dump, not of the event. Then live output resumes. Turning Wakes off loses the stored log.
"""
import argparse
import datetime
import pathlib
import sys

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing:  python -m pip install --user pyserial")

# CONFIG_SP1_USB_PID in firmware/Kconfig: 0x5213 from M5c (+ audio out), 0x5212 from M5a
# (console + MIDI), 0x5211 before (console only) -- all of them, so the logger works with
# any firmware. The VID is Zephyr's
# test VID until pid.codes grants one and may change, so match on PID.
WANT_PIDS = (0x5213, 0x5212, 0x5211)
PIDS_TEXT = " or ".join(f"{x:04x}" for x in WANT_PIDS)


def describe(p):
    vid = f"{p.vid:04x}" if p.vid is not None else "----"
    pid = f"{p.pid:04x}" if p.pid is not None else "----"
    return f"{p.device:10s} {vid}:{pid}  {p.description}"


def matches():
    return [p for p in list_ports.comports() if p.pid in WANT_PIDS]


def pick_port():
    """Only ever auto-pick a port that actually looks like the SP-1.

    Falling back to "any serial port" would silently attach to a motherboard COM
    port and sit there printing nothing, which is a worse failure than saying so."""
    m = matches()
    if len(m) == 1:
        return m[0].device
    if len(m) > 1:
        print("More than one SP-1-looking port; name one:")
        for p in m:
            print("  " + describe(p))
        sys.exit(1)

    others = list(list_ports.comports())
    print(f"No port with PID {PIDS_TEXT} (the SP-1 running M1c+).")
    if others:
        print("\nPorts that ARE present:")
        for p in others:
            print("  " + describe(p))
        print("\nIf one of these is the SP-1, name it explicitly:"
              "\n  python <this script> COM7")
    print("\nOtherwise, check in order:"
          "\n  1. is M1c (or later) actually FLASHED? earlier builds have no USB"
          "\n  2. is the device powered ON or in STANDBY, not fully off?"
          "\n  3. is the USB-C cable a data cable, not charge-only?")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="e.g. COM7 or /dev/ttyACM0")
    ap.add_argument("--list", action="store_true", help="list candidate ports and exit")
    ap.add_argument("--no-file", action="store_true", help="terminal only")
    ap.add_argument("--no-reconnect", action="store_true",
                    help="exit when the port disappears instead of waiting for it")
    args = ap.parse_args()

    if args.list:
        allp = list(list_ports.comports())
        if not allp:
            print("No serial ports at all.")
            return
        m = matches()
        for p in allp:
            tag = "  <-- SP-1" if p in m else ""
            print(describe(p) + tag)
        if not m:
            print(f"\n(nothing with PID {PIDS_TEXT} -- is M1c flashed?)")
        return

    port = args.port or pick_port()

    log = None
    if not args.no_file:
        # logs/ sits next to this script's parent (the repo root).
        d = pathlib.Path(__file__).resolve().parent.parent / "logs"
        d.mkdir(exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        path = d / f"sp1-{stamp}.log"
        log = open(path, "w", encoding="utf-8", buffering=1)
        print(f"logging to {path}")

    def emit(line, log_only=False):
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        out = f"{ts}  {line}"
        if not log_only:
            print(out)
        if log:
            log.write(out + "\n")

    # Baud is ignored by CDC ACM but pyserial wants one. A short timeout keeps
    # Ctrl-C responsive.
    def open_port():
        return serial.Serial(port, 115200, timeout=0.2)

    try:
        ser = open_port()
    except serial.SerialException as e:
        # On Windows a missing COM port surfaces as FileNotFoundError(2) wrapped in
        # a SerialException, which reads as "Errno 2 / no such file or directory"
        # and is easy to mistake for the SCRIPT being missing.
        if log:
            log.close()
        sys.exit(f"Could not open {port}: {e}\n"
                 f"That usually means the port does not exist (device not flashed "
                 f"with M1c+, powered off, or a charge-only cable) rather than a "
                 f"missing file.\nRun with --list to see what is present.")

    print(f"connected {port} — Ctrl-C to stop\n")
    buf = b""
    try:
        while True:
            try:
                chunk = ser.read(4096)
            except serial.SerialException as e:
                # The port went away. Three causes look identical from here:
                # unplugged, powered off, or THE DEVICE RESET. The last one is the
                # interesting case, so mark it in the log and try to come back --
                # a reconnect followed by a fresh banner is how a spontaneous
                # reboot becomes visible instead of the log just ending.
                emit(f"--- PORT LOST ({type(e).__name__}) — unplugged, powered off, "
                     f"or the device reset ---")
                try:
                    ser.close()
                except Exception:
                    pass
                if args.no_reconnect:
                    break
                emit("--- waiting for the port to come back (Ctrl-C to stop) ---")
                while True:
                    import time
                    time.sleep(1.0)
                    try:
                        ser = open_port()
                        break
                    except serial.SerialException:
                        continue
                emit("--- RECONNECTED ---")
                buf = b""
                continue

            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                emit(raw.decode("utf-8", "replace").rstrip("\r"))
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        try:
            ser.close()
        except Exception:
            pass
        if log:
            log.close()


if __name__ == "__main__":
    main()
