#!/usr/bin/env python3

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGINS = "/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins"
GDB = (PLUGINS + "/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32."
       "14.3.rel1.macos64_1.0.100.202602081740/tools/bin/arm-none-eabi-gdb")
GDBSERVER = (PLUGINS + "/com.st.stm32cube.ide.mcu.externaltools.stlink-gdb-server."
             "macos64_2.2.400.202601091506/tools/bin/ST-LINK_gdbserver")
CUBEPROG = (PLUGINS + "/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer."
            "macos64_2.2.400.202601091506/tools/bin")

ACCESS_PORT = 1
ELF = os.path.join(ROOT, "Appli", "Debug", "KikkarProject_Appli.elf")
PORT = 61234

ROM_ORIGIN = 0x34000400


def find_vcp():
    ports = [p for p in os.listdir("/dev") if p.startswith("cu.usbmodem")]
    if not ports:
        sys.exit("no /dev/cu.usbmodem* -- is the board plugged in?")
    return "/dev/" + sorted(ports)[0]


def check_free(vcp):
    """Refuses to run while STM32CubeIDE holds the probe."""
    held = []
    try:
        out = subprocess.run(["lsof", vcp], capture_output=True, text=True).stdout
        for line in out.splitlines()[1:]:
            held.append(line.split()[0])
    except Exception:                               # noqa: BLE001
        pass
    if held:
        sys.exit("%s is open in %s.\nClose that serial terminal and stop any "
                 "debug session, then re-run." % (vcp, ", ".join(sorted(set(held)))))

    ps = subprocess.run(["pgrep", "-f", "ST-LINK_gdbserver"],
                        capture_output=True, text=True).stdout.split()
    if ps:
        sys.exit("ST-LINK_gdbserver is already running (pid %s) -- stop the "
                 "STM32CubeIDE debug session first." % ", ".join(ps))


class SerialReader(threading.Thread):
    def __init__(self, port, baud):
        super().__init__(daemon=True)
        self.port, self.baud = port, baud
        self.lines, self.stop = [], False

    def run(self):
        import serial
        try:
            s = serial.Serial(self.port, self.baud, timeout=0.2)
        except Exception as e:                      # noqa: BLE001
            self.lines.append("<<serial open failed: %s>>" % e)
            return
        buf = b""
        while not self.stop:
            try:
                buf += s.read(4096)
            except Exception:                       # noqa: BLE001
                break
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.append(line.decode("utf-8", "replace").rstrip("\r"))
        if buf:
            self.lines.append(buf.decode("utf-8", "replace").rstrip("\r"))
        s.close()


def build():
    print("building ...")
    r = subprocess.run(["make", "-C", ROOT], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        sys.exit("build failed")
    for line in r.stdout.splitlines():
        if "text" in line or "collision" in line:
            print("  " + line.strip())


def start_gdbserver():
    log = open(os.path.join(tempfile.gettempdir(), "kikkar_gdbserver.log"), "w+")
    srv = subprocess.Popen(
        [GDBSERVER, "-p", str(PORT), "-m", str(ACCESS_PORT), "--halt",
         "-cp", CUBEPROG, "-e"],
        stdout=log, stderr=subprocess.STDOUT, text=True)
    time.sleep(2.5)
    if srv.poll() is not None:
        log.seek(0)
        print(log.read())
        sys.exit("gdbserver exited immediately")
    return srv


def gdb_script(elf, extra, seconds, free):
    cmds = [
        "set confirm off", "set pagination off",
        "target remote localhost:%d" % PORT,
        "monitor halt",
        "load",
        "set $sp = *(unsigned int *)0x%08X" % ROM_ORIGIN,
        "set $pc = *(unsigned int *)0x%08X" % (ROM_ORIGIN + 4),
    ] + extra

    if free:
        cmds += ["detach", "quit"]
    else:
        cmds += ["continue &", "shell sleep %g" % max(0.0, seconds),
                 "interrupt", "info registers pc sp", "detach", "quit"]
    return cmds


def run_gdb(elf, cmds, timeout):
    with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False) as f:
        f.write("\n".join(cmds) + "\n")
        path = f.name
    try:
        g = subprocess.run([GDB, "-batch", "-nx", "-x", path, elf],
                           capture_output=True, text=True, timeout=timeout)
    finally:
        os.unlink(path)
    return g.stdout + g.stderr


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    ap = argparse.ArgumentParser(description='Build, download to SRAM, run and capture the serial log.',
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--free", action="store_true",
                    help="detach after starting, so nothing is on SWD while it runs")
    ap.add_argument("--gdb", action="append", default=[],
                    help="extra GDB command after load (repeatable)")
    ap.add_argument("--elf", default=ELF)
    args = ap.parse_args()

    vcp = find_vcp()
    check_free(vcp)

    if not args.no_build:
        build()
    if not os.path.exists(args.elf):
        sys.exit("missing %s" % args.elf)

    reader = SerialReader(vcp, args.baud)
    reader.start()

    print("starting gdbserver ...")
    srv = start_gdbserver()

    print("loading and running (%.0f s) ..." % args.seconds)
    cmds = gdb_script(args.elf, args.gdb, args.seconds, args.free)
    gdb_out = run_gdb(args.elf, cmds, args.seconds + 90)

    if args.free:
        stop(srv)
        time.sleep(args.seconds)

    time.sleep(0.5)
    reader.stop = True
    reader.join(timeout=3)
    stop(srv)

    shown = [l for l in gdb_out.splitlines()
             if re.search(r"error|fail|warning|Remote|Load", l, re.I)]
    if shown:
        print("\n--- gdb ---")
        for line in shown:
            print("  " + line)

    print("\n================ TARGET OUTPUT ================")
    for line in reader.lines:
        print(line)
    print("============ %d lines ============" % len(reader.lines))
    return 0 if reader.lines else 1


if __name__ == "__main__":
    sys.exit(main())
