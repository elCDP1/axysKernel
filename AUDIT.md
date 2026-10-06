# AxysKernel security / correctness audit

## Scope

Audited the uploaded kernel tree for:

- boot protocol and early-boot state
- x86-64 page tables and memory permissions
- physical-page and heap ownership
- GDT/TSS/IST and interrupt entry/return frames
- exception/error-code handling
- CPU feature detection and privileged register setup
- Multiboot2 runtime data parsing
- RNG / ChaCha20 state handling
- VFS path and object validation
- printf formatting and bounds handling
- serial/PIT edge cases
- placeholders / TODO / FIXME markers
- host-side regression tests and static analysis

## Major fixes applied

### Boot / Multiboot2
- Replaced the incorrect runtime Multiboot2 structure model with the real `total_size + reserved` header and 8-byte-aligned tag stream.
- Added structural bounds checks, tag-size validation, end-tag validation, mmap entry-size/version checks, overflow checks, module limits, and NUL-termination validation for module strings.
- Added parsing for the basic-memory, memory-map, module, and framebuffer tags.
- Kept boot-information/module/framebuffer ranges reserved in the physical allocator.
- Fixed the Multiboot2 image header to a valid 48-byte header with a zero checksum modulo 2^32.

### Memory management
- Physical allocator now distinguishes reserved pages from pages actually allocated by the allocator; arbitrary/reserved pages cannot be returned through `free`.
- Reserves low memory, kernel image, Multiboot2 data, modules and framebuffer memory.
- Keeps a 4 GiB static bootstrap bitmap, then builds sparse identity mappings
  for firmware-reported usable RAM and grows its bitmaps from low memory. The
  physical-address ceiling is 512 GiB, also limited by CPUID; firmware holes
  remain reserved, and a failed extension leaves the original low pool usable.
- PCI MMIO BAR pages are checked against the firmware map, split from huge pages
  when needed, and changed to uncached identity mappings before AHCI/NVMe
  registers are accessed. This is not yet a general DMA/IOMMU mapping layer.
- Heap contiguous allocation requests a truly contiguous PMM run instead of
  repeatedly allocating individual frames, which could produce a fragmented
  run and fail heap growth.
- Heap allocator now has checked size/alignment arithmetic, bounded contiguous growth, rollback on mapping gaps, exact ownership validation, double-free protection and block coalescing.
- Linker script now includes COMMON, BSS, LBSS and SBSS input sections so all zero-initialized storage is inside the range cleared by early boot.

### Page permissions / CPU hardening
- First 2 MiB is mapped with 4 KiB pages so text is RX, rodata is R/NX and data/BSS are RW/NX.
- Larger identity-mapped pages remain supervisor-only RW/NX.
- CR0.WP is enabled before protected long-mode execution.
- NX is required by the early page-table contract; boot halts if the CPU does not advertise it.
- SMEP is enabled only when advertised by CPUID, avoiding writes to unsupported CR4 bits.
- CPU feature detection was corrected for x2APIC, NX, RDTSCP, 1 GiB pages, invariant TSC, SMEP, SMAP, FSGSBASE, APIC and logical CPU count.

### GDT / TSS / IST / interrupts
- Corrected the packed x86-64 TSS layout and selector/descriptor handling.
- IST stacks are distinct, page-aligned and range-validated.
- TSS I/O bitmap is fully denied by default and its terminating byte is covered by the descriptor limit.
- Corrected the IDT IST field encoding and rejected invalid DPL/IST inputs instead of silently truncating them.
- All 256 vectors have stubs with a single C-visible frame layout.
- Corrected the exception vectors that push CPU error codes, including AMD #VC and #SX, while keeping #HV and #VE as no-error exceptions.
- Corrected double-fault IST entry/return handling so the CPU-saved `SS:RSP` is preserved for `iretq`.
- Exception decoding now treats the saved stack layout correctly and no longer invents #DF error fields.

### RNG / cryptographic state
- Corrected RDRAND/RDSEED CPUID feature bits and CF-success handling.
- Fixed inline-assembly output constraints so the generated value cannot be overwritten by the success flag.
- ChaCha20 state now keeps the block counter in word 12 and the nonce in words 13..15 instead of overlapping them.
- TSC is used only as supplemental mixing; failed hardware RNG generation is not treated as successful entropy.

### Other robustness fixes
- `printf` now handles malformed/trailing `%` safely, clamps widths before arithmetic, prevents output-length wraparound and reads `%p` through the correct varargs pointer type.
- Serial early-boot polling is bounded and preserves the output byte across status reads.
- PIT millisecond conversion avoids multiplication overflow by using quotient/remainder arithmetic.
- VFS rejects invalid root/trailing-slash create cases, validates file types and rolls back parent linkage on failed insertion.
- Added regression coverage for VFS, interrupt frames, exception decoding and corrected Multiboot2 structures.

### AHCI persistence failure found during QEMU testing

- The AHCI driver used ATA opcode `0x93` for `FLUSH CACHE EXT`; the command is
  `0xEA`. QEMU rejected the wrong command with ATA status `ERR` and error
  register `ABRT` (`PxTFD=0x0441`). Since the driver also discarded the command
  result and returned success, snapshot writes were reported as durable even
  though the flush had failed. This caused data loss after restart on AHCI
  disks, while the IDE-backed test passed.
- Changed the opcode to `0xEA` and made `axys_ahci_flush()` propagate command
  failures. AHCI write/flush errors now report the port registers needed to
  distinguish a device rejection from a timeout.
- The QEMU persistence test now issues an explicit `sync` before orderly
  shutdown, in addition to checking restored contents after the next boot.

### AHCI last-port enumeration

- `CAP.NP` is the highest implemented port index, so the number of candidate
  ports is `CAP.NP + 1`. The driver treated it as a count and omitted the last
  port. On QEMU's ICH9 HBA, `CAP.NP=5` with a disk on port 5 reproduced the bug:
  the scan reported ports 0 through 4 and fell back to no disk.
- The enumerator now adds one before applying its implementation limit. The
  QEMU disk scenario can be run on that last port with `DISK_IF=if=none` and
  `DISK_DEV='-device ich9-ahci,id=s0 -device ide-hd,drive=d0,bus=s0.5'`.

### PCI BAR attributes and NVMe bring-up

- QEMU's NVMe controller exposed that PCI memory BAR address decoding masked
  only bits 1:0. Bits 3:0 carry BAR type/prefetch attributes, so the retained
  type bit made a valid BAR appear unaligned; for a 64-bit BAR, the upper dword
  is entirely address data and must not be masked. The decoder now strips all
  four low bits from the low dword and preserves the upper dword.
- Added a polled PCI NVMe driver for one controller and namespace 1 with
  512-byte logical blocks. It uses 4 KiB aligned static DMA queues, a single
  page bounce buffer, one outstanding command, bounded polling, and a flush
  command; the existing disk abstraction selects it before AHCI and ATA.
- QEMU testing found a completion-ring bug: the phase bit is bit 16 of CQE
  dword 3, after the 16-bit CID. Reading bit 0 instead confuses odd CIDs with
  phase tags and stalls when the command ID changes. The driver now reads the
  specified phase bit and validates returned CID/status.
- `make check-nvme` requires the NVMe backend, performs storage persistence
  across reboot, and passed with QEMU's emulated controller and namespace.
- `make check-ahci` requires AHCI on ICH9 port 5, guarding PCI BAR decoding and
  the last-port path while checking persistence. Both storage targets now wait
  for the guest's `axys shell ready` marker, avoiding lost input from a fixed
  startup sleep after slow device polling or snapshot restoration.

### PS/2 right-Ctrl release

- The set-1 extended scancode path returned early on key release, so releasing
  right Ctrl never cleared the shared `ctrl_down` modifier state. All following
  letters could be consumed as control characters. The extended release path
  now clears that state for scancode `0x1d`.

### ACPI power control used unrelated FADT fields

- The old offsets treated `FIRMWARE_CTRL` as the PM1 event block, read a byte
  from `PM1a_EVT_BLK` as the PM1 control address, and looked for reset GAS data
  at `0xf0`. Those are not the FADT fields for PM1a control or reset. On QEMU,
  poweroff appeared to work only because the driver later wrote guessed
  emulator-specific ports.
- Use the 36-byte SDT header and the ACPI FADT offsets: legacy `PM1a_CNT_BLK`
  at `0x40`, `PM1_CNT_LEN` at `0x59`, `RESET_REG` at `0x74`, `RESET_VALUE` at
  `0x80`, `X_DSDT` at `0x8c`, and `X_PM1a_CNT_BLK` at `0xac`. The UEFI Forum's
  [ACPI 6.5 FADT definition](https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html)
  is the source for these layout requirements.
- Validate RSDP checksums and lengths, SDT signatures/lengths/checksums, and
  the full physical range before following any firmware pointer. Prefer the
  Multiboot2 ACPI tag (including RSDP copies from the EBDA path), then try the
  BIOS scan; prefer XSDT's full 64-bit entries, falling back to RSDT only when
  usable. Reject physical addresses outside the current identity map and zero
  addresses, since page zero is deliberately unmapped.
- Read the static root `_S5` package for the firmware's two SLP_TYP values and
  preserve unrelated PM1 control bits. Write PM1b when present. Unsupported
  tables or hardware-reduced ACPI must not cause writes to guessed chipset
  ports; log the limitation and halt safely.
- Regression checks: `tests/test_acpi.c` exercises supported integer encodings
  and truncated/invalid packages; `tests/test_multiboot2.c` checks ACPI-tag
  bounds and retention; `tools/qemu-test.sh` asserts that QEMU reports the
  Multiboot2 RSDP, PM1a I/O port `0x604`, a parsed S5 pair, and exits cleanly
  after the shell poweroff command.

### ELF loader accepted malformed process images

- The process loader checked the ELF header and file ranges, but calculated
  load ends and the entry point with unchecked additions. An overflow in
  `p_vaddr + bias + p_memsz` could wrap before page alignment; the entry check
  only compared against the highest mapped address and could accept data or a
  hole rather than executable file bytes.
- Added a standalone ELF metadata validator. It enforces ELF64/little-endian
  x86-64, bounded program headers and aggregate segment memory, checked file
  ranges and virtual-address arithmetic, power-of-two/congruent alignment,
  W^X, an executable file-backed entry point, and a mapped `PT_DYNAMIC`.
  Unsupported interpreters and dynamic linking are rejected.
- The relocated-image path now requires a terminated dynamic table and
  accepts only symbol-free `R_X86_64_RELATIVE` RELA entries. Relocation table
  and target arithmetic are bounded, and unsupported dependency/PLT/REL/text/
  RELR entries fail closed.
- Added `tests/test_elf.c` with valid and malformed header, segment, alignment,
  executable-entry, and dynamic-segment cases. The complete loader ABI and
  its current limits are documented in `docs/EXECUTABLE_ABI.md`.

## Validation performed

- `make clean && make test && make kernel`: PASS
- 5 host test programs: PASS
- ASan + UBSan for all 5 host tests: PASS
- ASan + UBSan host RNG integration test: PASS
- GCC `-fanalyzer`: PASS
- extra strict warnings (`-Wshadow -Wundef -Wcast-align=strict -Wwrite-strings`): PASS
- Clang x86_64 freestanding cross-compilation of kernel C and assembly: PASS
- console configuration matrix (serial/vga enabled and disabled combinations): PASS
- Multiboot2 header checksum and ELF/section/program-header inspection: PASS
- TSS layout checked against expected architectural offsets with compile-time/runtime tests: PASS
- TODO/FIXME/placeholder scan of source: no remaining matches

## Revalidation on 2026-10-03

- Arch WSL2: `make clean && make check` passed, including all six host tests,
  kernel/user ELF and ISO builds, serial-driven shell inputs, process fault
  handling, permissions, and IDE-backed persistence over reboot.
- After ACPI hardening, Arch WSL2 `make check` passed with seven host test
  programs, an ISO build, a checked boot-log ACPI summary, shell inputs and
  poweroff, and IDE-backed persistence after reboot. QEMU reported the
  Multiboot2 RSDP, PM1a I/O `0x604`, and the firmware's `_S5` types `0/0`.
- Repeated the full QEMU input/persistence scenario with the disk on ICH9 AHCI
  port 5 after the ACPI changes: boot, clean guest poweroff, explicit `sync`,
  reboot restore, file contents, mode, and volatile `/tmp` all passed.
- The seven host tests passed. The new AML `_S5` and Multiboot2 parsers also
  passed targeted ASan/UBSan runs; GCC `-fanalyzer` reported no findings for
  ACPI table handling, AML extraction, or Multiboot2 tag parsing.
- Arch WSL2: the same `make check` passed with a 32 MiB disk attached through
  QEMU's ICH9 AHCI controller on port 5 (the highest implemented port). The
  test issues `sync`, reboots, verifies the saved file and mode, and confirms
  `/tmp` remains volatile.
- Manual QEMU PS/2 monitor check: sent right Ctrl, `a`, and Enter; the shell
  received `a` after right Ctrl was released, confirming the modifier state
  clears. This is a manual check, not part of `tools/qemu-test.sh`.
- All six host test programs passed under `-fsanitize=address,undefined` and
  separately under GCC `-fanalyzer`; the complete kernel and embedded user
  programs also compiled with `-fanalyzer` and warnings as errors.
- Native Windows host tests passed after replacing the persistence test's
  ABI-dependent `unsigned long` fields with `axys_uint64_t`. MinGW cannot
  assemble the Linux `.note.GNU-stack` directive, so the kernel image build was
  performed with the intended Arch GNU/Linux toolchain.

## Regression-prevention notes

These checks are intended to make future low-level failures easier to find and
to keep host-toolchain differences from being mistaken for kernel defects.

### Fixed-width data and host tests

- Use `axys_uint8_t`/`axys_uint16_t`/`axys_uint32_t`/`axys_uint64_t` for values
  whose width is defined by an on-disk format, a hardware structure, or an
  architecture ABI. Do not use C `long` for those fields: its width is 32 bits
  on Windows x64 and commonly 64 bits on Unix x86-64.
- When a test reads or constructs a persistent header, compare its field types
  with the format definition in `include/axys/persist.h` and `kernel/persist.c`.
  Compile the host tests with warnings as errors on more than one ABI where
  possible; this catches both truncation and out-of-bounds copies.
- A host-test stub (`tests/*_stub.c`) replaces hardware entry points only for
  the host test. Check that the real kernel build still includes the production
  implementation before treating a stub as a missing driver implementation.

### Boot-failure triage

1. Start at `arch/x86_64/boot.S`: verify the Multiboot2 header is in the first
   32 KiB, `.bss` is cleared before C globals are used, the boot registers are
   preserved, and paging/CPU feature checks precede long-mode entry.
2. Compare `scripts/linker.ld` with the boot assembly's address and page-table
   assumptions. Keep the image-size assertion and verify section boundaries
   used to assign text/rodata/data permissions.
3. Follow `scripts/grub.cfg` into `kernel/main.c`. The serial milestone strings
   are the boot test's progress markers; the last marker narrows the failing
   initialization stage. Check IDT/TSS setup before enabling interrupts.
4. Reproduce with the host tests first, then build the ELF and boot the ISO in
   QEMU with serial output captured. A successful compile or ELF inspection is
   not evidence that the CPU reached the kernel entry point.
5. For a suspected memory or interrupt failure, inspect both sides of the
   contract: the assembly frame/descriptor layout and the matching C structure,
   mask, or parser. Add a regression case for the malformed input or boundary
   that triggered the failure.

### Executable-loader triage

1. Treat ELF files as untrusted byte streams. Validate the fixed header before
   using offsets, then check every file range with `offset <= size` and
   `length <= size - offset`; avoid `offset + length <= size` arithmetic.
2. For every `PT_LOAD`, validate `p_filesz <= p_memsz`, address-end overflow,
   page-rounding overflow, power-of-two `p_align`, and the ELF offset/address
   congruence. Check the aggregate memory limit before allocating pages.
3. Require the entry point to lie in file-backed bytes of a `PF_X` segment.
   Check every load segment for W^X and reject overlapping page mappings.
4. For `ET_DYN`, choose a load bias divisible by the maximum `PT_LOAD` alignment
   so all segment congruences remain valid after ASLR. Validate the mapped
   `PT_DYNAMIC` table, require `DT_NULL`, and reject relocation types that need
   symbol lookup or a dynamic linker.
5. Keep malformed fixtures in `tests/test_elf.c`; run them under ASan/UBSan and
   GCC `-fanalyzer`. Also boot a compiler-generated image in QEMU, including a
   larger `p_align` value, because host parser success does not test page
   mapping or relocation behavior.

### Toolchain-specific findings

- The host persistence test previously used `unsigned long` to read 64-bit
  snapshot generations. That assumption failed on the available Windows x64
  ABI (where `unsigned long` is 32 bits). The test now uses `axys_uint64_t`,
  matching the persistent format.
- The Windows `clean` recipe must tolerate a missing `build` directory. Check
  for the directory before invoking `Remove-Item`; otherwise a fresh checkout
  reports a failed clean despite having nothing to remove.
- The available MinGW assembler rejects the GNU `.note.GNU-stack` section
  directive used by the x86-64 assembly sources. This is a toolchain-target
  limitation; do not remove the directive from the Linux build to make that
  toolchain pass. Validate this project with its intended GNU/Linux x86-64
  toolchain, then use a deliberate target-specific build rule if Windows-hosted
  assembly support is added.
- After changing sources, regenerate `SHA256SUMS` for files that exist in the
  checkout and verify it with `sha256sum -c SHA256SUMS`. Stale manifests can
  hide source changes or refer to artifacts that are no longer shipped.

### Current platform boundaries

- The current storage paths are a polled AHCI driver and primary-channel ATA
  PIO fallback. AHCI serializes commands and selects one drive; ATA PIO is
  limited to the primary channel and LBA28. Passing QEMU tests proves those
  configured paths, not broad hardware coverage.
- The codebase currently has no SMP bring-up, USB, network, or audio drivers.
  These are feature gaps to plan before describing this as a general-purpose
  kernel with complete hardware support.
- The current physical-memory manager supports sparse identity-mapped usable
  RAM up to 512 GiB, also limited by CPU physical-address width. High reserved
  ACPI/MMIO ranges do not yet have a general mapping policy. DDR generation is
  abstracted by firmware; supporting higher capacities needs a larger virtual
  map and page metadata, not DIMM-specific kernel drivers. See
  `docs/PLATFORM_SUPPORT.md`.
- User programs use the narrow static x86-64 ELF ABI described in
  `docs/EXECUTABLE_ABI.md`; a hosted GCC or Rust TUI port is not yet available
  inside the guest.
- ACPI currently supports conventional PM1 sleep control and System I/O or
  mapped System Memory GAS registers. It does not execute AML methods such as
  `_PTS`, implement hardware-reduced ACPI sleep control, or generally map
  reserved ACPI tables above the initial 4 GiB mapping. A static `_S5` Name
  package is parsed for the power-off type; this is not a general AML
  interpreter.

## Important limitation

A QEMU boot test validates the emulated configuration and the specific device
models attached by the test script; it does not establish compatibility with
all physical firmware or storage controllers.

The kernel is also still effectively single-core/single-threaded. PMM, heap and VFS global state are not designed for concurrent SMP access yet; this is an architectural limitation rather than a currently reachable race in the boot-only execution path.

## Revalidation after ELF ABI hardening

- Arch WSL2: `make test kernel` passed with the new ELF parser integration and
  all eight host test programs.
- Arch WSL2: `make check` passed after the ELF changes, including all eight
  host tests, the full interactive QEMU process/security scenario, orderly
  ACPI poweroff, and persistence across reboot.
- Built the `/bin/hello` fixture with 2 MiB `PT_LOAD` alignment and reran the
  full QEMU/persistence suite successfully. This exercises the ASLR-bias
  alignment path with linker output larger than the default 4 KiB alignment;
  the normal 4 KiB build artifacts were restored afterward.
- The new ELF host test passed under AddressSanitizer/UndefinedBehaviorSanitizer
  and GCC `-fanalyzer` reported no findings in the parser, test source, or
  process-loader integration.
- The axys user-binary format and platform/device boundaries are now written
  down in English in `docs/EXECUTABLE_ABI.md` and
  `docs/PLATFORM_SUPPORT.md`; these documents intentionally list unsupported
  hardware and runtime features rather than imply universal compatibility.
- The Rust axysCode source is tracked in a separate sibling checkout. Its
  current host-only dependencies and the interfaces required for a true Axys
  port are documented in `docs/AXYSCODE_PORTING.md`.

## Revalidation after high-memory and PCI storage work

- Arch WSL2 `make check-highmem` passed with the explicit `physical address
  limit > 4096 MiB` assertion, all eight host tests, QEMU shell/process tests,
  and reboot persistence. The 5 GiB QEMU map exposed usable RAM above 4 GiB.
- Arch WSL2 `make check-nvme` passed with the QEMU NVMe backend selected,
  snapshot write/flush, and data restored after reboot.
- Arch WSL2 `make check-ahci` passed with the disk on ICH9 AHCI port 5 and the
  AHCI backend explicitly asserted; persisted file contents and permissions
  were restored after reboot.
- Arch WSL2 `make check` passed with the standard IDE-backed QEMU scenario.
- All three storage scenarios ran after the PCI MMIO mapper began rejecting
  ranges that overlap firmware-available RAM and using uncached page-table
  entries for the device registers.

## Review of 2026-10-03 (syscall / VFS / address-space boundary)

Each item lists the symptom, the root cause and how to find the same class of
bug elsewhere.

### Stale file-descriptor handles (security)

- **Symptom:** none visible in the existing tests. After `unlink()` the VFS node
  slot goes on a free list and the next `create()` reuses it. A descriptor opened
  earlier still held the bare slot index, so `read()`/`write()` on it would reach
  the new, unrelated file. Permissions are checked only at `open()`, so this
  bypassed them (e.g. a file opened by user A, unlinked, slot reused by a root-only
  file).
- **Fix:** `axys_vfs_generation()` returns a per-slot counter (kept outside
  `struct vfs_node`, so `node_release()` cannot reset it). `struct axys_fd` records
  it at open and `fd_get()` rejects a mismatch with `EBADF`. `close()` uses
  `fd_get_raw()` so a dead descriptor can still be released.
- **How to find this class:** wherever a long-lived integer index into a pool is
  stored (fds, pids, node ids), ask what happens when the slot is freed and
  reallocated. Index + generation, or reference counting, is required.
  Regression: `test_slot_reuse_changes_generation` in `tests/test_vfs.c`.

### Data loss on a refused replace

- `vfs_write_nl()` set `size = 0` before the allocation that could fail, so an
  error return left the file empty. Reserve first, truncate second.
  Regression: `test_failed_replace_keeps_contents`.
- **How to find this class:** in any "replace whole object" routine, check that
  every fallible step happens before the first destructive step.

### Signed overflow in `lseek()`

- `base + off` with a user-supplied `int64` could overflow (undefined behaviour).
  The range is now checked (`off >= -base`, `off <= MAX - base`) before the add.
- **How to find this class:** any arithmetic on a syscall argument must be
  bounded before it is performed, never after.

### Imprecise or permissive error handling

- `open(O_CREAT)` reported every create failure as `ENOSPC`; it now uses
  `path_errno()` (`ENAMETOOLONG`, `ENOTDIR`, ...), like `mkdir`.
- `power(arg)` powered the machine off for any argument other than 1; only `0`
  and `1` are valid now (`EINVAL` otherwise). Unknown selector values should fail
  closed, especially for destructive operations.
- `axys_aspace_copy_string()` indexed `kernel_dst[max - 1]` when `max == 0`; it now
  returns an error first.

### Build / packaging

- `tools/qemu-test.sh` lost its executable bit when the tree was zipped, so
  `make check` failed with `Permission denied`. The Makefile now runs it through
  `sh`, which does not depend on file modes surviving an archive.

### Validation of this review

- 8 host test programs pass (including the two new regression cases).
- Full kernel and user programs build with `-Werror`.
- QEMU (Ubuntu 24.04, QEMU 8.2): `tools/qemu-test.sh` passes on IDE, NVMe and
  ICH9 AHCI port 5: boot milestones, shell input scenario, fault handling,
  permissions, poweroff and persistence across reboot.
- `make check-highmem` could not be run in that environment (guest needs 5 GiB of
  host RAM); rerun it on the target machine.

## Delivery notes

The workspace contains source files and no `.git` directory, so its relationship
to the public GitHub branch cannot be checked here. The old delivery note named
`head.bin` and `vga.bin`, but neither file exists in this workspace or is
referenced by the current build. `SHA256SUMS` was stale and included those
missing paths; it has been regenerated for the files actually present, excluding
the checksum file itself and ignored build outputs.

### Boot order: PIT before disk drivers

- `kernel/main.c` probed disks (`axys_disk_init()`) before the PIT was
  programmed and before interrupts were enabled. AHCI COMRESET and settle waits
  use `axys_pit_sleep_ms()`, which returns immediately while `pit_hz == 0`
  (skipping required delays) and would spin forever if the timer were programmed
  with interrupts still masked. Fixed by running `axys_sched_init()`,
  `axys_pit_start()`, `axys_pit_init()` and `axys_cpu_enable_interrupts()`
  before the disk probe. `make check`, `check-ahci` and `check-nvme` pass;
  `check-highmem` needs 5 GiB of host RAM and could not run in a 4 GiB sandbox.

### Shell input hardening (found by driving the booted kernel over serial)

- `console_read()` (`kernel/syscall.c`): after an over-long line the error was
  printed but `line_len` was not reset, so the editor buffer stayed full and every
  following line also reported "line too long": the shell never returned to its
  prompt. The partial line is now discarded and an empty line is returned.
- `parse_num()` (`user/init.c`): it wrapped on overflow and returned 0 when no
  digits were present, so `su abc` meant uid 0 and `su 99999999999` meant a
  truncated uid. It now rejects missing digits, trailing garbage and values above
  32 bits; `su`, `chmod` and `chown` report -22 (EINVAL).
- Regression coverage for both was added to stage 2 of `tools/qemu-test.sh`.

### Heap tail pointer and process-spawn race (found by in-QEMU fuzzing)

- `kmalloc_locked()` (`kernel/heap.c`): splitting the *last* block of the
  arena list left `heap_tail` pointing at the allocated half. The next
  `heap_grow()` then executed `heap_tail->next = new_arena`, overwriting the
  link to the split-off remainder and dropping it, and everything after it,
  from the list permanently (up to a whole arena of free memory), while
  `bytes_in_use` kept counting. The tail is now advanced to the remainder.
  `tests/test_heap.c` (in `make test`) includes the real allocator and checks,
  after random alloc/free traffic, that every byte of every arena is reachable
  from the block list, that the tail is the last block, and that
  `bytes_in_use`/`bytes_free` match the sum over the blocks. It fails on the
  old code ("heap_tail is not the last block").
- `axys_process_spawn_as()` (`kernel/process.c`): the new task became runnable
  inside `axys_task_create()`, before `axys_task_set_address_space()` attached
  its page tables and process. A timer tick in that window runs the task on the
  kernel's CR3 with no process: it takes a page fault on its first user
  instruction (`process -1 (?) killed: exception 14`) and the real process slot
  never exits, so the parent's `wait()` blocks forever. Interrupts are now
  disabled across create + attach + detach. This was observed once as a hang
  during fuzzing and could not be reproduced on demand afterwards, so the fix
  rests on the code analysis rather than on a failing regression test.

### USB HID keyboard (xHCI) and Intel Ethernet bring-up

- New PCI foundation: per-BAR kind/base/size discovery, capability walks,
  8/16-bit accessors, command-register updates that preserve W1C status, and
  a `/proc/pci` inventory published at boot. `tests/test_pci.c` covers
  decode, sizing, bridges, capabilities, rescans and the new DMA allocator.
- New `kernel/dma.c`: zeroed contiguous PMM-backed buffers with alignment,
  64 KiB-style boundary placement and an opt-in sub-4 GiB guarantee.
- `kernel/xhci.c` + `kernel/usb_hid.c`: polled xHCI init (reset, rings,
  DCBAA, scratchpad), root-hub reset, Address/Configure Endpoint, HID report
  descriptor fetch and interrupt-IN polling into console input; keyboards also
  hotplug after boot. `tests/test_usb.c` covers the report decoder;
  `make check-usb` boots with usb-kbd and injects keystrokes through QMP.
  Bugs found by bringing it up: the input-context Slot lives at offset 32
  (not 8), Setup TRT is 3 for IN / 2 for OUT, doorbell targets are DCIs
  (not DCI-1), Configure Endpoint needs A0 without A1, and polling must never
  hold an irqsave lock across PIT-bounded waits.
- `kernel/e1000.c` + `/bin/ping`: polled 82540EM rings, link/MAC readout, a
  kernel RX frame queue with NET_SEND/NET_RECV/NET_STAT syscalls, and ARP +
  ICMP echo in user space (QEMU user-network defaults). Bugs found: RDLEN and
  TDLEN collided with RDBAL/TDBAH (silently reprogramming both ring bases),
  and the transmit wait ran under an irqsave lock that would wedge the box
  if the transmitter ever stalled. `ping 10.0.2.2` answers 3/3.
- Robustness fix from USB testing: the process frame-leak selftest now
  settles (bounded) instead of sampling once after a fixed sleep, so slow
  reaping under scheduling variance cannot trip it; a genuine leak still
  fails after the timeout.

### USB hubs, USB mass storage and DHCPv4

- `kernel/xhci.c`: USB hub support for polled traversal (hub class descriptor,
  status/reset of downstream ports, hub- and device-level route strings, TT
  parent fields only under a high-speed hub, nesting up to 5 levels). Devices
  behind hubs now enumerate at boot *and* appear on the slow hotplug scan,
  which initially only walked the first 8 downstream ports a hub can report;
  the retry table now covers all 16. Bring-up bug: HID report completions that
  arrived while a hub control transfer was in flight were discarded, so a
  keyboard behind a hub went silent after the hub finished enumerating — those
  events are now serviced (and rearmed) where they are consumed.
- `kernel/usb_storage.c`: SCSI bulk-only transport (TEST UNIT READY,
  INQUIRY, READ(10), WRITE(10), 512-byte sectors, Bulk-Only Reset before the
  first command) exposed through `kernel/disk.c` as the `DISK_USB` backend so a
  stick can be the system disk, with the persistence layer unchanged. Bugs
  found: CBW and CSW signatures were compared byte-swapped (so only the
  byte-swapped-compare path of a real host would have matched) and the
  reset/ready constants were wrong, leaving sticks unusable.
- `/bin/dhcp` + `NET_SET_ADDR`: DHCPv4 DISCOVER/OFFER/REQUEST/ACK, server
  identification and option 3 router, built and parsed in user space with an
  explicit UDP pseudo-header checksum. The option scan is bounded by the
  received UDP length, so a truncated or hostile reply cannot walk past the
  frame. `NET_RECV` used to dequeue a frame before validating the caller's
  buffer, which both lost the packet and reported `EAGAIN` where `EFAULT` is
  the only correct answer; it now checks the destination range first.
- Automated coverage added (all passing): `make check-usb-hub` (keyboard behind
  a hub), `make check-usb-storage` and `make check-usb-storage-hub` (BOT disk as
  the system disk, persistence across two boots, on a root port and behind a
  hub), `make check-net` (DHCP lease then ICMP ping), with `probe_net()` in
  `/bin/probe` covering the `NET_*` syscall contract including `-EFAULT`
  destination validation.
- Attempted and rejected: a linear-framebuffer text console. GRUB's multiboot2
  loader reports an EGA-text framebuffer tag (`type=2`, 80x25 at `0xb8000`) for
  every VBE mode it sets, even with `all_video`, `gfxmode=1024x768x32`,
  `gfxpayload=keep` and a graphical terminal, on both BIOS and UEFI boots
  (`screendump` confirms the machine is back in 720x400 text mode once the
  payload starts). Without a framebuffer tag the console would be untestable
  here, so the code was removed rather than shipped unverified.
