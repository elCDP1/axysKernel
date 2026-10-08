#!/usr/bin/env python3
# Automated USB keyboard check: boot with xHCI + usb-kbd, wait for the kernel
# to configure the keyboard, inject keystrokes through the monitor and expect
# them echoed by the shell. Verdict: PASS/FAIL/NOBOOT/TIMEOUT.
# Topology is selected with TOPOLOGY=direct (default) or TOPOLOGY=hub, which
# puts the keyboard behind a USB hub so route strings and hub traversal are
# exercised too.
# Usage: python3 tools/qemu-usb.py (env: ISO, MEM, TIMEOUT, TOPOLOGY).
# Needs python3.
import socket, time, subprocess, os, sys, json, re

ISO = os.environ.get('ISO', 'build/axys.iso')
MEM = os.environ.get('MEM', '256M')
TIMEOUT = int(os.environ.get('TIMEOUT', '120'))
TOPOLOGY = os.environ.get('TOPOLOGY', 'direct')
QSOCK = '/tmp/axys-usb-qmp.sock'
SERLOG = '/tmp/axys-usb-serial.log'
if TOPOLOGY == 'hub':
    # The hub takes no id: its child bus is then named after the port path, so
    # the keyboard is attached with port=1.1 (port 1 of the hub) rather than
    # by bus name.
    USB_DEVICES = ['-device', 'usb-hub,bus=xhci.0',
                   '-device', 'usb-kbd,port=1.1']
    EXPECT = ('axys shell ready', 'usb: hub on port', 'keyboard on port')
elif TOPOLOGY == 'ps2':
    # No USB devices: the keys travel the PS/2 path, which isolates the input
    # layer and the shell from the xHCI driver.
    USB_DEVICES = []
    EXPECT = ('axys shell ready',)
else:
    USB_DEVICES = ['-device', 'usb-kbd,bus=xhci.0']
    EXPECT = ('axys shell ready', 'keyboard on port')
for f in (QSOCK, SERLOG):
    try:
        os.unlink(f)
    except OSError:
        pass
ser = open(SERLOG, 'wb')
q = subprocess.Popen(
    ['qemu-system-x86_64', '-cdrom', ISO, '-m', MEM, '-display', 'none',
     '-serial', 'stdio', '-no-reboot', '-device', 'qemu-xhci,id=xhci',
     *USB_DEVICES,
     '-qmp', 'unix:%s,server,nowait' % QSOCK],
    stdin=subprocess.DEVNULL, stdout=ser, stderr=subprocess.STDOUT)


def ser_contains(*needles):
    try:
        with open(SERLOG, 'rb') as f:
            data = f.read()
        return all(n.encode() in data for n in needles)
    except OSError:
        return False


def verdict(v, code):
    print('qemu-usb: %s' % v)
    sys.exit(code)


# wait for shell + keyboard
ok = False
for _ in range(TIMEOUT):
    time.sleep(1)
    if ser_contains(*EXPECT):
        ok = True
        break
if not ok:
    q.kill()
    verdict('NOBOOT' if not ser_contains('axys shell ready') else 'FAIL', 3)
time.sleep(2)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(QSOCK)
except OSError:
    q.kill()
    verdict('NOBOOT', 3)
s.settimeout(10)
f = s.makefile('rwb')


def cmd(o):
    f.write((json.dumps(o) + '\n').encode())
    f.flush()
    while True:
        d = json.loads(f.readline().decode())
        if 'return' in d or 'error' in d:
            return d


cmd({'execute': 'qmp_capabilities'})
# Typing far more keys than one xHCI ring segment holds: every report is one
# transfer TRB and one event TRB, and both rings are 256 entries, so 400 keys
# (800 reports) wrap each of them several times. A wrong cycle bit on either
# wrap makes the keyboard go silent at ~255 reports. The shell's line buffer is
# 256 bytes, so the keys are typed as lines of LINE keys: each line is echoed
# once as typed and once more in "unknown command: ...", i.e. twice.
KEYS = int(os.environ.get('KEYS', '400'))
KEY_GAP = float(os.environ.get('KEY_GAP', '0.04'))  # 25 keys/s: a real keyboard never exceeds one report per poll interval
LINE = int(os.environ.get('LINE', '50'))


def press(name):
    r = cmd({'execute': 'send-key',
             'arguments': {'keys': [{'type': 'qcode', 'data': name}]}})
    if 'error' in r:
        print(r)
        q.kill()
        verdict('FAIL', 1)
    time.sleep(KEY_GAP)


sent = 0
while sent < KEYS:
    for _ in range(min(LINE, KEYS - sent)):
        press('a')
        sent += 1
    press('ret')
def received_keys():
    with open(SERLOG, 'rb') as fh:
        text = fh.read().decode('utf-8', 'replace')
    runs = [len(m.group(0)) for m in re.finditer(r'a+', text)]
    return sum(r for r in runs if r >= 10) // 2


# The guest drains the keys at its own pace (the controller is polled every
# few milliseconds): wait for them instead of guessing a fixed delay.
deadline = time.time() + 30
while time.time() < deadline and received_keys() < sent:
    time.sleep(0.5)
s.close()
# graceful poweroff through the shell is racy now that we typed into it;
# just stop the machine: delivered input is the check.
q.terminate()
try:
    q.wait(timeout=15)
except subprocess.TimeoutExpired:
    q.kill()
with open(SERLOG, 'rb') as fh:
    serial_text = fh.read().decode('utf-8', 'replace')
runs = [len(m.group(0)) for m in re.finditer(r'a+', serial_text)]
# Runs shorter than 10 are prompts and words ("axys", "ready"); the rest are
# echoes of typed lines, two per line.
received = sum(r for r in runs if r >= 10) // 2
print('keys sent: %d, received: %d' % (sent, received))
if received >= int(sent * 0.98):
    verdict('PASS', 0)
print('--- tail ---')
os.system('tail -c 600 %s' % SERLOG)
verdict('FAIL', 1)
