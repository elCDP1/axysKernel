#!/usr/bin/env python3
"""Compare the kernel's /proc/pci with QEMU's own `info pci`.

usage: tools/qemu-pci-check.py [--qemu BINARY] [--iso FILE] [--] [QEMU ARGS...]

QEMU's monitor is the ground truth for every function's BAR kind, address and
size. The guest is booted with the given extra arguments (machine type and
devices), `cat /proc/pci` is run on its shell, and each BAR of each function
that both sides report must agree exactly. Exit status 0 only on a full match.

example:
  tools/qemu-pci-check.py -- -machine q35 -device qemu-xhci -device nvme,drive=d,serial=1 \\
      -drive id=d,if=none,file=@DISK2@,format=raw
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time


def parse_monitor(text):
    """-> {(bus, dev, fn): {"id": (ven, dev), "bars": {n: (kind, addr, size)}}}"""
    out, cur = {}, None
    for line in text.replace("\r", "").splitlines():
        m = re.match(r"\s*Bus\s+(\d+), device\s+(\d+), function (\d+):", line)
        if m:
            cur = tuple(int(x) for x in m.groups())
            out[cur] = {"id": None, "bars": {}}
            continue
        if cur is None:
            continue
        m = re.search(r"PCI device ([0-9a-f]{4}):([0-9a-f]{4})", line)
        if m:
            out[cur]["id"] = (int(m.group(1), 16), int(m.group(2), 16))
        m = re.match(r"\s*BAR(\d):\s+(?:(\d+) bit (prefetchable )?memory|I/O) at "
                     r"0x([0-9a-f]+) \[0x([0-9a-f]+)\]", line)
        if m:
            n, bits, pf, start, end = m.groups()
            start, end = int(start, 16), int(end, 16)
            if int(n) >= 6 or start == 0xffffffffffffffff:
                continue  # expansion ROM, or a BAR the firmware left unassigned
            kind = "io" if bits is None else ("mem64" if bits == "64" else "mem32")
            out[cur]["bars"][int(n)] = (kind, start, end - start + 1, bool(pf))
    return out


def parse_guest(text):
    out, cur = {}, None
    for line in text.replace("\r", "").splitlines():
        m = re.match(r"([0-9a-f]{2}):([0-9a-f]{2})\.(\d) ([0-9a-f]{4}):([0-9a-f]{4}) ", line)
        if m:
            b, d, f, ven, dev = m.groups()
            cur = (int(b, 16), int(d, 16), int(f))
            out[cur] = {"id": (int(ven, 16), int(dev, 16)), "bars": {}}
            continue
        m = re.match(r"\s+BAR(\d): (mem32|mem64|io)( prefetchable)? ([0-9a-f]+) size (\d+)([KMG]?)", line)
        if m and cur is not None:
            n, kind, pf, addr, size, unit = m.groups()
            size = int(size) << {"": 0, "K": 10, "M": 20, "G": 30}[unit]
            out[cur]["bars"][int(n)] = (kind, int(addr, 16), size, bool(pf))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64"))
    ap.add_argument("--iso", default="build/axys.iso")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("qemu_args", nargs="*")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="axys-pci-")
    sock_path = os.path.join(tmp, "mon.sock")
    disk = os.path.join(tmp, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(32 * 1024 * 1024)
    disk2 = os.path.join(tmp, "disk2.img")
    with open(disk2, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    extra = [a.replace("@DISK2@", disk2).replace("@DISK@", disk) for a in args.qemu_args]
    cmd = [args.qemu, "-cdrom", args.iso, "-m", "256M", "-display", "none", "-serial", "stdio",
           "-no-reboot", "-monitor", f"unix:{sock_path},server,nowait",
           "-drive", f"file={disk},format=raw,if=ide,index=0"] + extra
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(proc.stdout.fileno(), False)
    guest = b""
    sent = False
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        try:
            chunk = proc.stdout.read(4096)
        except BlockingIOError:
            chunk = None
        if chunk:
            guest += chunk
        else:
            time.sleep(0.1)
        text = guest.decode("latin-1")
        if not sent and "axys shell ready" in text and text.rstrip().endswith("#"):
            time.sleep(0.3)
            proc.stdin.write(b"cat /proc/pci\n")
            proc.stdin.flush()
            sent = True
        if sent and re.search(r"cat /proc/pci\r?\n(?:.*\r?\n)+axys# $", text):
            break
    time.sleep(0.3)
    try:
        while True:
            chunk = proc.stdout.read(4096)
            if not chunk:
                break
            guest += chunk
    except BlockingIOError:
        pass

    mon = socket.socket(socket.AF_UNIX)
    mon.settimeout(3)
    try:
        mon.connect(sock_path)
    except OSError as error:
        print(f"pci check: FAIL: cannot reach the QEMU monitor ({error}); QEMU said:")
        print(guest.decode("latin-1")[-1500:])
        proc.kill()
        return 1
    time.sleep(0.2)
    mon.recv(65536)
    mon.sendall(b"info pci\n")
    time.sleep(0.5)
    monitor_text = b""
    try:
        while True:
            part = mon.recv(65536)
            if not part:
                break
            monitor_text += part
    except socket.timeout:
        pass
    proc.kill()
    proc.wait()

    gtext = guest.decode("latin-1")
    start = gtext.find("cat /proc/pci")
    truth = parse_monitor(monitor_text.decode("latin-1"))
    got = parse_guest(gtext[start:] if start >= 0 else "")
    problems = []
    if not got:
        problems.append("the guest printed no /proc/pci (did it boot?)")
    for bdf, info in sorted(truth.items()):
        if bdf not in got:
            problems.append(f"{bdf[0]:02x}:{bdf[1]:02x}.{bdf[2]}: QEMU has it, the kernel did not find it")
            continue
        if info["id"] and got[bdf]["id"] != info["id"]:
            problems.append(f"{bdf}: id {got[bdf]['id']} != QEMU {info['id']}")
        for n, want in info["bars"].items():
            have = got[bdf]["bars"].get(n)
            if have != want:
                problems.append(f"{bdf[0]:02x}:{bdf[1]:02x}.{bdf[2]} BAR{n}: kernel {have} != QEMU {want}")
        for n in got[bdf]["bars"]:
            if n not in info["bars"]:
                problems.append(f"{bdf[0]:02x}:{bdf[1]:02x}.{bdf[2]} BAR{n}: kernel reports {got[bdf]['bars'][n]}, QEMU has none")
    for bdf in got:
        if bdf not in truth:
            problems.append(f"{bdf}: the kernel found a function QEMU does not have")
    bars = sum(len(v["bars"]) for v in truth.values())
    print(f"pci check: {len(truth)} functions, {bars} BARs compared: " + ("PASS" if not problems else "FAIL"))
    for p in problems:
        print("  " + p)
    if problems:
        print("--- guest /proc/pci ---")
        print(gtext[start:][:3000])
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
