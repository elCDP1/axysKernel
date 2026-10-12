#!/usr/bin/env python3
"""Drive the shell over the serial port and check what every command prints.

Each case sends one line and compares the output (up to the next prompt) with
regular expressions that must match, and others that must not. Covers quoting,
redirection, pipes, the text filters, the system commands, error handling, the
RTC, the kernel log, live /proc files (including USB hot-plug through QMP) and
permissions. Exit status 0 only if every case passes.

usage: tools/qemu-shell-test.py [-v]       (env: ISO, QEMU)
"""
import json, os, re, socket, subprocess, sys, tempfile, time

ISO = os.environ.get("ISO", "build/axys.iso")
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
VERBOSE = "-v" in sys.argv
PROMPT = re.compile(r"axys[#$] $")

tmp = tempfile.mkdtemp(prefix="axys-sh-")
qsock = os.path.join(tmp, "qmp.sock")
disk = os.path.join(tmp, "d.img")
open(disk, "wb").truncate(32 * 1024 * 1024)
proc = subprocess.Popen(
    [QEMU, "-cdrom", ISO, "-m", "256M", "-display", "none", "-serial", "stdio", "-no-reboot",
     "-device", "qemu-xhci,id=xhci", "-drive", f"file={disk},format=raw,if=ide,index=0",
     "-qmp", f"unix:{qsock},server,nowait"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
os.set_blocking(proc.stdout.fileno(), False)
buf = bytearray()


def pump(wait=0.03):
    try:
        chunk = proc.stdout.read(8192)
    except BlockingIOError:
        chunk = None
    if chunk:
        buf.extend(chunk)
    else:
        time.sleep(wait)


def wait_prompt(timeout):
    end = time.time() + timeout
    while time.time() < end:
        pump()
        if PROMPT.search(buf.decode("latin-1")):
            return True
    return False


def run(line, timeout=15.0):
    """Send a line; return the output between the echo and the next prompt."""
    del buf[:]
    proc.stdin.write((line + "\n").encode())
    proc.stdin.flush()
    ok = wait_prompt(timeout)
    text = buf.decode("latin-1").replace("\r", "")
    if not ok:
        text += "\n<<TIMEOUT: no prompt>>"
    # drop the echoed command line and the trailing prompt
    text = re.sub(r"axys[#$] $", "", text)
    first, _, rest = text.partition("\n")
    return rest if first.strip().endswith(line.strip()) or line.strip() in first else text


failures = []
checks = 0


def case(line, want=(), forbid=(), timeout=15.0, exact=None):
    global checks
    checks += 1
    out = run(line, timeout)
    bad = []
    if exact is not None and out.strip("\n") != exact:
        bad.append(f"expected exactly {exact!r}")
    for w in want:
        if not re.search(w, out, re.M):
            bad.append(f"missing /{w}/")
    for f in forbid:
        if re.search(f, out, re.M):
            bad.append(f"unexpected /{f}/")
    if "<<TIMEOUT" in out:
        bad.append("timeout")
    if bad:
        failures.append((line, bad, out))
        print(f"FAIL  {line}: {'; '.join(bad)}")
    elif VERBOSE:
        print(f"ok    {line}")
    return out


end = time.time() + 90
while time.time() < end and "axys shell ready" not in buf.decode("latin-1"):
    pump()
if not wait_prompt(30):
    print("shell-test: FAIL: no shell prompt")
    print(buf.decode("latin-1")[-800:])
    proc.kill()
    sys.exit(1)

# ---- help ---------------------------------------------------------------------------
case("help", want=[r"Files and directories", r"Text and data", r"System and users",
                   r"Processes and hardware", r"Programs in /bin", r"^  ls +list a directory",
                   r"^  grep +lines containing", r"^  sort +sort lines"], forbid=[r"no pipes yet"])
case("help grep", want=[r"^grep - ", r"-i -n -v -c -r"])
case("help nosuchcmd", want=[r"-\d+|unknown|no help"])

# ---- quoting, `;`, redirection --------------------------------------------------------
case('echo "a   b"  c', exact="a   b c")
case("echo 'x;y'; echo z", exact="x;y\nz")
case(r"echo a\ b", exact="a b")
case("echo -n hi; echo !", exact="hi!")
case("echo one > /tmp/f; echo two >> /tmp/f; cat /tmp/f", exact="one\ntwo")
case("echo three > /tmp/f; cat /tmp/f", exact="three")
case("cat < /tmp/f", exact="three")
case('echo "a > b" > /tmp/q; cat /tmp/q', exact="a > b")
case("echo hi >", want=[r"syntax error"])
case("echo hi | | cat", want=[r"syntax error"])
case("cat < /nonexistent", want=[r"nonexistent: -2"])
case("echo x > /nonexistent/dir/f", want=[r"-2"])
case("ls /nope > /tmp/err; cat /tmp/err", want=[r"^/nope: -2"], forbid=[r"^\s*$\n^.+"])  # error is not redirected
case("ls /tmp", forbid=[r"\.pipe"])

# ---- pipes and filters ---------------------------------------------------------------
for i, w in enumerate(["pear", "apple", "fig", "apple", "banana"]):
    run(f"echo {w} {'>' if i == 0 else '>>'} /tmp/fruit")
case("cat /tmp/fruit | wc -l", exact="5")
case("cat /tmp/fruit | sort", exact="apple\napple\nbanana\nfig\npear")
case("sort -r /tmp/fruit | head -n 2", exact="pear\nfig")
case("sort -u /tmp/fruit", exact="apple\nbanana\nfig\npear")
case("cat /tmp/fruit | sort | uniq | wc -l", exact="4")
case("grep -c apple /tmp/fruit", exact="2")
case("grep -n an /tmp/fruit", exact="5:banana")
case("grep -v a /tmp/fruit", exact="fig")
case("cat /tmp/fruit | grep -i APPLE | wc -l", exact="2")
case("grep p /tmp/fruit /tmp/fruit | head -n 1", exact="/tmp/fruit:pear")
case("tac /tmp/fruit | head -n 1", exact="banana")
case("nl /tmp/fruit | tail -n 1", want=[r"^\s+5\tbanana$"])
case("wc /tmp/fruit", want=[r"^\s+5\s+5\s+\d+ /tmp/fruit$"])
case("wc -l /tmp/fruit /tmp/fruit", want=[r"^5 /tmp/fruit$", r"^10 total$"])
case("echo a:b:c | cut -d: -f2", exact="b")
case("echo 10 9 100 > /tmp/n; seq 1 3 | sort -rn", exact="3\n2\n1")
case("cat /tmp/fruit | tail -n 2 | head -n 1", exact="apple")
case("hello world | cat", want=[r'args="world"'])
case("hello world > /tmp/h; cat /tmp/h | grep -c ring", exact="1")
case("echo a | cat | cat | cat | cat | cat | cat | cat | cat", want=[r"too many pipeline stages"])
case("ls /tmp", forbid=[r"\.pipe"])  # every temporary pipe is removed
case("echo hi > /tmp/x | cat", exact="")  # previous stage wrote a file: empty input, not a hang
case("echo hi > /tmp/x | grep hi", exact="")  # grep reads the empty pipe, like Linux

# ---- devices (/dev/null, /dev/zero) ------------------------------------------------------
case("ls /dev", want=[r"^null$", r"^zero$"])
case("ls -l /dev/null", want=[r"^-\S+ 0 0 0\t/dev/null$"])
case("stat /dev/null", want=[r"type: file", r"size: 0 bytes"])
case("cat /dev/null", exact="")
case("wc -c /dev/null", exact="0 /dev/null")
case("cmp /dev/null /dev/null", exact="identical")
case("echo data > /tmp/devd; cat /tmp/devd > /dev/null; echo done", exact="done")
case("head -c 8 /dev/zero | wc -c", exact="8")
case("head -c 4 /dev/zero > /dev/null; cat /tmp/fruit | head -n 1", exact="pear")  # recycled fd slot must not stay a device
case("head -c 5 /tmp/fruit", exact="pear")
case("tail -c 7 /tmp/fruit", exact="banana")

# ---- exact outputs -------------------------------------------------------------------------
case("uname -a", exact="axysOS 1.0 x86_64")
case("free", want=[r"^MemFree: \d+ KiB$", r"^Heap: \d+ used / \d+ free bytes$", r"^VfsNodes: \d+ live$"])
case("df", want=[r"^backend: ", r"^capacity: \d+ bytes \(\d+ sectors\)$"])
case("stat /tmp", want=[r"type: directory"])
case("du -h /tmp", want=[r"\t/tmp$"])
case("cat -n /tmp/fruit | head -n 1", want=[r"^1\tpear$"])
case("seq 1 3 | tail -c 2", exact="3")
case("basename /a/b/c", exact="c")
case("dirname /a/b/c", exact="/a/b")

# ---- ls, files ------------------------------------------------------------------------
case("ls /tmp/f", exact="/tmp/f")
case("ls -l /tmp/f", want=[r"^-rw-r--r-- 0 0 6\t/tmp/f$"])
case("ls /bin | grep -c .", want=[r"^\d+$"])
case("ls /sbin /bin | head -n 1", exact="/sbin:")
case("ls -l /sbin", want=[r"rwxr-xr-x 0 0 \d+\tinit"])
case("mkdir /tmp/d; cd /tmp/d; pwd", exact="/tmp/d")
case("touch a; echo hi > b; ls", exact="a\nb")
case("cp b c; cat c", exact="hi")
case("mv c e; ls", exact="a\nb\ne")
case("cd ..; pwd", exact="/tmp")
case("rmtree /tmp/d; ls /tmp/d", want=[r"-2"])

# ---- system commands -------------------------------------------------------------------
case("uname", want=[r"axys"])
case("hostname", exact="axys")
case("whoami", want=[r"root"])
case("id", want=[r"uid=0"])
case("ps", want=[r"^  PID  PPID   UID S NAME$", r"^\s+\d+\s+-?\d+\s+\d+ [RZ] init$"])
case("lscpu", want=[r"QEMU|vendor|Vendor"])
case("lspci", want=[r"8086:1237", r"1234:1111"])
case("df", want=[r"\S"])
case("ip", want=[r"10\.0\.2\.15", r"52:54:00:12:34:56"])
case("uptime", want=[r"\d"])
case("which ls", want=[r"ls"])
case("seq 3", exact="1\n2\n3")

# ---- real-time clock ------------------------------------------------------------------
out = case("date", want=[r"^(Sun|Mon|Tue|Wed|Thu|Fri|Sat) (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d\d \d\d:\d\d:\d\d UTC \d{4}$"])
case("date +%F", want=[r"^\d{4}-\d\d-\d\d$"])
t1 = int(run("date +%s").strip() or 0)
time.sleep(2.2)
t2 = int(run("date +%s").strip() or 0)
checks += 1
if not (1 <= t2 - t1 <= 4):
    failures.append(("date +%s", [f"clock advanced {t2 - t1}s in ~2s"], ""))
    print("FAIL  date +%s: clock did not advance with real time")
case("date -s '2031-02-03 04:05:06'; date +'%F %T'", want=[r"^2031-02-03 04:05:0[6-9]$"])
case("date -s 'garbage'", want=[r"-22"])
case("date -s '2031-02-30 00:00:00'", want=[r"-22"])  # not a real day

# ---- kernel log and clear ---------------------------------------------------------------
case("dmesg | head -n 1", exact="axys kernel online")
for _ in range(60):
    run("echo noise > /dev/null")
case("dmesg | grep -c 'pit:'", exact="1")       # boot messages survive heavy shell use
case("clear", want=[r"\x1b\[2J"])

# ---- /proc is live (USB hot-plug through QMP) ------------------------------------------------
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
case("cat /proc/usb", want=[r"keyboards: 0"], forbid=[r"^dev slot"])
qmp({"execute": "device_add", "arguments": {"driver": "usb-kbd", "id": "kb1", "bus": "xhci.0"}})
time.sleep(3)
case("cat /proc/usb", want=[r"keyboards: 1", r"^dev slot \d+ port \d+ ID [0-9a-f]{4}:[0-9a-f]{4} class \d\d \w+-speed keyboard$"])
case("lsusb", want=[r"keyboard"])
qmp({"execute": "device_del", "arguments": {"id": "kb1"}})
time.sleep(3)
case("cat /proc/usb", want=[r"keyboards: 0"], forbid=[r"^dev slot"])

# ---- permissions --------------------------------------------------------------------------
case("su 1000; whoami", want=[r"user|1000"])
case("echo x > /etc/shadow", want=[r"-13"])
case("echo x > /tmp/mine; cat /tmp/mine", exact="x")
case("date -s '2031-01-01 00:00:00'", want=[r"-1\b|-13"])  # only root sets the clock
case("rm /etc/passwd", want=[r"-13"])

proc.kill()
print(f"shell-test: {checks - len(failures)}/{checks} checks passed")
if failures:
    print("--- failures ---")
    for line, bad, out in failures:
        print(f"$ {line}\n  {'; '.join(bad)}\n  output: {out!r}")
    sys.exit(1)
print("shell-test: PASS")
