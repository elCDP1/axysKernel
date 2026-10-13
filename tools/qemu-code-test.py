#!/usr/bin/env python3
"""Drive /bin/code over serial: open, edit, save, quit, tree browse.

Sends real keys (arrows, Enter, Ctrl+S, lone Esc) and checks the saved
bytes plus a clean exit. Exit status 0 only if every check passes.

usage: tools/qemu-code-test.py       (env: ISO, QEMU)
"""
import os, subprocess, sys, tempfile, time

ISO = os.environ.get("ISO", "build/axys.iso")
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

tmp = tempfile.mkdtemp(prefix="axys-code-")
disk = os.path.join(tmp, "d.img")
open(disk, "wb").truncate(32 * 1024 * 1024)
log = os.path.join(tmp, "serial.log")
ser = open(log, "wb")
proc = subprocess.Popen(
    [QEMU, "-cdrom", ISO, "-m", "256M", "-display", "none", "-serial", "stdio", "-no-reboot",
     "-drive", "file=%s,format=raw,if=ide,index=0" % disk],
    stdin=subprocess.PIPE, stdout=ser, stderr=subprocess.STDOUT)

deadline = time.time() + 60
while time.time() < deadline:
    time.sleep(0.5)
    with open(log, "rb") as fh:
        if b"axys shell ready" in fh.read():
            break


def send(data, gap=0.4):
    proc.stdin.write(data)
    proc.stdin.flush()
    time.sleep(gap)


send(b"echo hello > /tmp/edit1\n")
send(b"code /tmp/edit1\n", 2.0)  # opens the file, focus=editor
send(b"\x1b[F")                   # End of "hello"
send(b"\n")                       # split line
send(b"second")                   # type
send(b"\x13")                     # Ctrl+S
send(b"\x1b", 1.0)                # lone Esc: quit, clean
send(b"cat /tmp/edit1\n", 1.0)
send(b"code /tmp\n", 2.0)         # tree only
send(b"\x1b[B")                   # Down
send(b"q", 1.0)                   # quit, nothing dirty
send(b"echo survived\n", 1.0)
send(b"poweroff\n")
time.sleep(3)
proc.terminate()
try:
    proc.wait(timeout=15)
except Exception:
    proc.kill()
ser.close()
with open(log, "rb") as fh:
    text = fh.read().decode("latin-1")

checks = [
    ("edit survives a save", "hello\nsecond" in text),
    ("shell back after quit", "survived" in text),
    ("alternate screen used", "?1049h" in text and "?1049l" in text),
    ("clean exit (no [exit N])", "[exit" not in text.split("cat /tmp/edit1")[0][-200:]),
]
rc = 0
for name, ok in checks:
    print(("ok    " if ok else "FAIL  ") + name)
    rc = rc or not ok
print("code-test: %s" % ("PASS" if rc == 0 else "FAIL"))
sys.exit(rc)
