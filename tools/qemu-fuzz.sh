#!/bin/sh
# QEMU syscall-fuzz orchestrator. Boots the ISO, runs /bin/fuzz, watches for
# heartbeats (hang detection) and kernel-failure signatures, then reports one
# verdict: PASS, FAIL, CRASH (panic/CPU exception), HANG (no heartbeat),
# TIMEOUT or NOBOOT.
# Usage: sh tools/qemu-fuzz.sh   (env: ISO, SEED, ITERS, TIMEOUT, MEM, QEMU_EXTRA)
# QEMU_EXTRA adds devices, e.g. "-device qemu-xhci -device usb-kbd": the fuzzer then
# runs while the USB poller and disk drivers are live.
set -u
ISO="${ISO:-build/axys.iso}"
SEED="${SEED:-1}"
ITERS="${ITERS:-2000}"
TIMEOUT="${TIMEOUT:-180}"
MEM="${MEM:-256M}"
LOG="$(mktemp)"
FIFO="$(mktemp -u)"
trap 'rm -f "$LOG" "$FIFO"' EXIT
verdict() { echo "qemu-fuzz: $1 (seed=$SEED iters=$ITERS)"; exit "$2"; }

mkfifo "$FIFO"
# shellcheck disable=SC2094
timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m "$MEM" -display none \
    -serial stdio -no-reboot ${QEMU_EXTRA:-} <"$FIFO" >"$LOG" 2>&1 &
QEMU_PID=$!
exec 3>"$FIFO"

wait_for() { # $1 = pattern, $2 = seconds
    waited=0
    while [ "$waited" -lt "$2" ]; do
        if grep -aq "$1" "$LOG" 2>/dev/null; then return 0; fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

if ! wait_for "axys shell ready" 60; then
    kill -9 "$QEMU_PID" 2>/dev/null
    verdict NOBOOT 3
fi
printf '%s\n' "fuzz $SEED $ITERS" >&3

last_beat=0
elapsed=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    sleep 5
    elapsed=$((elapsed + 5))
    if grep -aq "panic:\|Triple fault\|Stack overflow" "$LOG"; then
        printf '%s\n' "poweroff" >&3 || true
        wait "$QEMU_PID" 2>/dev/null
        verdict CRASH 4
    fi
    if grep -aq "fuzz: PASS" "$LOG"; then
        printf '%s\n' "poweroff" >&3 || true
        if wait_for "powering off" 20; then
            wait "$QEMU_PID" 2>/dev/null
            verdict PASS 0
        fi
        kill -9 "$QEMU_PID" 2>/dev/null
        verdict PASS 0
    fi
    if grep -aq "fuzz: FAIL" "$LOG"; then
        printf '%s\n' "poweroff" >&3 || true
        wait "$QEMU_PID" 2>/dev/null
        grep -a "fuzz: FAIL" "$LOG" | head -5
        verdict FAIL 1
    fi
    beats=$(grep -ac "fuzz: heartbeat" "$LOG" 2>/dev/null || true)
    if [ "$beats" != "$last_beat" ]; then
        last_beat=$beats
        elapsed=0
    fi
    if [ "$elapsed" -ge 90 ]; then
        kill -9 "$QEMU_PID" 2>/dev/null
        verdict HANG 2
    fi
done
kill -9 "$QEMU_PID" 2>/dev/null
if grep -aq "panic:\|Triple fault" "$LOG"; then verdict CRASH 4; fi
verdict TIMEOUT 5
