# axysCode porting boundary

The separate axysCode checkout remains a host application. The upstream
program uses Rust `std` for filesystem and terminal access, Crossterm for raw
terminal events, Ratatui for rendering, and tui-textarea for editing/search.
Those crates do not provide an AxysKernel backend, and the kernel currently
has no Rust user runtime or raw-terminal control interface. Copying the Linux
executable into the initrd would not make it an Axys program and is not part of
the port.

## Required target foundation

1. Freeze the user process ABI and syscall numbers in one versioned header;
   the current ELF and syscall contracts are still early-stage.
2. Add user-facing terminal capabilities: raw key events, cursor and screen
   control, terminal dimensions, and a console device that can coexist with
   shell line input.
3. Define a Rust target/runtime for Axys: `core`/`alloc`, a panic handler,
   allocator backed by `sbrk`, and wrappers for file, process, and terminal
   syscalls. Keep all of this in ring 3.
4. Separate the editor/file-tree state from host `std::fs` and terminal
   handling behind narrow filesystem and terminal interfaces. Keep the
   Crossterm/Ratatui host backend for development and add an Axys backend.
5. Port or replace dependencies that assume a hosted OS; verify a
   compiler-generated Axys ELF in QEMU before embedding it into the initrd.

The Rust source in the sibling axysCode checkout has been reviewed and fixed
independently. It is not yet compiled for or run by AxysKernel. The current
kernel must first gain the terminal and Rust runtime interfaces above.
