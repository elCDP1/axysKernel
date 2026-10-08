#include "axys/usb_desc.h"

static axys_uint16_t u16le(const axys_uint8_t *p)
{
    return (axys_uint16_t)((axys_uint16_t)p[0] | ((axys_uint16_t)p[1] << 8));
}

static axys_uint8_t min_length(axys_uint8_t type)
{
    switch (type) {
    case AXYS_USB_DESC_CONFIG:
    case AXYS_USB_DESC_INTERFACE:
        return 9u;
    case AXYS_USB_DESC_ENDPOINT:
        return 7u;
    default:
        return 2u;
    }
}

int axys_usb_desc_find(const axys_uint8_t *blob, axys_size_t length, axys_uint8_t type, int instance)
{
    axys_size_t offset = 0;
    int seen = 0;

    if (blob == AXYS_NULL || instance < 0) {
        return -1;
    }
    while (offset + 2u <= length) {
        axys_uint8_t dlen = blob[offset];
        axys_uint8_t dtype = blob[offset + 1u];

        if (dlen < 2u || (axys_size_t)dlen > length - offset) {
            return -1;
        }
        if (dtype == type) {
            if (dlen < min_length(type)) {
                return -1;
            }
            if (seen++ == instance) {
                return (int)offset;
            }
        }
        offset += dlen;
    }
    return -1;
}

axys_size_t axys_usb_desc_interface_end(const axys_uint8_t *blob, axys_size_t length, int iface_off)
{
    axys_size_t offset;

    if (blob == AXYS_NULL || iface_off < 0 || (axys_size_t)iface_off >= length) {
        return length;
    }
    offset = (axys_size_t)iface_off;
    for (;;) {
        axys_uint8_t dlen;

        if (offset + 2u > length) {
            return length;
        }
        dlen = blob[offset];
        if (dlen < 2u || (axys_size_t)dlen > length - offset) {
            return length;
        }
        offset += dlen;
        if (offset + 2u <= length && blob[offset + 1u] == AXYS_USB_DESC_INTERFACE) {
            return offset;
        }
    }
}

/* The configuration value comes from the configuration descriptor, which must
 * be the first descriptor of the blob. */
static int config_value_of(const axys_uint8_t *blob, axys_size_t length, axys_uint8_t *value)
{
    if (axys_usb_desc_find(blob, length, AXYS_USB_DESC_CONFIG, 0) != 0) {
        return -1;
    }
    *value = blob[5];
    return 0;
}

int axys_usb_desc_keyboard(const axys_uint8_t *blob, axys_size_t length, struct axys_usb_kbd_desc *out)
{
    axys_uint8_t config_value;

    if (out == AXYS_NULL || config_value_of(blob, length, &config_value) != 0) {
        return -1;
    }
    for (int inst = 0;; ++inst) {
        int iface = axys_usb_desc_find(blob, length, AXYS_USB_DESC_INTERFACE, inst);
        axys_size_t end;

        if (iface < 0) {
            return -1;
        }
        if (blob[iface + 5] != 3u || blob[iface + 6] != 1u || blob[iface + 7] != 1u) {
            continue;
        }
        end = axys_usb_desc_interface_end(blob, length, iface);
        for (int ep = 0;; ++ep) {
            int off = axys_usb_desc_find(blob, length, AXYS_USB_DESC_ENDPOINT, ep);

            if (off < 0 || (axys_size_t)off >= end) {
                break; /* no interrupt-IN endpoint inside this interface */
            }
            if (off < iface) {
                continue;
            }
            if ((blob[off + 2] & 0x80u) != 0u && (blob[off + 3] & 3u) == 3u &&
                (blob[off + 2] & 0x0fu) != 0u) {
                axys_uint16_t mps = u16le(blob + off + 4);

                out->config_value = config_value;
                out->iface = blob[iface + 2];
                out->ep_addr = blob[off + 2];
                out->mps = (mps == 0u || mps > 64u) ? 8u : mps;
                out->interval = blob[off + 6] == 0u ? 10u : blob[off + 6];
                return 0;
            }
        }
        /* a keyboard-class interface without a usable endpoint: try the next */
    }
}

int axys_usb_desc_storage(const axys_uint8_t *blob, axys_size_t length, struct axys_usb_msc_desc *out)
{
    axys_uint8_t config_value;

    if (out == AXYS_NULL || config_value_of(blob, length, &config_value) != 0) {
        return -1;
    }
    for (int inst = 0;; ++inst) {
        int iface = axys_usb_desc_find(blob, length, AXYS_USB_DESC_INTERFACE, inst);
        axys_size_t end;
        axys_uint8_t out_ep = 0, in_ep = 0;
        axys_uint16_t out_mps = 0, in_mps = 0;
        int bad = 0;

        if (iface < 0) {
            return -1;
        }
        if (blob[iface + 5] != 8u || blob[iface + 6] != 6u || blob[iface + 7] != 0x50u) {
            continue;
        }
        end = axys_usb_desc_interface_end(blob, length, iface);
        for (int ep = 0; !bad; ++ep) {
            int off = axys_usb_desc_find(blob, length, AXYS_USB_DESC_ENDPOINT, ep);
            axys_uint8_t addr;
            axys_uint16_t mps;

            if (off < 0 || (axys_size_t)off >= end) {
                break;
            }
            if (off < iface || (blob[off + 3] & 3u) != 2u) {
                continue; /* not ours, or not bulk */
            }
            addr = blob[off + 2];
            mps = u16le(blob + off + 4);
            if (mps < 8u || mps > 1024u || (addr & 0x0fu) == 0u) {
                bad = 1;
            } else if ((addr & 0x80u) != 0u) {
                if (in_ep != 0u) { bad = 1; } else { in_ep = addr; in_mps = mps; }
            } else {
                if (out_ep != 0u) { bad = 1; } else { out_ep = addr; out_mps = mps; }
            }
        }
        if (!bad && in_ep != 0u && out_ep != 0u) {
            out->config_value = config_value;
            out->iface = blob[iface + 2];
            out->out_ep = out_ep;
            out->in_ep = in_ep;
            out->out_mps = out_mps;
            out->in_mps = in_mps;
            return 0;
        }
    }
}
