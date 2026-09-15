// SPDX-License-Identifier: MIT
// Optional certificate-only Google CBMEM VPD publisher for QEMU.

#include "std/acpi.h"
#include "compat-fw-cfg.h"

#define CERT_VPD_MAGIC 0x43524f53
#define CERT_VPD_MAX_CERT 16384
#define CERT_VPD_MAX_BLOB (16 + 1 + 1 + 4 + 3 + CERT_VPD_MAX_CERT + 1)

struct cert_lbio {
    char signature[4];
    u32 header_bytes, header_checksum, table_bytes, table_checksum, table_entries;
    u32 tag, entry_bytes;
    u64 cbmem_addr;
} PACKED;

static u16
cert_ip_checksum(const void *data, u32 size)
{
    const u8 *p = data;
    u32 sum = 0;
    while (size >= 2) {
        sum += p[0] | (p[1] << 8);
        p += 2;
        size -= 2;
    }
    if (size)
        sum += p[0];
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

static int
cert_vpd_valid(const u8 *blob, u32 size)
{
    if (size < 25 || size > CERT_VPD_MAX_BLOB)
        return 0;
    const u32 *header = (const void *)blob;
    if (header[0] != CERT_VPD_MAGIC || header[1] != 1 ||
        header[2] != size - 16 || header[3] != 0)
        return 0;
    if (blob[16] != 1 || blob[17] != 4 || memcmp(blob + 18, "cert", 4))
        return 0;
    u32 pos = 22, length = 0, groups = 0;
    u8 byte;
    do {
        if (pos >= size || ++groups > 3)
            return 0;
        byte = blob[pos++];
        length = (length << 7) | (byte & 0x7f);
    } while (byte & 0x80);
    return pos < size && length > 0 && length <= CERT_VPD_MAX_CERT &&
        length == size - pos - 1 && blob[size - 1] == 0;
}

static struct acpi_table_header *
cert_append_acpi(struct acpi_table_header *old, u32 signature,
               struct acpi_table_header *extra, u32 stride)
{
    if (!old || old->signature != signature ||
        old->length < sizeof(*old) || old->length > 65536 ||
        (old->length - sizeof(*old)) % stride || checksum(old, old->length))
        panic("Certificate VPD: invalid ACPI root table\n");
    struct acpi_table_header *table = malloc_high(old->length + stride);
    if (!table)
        panic("Certificate VPD: ACPI root allocation failed\n");
    memcpy(table, old, old->length);
    u8 *entry = (void *)table + old->length;
    if (stride == 8)
        *(u64 *)entry = (u32)extra;
    else
        *(u32 *)entry = (u32)extra;
    table->length += stride;
    table->checksum -= checksum(table, table->length);
    return table;
}

static void
cert_vpd_setup(void)
{
    struct romfile_s *file = romfile_find(CERT_VPD_FW_CFG);
    if (!file)
        return;
    if (file->size < 25 || file->size > CERT_VPD_MAX_BLOB)
        panic("Certificate VPD: invalid fw_cfg size\n");
    if (!RsdpAddr || RsdpAddr->signature != RSDP_SIGNATURE ||
        checksum(RsdpAddr, 20) ||
        (RsdpAddr->revision > 1 &&
         (RsdpAddr->length != sizeof(*RsdpAddr) ||
          checksum(RsdpAddr, RsdpAddr->length))))
        panic("Certificate VPD: valid ACPI discovery is required\n");
    if (!RsdpAddr->rsdt_physical_address &&
        !(RsdpAddr->revision > 1 && RsdpAddr->xsdt_physical_address))
        panic("Certificate VPD: ACPI root pointer is missing\n");

    // ZoneHigh is permanent E820_RESERVED memory; no fixed address is assumed.
    u32 allocation = ALIGN(sizeof(struct cert_lbio) + file->size, PAGE_SIZE);
    struct cert_lbio *lbio = memalign_high(PAGE_SIZE, allocation);
    if (!lbio)
        panic("Certificate VPD: reserved memory allocation failed\n");
    memset(lbio, 0, allocation);
    u8 *blob = (void *)(lbio + 1);
    if (file->copy(file, blob, file->size) != (int)file->size ||
        !cert_vpd_valid(blob, file->size))
        panic("Certificate VPD: malformed certificate container\n");
    e820_add((u32)lbio, allocation, E820_RESERVED);
    memcpy(lbio->signature, "LBIO", 4);
    lbio->header_bytes = offsetof(struct cert_lbio, tag);
    lbio->table_bytes = sizeof(*lbio) - lbio->header_bytes;
    lbio->table_entries = 1;
    lbio->tag = 0x2c;
    lbio->entry_bytes = lbio->table_bytes;
    lbio->cbmem_addr = (u32)blob;
    lbio->table_checksum = cert_ip_checksum(&lbio->tag, lbio->table_bytes);
    lbio->header_checksum = cert_ip_checksum(lbio, lbio->header_bytes);

    // Scope(\_SB) { Device(CBTB) { _HID="BOOT0000"; _STA=15;
    //   _CRS=Memory32Fixed(ReadOnly, LBIO address, sizeof(LBIO)); } }
    u8 aml[] = {
        0x10, 0x3a, 0x5c, '_', 'S', 'B', '_',
        0x5b, 0x82, 0x32, 'C', 'B', 'T', 'B',
        0x08, '_', 'H', 'I', 'D', 0x0d, 'B', 'O', 'O', 'T', '0', '0', '0', '0', 0,
        0x08, '_', 'S', 'T', 'A', 0x0a, 0x0f,
        0x08, '_', 'C', 'R', 'S', 0x11, 0x11, 0x0a, 0x0e,
        0x86, 0x09, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x79, 0x00
    };
    *(u32 *)(aml + 49) = (u32)lbio;
    *(u32 *)(aml + 53) = sizeof(*lbio);
    struct acpi_table_header *ssdt = malloc_high(sizeof(*ssdt) + sizeof(aml));
    if (!ssdt)
        panic("Certificate VPD: SSDT allocation failed\n");
    memset(ssdt, 0, sizeof(*ssdt));
    memcpy(&ssdt->signature, "SSDT", 4);
    ssdt->length = sizeof(*ssdt) + sizeof(aml);
    ssdt->revision = 2;
    memcpy(ssdt->oem_id, "SVPD  ", 6);
    memcpy(ssdt->oem_table_id, "NATIVEPD", 8);
    ssdt->oem_revision = 1;
    memcpy(ssdt->asl_compiler_id, "SVPD", 4);
    ssdt->asl_compiler_revision = 1;
    memcpy(ssdt + 1, aml, sizeof(aml));
    ssdt->checksum = -checksum(ssdt, ssdt->length);

    if (RsdpAddr->rsdt_physical_address)
        RsdpAddr->rsdt_physical_address = (u32)cert_append_acpi(
            (void *)RsdpAddr->rsdt_physical_address, RSDT_SIGNATURE, ssdt, 4);
    if (RsdpAddr->revision > 1 && RsdpAddr->xsdt_physical_address) {
        if (RsdpAddr->xsdt_physical_address >> 32)
            panic("Certificate VPD: ACPI root above 4GiB unsupported\n");
        RsdpAddr->xsdt_physical_address = (u32)cert_append_acpi(
            (void *)(u32)RsdpAddr->xsdt_physical_address, XSDT_SIGNATURE, ssdt, 8);
    }
    RsdpAddr->checksum -= checksum(RsdpAddr, 20);
    if (RsdpAddr->revision > 1)
        RsdpAddr->extended_checksum -= checksum(RsdpAddr, RsdpAddr->length);
    dprintf(1, "Certificate VPD: reserved LBIO=%p CBMEM=%p size=%u SSDT=%p\n",
            lbio, blob, allocation, ssdt);
}
