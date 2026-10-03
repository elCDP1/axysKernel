# Axys user executable ABI v1

This is the binary contract implemented by the current process loader. It is
intentionally narrower than Linux's ELF ABI.

## File format

- ELF64, little-endian, x86-64; `ET_EXEC` or `ET_DYN` only.
- Maximum file size: 8 MiB. At most 16 program headers.
- At least one `PT_LOAD` segment; the entry address must point into the
  file-backed bytes of an executable load segment.
- Aggregate load-segment memory is limited to 64 MiB. A segment must satisfy
  `p_filesz <= p_memsz`, stay within the file, have valid alignment, and not
  request both write and execute permission. Segment address arithmetic is
  checked for overflow.
- `PT_INTERP` and `ET_EXEC` with `PT_DYNAMIC` are rejected. There is no dynamic
  linker, shared library loader, or symbol resolver.
- `ET_DYN` is relocated into the user address window. Its dynamic table is
  limited to 4 KiB and must end with `DT_NULL`. Only symbol-free
  `R_X86_64_RELATIVE` RELA entries are supported. Dependencies, PLT, REL, text,
  and RELR relocations are rejected. The ASLR load bias preserves the largest
  `PT_LOAD` alignment in the image.
- The loader enforces W^X for each load segment. It does not yet implement
  `PT_GNU_RELRO`, TLS, process environment vectors, ELF constructor/destructor
  hooks, or general ELF notes.

## Process entry and syscalls

The kernel enters user mode with the x86-64 System V register convention. The
current startup stub passes an optional argument-string pointer in `RDI` and
its byte length in `RSI`; it does not provide the conventional `argc/argv`
initial stack, environment, or auxiliary vector. The bundled `user/crt0.S`
and `user/axys.h` define the runtime that is available today.

Programs must be freestanding and use the Axys syscall interface. There is no
hosted libc, POSIX process model, dynamic loader, or shell toolchain inside the
kernel. The build embeds bundled ELF files into the initrd. A future guest C
compiler should be an isolated user process targeting this ABI, not code
executing inside ring 0.

## Compiler implications

The current build uses a host GCC/binutils pair to produce Axys ELF images.
For guest compilation, GCC needs a target description, assembler and linker,
runtime support (`libgcc` as needed), a C library or explicit freestanding
headers, writable persistent files, and a usable syscall/runtime layer. A
complete native GCC port is therefore a staged toolchain project; compiling
GCC into the kernel image would make the privileged kernel responsible for an
untrusted compiler and does not provide the missing hosted services.

When expanding this ABI, update this document, the loader's negative tests,
and a compiler-generated ELF fixture together. Never broaden accepted ELF
metadata solely because a file successfully compiles on the host.
