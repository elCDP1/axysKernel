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
# Typing far more keys than one xHCI ring segment holds: the link TRB of a
# full segment used to be written with the wrong cycle bit, so the keyboard
# went dead after ~63 keystrokes. 100 keys cross at least one wrap; the shell
# must echo (nearly) all of them as a single "unknown command: aaa...".
KEYS = int(os.environ.get('KEYS', '100'))
KEY_GAP = float(os.environ.get('KEY_GAP', '0.05'))
for _ in range(KEYS):
    r = cmd({'execute': 'send-key',
             'arguments': {'keys': [{'type': 'qcode', 'data': 'a'}]}})
    if 'error' in r:
        print(r)
        q.kill()
        verdict('FAIL', 1)
    time.sleep(KEY_GAP)
r = cmd({'execute': 'send-key',
         'arguments': {'keys': [{'type': 'qcode', 'data': 'ret'}]}})
if 'error' in r:
    print(r)
    q.kill()
    verdict('FAIL', 1)
time.sleep(2)
s.close()
# graceful poweroff through the shell is racy now that we typed into it;
# just stop the machine: survival this far with input delivered is the check.
q.terminate()
try:
    q.wait(timeout=15)
except subprocess.TimeoutExpired:
    q.kill()
with open(SERLOG, 'rb') as fh:
    serial_text = fh.read().decode('utf-8', 'replace')
# 90% of the keys must have survived: a dead ring stops at ~63, a couple of
# genuinely lost keystrokes do not fail the run. The console echoes the typed
# line back, so the run of 'a's in the serial log is the count that arrived
# (the shell drops unknown commands longer than 58 chars, so it cannot be
# read off "unknown command:").
want_keys = max(1, int(KEYS * 0.9))
if re.search(r'a{%d,}' % want_keys, serial_text):
    verdict('PASS', 0)
print('--- tail ---')
os.system('tail -c 800 %s' % SERLOG)
runs = [len(m.group(0)) for m in re.finditer(r'a+', serial_text)]
print('longest a-run: %d (wanted %d)' % (max(runs) if runs else 0, want_keys))
verdict('FAIL', 1)