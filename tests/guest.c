// SPDX-License-Identifier: MIT
// Freestanding multiboot diagnostic.
#include "fixture.h"
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
#define PACKED __attribute__((packed))
struct header { u32 signature, length; u8 rest[28]; } PACKED;
struct rsdp {
    char signature[8]; u8 checksum; char oem[6]; u8 revision;
    u32 rsdt, length; u64 xsdt; u8 ext_checksum, reserved[3];
} PACKED;
struct lbio {
    char signature[4]; u32 header_bytes, header_checksum, table_bytes;
    u32 table_checksum, entries, tag, entry_bytes; u64 cbmem;
} PACKED;
struct mmap_entry { u32 size; u64 base, length; u32 type; } PACKED;
struct multiboot {
    u32 flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
    u32 symbols[4], mmap_length, mmap_addr;
};

static void
out(u16 port, u8 value)
{
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

static void
say(const char *s)
{
    while (*s)
        out(0xe9, *s++);
}

static void __attribute__((noreturn))
finish(int success)
{
    say(success ? "GUEST PASS\n" : "GUEST FAIL\n");
    out(0xf4, success ? 0x10 : 0x11);
    for (;;)
        __asm__ volatile("hlt");
}

#define REQUIRE(x) do { if (!(x)) { say("Failed: " #x "\n"); finish(0); } } while (0)

static int
equal(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static u8
sum(const void *p, u32 n)
{
    const u8 *bytes = p;
    u8 s = 0;
    while (n--)
        s += *bytes++;
    return s;
}

static u16
ip_sum(const void *p, u32 n)
{
    const u8 *b = p;
    u32 s = 0;
    while (n >= 2) {
        s += b[0] | b[1] << 8;
        b += 2;
        n -= 2;
    }
    if (n) s += *b;
    while (s >> 16) s = (s & 65535) + (s >> 16);
    return ~s;
}

static struct rsdp *
find_rsdp(u32 start, u32 end)
{
    for (u32 addr = start; addr < end; addr += 16)
        if (equal((void *)addr, "RSD PTR ", 8))
            return (void *)addr;
    return 0;
}

static u32
find_lbio(struct header *root, u32 stride)
{
    REQUIRE(root && root->length >= 36 && root->length < 65536);
    REQUIRE(sum(root, root->length) == 0);
    REQUIRE((root->length - 36) % stride == 0);
    u32 result = 0;
    for (u32 offset = 36; offset < root->length; offset += stride) {
        u64 address = stride == 8 ? *(u64 *)((u8 *)root + offset) :
                                   *(u32 *)((u8 *)root + offset);
        REQUIRE(address < 0x100000000ULL);
        struct header *table = (void *)(u32)address;
        REQUIRE(table->length >= 36 && table->length < 65536);
        REQUIRE(sum(table, table->length) == 0);
        if (!equal(table, "SSDT", 4) || table->length != 36 + 59)
            continue;
        u8 *aml = (void *)(table + 1);
        if (!equal(aml + 20, "BOOT0000", 8))
            continue;
        REQUIRE(!result);
        REQUIRE(aml[1] == 58 && aml[9] == 50 && aml[42] == 17);
        REQUIRE(aml[45] == 0x86 && aml[46] == 9 && aml[48] == 0);
        REQUIRE(*(u32 *)(aml + 53) == 40);
        result = *(u32 *)(aml + 49);
    }
    return result;
}

void
guest_main(struct multiboot *info)
{
    say("Synthetic SeaBIOS VPD diagnostic\n");
    REQUIRE(info->flags & 4);
    const char *cmdline = (void *)info->cmdline;
    int expected = 0;
    for (const char *p = cmdline; *p; p++)
        if (equal(p, "vpd=1", 5)) expected = 1;
    u16 ebda_segment;
    __asm__ volatile("movw 0x40e,%0" : "=r"(ebda_segment));
    struct rsdp *r = find_rsdp((u32)ebda_segment << 4,
                              ((u32)ebda_segment << 4) + 1024);
    if (!r) r = find_rsdp(0xe0000, 0x100000);
    REQUIRE(r && sum(r, 20) == 0);
    u32 addr = find_lbio((void *)r->rsdt, 4);
    if (r->revision > 1 && r->xsdt) {
        REQUIRE(r->length == sizeof(*r) && sum(r, r->length) == 0);
        REQUIRE(r->xsdt < 0x100000000ULL);
        REQUIRE(find_lbio((void *)(u32)r->xsdt, 8) == addr);
    }
    REQUIRE((addr != 0) == expected);
    if (!expected) finish(1);
    struct lbio *l = (void *)addr;
    REQUIRE(addr % 4096 == 0 && equal(l, "LBIO", 4));
    REQUIRE(l->header_bytes == 24 && l->table_bytes == 16 && l->entries == 1);
    REQUIRE(l->tag == 0x2c && l->entry_bytes == 16);
    REQUIRE(ip_sum(l, 24) == 0 && ip_sum(&l->tag, 16) == l->table_checksum);
    REQUIRE(l->cbmem == addr + 40);
    u32 *vpd = (void *)(u32)l->cbmem;
    REQUIRE(vpd[0] == 0x43524f53 && vpd[1] == 1 && vpd[3] == 0);
    const char cert[] = SYNTHETIC_CERT;
    REQUIRE(vpd[2] == sizeof(cert) - 1 + 8);
    const u8 *record = (void *)(vpd + 4);
    REQUIRE(equal(record, "\1\4cert", 6) && record[6] == sizeof(cert) - 1);
    REQUIRE(equal(record + 7, cert, sizeof(cert)));
    REQUIRE(info->flags & (1 << 6));
    int reserved = 0;
    for (u32 pos = info->mmap_addr; pos < info->mmap_addr + info->mmap_length;) {
        const struct mmap_entry *m = (void *)pos;
        REQUIRE(m->size >= 20);
        if (m->type == 2 && m->base <= addr && m->base + m->length >= addr + 4096)
            reserved = 1;
        pos += m->size + 4;
    }
    REQUIRE(reserved);
    finish(1);
}
