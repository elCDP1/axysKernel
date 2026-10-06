#include "axys/usb_storage.h"
#include "axys/dma.h"
#include "axys/printf.h"
#include "axys/string.h"
#include "axys/xhci.h"

/* USB Mass Storage, Bulk-Only Transport (BBB, subclass 6, protocol 0x50).
 *
 * One command = CBW (31 bytes, bulk-OUT) + optional data stage in ≤4 KiB
 * single-TRB steps + CSW (13 bytes, bulk-IN). The controller layer moves one
 * TRB per doorbell, so every completion matches exactly one request and short
 * packets cannot strand a chained tail. A failed command runs Bulk-Only reset
 * recovery (class reset + endpoint resets) and retries once.
 *
 * Disk-sized transfers are split into 64-sector (32 KiB) commands so no
 * single command needs more than a small DMA buffer. Only 512-byte blocks. */

#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u
#define CBW_LEN 31u
#define CSW_LEN 13u

#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE 0x03u
#define SCSI_INQUIRY 0x12u
#define SCSI_READ_CAPACITY_10 0x25u
#define SCSI_READ_10 0x28u
#define SCSI_WRITE_10 0x2au

#define SECTORS_PER_COMMAND 64u
#define SECTOR_SIZE 512u

static axys_uint8_t slot;
static axys_uint8_t dci_out;
static axys_uint8_t dci_in;
static axys_uint64_t sectors;
static int ready;
static axys_uint32_t tag;

static void put32be(axys_uint8_t *at, axys_uint32_t v)
{
    at[0] = (axys_uint8_t)(v >> 24);
    at[1] = (axys_uint8_t)(v >> 16);
    at[2] = (axys_uint8_t)(v >> 8);
    at[3] = (axys_uint8_t)v;
}

/* CBW/CSW header fields are little-endian values (the "USBC" signature is the
 * u32 0x43425355 in LE order, i.e. the literal bytes U,S,B,C); only the CDB
 * itself uses big-endian (SCSI convention). Mixing them up spells "CBSU" and
 * every command is rejected for a bad signature. */
static void put32le(axys_uint8_t *at, axys_uint32_t v)
{
    at[0] = (axys_uint8_t)v;
    at[1] = (axys_uint8_t)(v >> 8);
    at[2] = (axys_uint8_t)(v >> 16);
    at[3] = (axys_uint8_t)(v >> 24);
}

static axys_uint32_t get32le(const axys_uint8_t *at)
{
    return (axys_uint32_t)at[0] | ((axys_uint32_t)at[1] << 8) |
           ((axys_uint32_t)at[2] << 16) | ((axys_uint32_t)at[3] << 24);
}

static axys_uint32_t get32be(const axys_uint8_t *at)
{
    return ((axys_uint32_t)at[0] << 24) | ((axys_uint32_t)at[1] << 16) |
           ((axys_uint32_t)at[2] << 8) | at[3];
}

static void put16be(axys_uint8_t *at, axys_uint16_t v)
{
    at[0] = (axys_uint8_t)(v >> 8);
    at[1] = (axys_uint8_t)v;
}

/* One CBW + data + CSW cycle on the configured bulk pair. `data` is a DMA
 * buffer (or AXYS_NULL with data_len 0). Returns 0 with residue 0. */
static int command(const axys_uint8_t *cdb, unsigned cdb_len, axys_uint64_t data_phys,
                   axys_uint32_t data_len, int data_in)
{
    axys_uint8_t *cbw;
    axys_uint64_t cbw_phys;
    axys_uint8_t *csw;
    axys_uint64_t csw_phys;
    int rc = -1;

    if (cdb_len < 1u || cdb_len > 16u) {
        return -1;
    }
    cbw = axys_dma_alloc(64, 16, 0, AXYS_DMA_32BIT, &cbw_phys);
    csw = axys_dma_alloc(64, 16, 0, AXYS_DMA_32BIT, &csw_phys);
    if (cbw == AXYS_NULL || csw == AXYS_NULL) {
        goto out;
    }
    axys_memset(cbw, 0, 64);
    put32le(cbw, CBW_SIG);
    put32le(cbw + 4, ++tag);
    put32le(cbw + 8, data_len);
    cbw[12] = data_in ? 0x80u : 0u;
    cbw[13] = 0; /* LUN 0 */
    cbw[14] = (axys_uint8_t)cdb_len;
    axys_memcpy(cbw + 15, cdb, cdb_len);
    if (axys_usb_bulk_transfer(slot, dci_out, cbw_phys, CBW_LEN, 0, 2000u) != 0) {
        goto out;
    }
    if (data_len != 0) {
        axys_uint32_t done = 0;

        while (done < data_len) {
            axys_uint32_t chunk = data_len - done > 4096u ? 4096u : data_len - done;

            if (axys_usb_bulk_transfer(slot, data_in ? dci_in : dci_out, data_phys + done,
                                       chunk, data_in, 5000u) != 0) {
                goto out;
            }
            done += chunk;
        }
    }
    axys_memset(csw, 0, 64);
    if (axys_usb_bulk_transfer(slot, dci_in, csw_phys, CSW_LEN, 1, 2000u) != 0) {
        goto out;
    }
    if (get32le(csw) != CSW_SIG || get32le(csw + 4) != tag || get32le(csw + 8) != 0u ||
        csw[12] != 0u) {
        goto out;
    }
    rc = 0;
out:
    axys_dma_free(cbw);
    axys_dma_free(csw);
    return rc;
}

/* Bulk-Only reset recovery: class reset, both endpoints un-stalled. */
static void reset_recovery(void)
{
    /* Bulk-Only Mass Storage Reset (0x21/0xFF) then Reset Endpoint both ways. */
    (void)axys_usb_control(slot, 0x21, 0xff, 0, 0, 0, 0, 0, 2000u);
    axys_usb_reset_endpoint(slot, dci_out);
    axys_usb_reset_endpoint(slot, dci_in);
}

static int command_retry(const axys_uint8_t *cdb, unsigned cdb_len, axys_uint64_t data_phys,
                         axys_uint32_t data_len, int data_in)
{
    if (command(cdb, cdb_len, data_phys, data_len, data_in) == 0) {
        return 0;
    }
    reset_recovery();
    return command(cdb, cdb_len, data_phys, data_len, data_in);
}

int axys_usb_storage_init(void)
{
    axys_uint8_t cdb[16];
    axys_uint8_t *buf;
    axys_uint64_t buf_phys;

    if (ready) {
        return 0;
    }
    sectors = 0;
    if (axys_usb_storage_info(&slot, &dci_out, &dci_in) != 0) {
        return -1; /* no configured drive */
    }
    buf = axys_dma_alloc(512, 16, 0, AXYS_DMA_32BIT, &buf_phys);
    if (buf == AXYS_NULL) {
        return -1;
    }
    /* Bulk-Only Reset first: firmware (or a previous failed command) may have
     * left the device stalled, in which case every CBW is rejected until a
     * reset clears it. */
    reset_recovery();
    /* Wait for the medium (spinning rust or slow sticks need a moment). */
    {
        int ok = 0;

        for (int i = 0; i < 20; ++i) {
            axys_memset(cdb, 0, sizeof(cdb));
            if (command(cdb, 6, 0, 0, 0) == 0) {
                ok = 1;
                break;
            }
            /* Best-effort settle between polls (PIT-based, always available
             * this late in boot). */
            for (volatile unsigned s = 0; s < 2000000u; ++s) {
            }
        }
        if (!ok) {
            axys_printf("usb-storage: test unit ready failed\n");
            axys_dma_free(buf);
            return -1;
        }
    }
    /* INQUIRY for the boot log (content otherwise ignored). */
    {
        axys_memset(cdb, 0, sizeof(cdb));
        cdb[0] = SCSI_INQUIRY;
        cdb[4] = 36;
        axys_memset(buf, 0, 512);
        if (command_retry(cdb, 6, buf_phys, 36, 1) == 0) {
            char vendor[9];
            char product[17];
            unsigned i;

            for (i = 0; i < 8; ++i) {
                vendor[i] = (char)buf[8 + i];
            }
            vendor[8] = '\0';
            for (i = 0; i < 16; ++i) {
                product[i] = (char)buf[16 + i];
            }
            product[16] = '\0';
            axys_printf("usb-storage: %s %s\n", vendor, product);
        }
    }
    /* READ CAPACITY(10): last LBA + block length; 512-byte blocks only. */
    {
        axys_uint32_t last;
        axys_uint32_t block;

        axys_memset(cdb, 0, sizeof(cdb));
        cdb[0] = SCSI_READ_CAPACITY_10;
        axys_memset(buf, 0, 512);
        if (command_retry(cdb, 10, buf_phys, 8, 1) != 0) {
            axys_dma_free(buf);
            return -1;
        }
        last = get32be(buf);
        block = get32be(buf + 4);
        if (block != SECTOR_SIZE || last == 0xffffffffu) {
            axys_dma_free(buf);
            return -1;
        }
        sectors = (axys_uint64_t)last + 1u;
    }
    axys_dma_free(buf);
    ready = 1;
    return 0;
}

axys_uint64_t axys_usb_storage_sectors(void)
{
    return ready ? sectors : 0u;
}

static int transfer(axys_uint64_t lba, axys_uint32_t count, axys_uint8_t *buffer, int is_write)
{
    axys_uint8_t *dma;
    axys_uint64_t dma_phys;

    if (!ready || count == 0 || sectors == 0 || lba >= sectors ||
        (axys_uint64_t)count > sectors - lba || lba > 0xffffffffull) {
        return -1; /* READ(10) carries a 32-bit LBA; bigger disks need READ(16) */
    }
    while (count != 0) {
        axys_uint32_t chunk = count > SECTORS_PER_COMMAND ? SECTORS_PER_COMMAND : count;
        axys_uint8_t cdb[16];
        axys_uint32_t bytes = chunk * SECTOR_SIZE;

        dma = axys_dma_alloc(bytes, 16, 0, AXYS_DMA_32BIT, &dma_phys);
        if (dma == AXYS_NULL) {
            return -1;
        }
        if (is_write) {
            axys_memcpy(dma, buffer, bytes);
        }
        axys_memset(cdb, 0, sizeof(cdb));
        cdb[0] = is_write ? SCSI_WRITE_10 : SCSI_READ_10;
        put32be(cdb + 2, (axys_uint32_t)lba);
        put16be(cdb + 7, (axys_uint16_t)chunk);
        if (command_retry(cdb, 10, dma_phys, bytes, !is_write) != 0) {
            axys_dma_free(dma);
            return -1;
        }
        if (!is_write) {
            axys_memcpy(buffer, dma, bytes);
        }
        axys_dma_free(dma);
        buffer += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

int axys_usb_storage_read(axys_uint64_t lba, axys_uint32_t count, void *buffer)
{
    if (buffer == AXYS_NULL) {
        return -1;
    }
    return transfer(lba, count, buffer, 0);
}

int axys_usb_storage_write(axys_uint64_t lba, axys_uint32_t count, const void *buffer)
{
    if (buffer == AXYS_NULL) {
        return -1;
    }
    return transfer(lba, count, (axys_uint8_t *)buffer, 1);
}
