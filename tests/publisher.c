// SPDX-License-Identifier: MIT
// Execute the actual firmware header against bounded, low-address host memory.
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __TYPES_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define PACKED __attribute__((packed))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define PAGE_SIZE 4096
#define E820_RESERVED 2
#include "std/acpi.h"
#include "romfile.h"

static u8 arena[131072] __attribute__((aligned(PAGE_SIZE)));
static u32 used, reserved_addr, reserved_size;
static int allocations, fail_allocation, short_read;
static struct rsdp_descriptor *RsdpAddr;
static jmp_buf panic_jump;
static const char *panic_message;
static struct romfile_s files[4];
static const u8 *file_data[4];
static int file_count;

static void __attribute__((noreturn))
panic(const char *message)
{
    panic_message = message;
    longjmp(panic_jump, 1);
}

#define dprintf(...) ((void)0)

static u8
checksum(const void *data, u32 size)
{
    const u8 *p = data;
    u8 sum = 0;
    while (size--)
        sum += *p++;
    return sum;
}

static void *
memalign_high(u32 align, u32 size)
{
    if (++allocations == fail_allocation)
        return NULL;
    used = ALIGN(used, align);
    assert(used + size <= sizeof(arena));
    void *p = arena + used;
    used += size;
    return p;
}

static void *
malloc_high(u32 size)
{
    return memalign_high(16, size);
}

static void
e820_add(u32 addr, u32 size, u32 type)
{
    assert(type == E820_RESERVED);
    assert(addr % PAGE_SIZE == 0 && size % PAGE_SIZE == 0);
    reserved_addr = addr;
    reserved_size = size;
}

struct romfile_s *
romfile_find(const char *name)
{
    for (int i = 0; i < file_count; i++)
        if (!strcmp(files[i].name, name))
            return &files[i];
    return NULL;
}

static int
copy_file(struct romfile_s *file, void *dest, u32 maxlen)
{
    assert(file->size <= maxlen);
    memcpy(dest, file_data[file - files], file->size);
    return short_read ? (int)file->size - 1 : (int)file->size;
}

#include "../platform/cert-vpd.h"

static u8 blob[CERT_VPD_MAX_BLOB + 1];
static u32 blob_size;

static void
make_blob(u32 size)
{
    assert(size <= CERT_VPD_MAX_CERT + 1);
    memset(blob, 0, sizeof(blob));
    u32 *header = (void *)blob;
    header[0] = CERT_VPD_MAGIC;
    header[1] = 1;
    memcpy(blob + 16, "\x01\x04" "cert", 6);
    u32 pos = 22;
    if (size >= 16384)
        blob[pos++] = 0x80 | (size >> 14);
    if (size >= 128)
        blob[pos++] = 0x80 | ((size >> 7) & 127);
    blob[pos++] = size & 127;
    memset(blob + pos, 'X', size);
    blob_size = pos + size + 1;
    header[2] = blob_size - 16;
}

static void
add_file(const char *name, const void *data, u32 size)
{
    assert(file_count < 4 && strlen(name) <= 55);
    struct romfile_s *file = &files[file_count];
    strcpy(file->name, name);
    file->size = size;
    file->copy = copy_file;
    file_data[file_count++] = data;
}

static void
fix_rsdp(void)
{
    RsdpAddr->checksum -= checksum(RsdpAddr, 20);
    RsdpAddr->extended_checksum -= checksum(RsdpAddr, sizeof(*RsdpAddr));
}

static void
reset(void)
{
    assert((uintptr_t)arena + sizeof(arena) <= UINT32_MAX);
    memset(arena, 0, sizeof(arena));
    used = reserved_addr = reserved_size = 0;
    allocations = fail_allocation = short_read = file_count = 0;
    panic_message = NULL;
    RsdpAddr = malloc_high(sizeof(*RsdpAddr));
    RsdpAddr->signature = RSDP_SIGNATURE;
    RsdpAddr->revision = 2;
    RsdpAddr->length = sizeof(*RsdpAddr);
    struct acpi_table_header *rsdt = malloc_high(sizeof(*rsdt) + 4);
    struct acpi_table_header *xsdt = malloc_high(sizeof(*xsdt) + 8);
    rsdt->signature = RSDT_SIGNATURE;
    xsdt->signature = XSDT_SIGNATURE;
    rsdt->length = sizeof(*rsdt) + 4;
    xsdt->length = sizeof(*xsdt) + 8;
    *(u32 *)(rsdt + 1) = 0x12340000;
    *(u64 *)(xsdt + 1) = 0x12340000;
    rsdt->checksum = -checksum(rsdt, rsdt->length);
    xsdt->checksum = -checksum(xsdt, xsdt->length);
    RsdpAddr->rsdt_physical_address = (u32)(uintptr_t)rsdt;
    RsdpAddr->xsdt_physical_address = (uintptr_t)xsdt;
    fix_rsdp();
    make_blob(127);
}

#define EXPECT_PANIC(expression, text) do { \
    panic_message = NULL; \
    if (!setjmp(panic_jump)) { \
        expression; \
        assert(!"expected firmware panic"); \
    } \
    assert(panic_message && strstr(panic_message, text)); \
} while (0)

static void
test_format(void)
{
    const u32 sizes[] = {1, 127, 128, 16383, 16384};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        make_blob(sizes[i]);
        assert(cert_vpd_valid(blob, blob_size));
        for (u32 n = 0; n < blob_size; n++)
            assert(!cert_vpd_valid(blob, n));
        assert(!cert_vpd_valid(blob, blob_size + 1));
    }
    make_blob(0);
    assert(!cert_vpd_valid(blob, blob_size));
    make_blob(16385);
    assert(!cert_vpd_valid(blob, blob_size));
    const unsigned fields[] = {0, 4, 8, 12, 16, 17, 18, 22};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        make_blob(127);
        blob[fields[i]] ^= 0xff;
        assert(!cert_vpd_valid(blob, blob_size));
    }
    make_blob(127);
    blob[blob_size - 1] = 1;
    assert(!cert_vpd_valid(blob, blob_size));
    memset(blob + 22, 0x80, 4);
    assert(!cert_vpd_valid(blob, blob_size));
    assert(cert_ip_checksum("\1\2", 2) == 0xfdfe);
    assert(cert_ip_checksum("\1\2\3", 3) == 0xfdfb);
}

static void
test_handoff(void)
{
    reset();
    assert(!compat_tpm_handoff_enabled());
    add_file(TPM_HANDOFF_FW_CFG, "1", 1);
    assert(compat_tpm_handoff_enabled());
    file_data[0] = (const u8 *)"0";
    assert(!compat_tpm_handoff_enabled());
    file_data[0] = (const u8 *)"1";
    assert(compat_tpm_handoff_enabled());
    file_data[0] = (const u8 *)"x";
    EXPECT_PANIC(compat_tpm_handoff_enabled(), "ASCII");
    file_data[0] = (const u8 *)"\1";
    EXPECT_PANIC(compat_tpm_handoff_enabled(), "ASCII");
    file_data[0] = (const u8 *)"1";
    files[0].size = 0;
    EXPECT_PANIC(compat_tpm_handoff_enabled(), "ASCII");
    files[0].size = 2;
    EXPECT_PANIC(compat_tpm_handoff_enabled(), "ASCII");
    files[0].size = 1;
    short_read = 1;
    EXPECT_PANIC(compat_tpm_handoff_enabled(), "ASCII");
}

static void
test_publish(void)
{
    for (int cert_size = 1; cert_size <= 127; cert_size += 126) {
        reset();
        make_blob(cert_size);
        add_file(CERT_VPD_FW_CFG, blob, blob_size);
        cert_vpd_setup();
        assert(reserved_size == PAGE_SIZE);
        struct cert_lbio *lbio = (void *)(uintptr_t)reserved_addr;
        assert(!memcmp(lbio->signature, "LBIO", 4));
        assert(sizeof(*lbio) == 40 && lbio->header_bytes == 24);
        assert(lbio->tag == 0x2c && lbio->table_entries == 1);
        assert(lbio->table_bytes == 16 && lbio->entry_bytes == 16);
        assert(cert_ip_checksum(lbio, 24) == 0);
        assert(cert_ip_checksum(&lbio->tag, 16) == lbio->table_checksum);
        assert(lbio->cbmem_addr == (uintptr_t)(lbio + 1));
        assert(!memcmp(lbio + 1, blob, blob_size));
        assert(!checksum(RsdpAddr, 20));
        assert(!checksum(RsdpAddr, sizeof(*RsdpAddr)));
        struct acpi_table_header *r = (void *)(uintptr_t)RsdpAddr->rsdt_physical_address;
        struct acpi_table_header *x = (void *)(uintptr_t)RsdpAddr->xsdt_physical_address;
        assert(r->length == sizeof(*r) + 8 && !checksum(r, r->length));
        assert(x->length == sizeof(*x) + 16 && !checksum(x, x->length));
        assert(*(u32 *)(r + 1) == 0x12340000);
        assert(*(u64 *)(x + 1) == 0x12340000);
        u32 addr = ((u32 *)(r + 1))[1];
        assert(addr == ((u64 *)(x + 1))[1]);
        struct acpi_table_header *ssdt = (void *)(uintptr_t)addr;
        assert(!memcmp(&ssdt->signature, "SSDT", 4));
        assert(ssdt->length == sizeof(*ssdt) + 59);
        assert(!checksum(ssdt, ssdt->length));
        const u8 *aml = (void *)(ssdt + 1);
        assert(aml[1] + 1 == 59 && aml[9] + 9 == 59 && aml[42] + 42 == 59);
        assert(!memcmp(aml + 20, "BOOT0000", 8));
        assert(aml[45] == 0x86 && aml[46] == 9 && aml[48] == 0);
        u32 resource[2];
        memcpy(resource, aml + 49, sizeof(resource));
        assert(resource[0] == reserved_addr && resource[1] == 40);
        assert(aml[57] == 0x79 && aml[58] == 0);
    }
    reset();
    RsdpAddr = NULL;
    cert_vpd_setup();
    assert(!reserved_size);
    reset();
    make_blob(16384);
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    cert_vpd_setup();
    assert(reserved_size == 5 * PAGE_SIZE);
    reset();
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    RsdpAddr->xsdt_physical_address = 0;
    RsdpAddr->revision = 0;
    fix_rsdp();
    cert_vpd_setup();
    assert(!checksum(RsdpAddr, 20));
    reset();
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    RsdpAddr->rsdt_physical_address = 0;
    fix_rsdp();
    cert_vpd_setup();
    assert(!checksum(RsdpAddr, sizeof(*RsdpAddr)));
}

static void
test_failures(void)
{
    reset();
    add_file(CERT_VPD_FW_CFG, blob, 0);
    EXPECT_PANIC(cert_vpd_setup(), "size");
    reset();
    add_file(CERT_VPD_FW_CFG, blob, CERT_VPD_MAX_BLOB + 1);
    EXPECT_PANIC(cert_vpd_setup(), "size");
    reset();
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    RsdpAddr = NULL;
    EXPECT_PANIC(cert_vpd_setup(), "ACPI discovery");
    reset();
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    RsdpAddr->checksum++;
    EXPECT_PANIC(cert_vpd_setup(), "ACPI discovery");
    reset();
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    RsdpAddr->rsdt_physical_address = RsdpAddr->xsdt_physical_address = 0;
    fix_rsdp();
    EXPECT_PANIC(cert_vpd_setup(), "root pointer");
    reset();
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    RsdpAddr->xsdt_physical_address = 1ULL << 32;
    fix_rsdp();
    EXPECT_PANIC(cert_vpd_setup(), "above 4GiB");
    for (volatile int failure = 1; failure <= 4; failure++) {
        reset();
        add_file(CERT_VPD_FW_CFG, blob, blob_size);
        fail_allocation = allocations + failure;
        EXPECT_PANIC(cert_vpd_setup(), "allocation");
    }
    reset();
    add_file(CERT_VPD_FW_CFG, blob, blob_size);
    short_read = 1;
    EXPECT_PANIC(cert_vpd_setup(), "malformed");
    short_read = 0;
    blob[0] = 0;
    EXPECT_PANIC(cert_vpd_setup(), "malformed");
    for (volatile int failure = 0; failure < 4; failure++) {
        reset();
        add_file(CERT_VPD_FW_CFG, blob, blob_size);
        struct acpi_table_header *r = (void *)(uintptr_t)RsdpAddr->rsdt_physical_address;
        if (failure == 0) r->signature = 0;
        if (failure == 1) r->length = 1;
        if (failure == 2) r->length = 65537;
        if (failure == 3) r->checksum++;
        EXPECT_PANIC(cert_vpd_setup(), "root table");
    }
}

int
main(void)
{
    test_format();
    test_handoff();
    test_publish();
    test_failures();
    puts("PASS: format/bounds, handoff, actual publisher/AML/checksums, failures");
    return 0;
}
