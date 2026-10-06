#ifndef AXYS_E1000_H
#define AXYS_E1000_H

#include "axys/types.h"

/*
 * Intel 82540EM-compatible PCI Ethernet (the QEMU default NIC), polled.
 * Transmit/receive descriptor rings, a small kernel RX frame queue drained by
 * a polling task, link detection and MAC readout. No interrupts, no checksum
 * or TCP segmentation offload, no jumbo frames. ARP/ICMP live in user space
 * (/bin/ping); the kernel only moves raw Ethernet frames.
 */

#define AXYS_NET_MTU 1500u
#define AXYS_NET_FRAME_MAX 2048u
#define AXYS_NET_QUEUE 32u /* frames held for NET_RECV */

/* Probe PCI, reset the controller and bring the link up. Returns 0 with a
 * usable interface, or -1 (no device, no link, no memory): the system still
 * boots, syscalls just report ENODEV. Safe to call when no NIC exists. */
int axys_e1000_init(void);

/* One-line summary for the boot log. */
const char *axys_e1000_summary(void);

int axys_net_present(void);

/* Transmit one frame (60..2048 bytes; shorter frames are padded). Returns the
 * wire length, or -1 (no device, bad length, link down, transmit timeout). */
int axys_net_send(const void *frame, axys_size_t length);

/* Oldest queued received frame, up to `capacity` bytes (truncated POSIX-style
 * when smaller). Returns the bytes copied, or -1 when the queue is empty
 * (EAGAIN semantics). */
int axys_net_recv(void *out, axys_size_t capacity);

struct axys_net_stat {
    axys_uint8_t mac[6];
    axys_uint8_t link;
    axys_uint8_t pad;
    axys_uint32_t speed_mbps;
    axys_uint8_t ip[4]; /* our IPv4 (defaults to QEMU user-net, settable) */
    axys_uint8_t ip_pad[4];
    axys_uint64_t tx_packets;
    axys_uint64_t rx_packets;
    axys_uint64_t rx_dropped;
};

void axys_net_stat(struct axys_net_stat *out);

/* Set our IPv4 address (used by user-space stacks; the link layer does not
 * care). Any uid may read it, only root may set it (enforced at syscall). */
void axys_net_set_addr(const axys_uint8_t ip[4]);

#endif
