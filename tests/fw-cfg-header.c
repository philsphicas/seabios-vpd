// SPDX-License-Identifier: MIT
extern void panic(const char *message) __attribute__((noreturn));
#include "../platform/compat-fw-cfg.h"

const char *
certificate_option(void)
{
    return CERT_VPD_FW_CFG;
}
