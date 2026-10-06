#!/usr/bin/env python3
# Automated USB keyboard check: boot with xHCI + usb-kbd, wait for the kernel
# to configure the keyboard, inject keystrokes through the monitor and expect
# them echoed by the shell. Verdict: PASS/FAIL/NOBOOT/TIMEOUT.
# Topology is selected with TOPOLOGY=direct (default) or TOPOLOGY=hub, which
# puts the keyboard behind a USB hub so route strings and hub traversal are
# exercised too.
# Usage: python3 tools/qemu-usb.py (env: ISO, MEM, TIMEOUT, TOPOLOGY).
# Needs python3.
import socket, time, subprocess, os, sys, json

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
for key in ('a', 'ret'):
    r = cmd({'execute': 'send-key',
             'arguments': {'keys': [{'type': 'qcode', 'data': key}]}})
    if 'error' in r:
        print(r)
        q.kill()
        verdict('FAIL', 1)
    time.sleep(1)
time.sleep(2)
s.close()
# graceful poweroff through the shell is racy now that we typed into it;
# just stop the machine: survival this far with input delivered is the check.
q.terminate()
try:
    q.wait(timeout=15)
except subprocess.TimeoutExpired:
    q.kill()
if ser_contains('unknown command: a'):
    verdict('PASS', 0)
print('--- tail ---')
os.system('tail -c 800 %s' % SERLOG)
verdict('FAIL', 1)