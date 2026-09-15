// SPDX-License-Identifier: MIT
// Compile the patched preparation and command encoders; fake only their I/O.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Exit normally on assertion failure so mutation checks do not dump core.
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Preparation assertion failed: %s\n", #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define __TYPES_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
union u64_u32_u { struct { u32 lo, hi; }; u64 val; };
#define PACKED __attribute__((packed))
#include "byteorder.h"
#include "std/tcg.h"
#include "hw/tpm_drivers.h"
#include "romfile.h"

#define CONFIG_TCGBIOS 1
#define dprintf(...) ((void)0)
#define tpm_simple_cmd(...) assert(!"Unexpected TPM 1.2 command")
static TPMVersion TPM_version = TPM_VERSION_2;
static int TPM_has_physical_presence;
static struct romfile_s option;
static const char *option_value;
static u32 events[16], event_count, failing_command, clock_reads;
enum { ACTION = 0x10000, SEPARATORS, FAILURE };

static void
record(u32 event)
{
    assert(event_count < sizeof(events) / sizeof(*events));
    events[event_count++] = event;
}

static void __attribute__((noreturn))
panic(const char *message)
{
    fputs(message, stderr);
    exit(EXIT_FAILURE);
}

#include "../platform/compat-fw-cfg.h"

struct romfile_s *
romfile_find(const char *name)
{
    assert(strcmp(name, TPM_HANDOFF_FW_CFG) == 0);
    return option_value ? &option : NULL;
}

static int
copy_option(struct romfile_s *file, void *destination, u32 maximum)
{
    assert(file == &option && maximum == 1 && file->size == 1);
    memcpy(destination, option_value, 1);
    return 1;
}

static u64
rdtscll(void)
{
    return ++clock_reads;
}

static u8
random_byte(u32 index)
{
    return 0x40 + index;
}

int
tpmhw_transmit(u8 locality, struct tpm_req_header *request, void *response,
               u32 *response_length, enum tpmDurationType duration)
{
    u32 command = be32_to_cpu(request->ordinal);
    assert(locality == 0);
    record(command);
    if (command == TPM2_CC_StirRandom) {
        struct tpm2_req_stirrandom *stir = (void *)request;
        assert(duration == TPM_DURATION_TYPE_SHORT);
        assert(be32_to_cpu(request->totlen) == sizeof(*stir));
        assert(be16_to_cpu(request->tag) == TPM2_ST_NO_SESSIONS);
        assert(be16_to_cpu(stir->size) == sizeof(stir->stir));
        assert(clock_reads == 2);
    } else if (command == TPM2_CC_GetRandom) {
        struct tpm2_req_getrandom *get = (void *)request;
        assert(duration == TPM_DURATION_TYPE_MEDIUM);
        assert(be32_to_cpu(request->totlen) == sizeof(*get));
        assert(be16_to_cpu(request->tag) == TPM2_ST_NO_SESSIONS);
        assert(be16_to_cpu(get->bytesRequested) == 20);
    } else {
        assert(command == TPM2_CC_HierarchyChangeAuth);
        struct tpm2_req_hierarchychangeauth *change = (void *)request;
        assert(duration == TPM_DURATION_TYPE_MEDIUM);
        assert(be32_to_cpu(request->totlen) == sizeof(*change));
        assert(be16_to_cpu(request->tag) == TPM2_ST_SESSIONS);
        assert(be32_to_cpu(change->authhandle) == TPM2_RH_PLATFORM);
        assert(be32_to_cpu(change->authblocksize) == sizeof(change->authblock));
        assert(be32_to_cpu(change->authblock.handle) == TPM2_RS_PW);
        assert(be16_to_cpu(change->newAuth.size) == 20);
        for (u32 i = 0; i < sizeof(change->newAuth.buffer); i++)
            assert(change->newAuth.buffer[i] == random_byte(i));
    }
    if (command == failing_command)
        return -1;
    u32 expected = command == TPM2_CC_GetRandom ?
        sizeof(struct tpm2_res_getrandom) : sizeof(struct tpm_rsp_header);
    assert(*response_length >= expected);
    memset(response, 0, expected);
    struct tpm_rsp_header *header = response;
    header->tag = cpu_to_be16(TPM2_ST_NO_SESSIONS);
    header->totlen = cpu_to_be32(expected);
    *response_length = expected;
    if (command == TPM2_CC_GetRandom) {
        struct tpm2_res_getrandom *get = response;
        get->rnd.size = cpu_to_be16(sizeof(get->rnd.buffer));
        for (u32 i = 0; i < sizeof(get->rnd.buffer); i++)
            get->rnd.buffer[i] = random_byte(i);
    }
    return 0;
}

static void
tpm_set_failure(void)
{
    record(FAILURE);
}

static void
tpm_add_action(u32 pcr, const char *message)
{
    assert(pcr == 4 && strcmp(message, "Calling INT 19h") == 0);
    record(ACTION);
}

static void
tpm_add_event_separators(void)
{
    record(SEPARATORS);
}

#include "prepboot-functions.h"

static void
check(const char *value, u32 failure, const u32 *expected, u32 count)
{
    option_value = value;
    option.size = 1;
    option.copy = copy_option;
    event_count = clock_reads = 0;
    failing_command = failure;
    tpm_prepboot();
    assert(event_count == count);
    assert(memcmp(events, expected, count * sizeof(*events)) == 0);
}

int
main(void)
{
    const u32 normal[] = {TPM2_CC_StirRandom, TPM2_CC_GetRandom,
                         TPM2_CC_HierarchyChangeAuth, ACTION, SEPARATORS};
    const u32 handoff[] = {TPM2_CC_StirRandom, TPM2_CC_GetRandom, ACTION, SEPARATORS};
    const u32 stir_failure[] = {TPM2_CC_StirRandom, FAILURE, ACTION, SEPARATORS};
    const u32 random_failure[] = {TPM2_CC_StirRandom, TPM2_CC_GetRandom,
                                 FAILURE, ACTION, SEPARATORS};
    const u32 auth_failure[] = {TPM2_CC_StirRandom, TPM2_CC_GetRandom,
                               TPM2_CC_HierarchyChangeAuth, FAILURE, ACTION, SEPARATORS};
    check(NULL, 0, normal, 5);
    check("0", 0, normal, 5);
    check("1", 0, handoff, 4);
    check(NULL, TPM2_CC_StirRandom, stir_failure, 4);
    check("1", TPM2_CC_StirRandom, stir_failure, 4);
    check("0", TPM2_CC_GetRandom, random_failure, 5);
    check("1", TPM2_CC_GetRandom, random_failure, 5);
    check("0", TPM2_CC_HierarchyChangeAuth, auth_failure, 6);
    check("1", TPM2_CC_HierarchyChangeAuth, handoff, 4);
    puts("PASS: patched TPM preparation command sequence, handoff, measurements and failures");
    return 0;
}
