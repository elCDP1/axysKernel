#!/bin/sh
# Headless QEMU boot tests. Stage 1 checks every boot milestone; stage 2 drives
# the user-space shell over the serial port and checks its answers; stage 3
# checks persistence across a reboot; stage 4 (EXPECT_NET=1) checks DHCP + ping.
# Usage: tools/qemu-test.sh [extra qemu args]
#   env: ISO, TIMEOUT, MEM, EXPECT_HIGHMEM, EXPECT_NVME, EXPECT_AHCI, EXPECT_USB,
#        EXPECT_NET, DISK_IF, DISK_DEV
set -u
ISO="${ISO:-build/axys.iso}"
# Stage 2 drives the shell for 7 s of settle plus one second per command (and
# five for probe), which is already 30 s, so the default has to clear that or
# the run is killed mid-script and the last answers are never checked.
TIMEOUT="${TIMEOUT:-150}"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
status=0

# ---- stage 1: boot log --------------------------------------------------
timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m "${MEM:-256M}" -display none \
    -serial stdio -no-reboot "$@" </dev/null >"$LOG" 2>&1

for want in "axys kernel online" "pagetest: page tables verified" "pmm: selftest passed" \
            "heap: selftest passed" "rng: chacha20 ready" "vfs: selftest passed" \
            "exception selftest: all [0-9]* checks passed" "sched: selftest passed" \
            "initrd: [0-9]* programs installed" "process: selftest passed" \
            "acpi: RSDP Multiboot2; PM1a I/O:0x604, PM1b absent, S5 [0-7]/[0-7]" \
            "boot complete" "axys shell ready"; do
    if ! grep -q -e "$want" "$LOG"; then echo "MISSING: $want"; status=1; fi
done
for bad in "panic:" "Triple fault" "FAILED" "WARNING" "no valid FADT"; do
    if grep -q -e "$bad" "$LOG"; then echo "FOUND: $bad"; status=1; fi
done
if [ "${EXPECT_HIGHMEM:-0}" = 1 ]; then
    high_limit_mib="$(sed -n 's/.*physical address limit \([0-9][0-9]*\) MiB.*/\1/p' "$LOG" | head -n 1)"
    if [ -z "$high_limit_mib" ] || [ "$high_limit_mib" -le 4096 ]; then
        echo "HIGHMEM: expected a managed physical-address limit above 4096 MiB, got ${high_limit_mib:-missing}"
        status=1
    fi
fi
if [ "$status" -ne 0 ]; then echo "--- boot log ---"; cat "$LOG"; exit 1; fi

# ---- stage 2: interactive shell ----------------------------------------
# The rmtree block is the A8 regression: uid 1000 must not be able to wipe a
# root-owned subtree living inside its own directory (it used to delete the
# whole thing). The shell only ever goes root -> user (su 0 is refused with
# -1), so the root half runs again after `exit`, which restarts init as root.
LONG="echo $(printf 'a%.0s' $(seq 1 400))"
(sleep 7
 for cmd in "$LONG" "su abc" "su 99999999999" "echo it-works > /tmp/t.txt" "cat /tmp/t.txt" "hello" "crash kexec" "crash spin" "heap" "fileio" "probe" "ls /sbin" \
     "pwd" "uname" "uname -a" "hostname" "date" "free" "df" "lspci" "lsusb" "sysinfo" "ip" \
     "echo abc > /tmp/sh-src" "cat /tmp/sh-src" "cp /tmp/sh-src /tmp/sh-dst" "cat /tmp/sh-dst" \
     "head -n 1 /tmp/sh-src" "tail -n 1 /tmp/sh-src" "wc /tmp/sh-src" "grep b /tmp/sh-src" \
     "stat /tmp/sh-src" "touch /tmp/sh-touch" "du /tmp" "find /tmp/sh-src" "clear" \
     "cd /tmp" "pwd" "cd /" "kill 99999" \
     "ps" "dmesg" "lscpu" "lsblk" "hexdump /bin/hello" "cmp /tmp/sh-src /tmp/sh-dst" \
     "whoami" "which hello" "seq 3" "basename /foo/bar" "dirname /foo/bar" "time hello" \
     "echo -n hi > /tmp/nonl" "wc /tmp/nonl" "cat -n /tmp/sh-src" \
     "free -h" "df -h" "du -h /tmp" \
     "echo ab > /tmp/m0; cat /tmp/m0" "touch /tmp/m1 /tmp/m2; ls /tmp" \
     "rm /tmp/m1 /tmp/m2; mkdir /tmp/md1 /tmp/md2; ls /tmp" "rm /tmp/md1 /tmp/md2" \
     "mkdir /tmp/cptree; echo x > /tmp/cptree/f; cp -r /tmp/cptree /tmp/cpcopy; find /tmp/cpcopy" \
     "chmod -R 600 /tmp/cptree" "echo hello-w > /tmp/g1; grep -r hello /tmp" \
     "ls -R /tmp/cptree" "find -name sh-* /tmp" "cat /tmp/sh-src /tmp/sh-dst" \
     "echo a:b:c > /tmp/cut1; cut -d : -f 2 /tmp/cut1" \
     "echo dup > /tmp/u1; echo dup >> /tmp/u1; echo x >> /tmp/u1; uniq -c /tmp/u1" \
     "echo abc > /tmp/tr1; tr a-c x-z /tmp/tr1" "strings /bin/hello" "df /tmp" \
     "$(printf 'a%.0s' $(seq 1 60))" \
     "mkdir /home/user/rt" "mkdir /home/user/rt/sub" "echo rootfile > /home/user/rt/sub/f" \
     "su 1000" "rmtree /home/user/rt" "ls /home/user/rt" "cat /home/user/rt/sub/f" \
     "exit" "rmtree /home/user/rt" "ls /home/user/rt" \
     "su 1000" "cat /etc/secret" "rm /etc/passwd" "poweroff" "id" "su 0" "exit" "id" "poweroff"; do
     printf '%s\n' "$cmd"; sleep 1
     [ "$cmd" = "crash spin" ] && { sleep 1; printf '\003'; sleep 1; }
     [ "$cmd" = "probe" ] && sleep 5
 done
 sleep 2) | timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m "${MEM:-256M}" -display none \
    -serial stdio -no-reboot "$@" >"$LOG" 2>&1
shell_qemu_status=$?
if [ "$shell_qemu_status" -ne 0 ]; then
    echo "SHELL: QEMU did not exit cleanly after poweroff (status $shell_qemu_status)"; status=1
fi

for want in "^it-works" "hello from ring 3" "\[exit 42\]" "\[killed by exception 14\]" \
            "\[killed by exception 9\]" "rwxr-xr-x 0 0 [0-9]*.init" "cat: -13" "rm: -13" "poweroff: -1" "su: -1" "uid=1000 gid=1000" "uid=0 gid=0" "powering off" \
            "probe: all checks passed" "line too long" "su: -22" \
            "fileio: all checks passed" "rmtree: -13" "sub/$" "^rootfile" "rmtree: 3" "ls: -2" \
            "^/$" "axysOS 1.0 x86_64" "^axys$" "since boot" "Heap: " "capacity: " "backend: " \
            "Host bridge" "keyboards:" "== axys sysinfo ==" "-- usb --" "link: up" \
            "^abc" "1 1 4" "size: 4 bytes" "^/tmp$" "kill: -3" \
            "PID  PPID" " R init$" "axys kernel online" "vendor: " "disk0" "7f 45 4c 46" \
            "identical" "^root$" "/bin/hello" "^3$" "^bar$" "^/foo$" "^real " "0 1 2" "1.abc" \
            "^ab$" "m1" "md1" "/tmp/cpcopy/f$" "/tmp/g1:hello-w" "cptree:$" "sh-dst" "^b$" \
            "^2 dup$" "^xyz$" "unknown command: aaaaa"; do
    if ! grep -q -e "$want" "$LOG"; then echo "SHELL MISSING: $want"; status=1; fi
done
if [ "$status" -ne 0 ]; then echo "--- shell log ---"; cat "$LOG"; else echo "boot test: OK"; fi

# ---- stage 3: persistence across a reboot --------------------------------
if [ "$status" -eq 0 ]; then
    DISK="$(mktemp)"; trap 'rm -f "$LOG" "$DISK"' EXIT
    truncate -s 32M "$DISK"
    # How the scratch disk reaches the machine. The default is the firmware's own
    # IDE controller; set DISK_IF/DISK_DEV to exercise a driver instead, e.g.
    #   DISK_IF=if=none DISK_DEV='-device ich9-ahci,id=s0 -device ide-hd,drive=d0,bus=s0.0'
    # NVMe QEMU tests attach the controller and namespace separately.
    DISK_IF="${DISK_IF:-if=ide,index=0}"
    DISK_DEV="${DISK_DEV:-}"
boot() { timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m "${MEM:-256M}" -display none \
    -serial stdio -no-reboot -drive file="$DISK",format=raw,$DISK_IF,id=d0 $DISK_DEV "$@" >"$LOG" 2>&1; }
wait_for_shell() {
    attempts=0
    while [ "$attempts" -lt 120 ]; do
        if grep -q "axys shell ready" "$LOG"; then return 0; fi
        sleep 0.5
        attempts=$((attempts + 1))
    done
    echo "PERSIST: shell did not become ready within 60 seconds" >&2
    return 1
}
# Wait for the guest's own ready marker. A fixed delay can expire while slow
# polling storage probes or persistence recovery are still running.
: > "$LOG"
(wait_for_shell || exit 1; for cmd in "mkdir /home/user/docs" "echo persistent-data > /home/user/docs/note.txt" \
    "echo keep > /etc/mine" "chmod 600 /etc/mine" "echo vanish > /tmp/gone" "sync" "poweroff"; do
    printf '%s\n' "$cmd"; sleep 1; done; sleep 2) | boot
    first_boot_status=$?
    if [ "$first_boot_status" -ne 0 ]; then echo "PERSIST: first QEMU boot did not power off cleanly (status $first_boot_status)"; status=1; fi
    grep -q "persist: disk 32 MiB, restored 0 files" "$LOG" || { echo "PERSIST: blank disk not detected"; status=1; }
    if [ "${EXPECT_NVME:-0}" = 1 ] && ! grep -q "^disk: nvme:" "$LOG"; then
        echo "PERSIST: expected the NVMe backend, got:"; grep "^disk:" "$LOG" || true; status=1
    fi
    if [ "${EXPECT_AHCI:-0}" = 1 ] && ! grep -q "^disk: ahci:" "$LOG"; then
        echo "PERSIST: expected the AHCI backend, got:"; grep "^disk:" "$LOG" || true; status=1
    fi
    if [ "${EXPECT_USB:-0}" = 1 ] && ! grep -q "^disk: usb storage: [1-9]" "$LOG"; then
        echo "PERSIST: expected the USB BOT backend, got:"; grep "^disk:" "$LOG" || true; status=1
    fi
    if grep -q "^sync: -" "$LOG"; then echo "PERSIST: explicit sync failed"; cat "$LOG"; status=1; fi
    # Restoring a larger snapshot through a polled AHCI port can take longer
    # than the blank-disk startup, so let init finish before the first key.
    : > "$LOG"
    (wait_for_shell || exit 1; for cmd in "cat /home/user/docs/note.txt" "ls /etc" "ls /tmp" "poweroff"; do
        printf '%s\n' "$cmd"; sleep 1; done; sleep 2) | boot
    second_boot_status=$?
    if [ "$second_boot_status" -ne 0 ]; then echo "PERSIST: second QEMU boot did not power off cleanly (status $second_boot_status)"; status=1; fi
    for want in "persist: disk 32 MiB, restored [1-9]" "^persistent-data" "^-rw------- 0 0 5.mine"; do
        grep -q -e "$want" "$LOG" || { echo "PERSIST MISSING: $want"; status=1; }
    done
    if grep -q "gone" "$LOG"; then echo "PERSIST: /tmp survived a reboot"; status=1; fi
    if [ "$status" -ne 0 ]; then echo "--- persistence log ---"; cat "$LOG"; fi
fi

# ---- stage 4: user-space networking over the firmware's SLIRP -----------------
# DHCP then ICMP to the gateway: proves the e1000 driver, the NET_* syscalls and
# both user tools (DHCPv4 + ARP/ICMP) work end to end.
if [ "$status" -eq 0 ] && [ "${EXPECT_NET:-0}" = 1 ]; then
    : > "$LOG"
    (attempts=0
     while [ "$attempts" -lt 120 ]; do
         grep -q "axys shell ready" "$LOG" && break
         sleep 0.5
         attempts=$((attempts + 1))
     done
     grep -q "axys shell ready" "$LOG" || { echo "NET: shell did not become ready"; exit 1; }
     # The client waits 4 s for an offer and 4 s for the ack before giving up.
     for cmd in "dhcp" "ping 10.0.2.2" "poweroff"; do
         printf '%s\n' "$cmd"
         case "$cmd" in dhcp) sleep 12 ;; ping*) sleep 8 ;; *) sleep 2 ;; esac
     done
     sleep 2) | timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m "${MEM:-256M}" -display none \
        -serial stdio -no-reboot "$@" >"$LOG" 2>&1
    net_boot_status=$?
    if [ "$net_boot_status" -ne 0 ]; then echo "NET: QEMU did not exit cleanly (status $net_boot_status)"; status=1; fi
    for want in "dhcp: offer 10\.0\.2\.15 from 10\.0\.2\.2" "dhcp: bound 10\.0\.2\.15 router 10\.0\.2\.2" \
                "sent 4 received 4"; do
        grep -q -e "$want" "$LOG" || { echo "NET MISSING: $want"; status=1; }
    done
    if [ "$status" -ne 0 ]; then echo "--- network log ---"; cat "$LOG"; else echo "network test: OK"; fi
fi
[ "$status" -eq 0 ] && echo "persistence test: OK"
exit $status
