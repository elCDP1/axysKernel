#ifndef AXYS_PAGETEST_H
#define AXYS_PAGETEST_H

/* Boot-time self-test for the page tables arch/x86_64/boot.S builds: identity
 * chain, W^X on the fine-grained first 2 MiB, the unmapped NULL and stack
 * guard pages, and NX placement in the 2 MiB PDEs beyond it. See
 * kernel/pagetest.c for the full rationale. Must run before the PMM trusts
 * memory. Returns 0 if every check passed, -1 otherwise (and never panics
 * itself -- the caller decides how serious a mapping failure is). */
int axys_pagetest_selftest(void);

#endif
