#!/usr/bin/env python3
"""USB hot-plug stress: add and remove a keyboard many times through QMP.

Every cycle must (1) be detected ("usb: keyboard on port"), (2) deliver typed
keys through the new device, and (3) be noticed on removal ("usb: device
removed"). Afterwards the kernel's free physical frames, kernel heap and VFS
node count must be back where they were: a leaked ring, context or slot shows
up as a drift. (QEMU sends keys to the newest keyboard, so keys typed while the
USB one is plugged in only arrive through it.)

usage: tools/qemu-usb-hotplug.py [--cycles N]   (env: ISO, QEMU)
Exit status 0 only on PASS.
"""
import argparse, json, os, re, socket, subprocess, sys, tempfile, time

ISO = os.environ.get("ISO", "build/axys.iso")
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=15)
    ap.add_argument("--topology", choices=("direct", "hub"), default="direct")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="axys-hp-")
    qsock = os.path.join(tmp, "qmp.sock")
    cmd = [QEMU, "-cdrom", ISO, "-m", "256M", "-display", "none", "-serial", "stdio",
           "-no-reboot", "-device", "qemu-xhci,id=xhci", "-qmp", f"unix:{qsock},server,nowait"]
    if args.topology == "hub":
        cmd += ["-device", "usb-hub,bus=xhci.0,port=1"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(proc.stdout.fileno(), False)
    log = bytearray()

    def pump(wait=0.05):
        try:
            chunk = proc.stdout.read(8192)
        except BlockingIOError:
            chunk = None
        if chunk:
            log.extend(chunk)
        else:
            time.sleep(wait)
        return log.decode("latin-1")

    def wait_for(pred, what, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred(pump()):
                return True
        print(f"hotplug: TIMEOUT waiting for {what}")
        return False

    def fail(msg):
        print("hotplug: FAIL:", msg)
        print("--- tail ---")
        print(log.decode("latin-1")[-1200:])
        proc.kill()
        return 1

    if not wait_for(lambda t: "axys shell ready" in t, "the shell", 60):
        return fail("no shell")
    time.sleep(1.0)
    q = socket.socket(socket.AF_UNIX)
    q.settimeout(10)
    q.connect(qsock)
    qf = q.makefile("rwb")

    def qmp(obj):
        qf.write((json.dumps(obj) + "\n").encode())
        qf.flush()
        while True:
            d = json.loads(qf.readline().decode())
            if "return" in d or "error" in d:
                return d

    qmp({"execute": "qmp_capabilities"})

    def serial_cmd(text, expect, timeout=10.0):
        start = len(log)
        proc.stdin.write((text + "\n").encode())
        proc.stdin.flush()
        wait_for(lambda t: expect in t[start:], f"{text!r} output", timeout)
        return log[start:].decode("latin-1")

    def meminfo():
        out = serial_cmd("meminfo", "live_nodes=")
        m = re.search(r"frames_free=(\d+) heap_used=(\d+) heap_free=(\d+) live_nodes=(\d+)", out)
        return tuple(int(x) for x in m.groups()) if m else None

    def key(name):
        qmp({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": name}]}})
        time.sleep(0.12)

    def plug(i):
        extra = {"bus": "xhci.0"} if args.topology == "direct" else {"bus": "xhci.0", "port": "1.1"}
        r = qmp({"execute": "device_add", "arguments": {"driver": "usb-kbd", "id": f"kb{i}", **extra}})
        return "error" not in r, r

    def unplug(i):
        return "error" not in qmp({"execute": "device_del", "arguments": {"id": f"kb{i}"}})

    def count(text, needle):
        return text.count(needle)

    def cycle(i):
        before = count(log.decode("latin-1"), "usb: keyboard on port")
        ok, r = plug(i)
        if not ok:
            return f"device_add failed: {r}"
        if not wait_for(lambda t: count(t, "usb: keyboard on port") > before, f"keyboard #{i} to enumerate", 20):
            return "keyboard was not detected after plug-in"
        time.sleep(0.5)
        start = len(log)
        for _ in range(5):
            key("a")
        key("ret")
        if not wait_for(lambda t: "unknown command: aaaaa" in t[start:], "typed keys to arrive", 10):
            return "keys typed on the new keyboard never arrived"
        removed = count(log.decode("latin-1"), "usb: device removed")
        if not unplug(i):
            return "device_del failed"
        if not wait_for(lambda t: count(t, "usb: device removed") > removed, "the removal to be noticed", 15):
            return "unplug was not noticed"
        time.sleep(0.3)
        return None

    problem = cycle(0)  # warm-up: lazily created structures are allocated here
    if problem:
        return fail(f"warm-up cycle: {problem}")
    base = meminfo()
    if base is None:
        return fail("could not read meminfo")
    for i in range(1, args.cycles + 1):
        problem = cycle(i)
        if problem:
            return fail(f"cycle {i}: {problem}")
    time.sleep(1.0)
    after = meminfo()
    if after is None:
        return fail("could not read meminfo afterwards")
    proc.kill()
    names = ("frames_free", "heap_used", "heap_free", "live_nodes")
    drift = {n: a - b for n, a, b in zip(names, after, base)}
    print(f"hotplug: {args.cycles} cycles, base {dict(zip(names, base))}, after {dict(zip(names, after))}")
    leaked = -drift["frames_free"] > 4 or drift["heap_used"] > 4096 or drift["live_nodes"] != 0
    print("hotplug:", "FAIL (resources leaked)" if leaked else "PASS")
    return 1 if leaked else 0


if __name__ == "__main__":
    sys.exit(main())
