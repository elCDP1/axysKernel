#include "axys.h"

/* DHCPv4 client (RFC 2131, the small subset that gets an address): DISCOVER
 * (broadcast) -> OFFER -> REQUEST (broadcast) -> ACK. Prints the lease
 * (IP, server, router, DNS) and installs our address via NET_SET_ADDR.
 * Usage: dhcp */

static u8 our_mac[6];

static u16 get16(const u8 *p) { return (u16)((u16)(p[0] << 8) | p[1]); }

static void put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* UDP checksum WITH the IPv4 pseudo-header (RFC 768): a nonzero but wrong
 * checksum gets the datagram dropped, so either do it right or send zero. */
static u16 checksum(const u8 *p, size_t n)
{
    u32 sum = 0;

    while (n > 1) {
        sum += (u32)((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }
    if (n) {
        sum += (u32)(p[0] << 8);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

static u16 udp_checksum(u32 src, u32 dst, const u8 *udp, size_t udp_len)
{
    u32 sum = 0;
    size_t n = udp_len;

    sum += (src >> 16) & 0xffff;
    sum += src & 0xffff;
    sum += (dst >> 16) & 0xffff;
    sum += dst & 0xffff;
    sum += 17; /* protocol */
    sum += (u32)udp_len;
    while (n > 1) {
        sum += (u32)((udp[0] << 8) | udp[1]);
        udp += 2;
        n -= 2;
    }
    if (n) {
        sum += (u32)(udp[0] << 8);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

static void print_ip(u32 ip)
{
    put_u64((ip >> 24) & 0xff);
    puts(".");
    put_u64((ip >> 16) & 0xff);
    puts(".");
    put_u64((ip >> 8) & 0xff);
    puts(".");
    put_u64(ip & 0xff);
}

static size_t build_dhcp(u8 *f, const u8 *dst_mac, u32 src_ip, u32 dst_ip, u16 src_port,
                         u16 dst_port, const u8 *payload, size_t payload_len)
{
    int i;
    size_t udp_len = 8 + payload_len;
    size_t ip_len = 20 + udp_len;

    for (i = 0; i < 6; ++i) {
        f[i] = dst_mac[i];
        f[6 + i] = our_mac[i];
    }
    put16(f + 12, 0x0800);
    f[14] = 0x45;
    f[15] = 0;
    put16(f + 16, (u16)ip_len);
    put16(f + 18, 0x1234);
    put16(f + 20, 0x0000);
    f[22] = 64;
    f[23] = 17;
    put16(f + 24, 0);
    put32(f + 26, src_ip);
    put32(f + 30, dst_ip);
    put16(f + 24, checksum(f + 14, 20));
    put16(f + 34, src_port);
    put16(f + 36, dst_port);
    put16(f + 38, (u16)udp_len);
    put16(f + 40, 0);
    for (i = 0; i < (int)payload_len; ++i) {
        f[42 + i] = payload[i];
    }
    put16(f + 40, udp_checksum(src_ip, dst_ip, f + 34, udp_len));
    return 42 + payload_len;
}

/* BOOTP fixed part (236 bytes) + magic cookie, ready for options. */
static size_t dhcp_base(u8 *p, u8 op, u32 xid, u32 yiaddr)
{
    int i;

    for (i = 0; i < 236; ++i) {
        p[i] = 0;
    }
    p[0] = op;
    p[1] = 1;
    p[2] = 6;
    p[3] = 0;
    put32(p + 4, xid);
    put16(p + 8, 0);
    put16(p + 10, 0x8000);
    put32(p + 12, 0);
    put32(p + 16, yiaddr);
    put32(p + 20, 0);
    put32(p + 24, 0);
    for (i = 0; i < 6; ++i) {
        p[28 + i] = our_mac[i];
    }
    p[236] = 99;
    p[237] = 130;
    p[238] = 83;
    p[239] = 99;
    return 240;
}

static size_t dhcp_opt(u8 *p, size_t n, u8 code, const u8 *data, size_t len)
{
    size_t i;

    p[n++] = code;
    p[n++] = (u8)len;
    for (i = 0; i < len; ++i) {
        p[n++] = data[i];
    }
    return n;
}

/* Parsed offer/ack fields. Returns 1 when the message type matches `want`. */
static int parse_dhcp(const u8 *f, long n, u32 xid, u8 want, u32 *yiaddr, u32 *server,
                      u32 *router)
{
    size_t off;
    size_t end;
    u8 msg = 0;
    u16 udp_len;

    *yiaddr = 0;
    *server = 0;
    *router = 0;
    if (n < 14 + 20 + 8 + 240 || get16(f + 12) != 0x0800 || f[23] != 17 ||
        get16(f + 36) != 68) {
        return 0;
    }
    /* Every later read is bounded by the UDP length, which is itself bounded
     * by the bytes actually received: no option scan can walk past `n`. */
    udp_len = get16(f + 38);
    if (14u + 20u + udp_len > (size_t)n || udp_len < 8 + 240) {
        return 0;
    }
    end = (size_t)udp_len - 8u; /* BOOTP/DHCP message length */
    {
        const u8 *b = f + 42;

        if (b[0] != 2 || get32(b + 4) != xid) {
            return 0;
        }
        {
            int same = 1;

            for (int i = 0; i < 6; ++i) {
                if (b[28 + i] != our_mac[i]) {
                    same = 0;
                }
            }
            if (!same) {
                return 0;
            }
        }
        *yiaddr = get32(b + 16);
        off = 240;
        while (off + 1 < end) {
            u8 code = b[off];

            if (code == 255) {
                break;
            }
            if (code == 0) {
                ++off;
                continue;
            }
            {
                u8 len = b[off + 1];

                if (off + 2u + len > end) {
                    return 0;
                }
                if (code == 53 && len == 1) {
                    msg = b[off + 2];
                } else if (code == 54 && len == 4 && *server == 0) {
                    *server = get32(b + off + 2);
                } else if (code == 3 && len >= 4 && *router == 0) {
                    *router = get32(b + off + 2);
                }
                off += 2u + len;
            }
        }
    }
    return msg == want;
}

/* Wait up to `ms` for a DHCP message of type `want` with our XID. */
static int wait_dhcp(u8 want, u32 xid, u32 *yiaddr, u32 *server, u32 *router, u64 ms)
{
    u64 deadline = uptime_ms() + ms;
    u8 f[1024];

    for (;;) {
        long n = net_recv(f, sizeof(f));

        if (n > 0 && parse_dhcp(f, n, xid, want, yiaddr, server, router)) {
            return 0;
        }
        if (n < 0 && n != -11) {
            return -1;
        }
        if (uptime_ms() >= deadline) {
            return -1;
        }
        sleep_ms(10);
    }
}

int main(const char *args, size_t len)
{
    struct net_stat st;
    u8 frame[576];
    u8 payload[300];
    u8 bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    u32 xid = 0x12345678u;
    u32 offered = 0, server = 0, router = 0;
    size_t n;
    u8 prl[3] = {1, 3, 6};

    (void)args;
    (void)len;
    if (net_stat(&st) != 0 || !st.link) {
        puts("dhcp: no link\n");
        return 1;
    }
    for (int i = 0; i < 6; ++i) {
        our_mac[i] = st.mac[i];
    }
    {
        long r = getrandom(&xid, sizeof(xid));

        if (r != sizeof(xid) || xid == 0) {
            xid = 0x12345678u;
        }
    }
    /* DISCOVER (broadcast). */
    n = dhcp_base(payload, 1, xid, 0);
    {
        u8 one = 1;
        u8 cid[7] = {1, 0, 0, 0, 0, 0, 0};

        for (int i = 0; i < 6; ++i) {
            cid[1 + i] = our_mac[i];
        }
        n = dhcp_opt(payload, n, 53, &one, 1);
        n = dhcp_opt(payload, n, 61, cid, 7);
        n = dhcp_opt(payload, n, 55, prl, 3);
        payload[n++] = 255;
    }
    while (n < 300) {
        payload[n++] = 0;
    }
    {
        size_t flen = build_dhcp(frame, bcast, 0, 0xffffffffu, 68, 67, payload, n);

        if (net_send(frame, flen) < 0) {
            puts("dhcp: send failed\n");
            return 1;
        }
    }
    if (wait_dhcp(2, xid, &offered, &server, &router, 4000) != 0 || offered == 0 ||
        server == 0) {
        puts("dhcp: no offer\n");
        return 1;
    }
    puts("dhcp: offer ");
    {
        u32 o = offered;

        put_u64((o >> 24) & 0xff);
        puts(".");
        put_u64((o >> 16) & 0xff);
        puts(".");
        put_u64((o >> 8) & 0xff);
        puts(".");
        put_u64(o & 0xff);
        puts(" from ");
        o = server;
        put_u64((o >> 24) & 0xff);
        puts(".");
        put_u64((o >> 16) & 0xff);
        puts(".");
        put_u64((o >> 8) & 0xff);
        puts(".");
        put_u64(o & 0xff);
        puts("\n");
    }
    /* REQUEST (broadcast, per RFC) + wait for ACK. */
    n = dhcp_base(payload, 1, xid, 0);
    {
        u8 one = 3;
        u8 rq[4], sv[4], cid[7] = {1, 0, 0, 0, 0, 0, 0};

        rq[0] = (u8)(offered >> 24);
        rq[1] = (u8)(offered >> 16);
        rq[2] = (u8)(offered >> 8);
        rq[3] = (u8)offered;
        sv[0] = (u8)(server >> 24);
        sv[1] = (u8)(server >> 16);
        sv[2] = (u8)(server >> 8);
        sv[3] = (u8)server;
        n = dhcp_opt(payload, n, 53, &one, 1);
        for (int i = 0; i < 6; ++i) {
            cid[1 + i] = our_mac[i];
        }
        n = dhcp_opt(payload, n, 61, cid, 7);
        n = dhcp_opt(payload, n, 50, rq, 4);
        n = dhcp_opt(payload, n, 54, sv, 4);
        n = dhcp_opt(payload, n, 55, prl, 3);
        payload[n++] = 255;
    }
    while (n < 300) {
        payload[n++] = 0;
    }
    {
        size_t flen = build_dhcp(frame, bcast, 0, 0xffffffffu, 68, 67, payload, n);
        u32 acked = 0, aserver = 0, arouter = 0;

        if (net_send(frame, flen) < 0) {
            puts("dhcp: send failed\n");
            return 1;
        }
        if (wait_dhcp(5, xid, &acked, &aserver, &arouter, 4000) != 0 || acked == 0) {
            puts("dhcp: no ack\n");
            return 1;
        }
        if (arouter != 0) {
            router = arouter;
        }
        {
            u8 ip[4];

            ip[0] = (u8)(acked >> 24);
            ip[1] = (u8)(acked >> 16);
            ip[2] = (u8)(acked >> 8);
            ip[3] = (u8)acked;
            if (net_set_addr(ip) != 0) {
                puts("dhcp: cannot set address\n");
                return 1;
            }
        }
        puts("dhcp: bound ");
        print_ip(acked);
        puts(" router ");
        print_ip(router);
        puts("\n");
    }
    return 0;
}
