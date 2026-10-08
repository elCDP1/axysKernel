/*
 * USB configuration-descriptor parsing against hostile input. Directed cases
 * cover the malformed shapes a device can present; the fuzzer feeds random and
 * mutated blobs in exactly-sized heap buffers so AddressSanitizer reports any
 * read past the end. Hosted build only.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "axys/usb_desc.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static size_t put_config(uint8_t *b, size_t total, uint8_t value)
{
    uint8_t d[9] = {9, 2, (uint8_t)total, (uint8_t)(total >> 8), 1, value, 0, 0x80, 50};
    memcpy(b, d, 9);
    return 9;
}
static size_t put_iface(uint8_t *b, uint8_t num, uint8_t eps, uint8_t cls, uint8_t sub, uint8_t proto)
{
    uint8_t d[9] = {9, 4, num, 0, eps, cls, sub, proto, 0};
    memcpy(b, d, 9);
    return 9;
}
static size_t put_ep(uint8_t *b, uint8_t addr, uint8_t attr, uint16_t mps, uint8_t interval)
{
    uint8_t d[7] = {7, 5, addr, attr, (uint8_t)mps, (uint8_t)(mps >> 8), interval};
    memcpy(b, d, 7);
    return 7;
}

static void directed(void)
{
    uint8_t b[256];
    size_t n;
    struct axys_usb_kbd_desc kd;
    struct axys_usb_msc_desc md;

    /* plain boot keyboard */
    n = put_config(b, 34, 1);
    n += put_iface(b + n, 0, 1, 3, 1, 1);
    n += 9; b[n - 9] = 9; b[n - 8] = 0x21; memset(b + n - 7, 0, 7); /* HID descriptor, skipped by the walker */
    n += put_ep(b + n, 0x81, 3, 8, 10);
    CHECK(axys_usb_desc_keyboard(b, n, &kd) == 0);
    CHECK(kd.config_value == 1 && kd.iface == 0 && kd.ep_addr == 0x81 && kd.mps == 8 && kd.interval == 10);
    /* every truncation is rejected or still in bounds (ASan below) */
    for (size_t cut = 0; cut < n; ++cut) {
        uint8_t *exact = malloc(cut ? cut : 1);
        memcpy(exact, b, cut);
        (void)axys_usb_desc_keyboard(exact, cut, &kd);
        free(exact);
    }

    /* an interface descriptor claiming length 2: reading class bytes would run past it */
    n = put_config(b, 11, 1);
    b[n] = 2; b[n + 1] = 4; n += 2;
    CHECK(axys_usb_desc_find(b, n, 4, 0) == -1);
    CHECK(axys_usb_desc_keyboard(b, n, &kd) == -1);

    /* a short endpoint descriptor ends the search instead of being read as 7 bytes */
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 1, 3, 1, 1);
    b[n] = 3; b[n + 1] = 5; b[n + 2] = 0x81; n += 3;
    CHECK(axys_usb_desc_find(b, n, 5, 0) == -1);
    CHECK(axys_usb_desc_keyboard(b, n, &kd) == -1);

    /* the keyboard interface has no endpoint of its own; a later vendor interface does.
     * Taking that one (the old behaviour) would drive the wrong endpoint. */
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 0, 3, 1, 1);
    n += put_iface(b + n, 1, 1, 0xff, 0, 0);
    n += put_ep(b + n, 0x82, 3, 8, 4);
    CHECK(axys_usb_desc_keyboard(b, n, &kd) == -1);

    /* keyboard as the second interface of a composite device, endpoint kept with its interface */
    n = put_config(b, 0, 2);
    n += put_iface(b + n, 0, 1, 3, 1, 2);      /* a mouse first */
    n += put_ep(b + n, 0x81, 3, 4, 10);
    n += put_iface(b + n, 1, 1, 3, 1, 1);      /* then the keyboard */
    n += put_ep(b + n, 0x82, 3, 8, 7);
    CHECK(axys_usb_desc_keyboard(b, n, &kd) == 0 && kd.iface == 1 && kd.ep_addr == 0x82 && kd.config_value == 2);

    /* out-of-range packet size and interval fall back to defaults; bulk is not an interrupt endpoint */
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 2, 3, 1, 1);
    n += put_ep(b + n, 0x81, 2, 64, 10);       /* bulk: skipped */
    n += put_ep(b + n, 0x82, 3, 4096, 0);
    CHECK(axys_usb_desc_keyboard(b, n, &kd) == 0 && kd.ep_addr == 0x82 && kd.mps == 8 && kd.interval == 10);

    /* bulk-only mass storage, endpoints in either order */
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 2, 8, 6, 0x50);
    n += put_ep(b + n, 0x02, 2, 512, 0);
    n += put_ep(b + n, 0x81, 2, 512, 0);
    CHECK(axys_usb_desc_storage(b, n, &md) == 0);
    CHECK(md.out_ep == 0x02 && md.in_ep == 0x81 && md.out_mps == 512 && md.in_mps == 512);
    /* two IN endpoints, a missing OUT, a zero endpoint number, a silly packet size: all refused */
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 3, 8, 6, 0x50);
    n += put_ep(b + n, 0x81, 2, 512, 0);
    n += put_ep(b + n, 0x82, 2, 512, 0);
    n += put_ep(b + n, 0x02, 2, 512, 0);
    CHECK(axys_usb_desc_storage(b, n, &md) == -1);
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 1, 8, 6, 0x50);
    n += put_ep(b + n, 0x81, 2, 512, 0);
    CHECK(axys_usb_desc_storage(b, n, &md) == -1);
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 2, 8, 6, 0x50);
    n += put_ep(b + n, 0x80, 2, 512, 0);
    n += put_ep(b + n, 0x01, 2, 512, 0);
    CHECK(axys_usb_desc_storage(b, n, &md) == -1);
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 2, 8, 6, 0x50);
    n += put_ep(b + n, 0x81, 2, 4, 0);
    n += put_ep(b + n, 0x01, 2, 512, 0);
    CHECK(axys_usb_desc_storage(b, n, &md) == -1);
    /* bulk endpoints of a *different* interface do not count */
    n = put_config(b, 0, 1);
    n += put_iface(b + n, 0, 0, 8, 6, 0x50);
    n += put_iface(b + n, 1, 2, 0xff, 0, 0);
    n += put_ep(b + n, 0x81, 2, 512, 0);
    n += put_ep(b + n, 0x01, 2, 512, 0);
    CHECK(axys_usb_desc_storage(b, n, &md) == -1);
    /* a descriptor must be able to hold the first config descriptor */
    CHECK(axys_usb_desc_keyboard(b, 4, &kd) == -1);
    CHECK(axys_usb_desc_find(NULL, 10, 4, 0) == -1);
    CHECK(axys_usb_desc_find(b, 0, 4, 0) == -1);
    CHECK(axys_usb_desc_find(b, n, 4, -1) == -1);
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng % n); }

static void fuzz(unsigned iterations)
{
    for (unsigned it = 0; it < iterations; ++it) {
        uint8_t proto[200];
        size_t n = put_config(proto, 0, (uint8_t)rnd(3));
        size_t len;
        uint8_t *blob;
        struct axys_usb_kbd_desc kd;
        struct axys_usb_msc_desc md;

        for (unsigned k = 0, count = 1 + rnd(5); k < count && n + 16 < sizeof(proto); ++k) {
            switch (rnd(4)) {
            case 0: n += put_iface(proto + n, (uint8_t)rnd(3), (uint8_t)rnd(3), rnd(2) ? 3 : 8, rnd(2) ? 1 : 6, rnd(2) ? 1 : 0x50); break;
            case 1: n += put_ep(proto + n, (uint8_t)(rnd(2) ? 0x80 | rnd(4) : rnd(4)), (uint8_t)rnd(4), (uint16_t)(rnd(3) ? (uint32_t)8 << rnd(7) : rnd(65536)), (uint8_t)rnd(20)); break;
            case 2: proto[n] = 9; proto[n + 1] = 0x21; memset(proto + n + 2, 0, 7); n += 9; break;
            default: { uint8_t l = (uint8_t)rnd(12); proto[n] = l; proto[n + 1] = (uint8_t)rnd(8); for (unsigned i = 2; i < l && n + i < sizeof(proto); ++i) proto[n + i] = (uint8_t)rnd(256); n += l < 2 ? 2 : l; }
            }
        }
        if (rnd(3) == 0) {
            for (unsigned f = 0, flips = 1 + rnd(4); f < flips; ++f) proto[rnd((uint32_t)n)] = (uint8_t)rnd(256);
        }
        len = rnd(4) == 0 ? rnd((uint32_t)n + 1) : n;
        blob = malloc(len ? len : 1);
        memcpy(blob, proto, len);

        for (int type = 2; type <= 5; ++type) {
            for (int inst = 0; inst < 4; ++inst) {
                int off = axys_usb_desc_find(blob, len, (uint8_t)type, inst);
                if (off >= 0) {
                    uint8_t need = (type == 2 || type == 4) ? 9 : (type == 5 ? 7 : 2);
                    CHECK((size_t)off + need <= len && blob[off + 1] == type && blob[off] >= need);
                }
            }
        }
        if (axys_usb_desc_keyboard(blob, len, &kd) == 0) {
            CHECK(kd.mps >= 1 && kd.mps <= 64 && kd.interval != 0 && (kd.ep_addr & 0x80) && (kd.ep_addr & 0x0f));
        }
        if (axys_usb_desc_storage(blob, len, &md) == 0) {
            CHECK(md.in_ep & 0x80 && !(md.out_ep & 0x80) && md.in_mps >= 8 && md.in_mps <= 1024 && md.out_mps >= 8 && md.out_mps <= 1024);
        }
        (void)axys_usb_desc_interface_end(blob, len, (int)rnd((uint32_t)len + 2) - 1);
        free(blob);
    }
}

int main(int argc, char **argv)
{
    directed();
    fuzz(argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 200000);
    puts("test_usb_desc: ok");
    return 0;
}
