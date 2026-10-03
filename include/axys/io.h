#ifndef AXYS_IO_H
#define AXYS_IO_H

#include "axys/types.h"

static inline void axys_outb(axys_uint16_t port, axys_uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Transfer `count` 16-bit words between a port and memory (ATA PIO). */
static inline void axys_insw(axys_uint16_t port, void *buffer, axys_size_t count)
{
    __asm__ volatile("rep insw" : "+D"(buffer), "+c"(count) : "d"(port) : "memory");
}

static inline void axys_outsw(axys_uint16_t port, const void *buffer, axys_size_t count)
{
    __asm__ volatile("rep outsw" : "+S"(buffer), "+c"(count) : "d"(port) : "memory");
}

static inline void axys_outw(axys_uint16_t port, axys_uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void axys_outd(axys_uint16_t port, axys_uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline axys_uint8_t axys_inb(axys_uint16_t port)
{
    axys_uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline axys_uint16_t axys_inw(axys_uint16_t port)
{
    axys_uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline axys_uint32_t axys_ind(axys_uint16_t port)
{
    axys_uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void axys_io_wait(void)
{
    axys_outb(0x80, 0);
}

#endif
