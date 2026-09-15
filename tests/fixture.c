// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fixture.h"

int
main(void)
{
    const char certificate[] = SYNTHETIC_CERT;
    const uint32_t length = sizeof(certificate) - 1;
    const uint32_t header[] = {0x43524f53, 1, length + 8, 0};
    const unsigned char prefix[] = {1, 4, 'c', 'e', 'r', 't', length};
    _Static_assert(sizeof(certificate) - 1 < 128, "single-group fixture");
    if (fwrite(header, sizeof(header), 1, stdout) != 1 ||
        fwrite(prefix, sizeof(prefix), 1, stdout) != 1 ||
        fwrite(certificate, length + 1, 1, stdout) != 1 ||
        fflush(stdout)) {
        perror("writing synthetic VPD");
        return 1;
    }
    return 0;
}
