#include "axys.h"

/* ICMP ping over the raw-frame syscalls: ARP resolution plus echo
 * request/reply, with answers to incoming ARP and echo requests on the side
 * (so the guest is a polite citizen, and pingable in principle).
 * Defaults match QEMU user networking: we are 10.0.2.15, gateway 10.0.2.2.
 * Usage: ping [host] [count] */

static u8 gw_ip[4] = {10, 0, 2, 2};
static u8 our_ip[4] = {10, 0, 2, 15};
static u8 our_mac[6];
static u8 gw_mac[6];
static int have_gw;

static u16 get16(const u8 *p) { return (u16)((u16)(p[0] << 8) | p[1]); }

static void put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

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

static int parse_ip(const char *s, u8 *out)
{
    for (int i = 0; i < 4; ++i) {
        u32 v = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (u32)(*s - '0');
            if (v > 255) {
                return -1;
            }
            ++s;
            ++digits;
        }
        if (!digits || (i < 3 && *s++ != '.') || (i == 3 && *s != '\0' && *s != ' ')) {
            return -1;
        }
        out[i] = (u8)v;
    }
    return 0;
}

static void print_ip(const u8 *ip)
{
    for (int i = 0; i < 4; ++i) {
        if (i) {
            puts(".");
        }
        put_u64(ip[i]);
    }
}

static void send_arp_request(void)
{
    u8 f[60];
    int i;

    for (i = 0; i < 6; ++i) {
        f[i] = 0xff;
        f[6 + i] = our_mac[i];
    }
    put16(f + 12, 0x0806);
    put16(f + 14, 1);
    put16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    put16(f + 20, 1);
    for (i = 0; i < 6; ++i) {
        f[22 + i] = our_mac[i];
    }
    for (i = 0; i < 4; ++i) {
        f[28 + i] = our_ip[i];
        f[32 + i] = 0;
        f[38 + i] = gw_ip[i];
    }
    for (i = 42; i < 60; ++i) {
        f[i] = 0;
    }
    net_send(f, sizeof(f));
}

static void send_arp_reply(const u8 *to_mac, const u8 *to_ip)
{
    u8 f[60];
    int i;

    for (i = 0; i < 6; ++i) {
        f[i] = to_mac[i];
        f[6 + i] = our_mac[i];
    }
    put16(f + 12, 0x0806);
    put16(f + 14, 1);
    put16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    put16(f + 20, 2);
    for (i = 0; i < 6; ++i) {
        f[22 + i] = our_mac[i];
    }
    for (i = 0; i < 4; ++i) {
        f[28 + i] = our_ip[i];
        f[32 + i] = to_mac[i];
        f[38 + i] = to_ip[i];
    }
    for (i = 42; i < 60; ++i) {
        f[i] = 0;
    }
    net_send(f, sizeof(f));
}

static void send_echo(u16 id, u16 seq, int reply, const u8 *dst_mac, const u8 *dst_ip,
                      const u8 *payload, size_t payload_len)
{
    u8 f[128];
    int i;
    size_t ip_len = 20 + 8 + payload_len;

    for (i = 0; i < 6; ++i) {
        f[i] = dst_mac[i];
        f[6 + i] = our_mac[i];
    }
    put16(f + 12, 0x0800);
    f[14] = 0x45;
    f[15] = 0;
    put16(f + 16, (u16)ip_len);
    put16(f + 18, seq);
    put16(f + 20, 0x4000);
    f[22] = 64;
    f[23] = 1;
    put16(f + 24, 0);
    for (i = 0; i < 4; ++i) {
        f[26 + i] = our_ip[i];
        f[30 + i] = dst_ip[i];
    }
    put16(f + 24, checksum(f + 14, 20));
    f[34] = reply ? 0 : 8;
    f[35] = 0;
    put16(f + 36, 0);
    put16(f + 38, id);
    put16(f + 40, seq);
    for (i = 0; i < (int)payload_len; ++i) {
        f[42 + i] = payload[i];
    }
    put16(f + 36, checksum(f + 34, 8 + payload_len));
    net_send(f, 14 + ip_len);
}

/* Handle one received frame: learn ARP replies, answer ARP/echo for us.
 * Returns 1 and fills reply fields when an echo REPLY for (id) arrives. */
static int handle_frame(const u8 *f, long n, u16 id, u16 *seq_out)
{
    u16 eth;

    if (n < 14 + 8) {
        return 0;
    }
    eth = get16(f + 12);
    if (eth == 0x0806 && n >= 42) {
        u16 op = get16(f + 20);
        int for_us = 1;

        for (int i = 0; i < 4; ++i) {
            if (f[38 + i] != our_ip[i]) {
                for_us = 0;
            }
        }
        if (op == 2 && !have_gw) {
            int from_gw = 1;

            for (int i = 0; i < 4; ++i) {
                if (f[28 + i] != gw_ip[i]) {
                    from_gw = 0;
                }
            }
            if (from_gw) {
                for (int i = 0; i < 6; ++i) {
                    gw_mac[i] = f[22 + i];
                }
                have_gw = 1;
            }
        } else if (op == 1 && for_us) {
            u8 who[6], wip[4];
            int i;

            for (i = 0; i < 6; ++i) {
                who[i] = f[22 + i];
            }
            for (i = 0; i < 4; ++i) {
                wip[i] = f[28 + i];
            }
            send_arp_reply(who, wip);
        }
        return 0;
    }
    if (eth == 0x0800 && n >= 14 + 20 + 8 && f[23] == 1) {
        u8 sip[4];
        int i, ours = 1, theirs = 1;

        for (i = 0; i < 4; ++i) {
            sip[i] = f[26 + i];
            if (f[30 + i] != our_ip[i]) {
                ours = 0;
            }
            if (sip[i] != gw_ip[i]) {
                theirs = 0;
            }
        }
        if (f[34] == 8 && ours) { /* echo request for us: answer it */
            u8 smac[6];

            for (i = 0; i < 6; ++i) {
                smac[i] = f[6 + i];
            }
            send_echo(get16(f + 38), get16(f + 40), 1, smac, sip, f + 42,
                      (size_t)n - 42 > 32 ? 32 : (size_t)n - 42);
            return 0;
        }
        if (f[34] == 0 && theirs && get16(f + 38) == id) {
            *seq_out = get16(f + 40);
            return 1;
        }
    }
    return 0;
}

/* Pump the receive queue until `deadline` (uptime ms), answering background
 * traffic. Returns 1 on matching echo reply (seq stored), 0 on timeout. */
static int wait_reply(u16 id, u16 want_seq, u64 deadline, u16 *got_seq)
{
    u8 f[2048];

    for (;;) {
        long n = net_recv(f, sizeof(f));

        if (n > 0) {
            u16 seq = 0;

            if (handle_frame(f, n, id, &seq) && seq == want_seq) {
                *got_seq = seq;
                return 1;
            }
        } else if (n != -11) {
            return 0; /* NIC vanished mid-ping */
        }
        if (uptime_ms() >= deadline) {
            return 0;
        }
        sleep_ms(10);
    }
}

int main(const char *args, size_t len)
{
    struct net_stat st;
    u16 id = 0xa5a5;
    int count = 4;
    int got = 0;
    const char *a = args;

    (void)len;
    if (net_stat(&st) != 0 || !st.link) {
        puts("ping: no link\n");
        return 1;
    }
    for (int i = 0; i < 6; ++i) {
        our_mac[i] = st.mac[i];
    }
    /* Source address follows the kernel's (set by /bin/dhcp, if used). */
    for (int i = 0; i < 4; ++i) {
        our_ip[i] = st.ip[i];
    }
    while (*a == ' ') {
        ++a;
    }
    if (*a) {
        char host[32];
        int i = 0;

        while (*a && *a != ' ' && i + 1 < (int)sizeof(host)) {
            host[i++] = *a++;
        }
        host[i] = '\0';
        if (parse_ip(host, gw_ip) != 0) {
            puts("ping: bad address\n");
            return 1;
        }
        while (*a == ' ') {
            ++a;
        }
        if (*a) {
            u64 c = 0;

            while (*a >= '0' && *a <= '9') {
                c = c * 10 + (u64)(*a - '0');
                ++a;
            }
            if (c < 1) {
                c = 1;
            }
            if (c > 100) {
                c = 100;
            }
            count = (int)c;
        }
    }
    puts("ping ");
    print_ip(gw_ip);
    puts(" from ");
    print_ip(our_ip);
    puts("\n");
    /* Resolve the gateway first. */
    for (int tries = 0; tries < 3 && !have_gw; ++tries) {
        u64 deadline = uptime_ms() + 2000;
        u8 f[2048];
        long n;

        send_arp_request();
        for (;;) {
            n = net_recv(f, sizeof(f));
            if (n > 0) {
                u16 dummy = 0;

                handle_frame(f, n, id, &dummy);
                if (have_gw) {
                    break;
                }
            } else if (n != -11) {
                puts("ping: net lost\n");
                return 1;
            }
            if (uptime_ms() >= deadline) {
                break;
            }
            sleep_ms(10);
        }
    }
    if (!have_gw) {
        puts("ping: ARP failed\n");
        return 1;
    }
    for (int seq = 1; seq <= count; ++seq) {
        u64 deadline = uptime_ms() + 2000;
        u16 got_seq = 0;
        u8 payload[16];
        int i;

        for (i = 0; i < 16; ++i) {
            payload[i] = (u8)(seq + i);
        }
        send_echo(id, (u16)seq, 0, gw_mac, gw_ip, payload, sizeof(payload));
        if (wait_reply(id, (u16)seq, deadline, &got_seq)) {
            puts("64 bytes from ");
            print_ip(gw_ip);
            puts(": seq=");
            put_u64((u64)got_seq);
            puts("\n");
            ++got;
        } else {
            puts("timeout seq=");
            put_u64((u64)seq);
            puts("\n");
        }
    }
    puts("sent ");
    put_u64((u64)count);
    puts(" received ");
    put_u64((u64)got);
    puts("\n");
    return got == count ? 0 : 1;
}
