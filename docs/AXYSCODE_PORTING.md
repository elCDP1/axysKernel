# axysCode porting boundary

The separate axysCode checkout remains a host application. The upstream
program uses Rust `std` for filesystem and terminal access, Crossterm for raw
terminal events, Ratatui for rendering, and tui-textarea for editing/search.
Those crates do not provide an AxysKernel backend, and the kernel has no
Rust user runtime. Copying the Linux executable into the initrd would not
make it an Axys program and is not part of the port.

## Decision: native C rewrite (`code`), same design

The editor is rebuilt as a native freestanding C program instead of porting
the Rust stack: same layout (file tree left, single document right, central
gap), same modes (normal/search/help/confirm-discard), same keymap, Unicode
handled as UTF-8 without ever splitting a sequence. It ships as `/bin/code`
(a separate process, so terminal flags die with it) and runs with the
`code` command. The Rust source stays the behavioural reference; the Rust
target/runtime backend below is deferred indefinitely.

## Terminal foundation (done)

The kernel grew what a fullscreen program needs, all bounds-checked and
covered by `tests/test_csi.c`, `tests/test_vga.c` and the in-guest fuzzer:

1. Raw console input: `SYS_CONSOLE` ops 3/4 toggle per-process raw mode
   (`console_raw()` in `user/axys.h`). Raw reads return bytes as they
   arrive with no line assembly, no echo and no Ctrl-D translation, and a
   raw reader is skipped by Ctrl-C kill delivery so `0x03` arrives as data
   (verified in QEMU: `61 62 1b 5b 41 ... 03 ...`). Raw programs must offer
   their own way out.
2. VGA output: SGR colours (0/1/30-37/40-47 with the ANSI↔VGA table),
   cursor hide/show (`?25l/h`, read-modify-write so the firmware scan
   lines survive), alternate screen (`?1049h/l`, nesting ignored).
   The serial console already passes every byte through untouched.
3. Screen size is fixed 80x25 for now (VGA text); no size query yet.

## Still needed for `code` (next)

1. UTF-8 line buffer with undo, file-tree walker over `readdir`, search.
2. Renderer on full redraws through cursor addressing (no relative moves
   needed), dirty flag, save/confirm dialogs.
3. Key decoder for the ESC sequences the kernel already delivers whole.

## Deferred (was: required target foundation)

1. Freezing the syscall ABI in one versioned header (still early-stage).
2. A Rust target/runtime for Axys (`core`/`alloc`, panic handler,
   `sbrk` allocator, syscall wrappers): not needed for the C rewrite.
3. Porting hosted-OS dependencies: not needed for the C rewrite.

## Toolchains (gcc/clang/rustc/cargo): not portable inward

These cannot run on axysKernel and are not being ported: they need
`fork`/`exec`, `mmap`, threads, signals, a full libc and gigabytes of
disk/RAM, none of which the kernel provides (RAM-only VFS capped at
128 MiB files, 4096 nodes, 16 fds). Axys programs are built the way every
hobby OS does it: the host gcc cross-compiles `user/*.c` into the initrd
(see `Makefile` `USER_PROGS`). A small hosted compiler (tcc-class) is the
realistic long-term path and needs a user libc first.
