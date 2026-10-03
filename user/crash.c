#include "axys.h"

/* Deliberately misbehaves so the kernel can prove ring 3 cannot hurt it.
 * Every mode must end with this process killed (or the syscall refused), never
 * with a kernel panic. */
static int streq(const char *a, const char *b) { return a && strcmp(a, b) == 0; }

int main(const char *args, size_t len)
{
    volatile u64 *p;

    (void)len;
    if (streq(args, "null")) {
        p = (volatile u64 *)0;
        return (int)*p;
    }
    if (streq(args, "kread")) { /* read kernel memory */
        p = (volatile u64 *)0x100000;
        return (int)*p;
    }
    if (streq(args, "kwrite")) {
        p = (volatile u64 *)0x100000;
        *p = 1;
        return 0;
    }
    if (streq(args, "kexec")) { /* jump into kernel text */
        void (*f)(void) = (void (*)(void))0x100000;
        f();
        return 0;
    }
    if (streq(args, "stackexec")) { /* NX: run code from the stack */
        u8 code[1] = {0xc3};
        void (*f)(void) = (void (*)(void))code;
        f();
        return 0;
    }
    if (streq(args, "cli")) {
        __asm__ volatile("cli");
        return 0;
    }
    if (streq(args, "hlt")) {
        __asm__ volatile("hlt");
        return 0;
    }
    if (streq(args, "inb")) {
        u8 v;
        __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"((unsigned short)0x64));
        return v;
    }
    if (streq(args, "wrmsr")) {
        __asm__ volatile("wrmsr" : : "c"(0xc0000082u), "a"(0), "d"(0));
        return 0;
    }
    if (streq(args, "cr3")) {
        u64 v;
        __asm__ volatile("mov %%cr3, %0" : "=r"(v));
        return (int)v;
    }
    if (streq(args, "ud2")) {
        __asm__ volatile("ud2");
        return 0;
    }
    if (streq(args, "div0")) {
        /* Inline asm: the compiler deletes a plain 1/0 as undefined behaviour. */
        __asm__ volatile("xorl %%eax, %%eax; xorl %%edx, %%edx; xorl %%ecx, %%ecx; divl %%ecx"
                         : : : "eax", "edx", "ecx");
        return 0;
    }
    if (streq(args, "badptr")) { /* kernel pointers through syscalls must be refused */
        if (write(1, (const void *)0x100000, 16) != -14) {
            return 1;
        }
        if (read(0, (void *)0x100000, 16) != -14) {
            return 2;
        }
        if (open((const char *)0x100000, 0) != -14) {
            return 3;
        }
        if (write(1, (const void *)~0UL, 16) != -14) {
            return 4;
        }
        if (getrandom((void *)0xffffffff80000000UL, 16) != -14) {
            return 5;
        }
        if (write(99, "x", 1) != -9) {
            return 6;
        }
        if (sys3(200, 0, 0, 0) != -38) {
            return 7;
        }
        return 0; /* every hostile request was refused */
    }
    if (streq(args, "spin")) { /* pure CPU hog: only preemption can stop it */
        for (;;) {
        }
    }
    puts("crash: unknown mode\n");
    return 99;
}
