#!/bin/sh
# Headless QEMU boot tests. Stage 1 checks every boot milestone; stage 2 drives
# the user-space shell over the serial port and checks its answers.
# Usage: tools/qemu-test.sh [extra qemu args]      (env: ISO, TIMEOUT, MEM, EXPECT_HIGHMEM, EXPECT_NVME, EXPECT_AHCI)
set -u
ISO="${ISO:-build/axys.iso}"
# Stage 2 drives the shell for 7 s of settle plus one second per command (and
# five for probe), which is already 30 s, so the default has to clear that or
# the run is killed mid-script and the last answers are never checked.
TIMEOUT="${TIMEOUT:-60}"
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
    if ! grep -q "$want" "$LOG"; then echo "MISSING: $want"; status=1; fi
done
for bad in "panic:" "Triple fault" "FAILED" "WARNING" "no valid FADT"; do
    if grep -q "$bad" "$LOG"; then echo "FOUND: $bad"; status=1; fi
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
LONG="echo $(printf 'a%.0s' $(seq 1 400))"
(sleep 7
 for cmd in "$LONG" "su abc" "su 99999999999" "echo it-works > /tmp/t.txt" "cat /tmp/t.txt" "hello" "crash kexec" "crash spin" "heap" "fileio" "probe" "ls /sbin" "su 1000" "cat /etc/secret" "rm /etc/passwd" "poweroff" "id" "su 0" "exit" "id" "poweroff"; do
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
            "probe: all checks passed" "line too long" "su: -22"; do
    if ! grep -q "$want" "$LOG"; then echo "SHELL MISSING: $want"; status=1; fi
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
    if grep -q "^sync: -" "$LOG"; then echo "PERSIST: explicit sync failed"; cat "$LOG"; status=1; fi
    # Restoring a larger snapshot through a polled AHCI port can take longer
    # than the blank-disk startup, so let init finish before the first key.
    : > "$LOG"
    (wait_for_shell || exit 1; for cmd in "cat /home/user/docs/note.txt" "ls /etc" "ls /tmp" "poweroff"; do
        printf '%s\n' "$cmd"; sleep 1; done; sleep 2) | boot
    second_boot_status=$?
    if [ "$second_boot_status" -ne 0 ]; then echo "PERSIST: second QEMU boot did not power off cleanly (status $second_boot_status)"; status=1; fi
    for want in "persist: disk 32 MiB, restored [1-9]" "^persistent-data" "^-rw------- 0 0 5.mine"; do
        grep -q "$want" "$LOG" || { echo "PERSIST MISSING: $want"; status=1; }
    done
    if grep -q "gone" "$LOG"; then echo "PERSIST: /tmp survived a reboot"; status=1; fi
    if [ "$status" -ne 0 ]; then echo "--- persistence log ---"; cat "$LOG"; fi
fi
[ "$status" -eq 0 ] && echo "persistence test: OK"
exit $status
